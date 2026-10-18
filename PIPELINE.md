# Camera, inference, and display pipelines

This describes the current application and `build/tab5/sdkconfig`. Resolutions
are **width x height**. Blue nodes run on dedicated hardware, orange nodes run
on the CPUs, green nodes hold data, and grey orphaned nodes are unused by this
application. Arrows show image or result flow, not physical wiring.

```mermaid
flowchart TD
    sensor["SC202CS camera sensor<br/>Hardware: sensor<br/>1280 x 720 RAW8 Bayer, 16:9<br/>Configured sensor rate: 30 fps"]
    csi["MIPI-CSI receiver<br/>Hardware: ESP32-P4 CSI, 1 data lane<br/>1280 x 720 RAW8"]
    isp["Image signal processing<br/>Hardware: P4 ISP; CPU IPA control task<br/>Demosaic, exposure / white balance control<br/>Output: 1280 x 720 RGB565"]
    capture["Capture buffers in PSRAM<br/>3 V4L2 MMAP buffers<br/>Each: 1280 x 720 RGB565<br/>Stride: 2560 bytes"]
    select["Capture / preview task<br/>CPU 0: acquire buffer and select preview frames<br/>V4L2 delivery: 5 fps using frame skipping"]
    ppa["Shared crop, scale and rotate<br/>Hardware: PPA SRM + DMA2D<br/>Centre crop (320,40), 640 x 640; scale 0.5<br/>Rotate 90 degrees clockwise; output 320 x 320 RGB565"]
    preview["Two aligned PPA output buffers in PSRAM<br/>Each: 320 x 320 RGB565; 204800 bytes<br/>Detector idle: write directly into inference buffer<br/>Detector busy: write into preview staging buffer"]

    sensor --> csi --> isp --> capture --> select --> ppa --> preview

    subgraph detection["Inference path"]
        snapshot["Private inference buffer: held until detector finishes<br/>320 x 320 RGB565; stride 640 bytes<br/>Same PPA output is displayed for this frame<br/>No second PPA pass or CPU inference snapshot copy"]
        preprocess["ESP-DL preprocessing<br/>CPU 1: RGB565 to RGB888, normalization / quantization<br/>320 x 320 to 320 x 320: no spatial resize or padding"]
        tensor["Model input tensor<br/>320 x 320 x 3 RGB, signed INT8<br/>All pixels contain image content"]
        model["YOLO11 nano, 320-input COCO model<br/>CPU 1: ESP-DL optimized CPU kernels, single-core mode<br/>80 classes; weights embedded in flash<br/>Model working memory in RAM / PSRAM"]
        post["ESP-DL postprocessing<br/>CPU 1: box decoding, score filtering, NMS<br/>Score threshold 0.25; NMS threshold 0.5<br/>Boxes already use preview coordinates, 0 to 319"]
        results["Latest results queue: 1 entry<br/>Up to 10 boxes, class IDs and confidence scores<br/>Also sequence number, source timestamp and inference time"]
        snapshot --> preprocess --> tensor --> model --> post --> results
    end

    preview -->|"Detector idle: notify with private buffer; otherwise skip inference"| snapshot

    subgraph display["Screen path"]
        canvas["CPU 0: memcpy under LVGL lock<br/>Persistent canvas buffer in PSRAM<br/>320 x 320 RGB565; centred without upscaling"]
        overlays["CPU 0: update LVGL objects<br/>Entire preview is the detection area<br/>Live and previous still both retain latest boxes<br/>Five-square timing guide uses last inference duration"]
        render["LVGL task: CPU software rendering<br/>Compose live view, two stills and text into 720 x 1280 UI<br/>2 draw buffers, each 720 x 50 RGB565, in internal RAM"]
        fb["Hardware: DMA2D copies rendered regions<br/>LCD framebuffer in PSRAM<br/>720 x 1280 RGB565; 1 framebuffer"]
        dsi["Hardware: LCD scanout + MIPI-DSI<br/>720 x 1280 RGB565"]
        panel["Tab5 LCD panel<br/>720 x 1280 portrait<br/>Detected board v1: ILI9881C"]
        canvas --> overlays --> render --> fb --> dsi --> panel
    end

    preview -->|"Every delivered frame: 5 fps"| canvas
    snapshot -->|"CPU 0: copy once per new inference, under LVGL lock"| analysing
    analysing["Analysing still canvas in PSRAM<br/>320 x 320 RGB565; fixed for this inference"]
    previous["Previous result canvas in PSRAM<br/>Copy analysing still on completion before replacing it<br/>320 x 320 RGB565; overlays from matching result"]
    analysing -->|"Copy on completion"| previous
    results -->|"Matching boxes and labels"| previous
    analysing --> render
    previous --> render
    results -->|"CPU 0 polls queue; keeps last completed result"| overlays

    subgraph unused["Relevant unused hardware / operations - deliberately orphaned"]
        jpeg["P4 hardware JPEG encoder / decoder<br/>Unused: pipeline keeps uncompressed RGB565"]
        h264["P4 hardware H.264 encoder<br/>Unused: no compressed video recording or streaming"]
        blend["PPA blend and fill operations<br/>Unused by this pipeline; PPA SRM is active above<br/>LVGL renders overlays on the CPU"]
        sd["SDMMC host and microSD slot<br/>Unused: no frame recording or model loading from SD"]
        isp_crop["ISP crop hardware<br/>Unavailable on this P4 revision 1.0<br/>Supported only on revision 3.0 and later"]
    end

    classDef hardware fill:#dbeafe,stroke:#2563eb,color:#111827;
    classDef cpu fill:#ffedd5,stroke:#ea580c,color:#111827;
    classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827;
    classDef idle fill:#f3f4f6,stroke:#9ca3af,color:#4b5563,stroke-dasharray:5 5;
    class sensor,csi,isp,ppa,fb,dsi,panel hardware;
    class select,snapshot,preprocess,model,post,canvas,overlays,render cpu;
    class capture,preview,tensor,results,analysing,previous memory;
    class jpeg,h264,blend,sd,isp_crop idle;
```

