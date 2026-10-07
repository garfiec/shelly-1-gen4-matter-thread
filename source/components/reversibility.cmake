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

# Included by every variant's CMakeLists.txt after project(). Fails the build
# if the firmware could stop a device from going back to stock: an eFuse-
# burning option, a non-stock partition table, or a linked eFuse write.
# See docs/REVERSIBILITY.md and scripts/check-reversibility.py.

# The same list as FORBIDDEN_CONFIG in scripts/check-reversibility.py, which
# checks it again after the build: keep the two in step.
foreach(opt CONFIG_SECURE_BOOT CONFIG_SECURE_BOOT_V2_ENABLED CONFIG_SECURE_FLASH_ENC_ENABLED
            CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK CONFIG_SECURE_DISABLE_ROM_DL_MODE
            CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE CONFIG_EFUSE_VIRTUAL)
    if(${opt})
        message(FATAL_ERROR "${opt} is enabled. It burns eFuses or blocks a UART restore, "
                            "so the device could not go back to stock. See docs/REVERSIBILITY.md.")
    endif()
endforeach()

idf_build_get_property(shelly_python PYTHON)
idf_build_get_property(shelly_sdkconfig SDKCONFIG)
idf_build_get_property(shelly_build_dir BUILD_DIR)
add_custom_target(reversibility_check ALL
    COMMAND ${shelly_python} ${CMAKE_CURRENT_LIST_DIR}/../../scripts/check-reversibility.py ${CMAKE_SOURCE_DIR}
            --sdkconfig ${shelly_sdkconfig} --build-dir ${shelly_build_dir}
    COMMENT "Checking reversibility (eFuses, secure boot, partition layout)"
    VERBATIM)
add_dependencies(reversibility_check app bootloader)
# `idf.py flash` and the partial flash targets build their own targets, not
# ALL: every target that writes the app, the bootloader or the partition
# table flashes only what passed.
foreach(shelly_flash_target flash app-flash bootloader-flash partition-table-flash
                            encrypted-flash encrypted-app-flash encrypted-bootloader-flash
                            encrypted-partition-table-flash)
    if(TARGET ${shelly_flash_target})
        add_dependencies(${shelly_flash_target} reversibility_check)
    endif()
endforeach()
