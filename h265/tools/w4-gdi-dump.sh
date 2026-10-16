#!/bin/sh
# Dump the WAVE420L memory interface (GDI) registers, 0x8800-0x891c, through
# the FIO window of the H.265 encoder (run on the board, as root). Run it after
# an H.265 hang, before the next stream. Needs devmem (tools/devmem.c) next to
# this script.
#   w4-gdi-dump.sh > gdi.txt
D=$(dirname "$0")
CTRL=0x0b020020
DATA=0x0b020024

fio() {
	"$D/devmem" $CTRL "$1" > /dev/null
	n=0
	while :; do
		c=$("$D/devmem" $CTRL | awk '{print $3}')
		[ $((c & 0x80000000)) -ne 0 ] && break
		n=$((n + 1))
		if [ $n -gt 100 ]; then
			echo "$1: no answer"
			return
		fi
	done
	echo "$1: $("$D/devmem" $DATA | awk '{print $3}')"
}

a=$((0x8800))
while [ $a -le $((0x891c)) ]; do
	fio "$(printf '0x%04x' $a)"
	a=$((a + 4))
done
