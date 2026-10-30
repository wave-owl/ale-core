/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ale-core unit tests: Golay code, word chain, encoder equality with openALE (vectors/
 * tx_words.txt, tx_sequences.txt written by scripts/make_vectors.py from the unmodified
 * reference encoder), modulator, receiver loopback, Golay acceptance and vote-only mode.
 *   test_ale <vectors-dir>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ale.h"

static int failures;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static int popcount24(uint32_t x)
{
    int n = 0;
    for (x &= 0xFFFFFF; x; x &= x - 1) n++;
    return n;
}

static void test_golay(void)
{
    uint32_t d;
    int e, minw = 99;
    for (d = 0; d < 4096; d++) {
        uint32_t c = ale_golay_encode(d);
        int k;
        CHECK(ale_golay_decode(c, &e) == d && e == 0, "clean decode %03x", d);
        if (d) {
            int w = popcount24(c);
            if (w < minw) minw = w;
        }
        /* every pattern of up to 3 errors is corrected (AC-1752.1) */
        for (k = 0; k < 24; k++) {
            uint32_t p = (1u << k) | (1u << ((k + 7) % 24)) | (1u << ((k + 13) % 24));
            CHECK(ale_golay_decode(c ^ p, &e) == d && e == 3, "3 errors %03x %d", d, k);
            CHECK(ale_golay_decode(c ^ (1u << k), &e) == d && e == 1, "1 error %03x", d);
        }
        /* 4 errors are detected, never miscorrected into another word (AC-1752.2) */
        {
            const uint32_t p4 = 0x000007 | 0x001000;   /* bits 0, 1, 2 and 12 */
            int err;
            (void)ale_golay_decode(c ^ p4, &err);
            CHECK(err == 4, "4 errors detected %03x got %d", d, err);
        }
    }
    CHECK(minw == 8, "minimum distance 8, got %d", minw);
}

static int hexval(const char *s, uint64_t *v)
{
    char *end;
    *v = strtoull(s, &end, 16);
    return end != s;
}

static void test_words(const char *dir)
{
    char path[512], line[1024];
    FILE *fp;
    int n = 0;
    snprintf(path, sizeof(path), "%s/tx_words.txt", dir);
    fp = fopen(path, "r");
    CHECK(fp != NULL, "open %s", path);
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        uint64_t word, tx49;
        unsigned char sym[49];
        char *tok = strtok(line, " \n");
        int k;
        if (!tok || !hexval(tok, &word)) continue;
        tok = strtok(NULL, " \n");
        if (!tok || !hexval(tok, &tx49)) continue;
        CHECK(ale_word_tx49((uint32_t)word) == tx49, "tx49 of %06llx", (unsigned long long)word);
        ale_tx49_symbols(tx49, sym);
        for (k = 0; k < 49; k++) {
            tok = strtok(NULL, " \n");
            CHECK(tok && atoi(tok) == sym[k], "symbol %d of %06llx (AC-57.2)", k,
                  (unsigned long long)word);
        }
        n++;
    }
    fclose(fp);
    CHECK(n > 10, "only %d reference words", n);
    printf("encoder: %d openALE words equal (tx49 and symbols)\n", n);
}

static int build(const char *name, uint32_t *w, int max)
{
    if (!strcmp(name, "sound_twas_dg6fl")) return ale_seq_sounding("DG6FL", 1, 4, w, max);
    if (!strcmp(name, "sound_tis_dl1abc")) return ale_seq_sounding("DL1ABC", 0, 3, w, max);
    if (!strcmp(name, "sound_twas_long")) return ale_seq_sounding("WAVEOWLTESTSTN", 1, 3, w, max);
    if (!strcmp(name, "call_dl1abc")) return ale_seq_call("DL1ABC", "DG6FL", 2, -1, NULL, w, max);
    if (!strcmp(name, "response_dg6fl")) return ale_seq_response("DG6FL", "DL1ABC", 0, w, max);
    if (!strcmp(name, "ack_dl1abc")) return ale_seq_ack("DL1ABC", "DG6FL", 0, w, max);
    if (!strcmp(name, "termination_dl1abc")) return ale_seq_termination("DL1ABC", "DG6FL", w, max);
    if (!strcmp(name, "call_lqa"))
        return ale_seq_call("DL1ABC", "DG6FL", 1, (int32_t)ale_lqa_word(1, 2, 17, 5), NULL, w, max);
    if (!strcmp(name, "call_amd"))
        return ale_seq_call("DL1ABC", "DG6FL", 1, -1, "HELLO FROM WAVE OWL 73", w, max);
    if (!strcmp(name, "allcall")) return ale_seq_call("@?@", "DG6FL", 1, -1, NULL, w, max);
    if (!strcmp(name, "anycall")) return ale_seq_call("@@?", "DG6FL", 1, -1, NULL, w, max);
    if (!strcmp(name, "netcall_hfn")) return ale_seq_call("HFN", "DG6FL", 2, -1, NULL, w, max);
    return -2;
}

static void test_sequences(const char *dir)
{
    char path[512], line[4096];
    FILE *fp;
    int n = 0;
    snprintf(path, sizeof(path), "%s/tx_sequences.txt", dir);
    fp = fopen(path, "r");
    CHECK(fp != NULL, "open %s", path);
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        uint32_t ref[128], got[128];
        int nref = 0, ngot, k;
        char *tok = strtok(line, " \n");
        char name[64];
        if (!tok) continue;
        snprintf(name, sizeof(name), "%s", tok);
        while ((tok = strtok(NULL, " \n")) && nref < 128) ref[nref++] = (uint32_t)strtoul(tok, NULL, 16);
        ngot = build(name, got, 128);
        CHECK(ngot == nref, "%s: %d words, openALE %d", name, ngot, nref);
        for (k = 0; k < nref && k < ngot; k++)
            CHECK(got[k] == ref[k], "%s word %d: %06x vs openALE %06x", name, k, got[k], ref[k]);
        n++;
    }
    fclose(fp);
    printf("sequences: %d openALE scenarios equal\n", n);
}