- **Frame rates differ:** 30 fps is the configured sensor mode. `VIDIOC_S_PARM`
  requests 5 fps delivery using driver frame skipping; this does not lower the
  physical sensor readout rate. Preview processes each delivered frame. If the
  driver rejects that request, an application 5 fps cap is used instead. Neither is a measured end-to-end rate. Inference
  runs as fast as it completes, without queuing a backlog of input images.
- **Results can lag the preview:** the screen keeps showing the live preview
  while CPU 1 detects objects in its private PPA output. The last completed boxes
  remain visible until replaced, and the UI reports their source frame's age.
- **Detection field of view:** capture remains 1280 x 720. A centred 720-square
  crop, halved to 360 and then cropped to 320, is geometrically equivalent to
  the implemented centred 640-square crop halved directly to 320. It discards
  40 source pixels from each edge of that 720-square view. The screen now shows only this
  cropped field of view; it has the same spatial coordinates as the model input.
- **Hardware crop and stride:** PPA reads a rectangular block inside the full
  camera buffer using `pic_w = stride / 2` and block offsets. There is no CPU
  crop copy. A single pass also applies the 270-degree counter-clockwise rotation.
  PPA writes directly into the inference buffer when it is free. During
  inference, subsequent preview frames use the other output buffer. Only the
  per-frame live canvas copy remains, protected by the LVGL lock. Two
  additional still buffers are copied only on inference submission / completion.
- **ESP-DL stride limitation:** the installed `img_t` has no independent stride;
  `row_step()` equals width times pixel size. The lower-level preprocessor does
  accept a crop rectangle within an image whose width describes its full row.
  The model tensor itself must be contiguous. Our direct packed PPA output
  avoids requiring a strided model input or modifying the dependency.
- **Preprocessing:** ESP-DL still converts colour and normalizes / quantizes
  into the INT8 tensor. Supplying RGB565 at 320 x 320 removes spatial resizing
  and padding, but does not remove this necessary conversion or change weights.
- **ISP colour correction:** the custom SC202CS profile omits calibrated CCM
  tables. ACC remains registered for other settings. The physical CCM hardware
  can still apply white-balance gains, so it is not listed as unused hardware.
- **DMA2D has two roles:** it transfers PPA image data and copies LVGL output
  into the LCD framebuffer. It does not execute the neural network or render
  the bounding-box labels. There is no JPEG/H.264 encode/decode stage here.

Implementation references: [camera capture](main/camera.cpp),
[PPA transform, task handoff, detection and UI](main/main.cpp),
[default configuration](sdkconfig.default), and
[ISP profile notes](config/README.md). The display-buffer and DMA2D details also
depend on the current Tab5 BSP and generated `build/tab5/sdkconfig`.

## Profiling the inference branch

Every completed detection emits a `PROFILE` serial line, with milliseconds for:

| Field | Measurement |
| --- | --- |
| `preview` | Zero: the separate full-view PPA pass has been removed |
| `ppa` | Shared preview/inference crop / half-scale / rotate, including DMA wait |
| `wait` | Detector task scheduling delay after notification is prepared |
| `pre` | ESP-DL colour conversion, normalization and quantization |
| `model` | Neural network execution, single-core mode |
| `post` | Box decoding, filtering, NMS and ESP-DL result extraction |
| `total` | Camera dequeue completion to mapped results ready, before logging |

