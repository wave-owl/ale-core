/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ale-core transmitter side: word, address, sequence and AMD encoding and the 8-FSK
 * modulator. The structure follows openALE (MIT; pinned in ref/PINS):
 *   src/FEC/ale_fec_codec.cpp     encode_word (A = Golay(upper), B = Golay(lower) with the
 *                                 check bits inverted)
 *   src/FEC/word_interleaver.cpp  interleave (A bit 23-k -> tx bit 2k, B -> 2k+1)
 *   src/Codec/ale_encoder.cpp     encode_tx49 (symbol k carries tx bits 3k..3k+2 mod 49)
 *   src/Word/ale_word.cpp         make_word / encode_ascii (character sets)
 *   src/Word/address_encoder.cpp  chunk / encode (pad '@', DATA/REP extension words)
 *   src/Word/ale_sequence.cpp     scanning/leading call, conclusion, response, ack, termination
 *   src/Protocol/Message/ale_orderwire_protocols.cpp  encode_amd
 *   src/LQA/lqa_metrics.cpp       encode_lqa_cmd (Table A-XIV)
 *   src/FSK/tone_generator.cpp    phase-continuous tones (no dither here, float output)
 * The Golay parity table is the receiver reference's (ale_tables.c); openALE's encoder gives
 * the same code words (tests/test_ale.c and the vector JSON files).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ale_internal.h"

#define ALE_CORE_VERSION "0.1.0"

const char *ale_version(void)
{
    return "ale-core " ALE_CORE_VERSION " contract=ale-c1 abi=1 tx=1";
}

int ale_abi(void) { return ALE_ABI; }

size_t ale_sizeof(int which)
{
    return which == 0 ? sizeof(ale_event) : 0;
}

const char *ale_char_name(int c)
{
    /* reference cdeco() prints ASCII[c]; the build shim of the reference (ref/kiwi_shim.h)
     * uses this same table: the character itself when printable, "ctl" otherwise */
    static const char *const printable[95] = {
        " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
        "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\", "]", "^", "_",
        "`", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
        "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "{", "|", "}", "~",
    };
    c &= 0x7F;
    if (c < 0x20 || c == 0x7F) return "ctl";
    return printable[c - 0x20];
}

/* tone order (FREQ_TO_SYMBOL of openALE, g_symbol_lookup of the receiver) */
static const unsigned char freq_to_symbol[8] = {0, 1, 3, 2, 6, 7, 5, 4};

int ale_symbol_tone_hz(int symbol)
{
    int rank;
    for (rank = 0; rank < 8; rank++)
        if (freq_to_symbol[rank] == (symbol & 7)) return 750 + 250 * rank;
    return 0;
}

uint64_t ale_word_tx49(uint32_t word24)
{
    uint32_t a = ale_golay_encode((word24 >> 12) & 0xFFF);
    uint32_t b = ale_golay_encode(word24 & 0xFFF) ^ 0xFFF;
    uint64_t out = 0;
    int k;
    for (k = 0; k < 24; k++) {
        if ((a >> (23 - k)) & 1u) out |= (1ULL << (2 * k));
        if ((b >> (23 - k)) & 1u) out |= (1ULL << (2 * k + 1));
    }
    return out;
}

void ale_tx49_symbols(uint64_t tx49, unsigned char symbols[ALE_SYMBOLS_PER_WORD])
{
    int k, b;
    for (k = 0; k < ALE_SYMBOLS_PER_WORD; k++) {
        unsigned char sym = 0;
        for (b = 0; b < ALE_BITS_PER_SYMBOL; b++) {
            int bit_pos = (k * ALE_BITS_PER_SYMBOL + b) % ALE_SYMBOLS_PER_WORD;
            if ((tx49 >> bit_pos) & 1u) sym |= (unsigned char)(1u << (ALE_BITS_PER_SYMBOL - 1 - b));
        }
        symbols[k] = sym;
    }
}

/* openALE ASCII_SET: 1 = expanded 64, 2 = basic 38 */
static int char_class(int ch)
{
    if (ch < 0 || ch >= 128) return 0;
    if (ch == '@' || ch == '?' || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) return 2;
    if (ch >= 0x20 && ch <= 0x5F) return 1;
    return 0;
}

