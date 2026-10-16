// SPDX-License-Identifier: GPL-2.0
/*
 * Serve an H.264 or H.265 stream from stdin over RTSP.
 *
 *   cam2h264 -c /dev/videoN -n 0 -o - | h264-rtsp [-p port]
 *   cam2h264 -c /dev/videoN -n 0 -x -o - | h264-rtsp -H [-p port]
 *
 * Play rtsp://<board>:8554/ in VLC, ffplay, mpv or Home Assistant.
 *
 * The input is an Annex B stream with one slice per picture, as coda and
 * the WAVE420L write it. RTP (RFC 6184, packetization mode 1; RFC 7798
 * for H.265) goes over the RTSP connection (RFC 2326, section 10.12). A
 * client that asks for UDP gets 461 Unsupported Transport, and then
 * retries over TCP.
 *
 * A client starts at the next IDR picture. A client that falls behind
 * skips to the next IDR picture. It does not slow the camera or the other
 * clients.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS	4
#define MAX_INPUT	(4 << 20)
#define MAX_AU		(2 << 20)
/* ffmpeg takes at most 8192 bytes per interleaved packet */
#define MAX_PAYLOAD	1400
/* Unsent bytes at which a client skips to the next IDR picture */
#define MAX_BEHIND	(64 << 10)
#define PT		96

enum { C_FREE, C_RTSP, C_PLAY, C_CLOSE_AFTER };

struct client {
	int fd, state, need_idr, chan;
	uint16_t seq;
	char req[4096];
	size_t req_len;
	uint8_t *out;
	size_t out_len, out_off, out_cap;
};

static struct client cl[MAX_CLIENTS];
static uint8_t in[MAX_INPUT];
static size_t in_len;
static uint8_t vps[256], sps[256], pps[256];
static size_t vps_len, sps_len, pps_len;
/* H.265 instead of H.264 */
static int hevc;

static int nal_type(const uint8_t *n)
{
	return hevc ? (n[0] >> 1) & 0x3f : n[0] & 0x1f;
}

static int is_vcl(int type)
{
	return hevc ? type < 32 : type == 1 || type == 5;
}

/* IDR picture; for H.265 any IRAP picture: BLA, IDR or CRA */
static int is_idr(int type)
{
	return hevc ? type >= 16 && type <= 21 : type == 5;
}
/* The NAL units of one picture, each after its 4-byte length */
static uint8_t au[MAX_AU];
static size_t au_len;
static uint32_t ssrc, session, ts_base;

