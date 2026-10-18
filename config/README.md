# Tab5 ISP profile

`sc202cs_tab5.json` derives from the SC202CS default profile shipped with
Espressif's `esp_cam_sensor` 2.0.1 component:
`sensors/sc202cs/cfg/sc202cs_default.json`.

The only semantic change is removal of `SC202CS.acc.ccm`. The original matrices
can exceed the ESP32-P4 CCM coefficient range when multiplied by automatic
white-balance gains. All other profile values are preserved, including ACC's
black-level and saturation parameters, AE, AWB, denoising, and enhancement.

Keep the ACC algorithm enabled: the profile requires it even without a CCM
table. The IPA generator emits a null CCM pointer when the table is absent.
The profile is licensed under Apache-2.0; see [LICENSE](LICENSE).

After building, run `python tests/check_isp_profile.py build/tab5` from the
project root. This checks that the generated pipeline uses this profile, every
requested algorithm is enabled, and the CCM table is absent while other profile
settings match the pinned upstream version.
