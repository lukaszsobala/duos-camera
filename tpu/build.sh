#!/bin/sh
# Cross-build the Cvitek TPU runtime (cviruntime) for the Milk-V Duo S, for
# the mainline TPU driver (Armbian patch 0064). Run on a PC with Docker.
#   [SDK=<duo-buildroot-sdk-v2>] build.sh [riscv64|arm64 ...]
# Output: cviruntime-<arch>.tar.gz next to this script, default both
# architectures. On the board, unpack it in /. Everything goes to
# /opt/cviruntime.
#
# The source is the branch mainline-tpu of leafpmpmp/duo-buildroot-sdk-v2:
# cviruntime with dma-buf heaps instead of ION. SDK=<path> takes it from a
# local checkout that has the commit below. Without SDK, the script clones
# only the five directories that it needs.
#
# The container uses Debian's cross compilers. The riscv64 baseline is
# rv64gc, like the SG2000's C906.
set -e
D=$(cd "$(dirname "$0")" && pwd)
REV=431122d308de94d8463820f19735bce28d67c9d0
REPO=https://github.com/leafpmpmp/duo-buildroot-sdk-v2
DIRS="flatbuffers cvibuilder cvikernel cnpy cviruntime"
ARCHS=${*:-riscv64 arm64}
W="$D/work"
mkdir -p "$W"

# Source tree at $REV, in work/src
if [ ! -f "$W/src/.rev" ] || [ "$(cat "$W/src/.rev")" != "$REV" ]; then
	rm -rf "$W/src"
	mkdir -p "$W/src"
	if [ -n "$SDK" ]; then
		git -C "$SDK" archive "$REV" $DIRS | tar -C "$W/src" -x
	else
		rm -rf "$W/clone"
		git clone -q --filter=blob:none --no-checkout --depth 1 \
			--branch mainline-tpu "$REPO" "$W/clone"
		git -C "$W/clone" fetch -q --depth 1 origin "$REV"
		git -C "$W/clone" sparse-checkout set $DIRS
		git -C "$W/clone" -c advice.detachedHead=false checkout -q "$REV"
		for d in $DIRS; do
			mv "$W/clone/$d" "$W/src/"
		done
		rm -rf "$W/clone"
	fi
	echo "$REV" > "$W/src/.rev"
fi

# work/apt keeps the downloaded packages for the next run
mkdir -p "$W/apt"
docker run --rm -v "$W:/w" -v "$W/apt:/var/cache/apt/archives" \
	-e REV="$REV" -e ARCHS="$ARCHS" -e UID_GID="$(id -u):$(id -g)" \
	--platform linux/amd64 debian:trixie sh -ec '
rm -f /etc/apt/apt.conf.d/docker-clean
for a in $ARCHS; do
	dpkg --add-architecture $a
done
apt-get update -q
pkgs="cmake ninja-build git ca-certificates g++"
for a in $ARCHS; do
	pkgs="$pkgs crossbuild-essential-$a zlib1g-dev:$a"
done
apt-get install -qy --no-install-recommends $pkgs

S=/w/src
B=/w/build
rm -rf $B
mkdir -p $B

# cviruntime takes its version from git describe
git config --global --add safe.directory "*"
git -C $S/cviruntime init -q
git -C $S/cviruntime -c user.name=build -c user.email=build@localhost \
	commit -q --allow-empty -m "$REV"
git -C $S/cviruntime tag -f "sdk-${REV%${REV#????????}}"

# Host flatc, and the cvimodel headers that it generates
cmake -S $S/flatbuffers -B $B/flatbuffers -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DFLATBUFFERS_BUILD_TESTS=OFF \
	-DCMAKE_CXX_FLAGS=-Wno-error -DCMAKE_INSTALL_PREFIX=$B/host
cmake --build $B/flatbuffers --target install
cmake -S $S/cvibuilder -B $B/cvibuilder -G Ninja \
	-DFLATBUFFERS_PATH=$B/host -DCMAKE_INSTALL_PREFIX=$B/host
cmake --build $B/cvibuilder --target install

for a in $ARCHS; do
	case $a in
	riscv64) tc=$S/cviruntime/scripts/toolchain-riscv64-linux-gnu.cmake ;;
	arm64) tc=$S/cviruntime/scripts/toolchain-aarch64-linux-gnu.cmake ;;
	esac
	P=/opt/cviruntime
	I=$B/dest-$a$P
	common="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$tc"

	cmake -S $S/cvikernel -B $B/cvikernel-$a $common -DCHIP=cv181x \
		-DCMAKE_INSTALL_PREFIX=$I
	cmake --build $B/cvikernel-$a --target install

	cmake -S $S/cnpy -B $B/cnpy-$a $common -DENABLE_STATIC=OFF \
		-DCMAKE_INSTALL_PREFIX=$I
	cmake --build $B/cnpy-$a --target install

	cmake -S $S/cviruntime -B $B/cviruntime-$a $common -DCHIP=cv181x \
		-DRUNTIME=SOC -DCVIKERNEL_PATH=$I -DCNPY_PATH=$I \
		-DFLATBUFFERS_PATH=$B/host -DCVIBUILDER_PATH=$B/host \
		-DENABLE_TEST=OFF -DCMAKE_INSTALL_PREFIX=$I
	cmake --build $B/cviruntime-$a --target install

	rm -f $I/bin/mat2npz $I/bin/npy2mat $I/bin/npz2mat
	tar -C $B/dest-$a -czf /w/cviruntime-$a.tar.gz opt
done
chown -R "$UID_GID" /w
'
for a in $ARCHS; do
	mv "$W/cviruntime-$a.tar.gz" "$D/"
	ls -l "$D/cviruntime-$a.tar.gz"
done