static void client_close(struct client *c)
{
	close(c->fd);
	free(c->out);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

/* Make room for n more bytes at the end of the output queue */
static int out_reserve(struct client *c, size_t n)
{
	if (c->out_off == c->out_len)
		c->out_off = c->out_len = 0;
	if (c->out_off && c->out_len + n > c->out_cap) {
		memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
		c->out_len -= c->out_off;
		c->out_off = 0;
	}
	if (c->out_len + n > c->out_cap) {
		size_t cap = 2 * (c->out_len + n);
		uint8_t *p = realloc(c->out, cap);

		if (!p)
			return -1;
		c->out = p;
		c->out_cap = cap;
	}
	return 0;
}

/*
 * Bytes not sent yet: in our queue, and in the socket. The socket buffer
 * grows to megabytes on a slow link, so count it too, or the delay grows.
 */
static size_t backlog(struct client *c)
{
	int q = 0;

	ioctl(c->fd, SIOCOUTQ, &q);
	return c->out_len - c->out_off + (q > 0 ? q : 0);
}

static void out_put(struct client *c, const void *d, size_t n)
{
	memcpy(c->out + c->out_len, d, n);
	c->out_len += n;
}

static void rtp_packet(struct client *c, uint32_t ts, int marker,
		       const uint8_t *h, size_t hlen,
		       const uint8_t *p, size_t plen)
{
	size_t len = 12 + hlen + plen;
	uint8_t hdr[16] = {
		'$', c->chan, len >> 8, len,
		0x80, (marker ? 0x80 : 0) | PT, c->seq >> 8, c->seq,
		ts >> 24, ts >> 16, ts >> 8, ts,
		ssrc >> 24, ssrc >> 16, ssrc >> 8, ssrc,
	};

	c->seq++;
	out_put(c, hdr, sizeof(hdr));
	out_put(c, h, hlen);
	out_put(c, p, plen);
}

/*
 * One NAL unit: a single packet, or fragments: FU-A (H.264, a 1-byte NAL
 * header) or FU (H.265, a 2-byte NAL header).
 */
static void rtp_nal(struct client *c, uint32_t ts, int last,
		    const uint8_t *n, size_t len)
{
	size_t nh = hevc ? 2 : 1, fh = nh + 1;
	uint8_t fu[3], *s;

	if (len <= MAX_PAYLOAD) {
		rtp_packet(c, ts, last, NULL, 0, n, len);
		return;
	}
	if (hevc) {
		fu[0] = (n[0] & 0x81) | 49 << 1;
		fu[1] = n[1];
	} else {
		fu[0] = (n[0] & 0xe0) | 28;
	}
	s = &fu[nh];
	*s = 0x80 | nal_type(n);
	n += nh;
	len -= nh;
	while (len) {
		size_t k = len > MAX_PAYLOAD - fh ? MAX_PAYLOAD - fh : len;

		if (k == len)
			*s |= 0x40;
		rtp_packet(c, ts, last && k == len, fu, fh, n, k);
		*s &= ~0x80;
		n += k;
		len -= k;
	}
}

static uint32_t au_nal_len(size_t p)
{
	return (uint32_t)au[p] << 24 | au[p + 1] << 16 | au[p + 2] << 8 |
	       au[p + 3];
}

static void queue_au(struct client *c, uint32_t ts, int idr, int has_ps)
{
	size_t p, need = au_len + vps_len + sps_len + pps_len +
		(au_len / (MAX_PAYLOAD - 3) + 8) * 19;

	if (out_reserve(c, need) < 0) {
		c->need_idr = 1;
		return;
	}
	/* Some encoders put SPS and PPS before the first IDR picture only */
	if (idr && !has_ps && sps_len && pps_len && (!hevc || vps_len)) {
		if (hevc)
			rtp_nal(c, ts, 0, vps, vps_len);
		rtp_nal(c, ts, 0, sps, sps_len);
		rtp_nal(c, ts, 0, pps, pps_len);
	}
	for (p = 0; p < au_len; ) {
		uint32_t len = au_nal_len(p);

		rtp_nal(c, ts, p + 4 + len == au_len, au + p + 4, len);
		p += 4 + len;
	}
}

static void flush_au(void)
{
	struct timespec t;
	int idr = 0, has_ps = 0, i;
	uint32_t ts;
	size_t p;

	for (p = 0; p < au_len; p += 4 + au_nal_len(p)) {
		int type = nal_type(au + p + 4);

		idr |= is_idr(type);
		has_ps |= type == (hevc ? 33 : 7);
	}
	clock_gettime(CLOCK_MONOTONIC, &t);
	ts = ts_base + (uint32_t)((uint64_t)t.tv_sec * 90000 +
				  (uint64_t)t.tv_nsec * 9 / 100000);

	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &cl[i];

		if (c->state != C_PLAY)
			continue;
		if (backlog(c) > MAX_BEHIND)
			c->need_idr = 1;
		else if (!c->need_idr || idr) {
			c->need_idr = 0;
			queue_au(c, ts, idr, has_ps);
		}
	}
	au_len = 0;
}

static void handle_nal(const uint8_t *n, size_t len)
{
	int type;

	if (len < 2)
		return;
	type = nal_type(n);
	/* Access unit delimiter, filler */
	if (type == (hevc ? 35 : 9) || type == (hevc ? 38 : 12))
		return;
	if (hevc && type == 32 && len <= sizeof(vps)) {
		memcpy(vps, n, len);
		vps_len = len;
	}
	if (type == (hevc ? 33 : 7) && len <= sizeof(sps)) {
		memcpy(sps, n, len);
		sps_len = len;
	}
	if (type == (hevc ? 34 : 8) && len <= sizeof(pps)) {
		memcpy(pps, n, len);
		pps_len = len;
	}
	if (au_len + 4 + len > sizeof(au)) {
		au_len = 0;
		return;
	}
	au[au_len++] = len >> 24;
	au[au_len++] = len >> 16;
	au[au_len++] = len >> 8;
	au[au_len++] = len;
	memcpy(au + au_len, n, len);
	au_len += len;
	if (is_vcl(type))		/* one slice per picture */
		flush_au();
}

