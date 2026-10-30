// SPDX-License-Identifier: GPL-3.0-or-later
//
// Build shim for the unmodified KiwiSDR extensions/ALE_2G decoder outside the KiwiSDR tree
// (bench tool only, force-included by ref/build_ref.sh). At the pinned commit the standalone
// test no longer compiles on its own: the string helpers moved to support/str.h and the
// ASCII name table used by cdeco() lives in the Kiwi runtime. The name table here equals
// ale_char_name() of the core: the character itself when printable, "ctl" otherwise.
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kiwi_snprintf_buf(buf, fmt, ...) snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__)
#define kiwi_snprintf_ptr(ptr, buflen, fmt, ...) snprintf(ptr, buflen, fmt, ##__VA_ARGS__)

struct kiwi_shim_ascii {
    char names[256][4];
    kiwi_shim_ascii()
    {
        for (int i = 0; i < 256; ++i) {
            int c = i & 0x7F;
            if (c < 0x20 || c == 0x7F) strcpy(names[i], "ctl");
            else { names[i][0] = (char)c; names[i][1] = '\0'; }
        }
    }
    const char* operator[](int i) const { return names[i & 0xFF]; }
};
static const kiwi_shim_ascii ASCII;
