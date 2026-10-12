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


# Included by every product line's CMakeLists.txt after project(). With
# -DSHELLY_WERROR=1 (CI sets it, scripts/ci-build.sh) any warning in this
# repository's own components and app is an error. ESP-IDF, esp-matter, CHIP
# and managed components keep their warnings as warnings.
#
# IDF builds with -Werror=all but turns some warnings back into plain warnings
# (-Wno-error=unused-function and others); naming them again here undoes that
# for our code. Target options come after IDF's global ones, so they win.

if(SHELLY_WERROR)
    get_filename_component(shelly_source_dir ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
    idf_build_get_property(shelly_components BUILD_COMPONENTS)
    foreach(component ${shelly_components})
        idf_component_get_property(component_dir ${component} COMPONENT_DIR)
        idf_component_get_property(component_lib ${component} COMPONENT_LIB)
        get_filename_component(component_dir ${component_dir} ABSOLUTE)
        string(FIND "${component_dir}/" "${shelly_source_dir}/" at)
        if(NOT at EQUAL 0 OR component_dir MATCHES "/managed_components/")
            continue()
        endif()
        get_target_property(component_type ${component_lib} TYPE)
        if(component_type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        target_compile_options(${component_lib} PRIVATE
            -Werror -Werror=extra
            -Werror=unused-function -Werror=unused-variable -Werror=unused-but-set-variable
            -Werror=deprecated-declarations -Werror=uninitialized -Werror=maybe-uninitialized
            -Werror=array-bounds)
        message(STATUS "Warnings are errors in ${component}")
    endforeach()
endif()