static long next_start_code(size_t from)
{
	uint8_t *p;

	if (from >= in_len)
		return -1;
	p = memmem(in + from, in_len - from, "\0\0\1", 3);
	return p ? p - in : -1;
}

/*
 * Read stdin and pass on each NAL unit. A NAL unit is complete at the
 * next start code. Return 0 at EOF.
 */
static int read_input(void)
{
	ssize_t n = read(0, in + in_len, sizeof(in) - in_len);

	if (n <= 0)
		return n < 0 && errno == EINTR;
	in_len += n;
	for (;;) {
		long s0 = next_start_code(0), s1;
		size_t end;

		if (s0 < 0) {
			/* Keep what may be the start of a start code */
			if (in_len > 2) {
				memmove(in, in + in_len - 2, 2);
				in_len = 2;
			}
			break;
		}
		s1 = next_start_code(s0 + 3);
		if (s1 < 0) {
			if (in_len == sizeof(in)) {
				in_len = 0;	/* too big: drop it */
			} else if (s0) {
				memmove(in, in + s0, in_len - s0);
				in_len -= s0;
			}
			break;
		}
		/* Trailing zeros belong to the next 4-byte start code */
		for (end = s1; end > (size_t)s0 + 3 && !in[end - 1]; end--)
			;
		handle_nal(in + s0 + 3, end - s0 - 3);
		memmove(in, in + s1, in_len - s1);
		in_len -= s1;
	}
	return 1;
}

static char *header(const char *req, const char *name, char *val, size_t vlen)
{
	size_t n = strlen(name), k;
	const char *p = req;

	while ((p = strstr(p, "\r\n"))) {
		p += 2;
		if (strncasecmp(p, name, n) || p[n] != ':')
			continue;
		for (p += n + 1; *p == ' '; p++)
			;
		k = strcspn(p, "\r\n");
		if (k >= vlen)
			k = vlen - 1;
		memcpy(val, p, k);
		val[k] = 0;
		return val;
	}
	return NULL;
}

static void reply(struct client *c, const char *status, const char *cseq,
		  const char *extra, const char *body)
{
	char buf[4096];
	int n = snprintf(buf, sizeof(buf),
			 "RTSP/1.0 %s\r\nCSeq: %s\r\nServer: h264-rtsp\r\n%s",
			 status, cseq, extra ? extra : "");

	if (body)
		n += snprintf(buf + n, sizeof(buf) - n,
			      "Content-Length: %zu\r\n\r\n%s", strlen(body),
			      body);
	else
		n += snprintf(buf + n, sizeof(buf) - n, "\r\n");
	if (n >= (int)sizeof(buf) || out_reserve(c, n) < 0) {
		client_close(c);
		return;
	}
	out_put(c, buf, n);
}

