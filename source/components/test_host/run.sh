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
