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

# The Matter setup code: the manual code, its check digit and the record.
OUT_CODE="${TMPDIR:-/tmp}/shelly-test-setup-code"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_app_common/include \
    test_setup_code.c \
    $C/shelly_app_common/setup_code.c \
    -o "$OUT_CODE"

"$OUT_CODE"

# The diagnostics log ring (CONFIG_SHELLY_DIAG_LOG builds).
OUT_DIAG="${TMPDIR:-/tmp}/shelly-test-diag"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_app_common/include \
    test_diag.c \
    $C/shelly_app_common/diag_ring.c \
    -o "$OUT_DIAG"

"$OUT_DIAG"

# Board data: the hardware snapshot and the ADE7953 register table.
OUT_HW="${TMPDIR:-/tmp}/shelly-test-hw"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_hw_snapshot/include -I$C/shelly_ade7953/include \
    test_hw.c \
    $C/shelly_hw_snapshot/hw_snapshot.c \
    $C/shelly_ade7953/ade7953_regs.c \
    -o "$OUT_HW"

"$OUT_HW"

# 2PM metering and the calibration page: the ADE7953 codec, the shared
# calibration logic and the metering helpers.
OUT_CAL="${TMPDIR:-/tmp}/shelly-test-cal"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I$C/shelly_ade7953/include -I$C/shelly_app_common/include -I$C/shelly_metering/include \
    test_cal.c \
    $C/shelly_ade7953/ade7953_codec.c \
    $C/shelly_app_common/calibration_logic.c \
    $C/shelly_metering/metering_logic.c \
    -lm -o "$OUT_CAL"

"$OUT_CAL"

# The ADE7953 driver writes the chip in one place only, write_reg(), which
# refuses anything ade7953_write_allowed() doesn't (tested above): exactly
# one plain transmit, and no other kind of write.
WRITES=$(grep -cE 'i2c_master_transmit[[:space:]]*\(' $C/shelly_ade7953/ade7953.cpp || true)
if [ "$WRITES" != "1" ] || grep -nE 'i2c_master_multi_buffer_transmit' $C/shelly_ade7953/*.c* ; then
    echo "The ADE7953 driver may only write the chip through write_reg()" >&2
    exit 1
fi

python3 ../../../scripts/check-ui-spec.py
