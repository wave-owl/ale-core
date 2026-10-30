/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ale_dump: decode an 8000 Hz 16-bit mono WAV with ale-core and print the events in the
 * text format of the reference standalone decoder (ref/kiwi_driver.cpp), so the two outputs
 * can be compared with diff after removing ANSI colours.
 *
 *   ale_dump [-w] [-r] [-v] file.wav
 *     -w  word trace (the reference's gdebug & 2 lines)
 *     -r  reference behaviour in every detail (ALE_RX_F_REFERENCE)
 *     -v  vote-only bench mode (ALE_RX_F_VOTE_ONLY)
 *
 * Like the reference's run_standalone(), input is fed in blocks of 1024 samples, a final
 * partial block is dropped, and the end of input flushes the pending log entry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ale.h"
#include "../src/ale_internal.h"

static const char *const preamble_types[] = {"[DATA]", "[THRU]", "[TO]", "[TWAS]",
                                             "[FROM]", "[TIS]", "[CMD]", "[REP]"};

static int read_wav(const char *path, short **samples, long *count)
{
    FILE *fp = fopen(path, "rb");
    unsigned char hdr[12], ch[8];
    int ok = 0;
    if (!fp) return 0;
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        goto done;
    while (fread(ch, 1, 8, fp) == 8) {
        unsigned long len = ch[4] | (ch[5] << 8) | ((unsigned long)ch[6] << 16)
                            | ((unsigned long)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char f[16];
            if (len < 16 || fread(f, 1, 16, fp) != 16) goto done;
            if ((f[0] | (f[1] << 8)) != 1 || (f[2] | (f[3] << 8)) != 1
                || (f[4] | (f[5] << 8) | (f[6] << 16)) != ALE_SAMPLE_RATE
                || (f[14] | (f[15] << 8)) != 16) {
                fprintf(stderr, "%s: need 16-bit mono PCM at 8000 Hz\n", path);
                goto done;
            }
            if (len > 16) fseek(fp, (long)(len - 16), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            long i, n = (long)(len / 2);
            unsigned char *raw = (unsigned char *)malloc(len ? len : 1);
            *samples = (short *)malloc((size_t)(n ? n : 1) * sizeof(short));
            if (!raw || !*samples || fread(raw, 1, len, fp) != len) { free(raw); goto done; }
            for (i = 0; i < n; i++) (*samples)[i] = (short)(raw[2 * i] | (raw[2 * i + 1] << 8));
            free(raw);
            *count = n;
            ok = 1;
            goto done;
        } else {
            fseek(fp, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
done:
    fclose(fp);
    return ok;
}

static void print_event(const ale_event *ev)
{
    char ts[32];
    snprintf(ts, sizeof(ts), "[%02d:%02d:%02d] ", (ev->secs / 3600) % 24, (ev->secs / 60) % 60,
             ev->secs % 60);
    switch (ev->kind) {
    case ALE_EV_WORD: {
        unsigned a = ALE_BF(ev->word, 20, 14), b = ALE_BF(ev->word, 13, 7), c = ALE_BF(ev->word, 6, 0);
        printf("%5d: LSYM%d NTIME%2d sym_rv%d // ber[n]|%2d < bestber|%2d %s // bestpos=%2d|0|%2d %s"
               " // w=%06x %-6s %s(%02x) %s(%02x) %s(%02x) // WORD %s\n",
               ev->nsym, ev->symbol, ev->nt, ev->rv, ev->ber, ev->bestber,
               ev->ber < ev->bestber ? "Y" : "N", ev->bestpos, ev->nt,
               (ev->bestpos == 0 || ev->bestpos == ev->nt) ? "Y" : "N", ev->word,
               preamble_types[ev->preamble & 7], ale_char_name((int)a), a, ale_char_name((int)b), b,
               ale_char_name((int)c), c, ev->best ? "BEST" : "");
        break;
    }
    case ALE_EV_SOUND_TWAS:
        printf(">>>>  %s[Sounding THIS WAS] [From: %s] [His BER: %d]\n", ts, ev->from, ev->ber);
        break;
    case ALE_EV_SOUND_TIS:
        printf(">>>>  %s[Sounding THIS IS] [From: %s] [His BER: %d]\n", ts, ev->from, ev->ber);
        break;
    case ALE_EV_TO:
        printf(">>>>  %s[To: %s] [His BER: %d]\n", ts, ev->to, ev->ber);
        break;
    case ALE_EV_CALL:
        printf(">>>>  %s[%s] [From: %s] [To: %s] [His BER: %d]\n", ts,
               ev->stage == 1 ? "Call" : ev->stage == 2 ? "Call ACK" : "Call EST", ev->from,
               ev->to, ev->ber);
        break;
    case ALE_EV_CMD:
        printf(">>>>  %s[%s] [%s] [His BER: %d]\n", ts, ev->is_message ? "MSG" : "CMD", ev->text,
               ev->ber);
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    int flags = 0, ai;
    const char *path = NULL;
    short *pcm = NULL;
    long n = 0, off;
    float buf[1024];
    ale_rx *rx;
    ale_event ev;

    for (ai = 1; ai < argc; ai++) {
        if (!strcmp(argv[ai], "-w")) flags |= ALE_RX_F_WORD_EVENTS;
        else if (!strcmp(argv[ai], "-r")) flags |= ALE_RX_F_REFERENCE;
        else if (!strcmp(argv[ai], "-v")) flags |= ALE_RX_F_VOTE_ONLY;
        else path = argv[ai];
    }
    if (!path || !read_wav(path, &pcm, &n)) {
        fprintf(stderr, "usage: ale_dump [-w] [-r] [-v] file.wav (8000 Hz, 16-bit, mono)\n");
        return 2;
    }
    rx = ale_rx_create(flags);
    for (off = 0; off + 1024 <= n; off += 1024) {
        int i;
        for (i = 0; i < 1024; i++) buf[i] = (float)pcm[off + i];
        ale_rx_process(rx, buf, 1024);
        while (ale_rx_next_event(rx, &ev)) print_event(&ev);
    }
    ale_rx_process(rx, buf, 0);
    while (ale_rx_next_event(rx, &ev)) print_event(&ev);
    if (ale_rx_dropped(rx)) fprintf(stderr, "ale_dump: %ld events dropped\n", ale_rx_dropped(rx));
    ale_rx_destroy(rx);
    free(pcm);
    return 0;
}
