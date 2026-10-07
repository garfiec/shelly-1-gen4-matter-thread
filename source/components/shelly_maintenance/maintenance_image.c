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

// maintenance_image.c

#include "maintenance_image.h"

#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void copy_str(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t n = 0;
    while (n < src_len && n + 1 < dst_len && src[n] != '\0') {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

mi_result_t mi_check(const uint8_t *buf, size_t len, const char *expect_hw_code, mi_info_t *info)
{
    memset(info, 0, sizeof(*info));

    if (len < 4) {
        info->need_bytes = MI_OTA_FIXED_HEADER;
        return MI_NEED_MORE;
    }
    uint32_t magic = le32(buf);
    if (magic != MI_OTA_MAGIC) {
        // An app .bin and a merged UART .bin both start with the image magic.
        return buf[0] == MI_IMAGE_MAGIC ? MI_NOT_OTA_FILE : MI_BAD_HEADER;
    }
    if (len < MI_OTA_FIXED_HEADER) {
        info->need_bytes = MI_OTA_FIXED_HEADER;
        return MI_NEED_MORE;
    }
    info->total_size = le64(buf + 4);
    uint32_t tlv_size = le32(buf + 12);
    if (tlv_size == 0 || tlv_size > MI_OTA_MAX_TLV) {
        return MI_BAD_HEADER;
    }
    info->payload_offset = MI_OTA_FIXED_HEADER + tlv_size;
    uint64_t min_payload = MI_HW_DESC_OFFSET + sizeof(shelly_hw_desc_t);
    if (info->total_size < info->payload_offset + min_payload) {
        return MI_BAD_HEADER;
    }
    info->payload_size = info->total_size - info->payload_offset;

    size_t need = info->payload_offset + MI_HW_DESC_OFFSET + sizeof(shelly_hw_desc_t);
    if (len < need) {
        info->need_bytes = need;
        return MI_NEED_MORE;
    }

    const uint8_t *img = buf + info->payload_offset;
    if (img[0] != MI_IMAGE_MAGIC) {
        return MI_NOT_AN_APP;
    }
    uint16_t chip_id = (uint16_t)(img[12] | (img[13] << 8));
    if (chip_id != MI_CHIP_ID_ESP32C6) {
        return MI_WRONG_CHIP;
    }
    const uint8_t *desc = img + MI_APP_DESC_OFFSET;
    if (le32(desc) != MI_APP_DESC_MAGIC) {
        return MI_NOT_AN_APP;
    }
    copy_str(info->version, sizeof(info->version), desc + 16, 32);
    copy_str(info->project, sizeof(info->project), desc + 48, 32);

    const uint8_t *hw = img + MI_HW_DESC_OFFSET;
    if (le32(hw) != SHELLY_HW_DESC_MAGIC) {
        return MI_NO_HW_CODE;
    }
    copy_str(info->hw_code, sizeof(info->hw_code), hw + 4, MI_HW_CODE_LEN);
    if (expect_hw_code == NULL || strcmp(info->hw_code, expect_hw_code) != 0) {
        return MI_WRONG_HARDWARE;
    }
    return MI_OK;
}

const char *mi_result_text(mi_result_t r)
{
    switch (r) {
    case MI_OK:
        return "OK";
    case MI_NEED_MORE:
        return "The file is too short";
    case MI_NOT_OTA_FILE:
        return "This is a .bin file. Upload the .ota file for this device instead";
    case MI_BAD_HEADER:
        return "Not a Matter .ota firmware file, or the file is damaged";
    case MI_WRONG_CHIP:
        return "This firmware is for a different chip";
    case MI_NOT_AN_APP:
        return "The file does not contain a firmware image";
    case MI_NO_HW_CODE:
        return "This firmware predates Wi-Fi updates and carries no hardware code. Install it by Matter OTA or UART";
    case MI_WRONG_HARDWARE:
        return "This firmware is for a different Shelly model";
    default:
        return "Unknown error";
    }
}

static const char *parse_part(const char *s, unsigned long *out)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long)(*s - '0');
        s++;
    }
    *out = v;
    return *s == '.' ? s + 1 : s;
}

int mi_version_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        unsigned long va, vb;
        a = parse_part(a, &va);
        b = parse_part(b, &vb);
        if (va != vb) {
            return va < vb ? -1 : 1;
        }
    }
    return 0;
}
