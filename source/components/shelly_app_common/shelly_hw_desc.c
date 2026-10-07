//
// Copyright 2026 AUTOMATOUS.IO
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


// shelly_hw_desc.c

#include "shelly_hw_desc.h"

#include <sdkconfig.h>

#ifndef CONFIG_SHELLY_HARDWARE_CODE
#error "CONFIG_SHELLY_HARDWARE_CODE must be declared in the variant's Kconfig.projbuild"
#endif

_Static_assert(sizeof(CONFIG_SHELLY_HARDWARE_CODE) <= SHELLY_HW_CODE_LEN, "hardware code too long");

// Kept by the linker even though nothing references it (CMakeLists.txt,
// "-u shelly_hw_desc"); the linker script places the section right after
// the app description.
const __attribute__((section(".rodata_custom_desc"))) shelly_hw_desc_t shelly_hw_desc = {
    .magic = SHELLY_HW_DESC_MAGIC,
    .hw_code = CONFIG_SHELLY_HARDWARE_CODE,
};
