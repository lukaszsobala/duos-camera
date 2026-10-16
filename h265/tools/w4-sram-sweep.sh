#!/bin/sh
# Try the H.265 encoder with each value of the SRAM switch (0x0b030024) and
# with and without the line buffers in the SRAM (run on the board, as root).
# Needs cam2h264 and a raw capture, cap.raw, in the current directory.
#   w4-sram-sweep.sh
P=/sys/module/wave420l/parameters
echo 1 > $P/debug
for s in 2 0 1 3 -1; do
	for m in Y N; do
		echo "$s" > $P/sram_share
		echo "$m" > $P/sram
		echo "== sram_share $s, sram $m"
		./cam2h264 -i cap.raw -n 2 -x -o "sweep-$s-$m.h265" 2>&1 | tail -1
		dmesg | grep -i wave420l | grep -E 'hang|frame 0|failed' | tail -2
	done
done
echo 2 > $P/sram_share
echo Y > $P/sram
ls -l sweep-*.h265
