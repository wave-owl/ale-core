/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ALE_CORE_INTERNAL_H
#define ALE_CORE_INTERNAL_H

#include "ale.h"

/* decode_ff_impl.h constants (KiwiSDR extensions/ALE_2G). */
#define ALE_NFREQ 33
#define ALE_NTIME 17
#define ALE_NTIME_POW2 32
#define ALE_FFT_SIZE 64
#define ALE_HALF_FFT_SIZE 32
#define ALE_NSYMBOLS 8
#define ALE_VOTE_BUFFER_LENGTH 48
#define ALE_BITS_PER_SYMBOL 3
#define ALE_VOTE_ARRAY_LENGTH (ALE_SYMBOLS_PER_WORD * ALE_BITS_PER_SYMBOL)
#define ALE_BAD_VOTE_THRESHOLD 25
#define ALE_SYNC_ERROR_THRESHOLD 1
#define ALE_HIGH_BER 26
#define ALE_N_CUR 256
#define ALE_N_MSG 256
#define ALE_N_CMDS 16

extern const unsigned int ale_golay_encode_table[4096];
extern const unsigned int ale_golay_error_table[4096];
extern const unsigned char ale_ascii_set[128];
extern const unsigned char ale_symbol_lookup[ALE_NTIME][ALE_NFREQ];

/* decode_ff.h ASCII_Set classes. */
#define ALE_ASCII_38 1
#define ALE_ASCII_64 3
#define ALE_ASCII_NL 4

/* bits.h: bf(f, s, e) = bits s..e of f. */
#define ALE_BF(f, s, e) (((f) >> (e)) & ((1U << ((s) - (e) + 1)) - 1))
#define ALE_B(f, n) (((f) >> (n)) & 1U)

/* Reference printing name of a 7-bit code in "Other" command lines (cdeco). */
const char *ale_char_name(int c);

#endif
