#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Transcribe the tables of KiwiSDR extensions/ALE_2G/decode_ff.h into src/ale_tables.c.

Usage: extract_tables.py <KiwiSDR checkout> <out: src/ale_tables.c>
The output is checked in; re-running against the pinned reference must reproduce it.
"""
import re
import sys


def main(kiwi: str, out_path: str) -> None:
    s = open(f"{kiwi}/extensions/ALE_2G/decode_ff.h").read()

    def arr(name):
        i = s.index(name)
        body = s[s.index('{', i) + 1:s.index('};', i)]
        return re.sub(r'//[^\n]*', '', body)

    def nums(body):
        return [int(x, 0) for x in re.findall(r'0x[0-9A-Fa-f]+|\d+', body)]

    enc = nums(arr('const unsigned int encode_table[4096]'))
    err = nums(arr('const unsigned int error[4096]'))
    asc = nums(arr('const char ASCII_Set[128]'))
    rows = re.findall(r'\{([^}]*)\}', arr('const char g_symbol_lookup[NTIME][NFREQ]').replace('_', '8'))
    sym = [[int(x) for x in r.split(',') if x.strip()] for r in rows]
    assert len(enc) == 4096 and len(err) == 4096 and len(asc) == 128 and len(sym) == 17

    def fmt(vals, per=12, f='0x%03X'):
        return '\n'.join('    ' + ', '.join(f % v for v in vals[i:i + per]) + ','
                         for i in range(0, len(vals), per))

    out = ('/* SPDX-License-Identifier: GPL-3.0-or-later\n *\n'
           ' * ale-core tables, transcribed verbatim (by scripts/extract_tables.py) from KiwiSDR\n'
           ' * extensions/ALE_2G/decode_ff.h (Copyright 2015 Milen Rangelov, GPL-3.0-or-later; portions\n'
           ' * from LinuxALE, Copyright 2000-2001 Charles Brain and Ilkka Toivanen, GNU GPL).\n'
           ' * Do not edit by hand.\n */\n#include "ale_internal.h"\n\n')
    out += ('/* Golay(24,12) parity of every 12-bit data word (decode_ff.h encode_table). */\n'
            'const unsigned int ale_golay_encode_table[4096] = {\n' + fmt(enc) + '\n};\n\n')
    out += ('/* Golay(24,12) syndrome -> error pattern (bits 11..0) and error count (bits 15..12; 4 =\n'
            ' * detected, uncorrectable) (decode_ff.h error). */\n'
            'const unsigned int ale_golay_error_table[4096] = {\n' + fmt(err, 12, '0x%04X') + '\n};\n\n')
    out += ('/* Character set class of every 7-bit code: 1 basic ASCII-38, 3 ASCII-64, 4 line\n'
            ' * control (NL, CR), 0 other (decode_ff.h ASCII_Set). */\n'
            'const unsigned char ale_ascii_set[128] = {\n' + fmt(asc, 16, '%d') + '\n};\n\n')
    out += ('/* FFT bin -> tribit for each of the NTIME frequency hypotheses; 8 = no symbol\n'
            ' * (decode_ff.h g_symbol_lookup, rows of 32 with the 33rd element zero-initialised). */\n'
            'const unsigned char ale_symbol_lookup[ALE_NTIME][ALE_NFREQ] = {\n')
    out += ''.join('    {' + ','.join(str(v) for v in r) + '},\n' for r in sym) + '};\n'
    open(out_path, 'w').write(out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
