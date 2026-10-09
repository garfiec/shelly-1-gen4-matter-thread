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


// qr_svg.h
//
// A QR code as an SVG image, for the setup code on the Wi-Fi mode page.
// Pure C (with the vendored qrcodegen), so the host tests cover it.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*qr_svg_emit_fn)(const char *text, size_t len, void *ctx);

// Encode text (up to version 10, error correction raised as far as the
// size allows) and write the SVG in pieces through emit: a white square
// with a 4-module quiet zone, the dark modules as one path. False if the
// text doesn't fit.
bool qr_svg_write(const char *text, qr_svg_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif
