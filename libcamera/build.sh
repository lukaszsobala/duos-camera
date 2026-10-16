#!/bin/sh
# Cross-build libcamera for the Milk-V Duo S (Debian trixie, riscv64), with
# the cv1800-vi and GC2083 patch. Run on a PC with Docker.
#   build.sh [libcamera commit]
# Output: libcamera-riscv64.tar.gz next to this script. On the board, unpack
# it in / and run ldconfig. Everything goes to /usr/local.
#
# The container uses Debian's riscv64 cross compiler: its baseline is
# rv64gc, like the SG2000's C906. (The Ubuntu 26.04 cross compiler targets
# RVA23, and its binaries stop with "Illegal instruction" on the board.)
set -e
D=$(cd "$(dirname "$0")" && pwd)
REV=${1:-a92285e9c8f8d5aba24c6f72f49efc73074002e1}
W="$D/work"
mkdir -p "$W"

docker run --rm -v "$W:/w" -v "$D:/p:ro" -e REV="$REV" \
	-e UID_GID="$(id -u):$(id -g)" debian:trixie sh -ec '
dpkg --add-architecture riscv64
apt-get update -q
apt-get install -qy --no-install-recommends \
	crossbuild-essential-riscv64 dpkg-dev meson ninja-build pkg-config git \
	ca-certificates openssl python3-yaml python3-ply python3-jinja2 \
	libyaml-dev:riscv64 libevent-dev:riscv64 libssl-dev:riscv64 \
	libgstreamer1.0-dev:riscv64 libgstreamer-plugins-base1.0-dev:riscv64

cd /w
[ -d libcamera ] || git clone https://git.libcamera.org/libcamera/libcamera.git
cd libcamera
git -c advice.detachedHead=false checkout -f "$REV"
git -c user.name=build -c user.email=build@localhost am /p/0001-*.patch

meson env2mfile --cross --debarch riscv64 -o /w/cross.txt
rm -rf /w/build /w/dest
meson setup /w/build --cross-file /w/cross.txt --prefix=/usr/local \
	--buildtype=release -Dpipelines=simple -Dipas=softisp \
	-Dcam=enabled -Dcam-output-kms=disabled -Dcam-output-sdl2=disabled \
	-Dcam-jpeg=disabled -Dgstreamer=enabled -Dv4l2=true \
	-Ddocumentation=disabled -Dlc-compliance=disabled -Dqcam=disabled \
	-Dpycamera=disabled -Dtest=false -Dtracing=disabled -Dudev=disabled \
	-Dsoftisp-gpu=disabled -Dlibdw=disabled -Dlibunwind=disabled \
	-Dandroid=disabled
ninja -C /w/build
DESTDIR=/w/dest ninja -C /w/build install
tar -C /w/dest -czf /w/libcamera-riscv64.tar.gz .
chown -R "$UID_GID" /w
'
mv "$W/libcamera-riscv64.tar.gz" "$D/"
ls -l "$D/libcamera-riscv64.tar.gz"
