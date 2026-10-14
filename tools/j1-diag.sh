#!/bin/sh
# While the GC2083 streams on J1: sample the receiver's lane state many times
# and dump the sensor registers. Run j1-stream.sh once first, so that the
# pipeline formats are set. Needs v4l-utils and i2c-tools; run from tools/.
#   j1-diag.sh [samples]
N=${1:-200}
CSI=/sys/kernel/debug/a0c2000.csi/status
VIDEO=$(media-ctl -d ${MEDIA:-/dev/media0} -e cv1800-vi-raw)

echo "=== module parameters"
grep -H . /sys/module/gc2083/parameters/* /sys/module/cv1800_csi2/parameters/*
echo 'module cv1800_csi2 +p' > /sys/kernel/debug/dynamic_debug/control

timeout 15 v4l2-ctl -d "$VIDEO" --stream-mmap=4 --stream-count=300 \
	--stream-to=/dev/null >/dev/null 2>&1 &
sleep 2

echo "=== lane LP p/n, $N samples (count, value)"
i=0
while [ $i -lt $N ]; do
	sed -n 's/^lane LP p\/n *\([0-9a-f]*\/[0-9a-f]*\).*/\1/p' $CSI
	i=$((i + 1))
done | sort | uniq -c

echo "=== csi status"
cat $CSI
echo "=== sensor registers"
./sensor-dump.sh
wait

echo "=== dmesg"
dmesg | grep -iE 'cv1800|gc2083' | tail -15