static void base64(const uint8_t *d, size_t len, char *out)
{
	static const char t[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;

	for (i = 0; i + 2 < len; i += 3) {
		*out++ = t[d[i] >> 2];
		*out++ = t[(d[i] & 3) << 4 | d[i + 1] >> 4];
		*out++ = t[(d[i + 1] & 15) << 2 | d[i + 2] >> 6];
		*out++ = t[d[i + 2] & 63];
	}
	if (i < len) {
		*out++ = t[d[i] >> 2];
		if (i + 1 < len) {
			*out++ = t[(d[i] & 3) << 4 | d[i + 1] >> 4];
			*out++ = t[(d[i + 1] & 15) << 2];
		} else {
			*out++ = t[(d[i] & 3) << 4];
			*out++ = '=';
		}
		*out++ = '=';
	}
	*out = 0;
}

static void describe(struct client *c, const char *url, const char *cseq)
{
	char sdp[3072], fmtp[1536] = "", extra[1024];
	char v64[sizeof(vps) * 4 / 3 + 4];
	char s64[sizeof(sps) * 4 / 3 + 4], p64[sizeof(pps) * 4 / 3 + 4];

	/* Without the parameter sets, the client takes them from the stream */
	base64(vps, vps_len, v64);
	base64(sps, sps_len, s64);
	base64(pps, pps_len, p64);
	if (!hevc && sps_len >= 4 && pps_len)
		snprintf(fmtp, sizeof(fmtp),
			 "a=fmtp:%d packetization-mode=1;profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s\r\n",
			 PT, sps[1], sps[2], sps[3], s64, p64);
	else if (!hevc)
		snprintf(fmtp, sizeof(fmtp),
			 "a=fmtp:%d packetization-mode=1\r\n", PT);
	else if (vps_len && sps_len && pps_len)
		snprintf(fmtp, sizeof(fmtp),
			 "a=fmtp:%d sprop-vps=%s;sprop-sps=%s;sprop-pps=%s\r\n",
			 PT, v64, s64, p64);
	snprintf(sdp, sizeof(sdp),
		 "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=Milk-V Duo S camera\r\n"
		 "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
		 "a=range:npt=0-\r\nm=video 0 RTP/AVP %d\r\n"
		 "a=rtpmap:%d %s/90000\r\n%sa=control:track1\r\n",
		 session, PT, PT, hevc ? "H265" : "H264", fmtp);
	snprintf(extra, sizeof(extra),
		 "Content-Type: application/sdp\r\nContent-Base: %s%s\r\n", url,
		 url[strlen(url) - 1] == '/' ? "" : "/");
	reply(c, "200 OK", cseq, extra, sdp);
}

static void handle_request(struct client *c, const char *req)
{
	char method[32], url[512], cseq[32] = "0", tr[256], extra[512];
	int a = 0, b = 1;
	const char *i;

	if (sscanf(req, "%31s %511s", method, url) != 2) {
		client_close(c);
		return;
	}
	header(req, "CSeq", cseq, sizeof(cseq));
	snprintf(extra, sizeof(extra), "Session: %08X\r\n", session);

	if (!strcmp(method, "OPTIONS")) {
		reply(c, "200 OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, "
		      "PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n", NULL);
	} else if (!strcmp(method, "DESCRIBE")) {
		describe(c, url, cseq);
	} else if (!strcmp(method, "SETUP")) {
		if (!header(req, "Transport", tr, sizeof(tr)) ||
		    !strstr(tr, "RTP/AVP/TCP")) {
			reply(c, "461 Unsupported Transport", cseq, NULL, NULL);
			return;
		}
		i = strstr(tr, "interleaved=");
		if (i)
			sscanf(i + 12, "%d-%d", &a, &b);
		c->chan = a;
		snprintf(extra, sizeof(extra),
			 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X\r\n"
			 "Session: %08X;timeout=60\r\n", a, b, ssrc, session);
		reply(c, "200 OK", cseq, extra, NULL);
	} else if (!strcmp(method, "PLAY")) {
		reply(c, "200 OK", cseq, extra, NULL);
		c->state = C_PLAY;
		c->need_idr = 1;
	} else if (!strcmp(method, "PAUSE")) {
		reply(c, "200 OK", cseq, extra, NULL);
		c->state = C_RTSP;
	} else if (!strcmp(method, "TEARDOWN")) {
		reply(c, "200 OK", cseq, extra, NULL);
		c->state = C_CLOSE_AFTER;
	} else if (!strcmp(method, "GET_PARAMETER") ||
		   !strcmp(method, "SET_PARAMETER")) {
		reply(c, "200 OK", cseq, extra, NULL);
	} else {
		reply(c, "501 Not Implemented", cseq, NULL, NULL);
	}
}

/* Requests, and interleaved RTCP from the client, which is dropped */
static void client_read(struct client *c)
{
	ssize_t n = read(c->fd, c->req + c->req_len,
			 sizeof(c->req) - c->req_len);

	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR))
			client_close(c);
		return;
	}
	c->req_len += n;
	while (c->req_len && c->state != C_FREE) {
		char hdr[sizeof(c->req) + 1], len_s[16];
		size_t used;
		char *e;

		if (c->req[0] == '$') {
			if (c->req_len < 4)
				break;
			used = 4 + ((uint8_t)c->req[2] << 8 |
				    (uint8_t)c->req[3]);
			if (used > c->req_len)
				break;
		} else {
			e = memmem(c->req, c->req_len, "\r\n\r\n", 4);
			if (!e) {
				if (c->req_len == sizeof(c->req))
					client_close(c);
				break;
			}
			used = e + 4 - c->req;
			memcpy(hdr, c->req, used);
			hdr[used] = 0;
			if (header(hdr, "Content-Length", len_s, sizeof(len_s)))
				used += strtoul(len_s, NULL, 10);
			if (used > c->req_len) {
				if (used > sizeof(c->req))
					client_close(c);
				break;
			}
			if (c->state != C_CLOSE_AFTER)
				handle_request(c, hdr);
			if (c->state == C_FREE)
				break;
		}
		memmove(c->req, c->req + used, c->req_len - used);
		c->req_len -= used;
	}
}

static void client_write(struct client *c)
{
	ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);

	if (n < 0) {
		if (errno != EAGAIN && errno != EINTR)
			client_close(c);
		return;
	}
	c->out_off += n;
	if (c->out_off == c->out_len && c->state == C_CLOSE_AFTER)
		client_close(c);
}

int main(int argc, char **argv)
{
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6 };
	struct pollfd pfd[MAX_CLIENTS + 2];
	int port = 8554, lfd, opt, i, one = 1, zero = 0;

	while ((opt = getopt(argc, argv, "Hp:")) != -1) {
		if (opt == 'H') {
			hevc = 1;
		} else if (opt == 'p') {
			port = atoi(optarg);
		} else {
			fprintf(stderr, "usage: ... | %s [-H] [-p port]\n",
				argv[0]);
			return 1;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	srandom(time(NULL) ^ getpid());
	ssrc = random();
	session = random();
	ts_base = random();
	for (i = 0; i < MAX_CLIENTS; i++)
		cl[i].fd = -1;

	lfd = socket(AF_INET6, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
	sa.sin6_port = htons(port);
	sa.sin6_addr = in6addr_any;
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(lfd, 8) < 0) {
		perror("bind");
		return 1;
	}
	fprintf(stderr, "h264-rtsp: %s, rtsp://<board>:%d/\n",
		hevc ? "H.265" : "H.264", port);

	for (;;) {
		int n = 2;

		pfd[0] = (struct pollfd){ .fd = 0, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &cl[i];

			if (c->state == C_FREE)
				continue;
			pfd[n].fd = c->fd;
			pfd[n].events = POLLIN |
				(c->out_off < c->out_len ? POLLOUT : 0);
			pfd[n++].revents = 0;
		}
		if (poll(pfd, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return 1;
		}

		/* Clients first: pfd[] matches cl[] only before any change */
		n = 2;
		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &cl[i];
			short ev;

			if (c->state == C_FREE)
				continue;
			ev = pfd[n++].revents;
			if (ev & (POLLERR | POLLNVAL)) {
				client_close(c);
				continue;
			}
			if (ev & POLLOUT)
				client_write(c);
			if (c->state != C_FREE && ev & (POLLIN | POLLHUP))
				client_read(c);
		}

		if (pfd[1].revents & POLLIN) {
			int fd = accept(lfd, NULL, NULL);

			for (i = 0; fd >= 0 && i < MAX_CLIENTS; i++)
				if (cl[i].state == C_FREE)
					break;
			if (fd >= 0 && i == MAX_CLIENTS) {
				close(fd);
			} else if (fd >= 0) {
				fcntl(fd, F_SETFL, O_NONBLOCK);
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
					   sizeof(one));
				cl[i].fd = fd;
				cl[i].state = C_RTSP;
			}
		}

		if (pfd[0].revents & (POLLIN | POLLHUP) && !read_input()) {
			fprintf(stderr, "h264-rtsp: end of input\n");
			return 0;
		}
	}
}
