#!/usr/bin/env python3
# Print the inputs, outputs and routines of a cvimodel (run on the PC).
#   cvimodel-info.py model.cvimodel ...
# Needs only Python 3. The cvimodel body is a FlatBuffer; the schema is
# cvibuilder/proto/cvimodel.fbs in the vendor SDK. This reader knows only
# the fields that it prints.
import struct
import sys

DTYPES = ["FP32", "INT32", "UINT32", "BF16", "INT16", "UINT16", "INT8",
          "UINT8"]
QUANT = ["NONE", "BF16", "INT8_SYM", "INT8_ASYM"]
HEADER = 0x30  # magic 8, body size 4, major, minor, md5 16, chip 16, pad 2


class Table:
    def __init__(self, buf, pos):
        self.buf, self.pos = buf, pos
        vt = pos - struct.unpack_from("<i", buf, pos)[0]
        self.vlen = struct.unpack_from("<H", buf, vt)[0]
        self.vt = vt

    def _field(self, i):
        o = 4 + 2 * i
        if o >= self.vlen:
            return 0
        return struct.unpack_from("<H", self.buf, self.vt + o)[0]

    def scalar(self, i, fmt, default=0):
        o = self._field(i)
        if not o:
            return default
        return struct.unpack_from("<" + fmt, self.buf, self.pos + o)[0]

    def _ref(self, i):
        o = self._field(i)
        if not o:
            return None
        p = self.pos + o
        return p + struct.unpack_from("<I", self.buf, p)[0]

    def table(self, i):
        p = self._ref(i)
        return Table(self.buf, p) if p is not None else None

    def string(self, i):
        p = self._ref(i)
        if p is None:
            return None
        n = struct.unpack_from("<I", self.buf, p)[0]
        return self.buf[p + 4:p + 4 + n].decode(errors="replace")

    def _vec(self, i):
        p = self._ref(i)
        if p is None:
            return None, 0
        return p + 4, struct.unpack_from("<I", self.buf, p)[0]

    def scalars(self, i, fmt):
        p, n = self._vec(i)
        if p is None:
            return []
        size = struct.calcsize(fmt)
        return [struct.unpack_from("<" + fmt, self.buf, p + k * size)[0]
                for k in range(n)]

    def tables(self, i):
        p, n = self._vec(i)
        if p is None:
            return []
        return [Table(self.buf, p + 4 * k +
                      struct.unpack_from("<I", self.buf, p + 4 * k)[0])
                for k in range(n)]

    def strings(self, i):
        p, n = self._vec(i)
        if p is None:
            return []
        out = []
        for k in range(n):
            q = p + 4 * k
            q += struct.unpack_from("<I", self.buf, q)[0]
            m = struct.unpack_from("<I", self.buf, q)[0]
            out.append(self.buf[q + 4:q + 4 + m].decode(errors="replace"))
        return out


def tensor_line(t):
    name = t.string(1)
    dtype = DTYPES[t.scalar(3, "B")]
    shape = t.table(4).scalars(0, "q")
    s = "%-40s %-5s %s" % (name, dtype, "x".join(map(str, shape)))
    q = t.table(6)
    if q:
        s += "  quant %s max %g qscale %g zp %g" % (
            QUANT[q.scalar(0, "B")], q.scalar(1, "f"), q.scalar(4, "f"),
            q.scalar(3, "f"))
    fmt = t.string(10)
    if fmt:
        s += "  pixel_format %s" % fmt
    scale, mean = t.scalars(8, "f"), t.scalars(9, "f")
    if scale or mean:
        s += "  scale %s mean %s" % (
            ",".join("%g" % v for v in scale), ",".join("%g" % v for v in mean))
    if t.scalar(11, "B"):
        s += "  aligned"
    return s


def show(path):
    data = open(path, "rb").read()
    if data[:8] != b"CviModel":
        print("%s: not a cvimodel" % path)
        return
    body, major, minor = struct.unpack_from("<IBB", data, 8)
    chip = data[0x1e:0x2e].split(b"\0")[0].decode()
    buf = data[HEADER:HEADER + body]
    m = Table(buf, struct.unpack_from("<I", buf, 0)[0])
    v = m._field(0)
    ver = struct.unpack_from("<BBB", buf, m.pos + v) if v else (0, 0, 0)
    print("%s: header %d.%d, chip %s, model %d.%d.%d, name %s, built %s, "
          "mlir %s" % (path, major, minor, chip, ver[0], ver[1], ver[2],
                       m.string(1), m.string(2), m.string(9)))
    for n, p in enumerate(m.tables(6)):
        tensors = {t.string(1): t for t in p.tables(4)}
        print("program %d: batch %d, neuron %d bytes, shared %d, private %d"
              % (n, p.scalar(0, "I"), p.scalar(1, "I"), p.scalar(6, "I"),
                 p.scalar(7, "I")))
        for kind, i in (("in ", 2), ("out", 3)):
            for name in p.strings(i):
                print("  %s %s" % (kind, tensor_line(tensors[name])))
        for r in p.tables(5):
            print("  routine %s: %d in, %d out" % (
                "TPU" if r.scalar(0, "B") == 0 else "CPU",
                len(r.strings(1)), len(r.strings(2))))


for f in sys.argv[1:]:
    show(f)
