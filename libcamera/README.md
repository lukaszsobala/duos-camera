# libcamera for the J1 camera

libcamera's simple pipeline handler, with its software ISP, works with the
raw capture driver `cv1800-vi`. The software ISP debayers on the CPU and
runs auto exposure and auto white balance on the sensor controls. Standard
applications then work: `cam`, GStreamer `libcamerasrc`, and V4L2
applications through `libcamerify`.

The patch adds two things to libcamera:

- `cv1800-vi` in the simple pipeline handler's device list, with the
  software ISP.
- A camera sensor helper for the GC2083: black level 64 at 10 bits, and the
  analogue gain code is linear (64 = 1x).

## Build (on the PC)

Needs Docker.

    ./build.sh

The script cross-builds in a `debian:trixie` container, for a board with
Debian trixie. The output is `libcamera-riscv64.tar.gz`. It installs to
`/usr/local`, so it does not conflict with Debian packages.

## Install (on the board)

    apt install libyaml-0-2 libevent-2.1-7t64 libevent-pthreads-2.1-7t64 \
        libssl3t64 gstreamer1.0-tools gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good
    tar -C / -xzf libcamera-riscv64.tar.gz
    ldconfig

The libraries go to `/usr/local/lib`, which Debian's `ld.so.conf` already
lists. The GStreamer plugin is in `/usr/local/lib/gstreamer-1.0`: set
`GST_PLUGIN_PATH` to that directory to use it.

## Test

Do not run `j1-stream.sh` first: libcamera sets up the media pipeline
itself.

1. List the cameras. Expected: one camera, with `gc2083` in its name.

       cam -l

2. Show the stream formats and sizes:

       cam -c 1 --info

3. Capture 10 frames as PPM files, with the software ISP's auto exposure
   and white balance. Look at the last ones: the first frames are still
   converging.

       cam -c 1 --capture=10 --stream pixelformat=BGR888 --file=/dev/shm/f-#.ppm

4. Frame rate without file output:

       cam -c 1 --capture=100 --stream pixelformat=BGR888

The binaries need only rv64gc (checked with `readelf -A`).

## Results

None yet.
