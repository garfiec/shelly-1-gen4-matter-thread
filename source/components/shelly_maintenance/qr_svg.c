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


// qr_svg.c

#include "qr_svg.h"

#include <stdint.h>
#include <stdio.h>

#include "qrcodegen/qrcodegen.h"

#define QR_MAX_VERSION 10
#define QUIET          4

bool qr_svg_write(const char *text, qr_svg_emit_fn emit, void *ctx)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    if (!qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                              qrcodegen_Mask_AUTO, true)) {
        return false;
    }
    int size = qrcodegen_getSize(qr);
    int full = size + 2 * QUIET;
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" shape-rendering=\"crispEdges\">"
                     "<rect width=\"%d\" height=\"%d\" fill=\"#fff\"/><path fill=\"#000\" d=\"",
                     full, full, full, full);
    emit(buf, (size_t)n, ctx);
    // Each row's runs of dark modules as rectangles.
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size;) {
            if (!qrcodegen_getModule(qr, x, y)) {
                x++;
                continue;
            }
            int run = 1;
            while (x + run < size && qrcodegen_getModule(qr, x + run, y)) {
                run++;
            }
            n = snprintf(buf, sizeof(buf), "M%d %dh%dv1h-%dz", x + QUIET, y + QUIET, run, run);
            emit(buf, (size_t)n, ctx);
            x += run;
        }
    }
    emit("\"/></svg>", 9, ctx);
    return true;
}
