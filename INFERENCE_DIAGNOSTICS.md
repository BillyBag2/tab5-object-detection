# Inference repeatability investigation, 2026-10-18

Subsequent sensor-rate trial: the app now targets physical capture/preview at
5 fps. Another 24 checks passed at that rate, and the deterministic raw outputs
matched the corrected 1 fps build. See [the 5 fps trial](PIPELINE.md#physical-5-fps-sensor-trial)
for current live timing; the 1 fps measurements below remain historical records.

The reported improvement in person detection after increasing cache prompted a
correctness investigation. A faster cache should not change the network's
predictions for identical input. The tests reproduced changing **raw network
outputs**, independently of the live scene and display labels.

## What was tested

`TAB5_INFERENCE_AUDIT=ON` enables a one-time startup diagnostic. It holds the
inference semaphore while the camera, ISP, PPA and LVGL continue working on
other buffers. It runs six repeats of each of these fixtures:

- A CPU-generated deterministic 320 x 320 RGB565 pattern, independent of DMA.
- One frozen PPA camera image, protected from capture reuse throughout testing.
- The same pattern with task switching suspended only around `Model::run()`.
- The same camera image with that diagnostic suspension.

The suspension leaves interrupts and the other core running. It is a diagnostic
comparison, not the normal application scheduling policy. Each iteration checks:

- Image CRC before/after inference and preprocessed INT8 input-tensor CRC.
- All six raw output tensors immediately after inference, after postprocessing,
  and after a 100 ms delay with background image/display activity.
- CRC of final classes, scores and boxes, serialized without struct padding.
- Full-line guards around all five application images and the temporary pattern.
- Heap metadata integrity. Tensor pointers are checked for 16-byte SIMD alignment.

CRC covers logical tensor elements, excluding allocator tail padding. A second
test build also snapshots and directly compares every output byte, ruling out a
checksum-only measurement issue. An independent host implementation confirms the
pattern image CRC is `d84f4f2c`; its preprocessed tensor CRC is `89fefc03`.

## Findings

| Configuration | Normal switching | Switching suspended around network |
| --- | --- | --- |
| 256 KiB / 128-byte lines, ESP-DL `-O3`, original scheduling | Both fixtures had 6 different raw-output CRCs in 6 repeats | Not included in initial build |
| 256 KiB / 128-byte lines, ESP-DL `-O2`, original scheduling | Both fixtures had 6 different raw-output CRCs in 6 repeats | Both fixtures repeatable in all 6 runs |
| 128 KiB / 64-byte lines, ESP-DL `-O3`, original scheduling | Both fixtures had 6 different raw-output CRCs in 6 repeats | Both fixtures repeatable in all 6 runs; pattern matches the 256 KiB test |
| 128 KiB / 64-byte lines, ESP-DL `-O3`, GUI CPU 0 / detector priority 1 CPU 1 | Both fixtures repeatable in all 6 runs | Identical to normal-switching outputs; all 24 checks passed |
| 256 KiB / 128-byte lines, ESP-DL `-O3`, GUI CPU 0 / detector priority 1 CPU 1 | Both fixtures repeatable in all 6 runs | Identical to normal-switching outputs; all 24 checks passed |

Image and preprocessed input CRCs were constant within each fixture. The raw
outputs remained unchanged after postprocessing and during the subsequent
100 ms wait. Guards and heap checks passed. Direct comparison found tens of
thousands of changed bytes in individual heads between network executions.
This is not limited to the first cold run.

The deterministic pattern's raw-output CRC with switching suspended is
`33c860ab`, matching across the tested cache sizes and compiler levels. This
supports a preemption-sensitive execution/state problem, rather than an
intrinsic change in quantized arithmetic from larger cache lines.

The retained scheduling passed **48 checks across both cache configurations**.
The pattern's image, quantized input and raw-output checksums match between
those builds. Frozen fixtures in these runs produced empty final detection
lists, so their result-list CRC check is trivial; the full raw box and class
score tensors were still compared. This is not a positive person-detection test.

## Causes and limits of the evidence

1. **Scheduling / coprocessor state during inference is the leading suspect.**
   The original detector ran at idle priority on CPU 1, while LVGL was priority
   4 and unpinned. Normal execution was preemptible and time-sliced with IDLE1.
   Preventing task switches removes the observed raw-output variation.
   This does not identify a particular register, kernel or RTOS defect.
2. **A missing camera/PPA DMA flush is not sufficient to explain this test.**
   The CPU-generated pattern also exhibits the failure, with identical
   preprocessed input. CSI, PPA and LCD synchronization was separately reviewed.
3. **Postprocessing or the GUI result queue is not the first point of failure.**
   Outputs already differ immediately after the network call. Postprocessing
   reads raw outputs; the application copies result records by value into its
   queue, rather than queuing pointers into mutable model output memory.
4. **`-O3` alone is not the cause.** The issue persists with verified final
   `-O2` flags on all 97 ESP-DL C/C++ units. Assembly kernels remain SIMD.
5. **Memory corruption is not completely excluded.** Image guards and heap
   metadata checks do not detect every invalid access within the model's large
   activation allocation. An uninitialized scratch buffer or context-sensitive
   kernel remains possible. Raw numerical instability is established; a
   physical overwrite of a particular output buffer is not established.
6. **Alignment was not isolated as the historical explanation.** The earlier
   change aligned CPU canvases, while camera/PPA inference buffers were already
   aligned. Both cache configurations fail repeatability under the original
   scheduling. The timing/layout change may have masked the underlying issue;
   it does not prove why person scores improved in the live scene.

Espressif has separately confirmed preemption-sensitive P4 hardware-loop
constraints in [IDF issue 19025](https://github.com/espressif/esp-idf/issues/19025).
That is relevant background, not proof that this application has the same
defect. No SDK assembly patch was applied on that basis. The
[official DMA synchronization guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/system/mm_sync.html)
describes why DMA buffers require explicit ownership and synchronization.

## Reproduction

The normal app now uses LVGL affinity 0 and detector priority 1 on CPU 1. These
two changes were tested together; the evidence does not establish which one
alone is sufficient. There is a 10 ms yield after model loading before the first
inference, plus normal blocking between submissions, to service IDLE1 while
retaining the 5-second task watchdog. No production scheduler suspension or
additional DMA flush was introduced.

The final normal build was flashed and hash-verified with 256 KiB / 128-byte
cache, `-O3` and both audit options OFF. A 40-second serial capture completed
35 warm profiles without captured errors, panics or watchdog resets. Sensor
cadence remained approximately 1 fps and LCD remained at 60 MHz.

| Final normal build stage | Mean | p95 |
| --- | ---: | ---: |
| PPA | 18.200 ms | 18.623 ms |
| Preprocessing | 13.913 ms | 16.922 ms |
| Network | 677.563 ms | 686.378 ms |
| Postprocessing | 4.962 ms | 8.238 ms |
| Dequeue to result | 719.517 ms | 730.095 ms |

Full detector time is about 696 ms per frame; live sampling is now one
inference per physical 1 fps camera frame. The earlier cache-performance
figures came from the scheduling configuration that failed repeatability, so
they are not an accuracy-equivalent benchmark against this build.

From an initialized ESP-IDF shell:

```powershell
idf.py -B build/tab5 -DTAB5_INFERENCE_AUDIT=ON -DTAB5_AUDIT_DL_O2=OFF build
idf.py -B build/tab5 -p COM3 flash monitor
```

Those commands test the current scheduling. To recreate the original failure,
temporarily use LVGL affinity -1 and detector priority `tskIDLE_PRIORITY` in
`main/main.cpp`. Restore affinity 0 / priority 1 afterward.

Return to the normal build after testing:

```powershell
idf.py -B build/tab5 -DTAB5_INFERENCE_AUDIT=OFF -DTAB5_AUDIT_DL_O2=OFF build
idf.py -B build/tab5 -p COM3 flash
```

Set `TAB5_AUDIT_DL_O2=ON` for the compiler comparison. Inspect final compiler
flags; CMake normally deduplicates a later plain `-O2` if an earlier IDF `-O2`
exists before the component's `-O3`. The diagnostic override uses `SHELL:-O2`
to retain the final override. Production builds keep `-O3`.

Validate captured logs with:

```powershell
python tests/check_inference_audit.py path/to/runtime.log
```

The checker intentionally fails the original-scheduling logs. A passed test
establishes repeatability for these inputs, not agreement with an independent
golden model or person-detection accuracy across a dataset. Camera fixtures
are frozen within each boot, not assumed identical across different boots.

Local artifacts include `build/tab5/integrity_256_runtime.log`,
`integrity_heads_runtime.log`, `integrity_o2_runtime.log` and
`integrity_128_runtime.log`. The passing scheduling trials are
`integrity_scheduling_128_runtime.log` and `integrity_scheduling_256_runtime.log`.
Final normal-build artifacts are `integrity_release_build.log`,
`integrity_release_flash.log` and `integrity_release_runtime.log` in that directory.
Failed attempts at selecting `-O2` were excluded;
the valid compiler trial was built using `integrity_o2_final_build.log` and
`integrity_o2_scheduler_build.log`.