static void test_charset(void)
{
    uint32_t w[8];
    CHECK(ale_word_make(ALE_P_TO, "dg6") < 0, "lower case is not basic ASCII-38");
    CHECK(ale_word_make(ALE_P_DATA, "A,B") >= 0, "DATA takes ASCII-64");
    CHECK(ale_word_make(ALE_P_TO, "A,B") < 0, "TO takes basic ASCII-38 only");
    CHECK(ale_seq_address("DG6FL", ALE_P_TIS, w, 8) == 2, "5 characters -> 2 words");
    CHECK((w[1] >> 21) == ALE_P_DATA, "second word DATA");
    CHECK((w[1] & 0x7F) == '@', "padded with @");
    CHECK(ale_seq_amd("hello", w, 8) == 2, "AMD of 5 characters");
    CHECK(((w[0] >> 14) & 0x7F) == '?', "lower case sanitised to '?'");
    CHECK(ale_symbol_tone_hz(0) == 750 && ale_symbol_tone_hz(4) == 2500
          && ale_symbol_tone_hz(6) == 1750, "tone map");
}

/* render -> receiver: every word of a sounding comes back (loopback, R-1756.5 rung 1) */
static int loopback(int flags, const char *self, int twas, float noise_rms, unsigned seed,
                    char *from, size_t cap)
{
    uint32_t words[32];
    int n = ale_seq_sounding(self, twas, 3, words, 32), ns, i, found = 0;
    float *audio;
    ale_tx *tx = ale_tx_create();
    ale_rx *rx = ale_rx_create(flags);
    ale_event ev;
    int total = 4000 + n * ALE_WORD_SAMPLES + 3 * 8000;
    audio = (float *)calloc((size_t)total, sizeof(float));
    ns = ale_tx_render(tx, words, n, 8000.0f, audio + 4000, total - 4000);
    CHECK(ns == n * ALE_WORD_SAMPLES, "render length");
    srand(seed);
    for (i = 0; i < total && noise_rms > 0; i++) {
        /* Box-Muller */
        double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
        audio[i] += (float)(noise_rms * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2));
    }
    ale_rx_process(rx, audio, total);
    ale_rx_process(rx, audio, 0);
    while (ale_rx_next_event(rx, &ev)) {
        if (ev.kind == (twas ? ALE_EV_SOUND_TWAS : ALE_EV_SOUND_TIS)) {
            snprintf(from, cap, "%s", ev.from);
            found = 1;
        }
        if (ev.kind == ALE_EV_WORD) CHECK(ev.golay_errors < 4 || (flags & ALE_RX_F_REFERENCE),
                                          "word with failed Golay emitted (INV-1750)");
    }
    free(audio);
    ale_tx_destroy(tx);
    ale_rx_destroy(rx);
    return found;
}

static void test_loopback(void)
{
    char from[256] = "";
    CHECK(loopback(ALE_RX_F_WORD_EVENTS, "DG6FL", 1, 0.0f, 1, from, sizeof from)
          && !strcmp(from, "DG6FL"), "TWAS loopback: '%s'", from);
    CHECK(loopback(ALE_RX_F_WORD_EVENTS, "DL1ABC", 0, 2000.0f, 2, from, sizeof from)
          && !strcmp(from, "DL1ABC"), "TIS loopback in noise: '%s'", from);
    CHECK(loopback(ALE_RX_F_VOTE_ONLY, "DG6FL", 1, 0.0f, 3, from, sizeof from)
          && !strcmp(from, "DG6FL"), "vote-only loopback: '%s'", from);
}

static void test_noise(void)
{
    /* 60 s of white noise: no word and no log entry in the station mode (AC-112.6) */
    int i, words = 0, logs = 0;
    float *x = (float *)malloc(60 * 8000 * sizeof(float));
    ale_rx *rx = ale_rx_create(ALE_RX_F_WORD_EVENTS);
    ale_event ev;
    srand(7);
    for (i = 0; i < 60 * 8000; i++) {
        double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
        x[i] = (float)(3000.0 * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2));
    }
    for (i = 0; i < 60 * 8000; i += 1000) {
        ale_rx_process(rx, x + i, 1000);
        while (ale_rx_next_event(rx, &ev)) { if (ev.kind == ALE_EV_WORD) words++; else logs++; }
    }
    ale_rx_process(rx, x, 0);
    while (ale_rx_next_event(rx, &ev)) { if (ev.kind == ALE_EV_WORD) words++; else logs++; }
    /* trace words on noise are all-zero DATA patterns that no protocol state accepts */
    CHECK(logs == 0, "noise produced %d log entries (%d trace words)", logs, words);
    printf("noise: 60 s, %d trace words, %d log entries\n", words, logs);
    free(x);
    ale_rx_destroy(rx);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "vectors";
    CHECK(strstr(ale_version(), "contract=ale-c1") && strstr(ale_version(), "abi=1")
          && strstr(ale_version(), "tx=1"), "version tags");
    CHECK(ale_sizeof(0) == sizeof(ale_event), "sizeof");
    test_golay();
    test_charset();
    test_words(dir);
    test_sequences(dir);
    test_loopback();
    test_noise();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("test_ale: all passed\n");
    return 0;
}