int32_t ale_word_make(int preamble, const char chars[3])
{
    int i, basic = preamble != ALE_P_DATA && preamble != ALE_P_REP;
    uint32_t payload = 0;
    preamble &= 7;
    for (i = 0; i < 3; i++) {
        int ch = (unsigned char)chars[i];
        if (preamble != ALE_P_CMD) {
            int cls = char_class(ch);
            if (cls == 0 || (basic && cls != 2)) return -1;
        }
        payload |= (uint32_t)(ch & 0x7F) << (14 - 7 * i);
    }
    return (int32_t)(((uint32_t)preamble << 21) | (payload & 0x1FFFFF));
}

/* ---- sequences ----------------------------------------------------------------------- */

static int put(uint32_t *words, int *n, int max, int32_t w)
{
    if (w < 0 || *n >= max) return -1;
    words[(*n)++] = (uint32_t)w;
    return 0;
}

int ale_seq_address(const char *addr, int first_preamble, uint32_t *words, int max)
{
    static const int ext[4] = {ALE_P_DATA, ALE_P_REP, ALE_P_DATA, ALE_P_REP};
    size_t len = addr ? strlen(addr) : 0, i;
    int n = 0, idx = 0;
    if (len > 15) len = 15;                    /* A.5.2.4.2: at most 15 characters */
    if (len == 0) {
        return put(words, &n, max, ale_word_make(first_preamble, "@@@")) ? -1 : n;
    }
    for (i = 0; i < len; i += 3, idx++) {
        char c[3] = {'@', '@', '@'};          /* A.5.2.4.3: pad with '@' */
        size_t j;
        for (j = 0; j < 3 && i + j < len; j++) c[j] = addr[i + j];
        if (put(words, &n, max, ale_word_make(idx == 0 ? first_preamble : ext[idx - 1], c)))
            return -1;
    }
    return n;
}

int ale_seq_amd(const char *text, uint32_t *words, int max)
{
    size_t len = text ? strlen(text) : 0, i;
    int n = 0;
    if (len > 90) len = 90;                    /* 30 words x 3 characters (A.5.7.2.3) */
    for (i = 0; i < len; i += 3) {
        char c[3] = {' ', ' ', ' '};
        size_t j;
        int32_t w;
        for (j = 0; j < 3 && i + j < len; j++) {
            unsigned char ch = (unsigned char)text[i + j];
            c[j] = (ch >= 0x20 && ch <= 0x5F) ? (char)ch : '?';
        }
        if (n == 0) {
            /* CMD AMD: CMD preamble, expanded-64 payload */
            w = ale_word_make(ALE_P_DATA, c);
            if (w >= 0) w = (int32_t)(((uint32_t)ALE_P_CMD << 21) | ((uint32_t)w & 0x1FFFFF));
        } else {
            w = ale_word_make((n % 2 == 1) ? ALE_P_DATA : ALE_P_REP, c);
        }
        if (put(words, &n, max, w)) return -1;
    }
    return n;
}

uint32_t ale_lqa_word(int ka1, int mp, int sinad, int ber)
{
    return (6u << 21) | (0x61u << 14) | ((ka1 ? 1u : 0u) << 13) | (((uint32_t)mp & 7u) << 10)
           | (((uint32_t)sinad & 0x1Fu) << 5) | ((uint32_t)ber & 0x1Fu);
}

static int append(uint32_t *words, int n, int max, const uint32_t *src, int k)
{
    if (k < 0 || n + k > max) return -1;
    memcpy(words + n, src, (size_t)k * sizeof(uint32_t));
    return n + k;
}

int ale_seq_sounding(const char *self, int twas, int reps, uint32_t *words, int max)
{
    uint32_t c[8];
    int k = ale_seq_address(self, twas ? ALE_P_TWAS : ALE_P_TIS, c, 8), n = 0, i;
    if (k < 0 || reps < 1) return -1;
    for (i = 0; i < reps; i++)
        if ((n = append(words, n, max, c, k)) < 0) return -1;
    return n;
}

