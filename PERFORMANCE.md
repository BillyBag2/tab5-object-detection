# Inference performance audit — 2026-10-18

This audit checks the existing ESP-IDF 6.1 build, ESP-DL 3.3.13, coco_detect
0.4.1 and ESP32-P4 revision 1.0 configuration. It does not change or flash the
firmware. Runtime figures below come from the earlier captured logs, not a new
controlled benchmark.

Subsequent change on the same date: the sensor now has a verified physical
1 fps mode, replacing the 5 fps post-DMA delivery skipping audited below.
See [the current pipeline](PIPELINE.md#physical-1-fps-sensor-mode). The capture
bandwidth discussion below describes the previous mode and why it was changed.

LCD trials on the same date: 40 MHz and 50 MHz pixel clocks both initialized and
ran inference but failed the user's visual stability check. The original 60 MHz
rate was restored and the user confirmed normal display operation. Those lower
clock configurations are not accepted performance settings; see the
[LCD trial record](PIPELINE.md#lcd-scanout-clock-trials).

Subsequent cache trial: the current build uses 256 KiB L2 with 128-byte lines,
replacing the 128 KiB / 64-byte configuration audited below. CPU caching stays
enabled. All five application image buffers are now cache-line aligned; the
camera, PPA and display drivers already own DMA cache maintenance. See
[cache ownership and results](PIPELINE.md#cache-and-dma-ownership).

Correctness follow-up on the same date: frozen input exposed changing raw
network outputs under the original idle-priority scheduling in both cache
configurations, also at `-O2`. Suspending task switches removed the variation.
The current app pins LVGL to CPU 0 and runs inference at priority 1 on CPU 1.
See [the repeatability investigation](INFERENCE_DIAGNOSTICS.md) for evidence,
limits and validation. Scheduling rows below describe the earlier audited build.

## Expected latency

The installed COCO component and the [raw upstream README](https://raw.githubusercontent.com/espressif/esp-dl/master/models/coco_detect/README.md)
list the P4 320-square model at **321.5 ms**, with 4.8 ms preprocessing and
4.8 ms postprocessing. The GitHub rendered directory returned a different
table during this audit (550.1 ms model, 5.2 / 5.3 ms pre/post). Both put the
network in the hundreds of milliseconds. The sub-10-ms entries are pre/post
processing, not complete COCO inference. Neither table documents enough test
conditions to promise an exact latency on our live-camera application.

The previous six warm live-system samples averaged 4938.613 ms in the network
and 5042.923 ms from dequeue to result (`build/tab5/stills_runtime.log`). That
is about 15.4 times the installed component's network figure. The discrepancy
still warrants investigation even after correcting the benchmark reading.

## Confirmed build findings

| Item | Existing build | Assessment |
| --- | --- | --- |
| Model | P4-specific YOLO11n 320 INT8, from `models/p4` | Correct variant; no evidence of accidentally embedding the S3 or 640 model |
| C/C++ optimization | 1638 units have effective `-O3`; six libc units retain required `-O2` | The optimization checker passes. ESP-DL already supplies its own `-O3` / fast-math. More compiler optimization is not the first missing switch |
| P4 SIMD selection | `CONFIG_PIE_V2_BOOST=1`; generated Conv selection is `esp32p4 (pie_v2)` | Correct P4 dispatch, including aligned and unaligned SIMD kernels |
| Linked instructions | ELF routine `dl_esp32p4_s8_conv2d_11cn_bias` contains `esp.vld.128.ip` and `esp.vsmulas.s8.qacc.ld.incp` | SIMD instructions are in the actual firmware. This is not a per-layer runtime trace or a claim that every operation is vectorized |
| Silicon support | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` | Correct for the connected revision 1.0. PIE V2 names the instruction-extension generation, not silicon revision 2/3; switching to PIE V1 would be wrong |
| Inference runtime | `m_model->run()` defaults to single-core | CPU 1 only; no separate neural-network accelerator is being omitted. PPA / ISP / DMA2D do not run this network |
| Inference task | CPU 1, `tskIDLE_PRIORITY` | Chosen earlier to avoid watchdog starvation; poor for latency benchmarking. It competes with idle and higher-priority tasks |
| Idle scheduling | `configIDLE_SHOULD_YIELD=0`, time slicing enabled | Idle need not immediately give up its equal-priority slot. Our model timer includes lost CPU time |
| LVGL task | Priority 4, affinity -1 | Can preempt inference on CPU 1. Capture task is priority 5 on CPU 0 |
| CPU / PSRAM clocks | 360 MHz / 200 MHz | Check benchmark clocks before comparison; modest clock differences cannot alone explain a 15-fold gap |
| L2 cache | 128 KiB, 64-byte lines | Different from Espressif's example; plausible instruction/data cache bottleneck |
| Code execution | Flash, DIO 40 MHz; PSRAM XiP disabled | Slow cache-miss refill path compared with the reference example's PSRAM execution |
| Stored model | Embedded in flash rodata | This is the loading source, not necessarily the weights' runtime location |
| Weight copying | COCO loader passes `param_copy=true` when PSRAM is at least 9 MiB | On this Tab5, copying to RAM is requested. Moving the stored model to SD or another flash partition would not by itself accelerate the network |
| Tensor memory budget | COCO constructor supplies `max_internal_size=0` | Greedy planner uses PSRAM-backed activation storage. Internal SRAM placement is an optimization opportunity, not a proven explanation: the supplied model component also uses this default |

Source evidence is in `main/profiled_detector.hpp`, `main/main.cpp`,
`build/tab5/sdkconfig`, `build/tab5/compile_commands.json`, the ELF/map, and the
pinned dependencies' COCO constructor, generated Conv selection and memory
planner. Actual runtime weight/activation placement should be confirmed with
`get_raw_model()->profile_memory()` before changing the memory budget.

## Concrete differences from Espressif's example

The upstream [P4 example defaults](https://raw.githubusercontent.com/espressif/esp-dl/master/examples/yolo11_detect/sdkconfig.defaults.esp32p4)
enable PSRAM XiP, 256 KiB L2 cache, 128-byte cache lines and QIO flash mode.
Our build has XiP disabled, 128 KiB / 64-byte cache settings and DIO mode.
The installed IDF allows the cache and XiP settings with pre-v3 P4 support;
they do not require pretending this board is revision 3.0.

These are experiment candidates, not proof that switching all settings would
recover the benchmark. A larger cache consumes internal SRAM. Moving code to
PSRAM also adds PSRAM bus traffic; [Espressif explicitly recommends profiling
that tradeoff](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-guides/external-ram.html).
Keep the supported CPU / PSRAM clocks and revision selection while testing.
Do not copy newer-revision 400 MHz / 250 MHz settings into this revision-1 build.

The supplied [model memory guidance](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_load_test_profile_model.html)
also distinguishes parameter copying from flash/SD storage. Confirm the memory
profile, then consider a bounded internal activation arena using available
contiguous SRAM after display/camera initialization. Do not move all weights to
internal SRAM: the model is much larger than the available internal memory.

## Camera frame dropping and memory bandwidth

`main/camera.cpp` requests 5 fps using `VIDIOC_S_PARM`. In installed esp_video,
`csi_video_set_parm` configures a frame skip count, and
`csi_video_on_trans_finished` drops frames **after the full DMA transfer**.
This reduces application deliveries and PPA / UI work, but not sensor rate or
the full-frame ISP-to-PSRAM write traffic.

At 1280 x 720 RGB565 and 30 sensor fps, capture writes approximately
**55.3 MB/s** of image payload. Configured ILI9881C timing is a 60 MHz pixel
clock with totals 940 x 1324, approximately 48.2 screen refreshes per second.
Reading its 720 x 1280 RGB565 framebuffer is approximately **88.9 MB/s** of
pixel payload. These are calculations from configured geometry/timing, not
measured bus counters; buffering, bursts and arbitration can change behaviour.
Together they explain why a 5 fps preview does not mean an otherwise idle PSRAM
bus. Neural-network tensors, copied weights, PPA, CPU canvas copies and LVGL
add traffic and cache pressure.

To avoid writing the unwanted frames, the drop must happen before the capture
DMA write. The current driver's delivery-skip mechanism does not provide that.
Options needing separate validation are:

- A genuine slower SC202CS sensor timing mode / vertical blanking change,
  with matching driver and exposure-control metadata.
- A validated smaller sensor output/window mode, reducing pixels per frame.
- A supported upstream receiver discard mechanism before PSRAM, if one can be
  established for this silicon/driver; none was verified in this audit.

PPA cropping happens after capture and therefore cannot save capture writes.
ISP cropping is unavailable on P4 revision 1.0; its driver rejects revisions
below 3.0. Merely reducing the backlight does not stop LCD framebuffer scanout.

## Tests that isolate the cause

Use the same frozen input, model, clocks and warmed run count, and record model
wall time separately from CPU runtime. Change one factor at a time:

1. Run the supplied detector without starting camera/LCD/LVGL. This establishes
   whether the network/build itself is slow, independently of the live system.
2. Compare idle-priority inference with a normal-priority detector, and place
   LVGL on CPU 0. Preserve watchdog servicing through deliberate scheduling;
   simply removing the watchdog would hide a separate problem.
3. With identical display/task settings, stop camera/ISP capture during the
   frozen-input run. This tests capture bandwidth rather than app frame skipping.
4. Separately stop LCD scanout / GUI rendering for a run. Backlight-off alone is
   not this test.
5. Compare 128 KiB/64-byte and 256 KiB/128-byte cache configurations, then test
   XiP separately. Record free/largest internal blocks and PSRAM usage as well.
6. Profile slow layers and memory placement; only then test a bounded internal
   activation budget or dual-core inference under the live workload.

The strongest current suspects are scheduling losses and memory/cache
contention. SIMD and compiler optimization are enabled. The exact share of
each bottleneck has not yet been measured by these controlled tests.
