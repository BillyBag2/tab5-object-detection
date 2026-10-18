"""Check the generated Tab5 ISP pipeline against its linked configuration.

Run after building: python tests/check_isp_profile.py [build/tab5]
"""
import json
from pathlib import Path
import re
import sys


def require(condition, message):
    if not condition:
        raise SystemExit(message)


root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build/tab5"
profile = json.loads((root / "config/sc202cs_tab5.json").read_text())
upstream = json.loads((root / "managed_components/espressif__esp_cam_sensor/sensors/"
                      "sc202cs/cfg/sc202cs_default.json").read_text())
del upstream["SC202CS"]["acc"]["ccm"]
require(profile == upstream, "Custom profile changed settings beyond the CCM workaround")

config = (build / "sdkconfig").read_text()
generated = (build / "esp-idf/espressif__esp_ipa/esp_video_ipa_config.c").read_text()
require('CONFIG_CAMERA_SC202CS_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE=y' in config,
        "Build does not select the custom SC202CS profile")
require('CONFIG_CAMERA_SC202CS_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH="config/sc202cs_tab5.json"' in config,
        "Build selects the wrong SC202CS profile path")
names = re.search(r's_ipa_SC202CS_names\[\]\s*=\s*\{(.*?)\};', generated, re.S)
require(names is not None, "Generated SC202CS pipeline is missing")
requested = set(re.findall(r'"esp_ipa_(\w+)"', names.group(1)))
require(requested == set(profile["SC202CS"]), "Generated pipeline does not match the custom profile")
for name in requested:
    require(f"CONFIG_ESP_IPA_{name.upper()}_ALGORITHM=y" in config,
            f"Pipeline requests esp_ipa_{name} but its algorithm is disabled")
require(re.search(r'\.ccm\s*=', generated) is None,
        "Generated pipeline still includes a CCM calibration table")
require('config/sc202cs_tab5.json' in generated,
        "IPA generator did not consume the project-owned sensor profile")
print("ISP profile OK: all requested algorithms enabled; CCM table omitted; other settings preserved")