static int addressed_then_conclusion(const char *addressed, const char *self, int conclusion,
                                     uint32_t *words, int max)
{
    uint32_t a[8], c[8];
    int ka = ale_seq_address(addressed, ALE_P_TO, a, 8);
    int kc = ale_seq_address(self, conclusion, c, 8);
    int n = 0;
    if ((n = append(words, n, max, a, ka)) < 0) return -1;
    if ((n = append(words, n, max, a, ka)) < 0) return -1;
    return append(words, n, max, c, kc);
}

int ale_seq_call(const char *dest, const char *self, int scan_channels, int32_t lqa,
                 const char *amd, uint32_t *words, int max)
{
    uint32_t a[8], t[64];
    int ka = ale_seq_address(dest, ALE_P_TO, a, 8), n = 0, i;
    if (ka < 0 || scan_channels < 0) return -1;
    for (i = 0; i < 2 * scan_channels; i++)              /* scanning call: first word only */
        if ((n = append(words, n, max, a, 1)) < 0) return -1;
    if ((n = append(words, n, max, a, ka)) < 0) return -1;  /* leading call: address twice */
    if ((n = append(words, n, max, a, ka)) < 0) return -1;
    if (amd && amd[0]) {
        int k = ale_seq_address(self, ALE_P_FROM, t, 64);
        if ((n = append(words, n, max, t, k)) < 0) return -1;
    }
    if (lqa >= 0) {
        uint32_t w = (uint32_t)lqa & 0xFFFFFF;
        if ((n = append(words, n, max, &w, 1)) < 0) return -1;
    }
    if (amd && amd[0]) {
        int k = ale_seq_amd(amd, t, 64);
        if ((n = append(words, n, max, t, k)) < 0) return -1;
    }
    {
        int k = ale_seq_address(self, ALE_P_TIS, t, 64);
        return append(words, n, max, t, k);
    }
}

int ale_seq_response(const char *caller, const char *self, int reject, uint32_t *words, int max)
{
    if (reject) return ale_seq_address(self, ALE_P_TWAS, words, max);
    return addressed_then_conclusion(caller, self, ALE_P_TIS, words, max);
}

int ale_seq_ack(const char *peer, const char *self, int no_link, uint32_t *words, int max)
{
    return addressed_then_conclusion(peer, self, no_link ? ALE_P_TWAS : ALE_P_TIS, words, max);
}

int ale_seq_termination(const char *peer, const char *self, uint32_t *words, int max)
{
    if (!peer || !peer[0]) return ale_seq_address(self, ALE_P_TWAS, words, max);
    return addressed_then_conclusion(peer, self, ALE_P_TWAS, words, max);
}

/* ---- modulator ----------------------------------------------------------------------- */

struct ale_tx {
    uint32_t phase;
    uint32_t inc[8];
};

void ale_tx_reset(ale_tx *tx)
{
    tx->phase = 0x40000000u;   /* openALE ToneGenerator::reset */
}

ale_tx *ale_tx_create(void)
{
    int rank;
    ale_tx *tx = (ale_tx *)calloc(1, sizeof(*tx));
    if (!tx) return NULL;
    for (rank = 0; rank < 8; rank++)
        tx->inc[freq_to_symbol[rank]] =
            (uint32_t)(((uint64_t)(750 + 250 * rank) << 32) / ALE_SAMPLE_RATE);
    ale_tx_reset(tx);
    return tx;
}

void ale_tx_destroy(ale_tx *tx) { free(tx); }

int ale_tx_render(ale_tx *tx, const uint32_t *words, int n_words, float amplitude, float *out,
                  int max_samples)
{
    int w, k, s, o = 0;
    unsigned char sym[ALE_SYMBOLS_PER_WORD];
    const double two_pi_over_2_32 = 2.0 * M_PI / 4294967296.0;
    if (n_words < 0 || (long)n_words * ALE_WORD_SAMPLES > (long)max_samples) return -1;
    for (w = 0; w < n_words; w++) {
        ale_tx49_symbols(ale_word_tx49(words[w] & 0xFFFFFF), sym);
        for (k = 0; k < ALE_SYMBOLS_PER_WORD; k++) {
            uint32_t inc = tx->inc[sym[k] & 7];
            for (s = 0; s < ALE_SYMBOL_SAMPLES; s++) {
                out[o++] = amplitude * (float)sin((double)tx->phase * two_pi_over_2_32);
                tx->phase += inc;
            }
        }
    }
    return o;
}
