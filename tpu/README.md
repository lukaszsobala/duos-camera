# TPU object detection

Goal: detect objects in the J1 camera picture with the SG2000 TPU, and draw
the boxes into the H.264 or MJPEG stream.

## Parts

- **Driver.** Armbian patch 0064 (`drivers/soc/sophgo/tpu`) ports the vendor
  TPU driver to mainline. ION is gone: buffers come from the CMA dma-buf heap
  (128 MiB, patch 0066). Module `cv181x_tpu`, device `/dev/cvi-tpu0`. In
  Armbian main since `8fb8b36a` (2026-09-19): edge and bleedingedge, riscv64
  and arm64.
- **Runtime.** The vendor `cviruntime`, with the allocator port to dma-buf
  heaps: branch `mainline-tpu` of
  [leafpmpmp/duo-buildroot-sdk-v2](https://github.com/leafpmpmp/duo-buildroot-sdk-v2/tree/mainline-tpu).
  [build.sh](build.sh) cross-builds it.
- **Models.** The vendor TDL models,
  [sophgo/tdl_models](https://github.com/sophgo/tdl_models), directory
  `cv181x/`. The repository has no license file: download the models on the
  board, do not copy them into this repository.
- **Programs.**
  - `tpu-det.c`: the detector. It loads a YOLOv8 TDL model, fills its input
    from a frame, runs the TPU, and decodes the boxes.
  - `det-image.c`: runs a model on one picture. Use it to check a model.
  - `../tools/cam2h264.c`, option `-d`: detection on the camera frames,
    with boxes and labels in the encoded picture.
- `cvimodel-info.py` (PC): prints the inputs and outputs of a model.

## How it works

The vendor TDL SDK needs the vendor media stack (VPSS, VB pools), which
mainline does not have. The programs here use the runtime alone, like the
runtime's own samples (`cviruntime/samples/samples_extra`).

- `cam2h264` converts each camera frame to 960x540 NV12. When the TPU is
  free, a second thread takes a copy of the frame. Each frame gets the
  latest boxes before it goes to the encoder.
- In ISP mode (`ISP=1`, or the `j2-*.sh` scripts), the frames are 1920x1080
  NV12 from the ISP, and the boxes go into the camera buffer, which the
  encoder reads by DMA. `VIDIOC_PREPARE_BUF` writes the boxes out of the
  CPU cache before the encoder reads the frame. The camera buffers get no
  cache hints in this mode.
- The detector scales the frame into the model input, keeps the aspect
  ratio, and fills the rest with black. 960x540 into 640x384 gives 640x360
  and 12 black rows at the top and bottom.
- The TDL models take INT8 RGB planar: value / 255 x 127. TDL does this in
  the VPSS hardware; here the CPU does it, with a lookup table.
- Six INT8 outputs: per stride 8, 16 and 32, a box map (4 x 16 bins) and a
  class map. The decoder follows TDL's `yolov8.cpp`: sigmoid on the class
  logits, box sides from a 16-bin softmax, NMS per class.

## Models

Any `yolov8n_det_*` model from `cv181x/` works. Two to start with:

| Model                                  | Input   | Classes                      | Size   |
|----------------------------------------|---------|------------------------------|--------|
| `yolov8n_det_coco80_640_640`           | 640x640 | 80 (COCO)                    | 3.5 MB |
| `yolov8n_det_hand_face_person_384_640` | 384x640 | 3: `-L hand,face,person`     | 1.3 MB |

A 384x640 input wastes less of the 16:9 camera picture than 640x640. For
models other than COCO, give the class names with `-L`. The class lists
are in the `README.md` of tdl_models.

Download on the board (the commit is pinned):

    M=https://github.com/sophgo/tdl_models/raw/62b3dff417c01dfe2a21872c26691a4c9a009da5/cv181x
    wget $M/yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel
    wget $M/yolov8n_det_hand_face_person_384_640_INT8_cv181x.cvimodel

To see the tensors of a model on the PC:

    python3 cvimodel-info.py yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel

## Build the runtime (on the PC)

Needs Docker.

    SDK=<duo-buildroot-sdk-v2> ./build.sh

`SDK` is optional: a checkout with the `mainline-tpu` commit saves the
download. Without it, the script clones only the five directories it
needs. The output is `cviruntime-riscv64.tar.gz` and
`cviruntime-arm64.tar.gz`. `./build.sh riscv64` builds one architecture.

## Install (on the board)

    tar -C / -xzf cviruntime-riscv64.tar.gz

Everything goes to `/opt/cviruntime`. The programs find the libraries
through their RPATH: no `ldconfig`.

Then build `cam2h264` with detection, and install it with the scripts,
from a clone of this repository:

    make -C tools clean && make -C tools TPU=1 install

`det-image` for step 2, in `tpu/`:

    R=/opt/cviruntime
    gcc -O3 -mcpu=thead-c906 -o det-image det-image.c tpu-det.c \
        -I$R/include -L$R/lib -Wl,-rpath,$R/lib -lcviruntime -lpthread -lm

On arm64, leave out `-mcpu=thead-c906`.

## Steps

1. **Driver.**

       dmesg | grep -i tpu
       ls -l /dev/cvi-tpu0 /dev/dma_heap/

   Expected: `/dev/cvi-tpu0`, and a CMA heap next to `system` (normally
   `linux,cma`).

2. **One picture.** On the PC, convert the vendor test picture
   `<duo-buildroot-sdk-v2>/cviruntime/samples/samples_extra/data/dog.jpg`:

       python3 -c 'from PIL import Image; Image.open("dog.jpg").convert("RGB").save("dog.ppm")'

   Copy `dog.ppm` to the board, then:

       ./det-image -n 10 yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel dog.ppm out.ppm

   Expected: the tensors of the model, then `dog`, `bicycle` and `truck`
   (or `car`) with scores, and the times per run. Copy `out.ppm` to the
   PC: the boxes must be on the objects.

3. **A raw capture, without the camera.** With a `cap.raw` from
   `j1-stream.sh`:

       ./cam2h264 -i cap.raw -n 1 -j -d yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel -o det.jpg

   Expected: a list of objects, and `det.jpg` with the boxes. With `-i`,
   each frame waits for its own boxes.

4. **Live, over RTSP.**

       ISP=1 j1-rtsp.sh -d yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel   # 1920x1080, the ISP
       j1-rtsp.sh -d yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel         # 960x540, CPU debayer
       j2-rtsp.sh -d yolov8n_det_coco80_640_640_INT8_cv181x.cvimodel         # camera on J2

   On the PC: `ffplay -rtsp_transport tcp -fflags nobuffer
   rtsp://<board>:8554/`. Expected: the picture with boxes that follow the
   objects, a little late. Every 30 frames, `cam2h264` prints the objects.
   Stop with Ctrl-C: the summary has the frame rate and the detector
   times.

5. **The 384x640 model.**

       ISP=1 j1-rtsp.sh -d yolov8n_det_hand_face_person_384_640_INT8_cv181x.cvimodel -L hand,face,person

   Compare the frame rate and the detector times with step 4.

`-t 0.3` shows less certain objects too. MJPEG over HTTP works the same:
`ISP=1 j1-http.sh -d <model>`.

## If a step fails

- `CVI_NN_RegisterModel failed`: the runtime writes its errors to the
  system log, not to the terminal. Look at `journalctl -n 50`.
  `cvimodel(1.x) is not supported` means a model newer than the runtime.
  `dma-heap alloc fail` means the CMA pool is full: check
  `grep -i cma /proc/meminfo`.
- `no usable dma-buf heap`: the kernel has no CMA heap
  (`CONFIG_DMABUF_HEAPS_CMA`). `TPU_DMA_HEAP=<name>` picks a heap by name.
- `need one INT8 1x3xHxW input` or `unexpected ... channels`: the model is
  not a TDL YOLOv8 detector. Check it with `cvimodel-info.py`.
- Boxes in the wrong place, but the right objects: the letterbox mapping.
  Report the `det-image` output and `out.ppm`.

## Not known yet

- The TPU time per model on the SG2000 with the mainline driver.
- The cost in frame rate. The conversion alone takes 24.6 ms of each 33.7
  ms frame. The detector thread needs the CPU to fill the input and to
  decode the output, not only to wait for the TPU.
- The camera, the codecs and the TPU share the 128 MiB CMA pool.

## What to record

Step 1: the output. Step 2: the output and `out.ppm`. Steps 3-5: the
object lines, the summary lines of `cam2h264` (frame rate, input, convert,
encode, detector), and a screenshot or a short recording.
