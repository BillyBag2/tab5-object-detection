# Tab5 object detection

ESP-IDF firmware for the **M5Stack Tab5 / ESP32-P4**, using its built-in MIPI
camera and 720×1280 display. Runs entirely on the device; no Wi-Fi, server, or
SD card is needed.

- Live RGB565 camera preview, rotated to match the Tab5 camera mounting.
- COCO-trained **YOLO11 nano INT8, 320×320 input**, with all 80 classes.
- Up to 10 bounding boxes with class names and confidence percentages.
- Separate capture/preview and detection tasks. The sensor and preview target 5 fps;
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

Detection runs at priority 1 on CPU 1; LVGL and capture use CPU 0. Frozen-input
tests found changing raw network outputs with the previous idle-priority /
unpinned-GUI scheduling. The new scheduling passed 48 repeatability checks
across both 128 KiB and 256 KiB cache configurations, recorded in
[INFERENCE_DIAGNOSTICS.md](INFERENCE_DIAGNOSTICS.md). The watchdog stays enabled:
the detector yields after model loading and blocks between frame submissions.
Serial logs mark model loading and each completed inference with its object count.
`PROFILE` lines separately measure preview PPA, inference PPA, scheduling wait,
preprocessing, neural network execution, postprocessing and total elapsed time.
The first run is marked cold. Save serial output and run
`python tests/summarize_profile.py capture.log` for warm-run statistics.
See [PIPELINE.md](PIPELINE.md) for stage definitions and the updated diagram. Camera
capture and display tasks run on the other core; detection latency still
includes any preemption by system tasks.

The confidence score threshold is 0.25 and NMS threshold is 0.5 in `main/profiled_detector.hpp`.
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

The SC202CS now uses a custom **physical 5 fps sensor mode**. It retains the
1280 x 720 RAW8 geometry, PLL, MIPI bitrate and line timing, but extends vertical
frame length (VTS) from 1250 to 7500 lines. Driver format metadata is updated
and the VTS registers are read back before streaming. Every sensor frame is
then delivered: this does not rely on post-DMA frame skipping.
`Camera::kTargetFps` and the custom-mode implementation are in `main/camera.*`.
The low-fps capture timeout is 3 seconds. Serial cadence logs independently
measure groups of five frame intervals.

Two additional 320-square RGB565 still buffers cost 400 KiB total; each changes
only at an inference handoff, rather than being recopied every preview frame.

The original ILI9881C LCD retains its native **60 MHz pixel clock**, approximately
48 Hz full-frame scanout. Trials at 40 and 50 MHz both caused visible instability
and were reverted. LVGL still renders only dirty regions. The clock setting is
under `Tab5 application` in `idf.py -B build/tab5 menuconfig`; the saved default
is 60 MHz. Other Tab5 panel revisions keep their native timings. The application
uses a DPI configuration wrapper without editing managed components.

The saved cache configuration is **256 KiB L2 with 128-byte lines**. All five
application image buffers are aligned to the runtime cache requirement. PSRAM
remains cacheable; CSI, PPA and LCD drivers perform DMA synchronization. Cache
ownership and measured trial results are documented in
[PIPELINE.md](PIPELINE.md#cache-and-dma-ownership).
