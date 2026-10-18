# Tab5 object detection

ESP-IDF firmware for the **M5Stack Tab5 / ESP32-P4**, using its built-in MIPI
camera and 720×1280 display. Runs entirely on the device; no Wi-Fi, server, or
SD card is needed.

- Live RGB565 camera preview, rotated to match the Tab5 camera mounting.
- COCO-trained **YOLO11 nano INT8, 320×320 input**, with all 80 classes.
- Up to 10 bounding boxes with class names and confidence percentages.
- Separate capture/preview and detection tasks. Camera delivery and preview target 5 fps;
  inference takes the next available frame without accumulating a backlog.
- P4 PPA hardware scales and rotates frames while the capture task waits for
  DMA completion, giving CPU 0 time to run its idle task and service the watchdog.
- On-screen preview rate, detection time, object count, and startup/camera errors.
- Live view at the top, with an analysing still and the previous completed
  result below. Bounding boxes remain on both live and previous-result images.
- Five square indicators estimate the next sample time from the last inference
  duration; the guide starts after the first result.
- Text sizes: 28 pixels for status and detection labels, 32 pixels for the title.

Use ESP-IDF **6.0 or 6.1** with the ESP32-P4 tools installed. The component
manager downloads the pinned Tab5 BSP and detector, including pretrained weights,
on the first build. `dependencies.lock` records the resolved dependencies.

`cmake/optimization.cmake` applies `-O3` to all source-built application C/C++
targets, including ESP-IDF components and nested dependency libraries. ESP-DL's
existing component-specific math flags are retained. Six ESP-IDF libc memory /
string routines retain upstream source-specific `-O2` settings for the
GCC-419 / IDF-13089 compiler workaround. Global `-Ofast` / fast-math
and LTO are not enabled. Precompiled vendor libraries and assembly kernels keep
their supplied implementations. The separate bootloader uses `-Os` to fit its
small flash slot. Debug symbols and assertions remain enabled.

After configuration, check the effective flags with
`python tests/check_optimization.py build/tab5`. Faster execution is workload
dependent; `-O3` can increase code size and needs runtime validation on the Tab5.

From an ESP-IDF terminal:

```powershell
idf.py -B build/tab5 build
idf.py -B build/tab5 -p COM5 flash monitor
```

Replace `COM5` with the Tab5 USB serial port. Flash through the USB-C device port.
The project selects `esp32p4` automatically. `sdkconfig.default` (singular, as
requested) is explicitly loaded by `CMakeLists.txt`. It enables 16 MB flash,
PSRAM, pre-v3 ESP32-P4 compatibility, the SC202CS sensor driver, automatic ISP
control, LVGL 9, and only the smaller 320-input model. The app partition includes
the model weights; a normal `flash` writes everything needed.

For an existing checkout with an old generated configuration/build cache, first
back up `sdkconfig`, then remove that generated file and use a fresh build folder:

```powershell
Move-Item sdkconfig sdkconfig.backup
idf.py -B build/tab5 build
idf.py -B build/tab5 -p COM5 flash monitor
```

The driver captures its default 1280 x 720 sensor mode. One PPA operation crops
its centred 640 x 640 area at (320,40), halves and rotates it 90 degrees clockwise into
320 x 320 RGB565. **The screen preview now shows only the detection area**, at
320 pixels wide and 320 pixels high. When the detector is idle, PPA writes
straight into its private input buffer, which is also copied to the display.
While detection runs, PPA writes later live preview frames to the other buffer.
There is no second preview PPA pass or CPU inference snapshot/crop copy.
ESP-DL still converts colour and normalizes / quantizes the model input.

Bounding boxes use the same coordinates as the displayed crop.
Capture buffers, the LVGL canvas,
and the in-flight inference image have separate ownership. Overlays show the
most recent completed inference and can lag the live preview when objects move;
boxes remain visible until the next completed result replaces them. The status
shows the result number, object count, inference time, and source frame age so
slow detections are visible without implying that boxes track current motion.
A completed result with no objects clears the previous boxes.

Automatic exposure and white balance remain enabled. The project uses
`config/sc202cs_tab5.json`, which omits the bundled SC202CS CCM calibration
matrices: combined with white-balance gains they can exceed the P4 hardware's
coefficient range. The ACC algorithm stays enabled for its black-level and
saturation settings, and the sensor profile still registers all its algorithms.
Colours may be less accurate than a correctly calibrated sensor profile.
Existing generated `sdkconfig` files must also select this custom profile and
enable `CONFIG_ESP_IPA_ACC_ALGORITHM`; defaults do not override saved settings.
The task watchdog remains enabled.

Detection runs at idle priority on CPU 1 so FreeRTOS time slicing lets IDLE1
service the watchdog during model loading and inference, including the first
run. Serial logs mark model loading and each completed inference with its object count.
`PROFILE` lines separately measure preview PPA, inference PPA, scheduling wait,
preprocessing, neural network execution, postprocessing and total elapsed time.
The first run is marked cold. Save serial output and run
`python tests/summarize_profile.py capture.log` for warm-run statistics.
See [PIPELINE.md](PIPELINE.md) for stage definitions and the updated diagram. Camera
capture and display tasks retain higher priority; detection latency includes
time spent sharing the CPU.

The confidence score threshold is 0.25 and NMS threshold is 0.5 in `main/main.cpp`.
Lowering the confidence threshold admits weaker detections, increasing both
sensitivity and the chance of false positives.
Espressif's published ESP32-P4 benchmark for the 320-input model is roughly
322 ms inference, versus 1414 ms for the 640-input version. Actual speed with
camera and display active must be measured on the board; preview rate is a cap,
not a guaranteed frame rate.

References: [official Tab5 BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_tab5)
and [COCO model details and benchmarks](https://github.com/espressif/esp-dl/tree/master/models/coco_detect).

Validation: compiled and linked with ESP-IDF 6.1.0 for ESP32-P4; the firmware
passes the partition size check and was flashed and verified on the connected
Tab5 revision 1.0. The stills / 5 fps build completed six complete warm profile
samples without captured crashes or watchdog resets, including an inference
with one object. Current and historical timing results are in
[PIPELINE.md](PIPELINE.md). Visual overlay alignment and detection
accuracy still require testing with recognizable objects in the cropped preview.

The camera requests 5 fps delivery through V4L2 frame skipping. The SC202CS
physical sensor mode remains 30 fps. Two additional 320-square RGB565 still
buffers cost 400 KiB total; each changes only at an inference handoff, rather
than being recopied every preview frame.
