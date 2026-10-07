#!/bin/sh
#
# Copyright 2026 AUTOMATOUS.IO
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Build and run the host tests for the shared components' pure logic.
# Needs only a host C compiler: cc (or $CC).

set -e
cd "$(dirname "$0")"
C=..
OUT="${TMPDIR:-/tmp}/shelly-test-logic"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_bl0942/include -I$C/shelly_metering/include -I$C/shelly_protection/include \
    -I$C/shelly_app_common/include -I$C/shelly_thermal/include \
    test_logic.c \
    $C/shelly_bl0942/bl0942_codec.c \
    $C/shelly_metering/metering_logic.c \
    $C/shelly_protection/protection_logic.c \
    $C/shelly_thermal/thermal_logic.c \
    -lm -o "$OUT"

"$OUT"

# The device UI: gestures, decisions and LED patterns. Each test row is a
# row of docs/BUTTON-AND-LED.md; check-ui-spec.py keeps the two identical.
OUT_UI="${TMPDIR:-/tmp}/shelly-test-ui"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_button/include -I$C/shelly_status_led/include -I$C/shelly_app_common/include \
    test_ui.c \
    $C/shelly_button/button_logic.c \
    $C/shelly_app_common/ui_policy.c \
    -o "$OUT_UI"

"$OUT_UI"

# Wi-Fi maintenance mode: the upload check and the captive-portal DNS.
OUT_MAINT="${TMPDIR:-/tmp}/shelly-test-maint"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_maintenance/include -I$C/shelly_app_common/include \
    test_maint.c \
    $C/shelly_maintenance/maintenance_image.c \
    $C/shelly_maintenance/maintenance_dns.c \
    -o "$OUT_MAINT"

"$OUT_MAINT"

python3 ../../../scripts/check-ui-spec.py