These are wall times under the live camera/display workload, including task
preemption, not isolated CPU execution times. `total` also includes camera
buffer release, result mapping and small gaps between stages. It excludes sensor
exposure/readout, time already spent queued in V4L2, and display presentation.
The first inference is marked `cold`; model loading is outside these timings.
Serial logging happens after stage measurements. Compare several dozen warm
samples with the same scene and workload before claiming a speed improvement.

Save the serial output to a file, then run:

```powershell
python tests/summarize_profile.py capture.log
```

The script excludes cold samples and prints mean, median, p95 and maximum for
each stage. No performance numbers are assumed from the build alone.

### Historical measurement before shared-preview change, 2026-10-16

The previous two-pass version was flashed and verified on the connected Tab5, ESP32-P4 revision 1.0, with the live
camera and screen active. Four warm samples after the first cold run gave:

| Stage | Mean elapsed time |
| --- | ---: |
| Preview PPA | 41.108 ms |
| Inference PPA crop / half-scale / rotation | 19.057 ms |
| Scheduling wait | 1.702 ms |
| Input conversion / normalization / quantization | 64.040 ms |
| Neural network | 8327.410 ms |
| Postprocessing | 21.239 ms |
| Total, dequeue to mapped results | 8474.697 ms |

The network accounts for about 98% of branch elapsed time. The direct crop
removes the CPU snapshot copy and spatial preprocessing, but these measurements
do not demonstrate an overall speedup: there is no controlled before/after run
of the same scene. This small sample establishes where to investigate next:
model execution, task scheduling and memory/cache behaviour. It is not a
representative accuracy or performance benchmark. All four samples returned
zero objects, so bounding-box alignment and detection accuracy still require
testing with recognizable objects in the cropped preview. No PPA errors, watchdog
resets or crashes appeared during the captured interval.

Raw output: `build/tab5/inference_profile.log` (local build artifact).

## ISP crop and recommended path for this board

The connected ESP32-P4 is revision 1.0. ISP hardware cropping is supported only
from revision 3.0; the installed ISP driver explicitly returns NOT_SUPPORTED on
older silicon. The esp_video V4L2 crop interface is also compiled out for this
build. See [Espressif ISP crop documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/isp.html).

For the present board and sensor mode, use CSI / ISP at 1280 x 720 RGB565,
then one PPA pass to crop 640-square, half-scale and rotate to 320-square.
Share that output between display and inference as described above. This
removes the separate 360 x 640 preview pass and reduces display copying to
204800 bytes per preview frame. The model still needs RGB conversion and INT8
normalization/quantization on the CPU. A suitable sensor crop/window mode could
reduce capture bandwidth further, but would require separate SC202CS mode and
ISP calibration validation; it is not configured here. Pipeline simplification
does not resolve the previously measured multi-second neural-network runtime.

### Shared-preview runtime check, 2026-10-16

The single-pass build was flashed and its contents verified on the same Tab5.
Three complete warm profile samples averaged 17.949 ms for the shared PPA pass,
45.337 ms for preprocessing, 6019.960 ms for the model and 12.710 ms for
postprocessing; dequeue-to-result time averaged 6097.739 ms. `preview=0` confirms
there is no additional full-view PPA pass. This short run completed without
captured PPA errors or watchdog resets. All samples returned zero objects, so
visual accuracy and box placement remain unverified. The lower elapsed times
than the earlier two-pass run are observations under the running workload,
not a controlled same-scene performance benchmark.

Raw output: `build/tab5/shared_preview_profile.log` (local build artifact).

## Live view, stills and sampling guide

The live 320-square view starts at (200,118). Below it, five 32-pixel squares
form a simple timing guide. The analysing and previous-result 320-square stills
are side by side at (24,638) and (376,638). All dimensions are screen pixels.

The analysing still changes only when a new image is submitted to inference.
When inference completes, that still is copied to the previous-result canvas
before the next sample replaces it, so the previous still and boxes stay paired.
The same completed boxes remain on the live view until the next result arrives.
A result with no detections clears boxes in both views.

No square is highlighted during the first inference. Afterwards, exactly one
square is highlighted, advancing at 0%, 25%, 50%, 75% and 100% of the previous
measured inference duration. A 100 ms LVGL timer updates the guide independently
of the 5 fps preview. The last square stays highlighted if the current run takes
longer; the next submitted sample resets it. It is an estimate, not measured
model-layer progress, and does not delay inference or capture.

The stills / 5 fps build was compiled, flashed and verified on the Tab5. Six
complete warm profiles in `build/tab5/stills_runtime.log` averaged 5042.923 ms
dequeue-to-result, with no captured watchdog resets or crashes. One completed
inference returned an object, exercising the overlay update path. These are
short runtime checks, not visual validation of the panel or an accuracy test.
