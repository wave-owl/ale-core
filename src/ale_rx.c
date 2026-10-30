/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ale-core receiver: C99 port of KiwiSDR extensions/ALE_2G/decode_ff_impl.cpp (pinned in
 * ref/PINS; Copyright 2015 Milen Rangelov, GPL-3.0-or-later, "Significant portions of source
 * code were based on the LinuxALE project", Copyright 2000-2001 Charles Brain and Ilkka
 * Toivanen). Function and variable names follow the reference so the two can be read side by
 * side; citations are decode_ff_impl.cpp line ranges at the pin.
 *
 * Bit-exact against the reference built by ref/build_ref.sh (same compiler flags, no fast
 * math, no FP contraction): the word trace (gdebug & 2 lines) and every log line. Deviations
 * are listed in README.md "Quirks" and only change behaviour where the reference has
 * undefined behaviour or when ALE_RX_F_REFERENCE_LOCKED is off (INV-1750).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ale_internal.h"

#define ALE_EVENT_QUEUE 256

/* S_* states of decode_ff.h state_e */
enum { S_START = 0, S_TO = 1, S_TWAS = 2, S_TIS = 3, S_FROM = 4, S_CALL_EST = 5, S_CMD = 6,
       S_END = 7 };

typedef struct {
    int cmd;
    uint32_t data;
    char cmd_s[256];
} cmd_t;

struct ale_rx {
    int flags;

    int inbuf_i;
    float inbuf[ALE_HALF_FFT_SIZE];
    float fft_cs_twiddle[ALE_FFT_SIZE];
    float fft_ss_twiddle[ALE_FFT_SIZE];
    float fft_out_real[ALE_HALF_FFT_SIZE];
    float fft_out_imag[ALE_HALF_FFT_SIZE];
    int fft_mag[ALE_HALF_FFT_SIZE];
    float fft_history[ALE_FFT_SIZE];
    int fft_history_offset;

    int mag_sum[ALE_NTIME_POW2][ALE_FFT_SIZE];
    int mag_hist_offset_per_word;
    int word_sync[ALE_NTIME];

    int bits[ALE_NTIME][ALE_VOTE_ARRAY_LENGTH];
    int input_buffer_pos[ALE_NTIME];
    int word_sync_position[ALE_NTIME];

    int ito, ifrom, itis, itwas, idata, irep, icmd;
    int ito2, ifrom2, itis2, itwas2, idata2, irep2, icmd2;
    char to[4], from[4], tis[4], twas[4], data[4], rep[4], cmd[4];
    char to2[4], from2[4], tis2[4], twas2[4], data2[4], rep2[4], cmd2[4];
    char prev_data2[4];

    char current[ALE_N_CUR];
    char current2[ALE_N_CUR];
    int in_cmd, binary;
    uint32_t current_word;
    int ascii_38_ok, ascii_64_ok, ascii_nl_ok;

    cmd_t cmds[ALE_N_CMDS];
    int cmd_cnt;

    int ber[ALE_NTIME];
    int lastber;
    int bestpos;
    int nt_new;
    char s1[4], s2[4];
    int bestber;

    int state;
    int state_count;
    int stage_num;

    int last_symbol[ALE_NTIME];
    int last_sync_position[ALE_NTIME];
    int nsym;
    int samp_count_per_symbol;
    int activity_cnt;
    int active;
    int secs, timer_samps;

    int golay_errors[ALE_NTIME];   /* of the last word per hypothesis (event only) */
    int64_t samples;

    ale_event *queue;
    int q_head, q_count;
    long dropped;
};

/* ---- events -------------------------------------------------------------------------- */

static ale_event *push_event(ale_rx *r, int kind)
{
    ale_event *ev;
    if (r->q_count == ALE_EVENT_QUEUE) {           /* drop the oldest */
        r->q_head = (r->q_head + 1) % ALE_EVENT_QUEUE;
        r->q_count--;
        r->dropped++;
    }
    ev = &r->queue[(r->q_head + r->q_count) % ALE_EVENT_QUEUE];
    r->q_count++;
    memset(ev, 0, sizeof(*ev));
    ev->kind = kind;
    ev->secs = r->secs;
    ev->sample = r->samples;
    return ev;
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Append like strcat into a 256-byte reference buffer; the reference overflows (undefined
 * behaviour) past 255 characters, the port stops appending (README quirk Q3). */
static void cat_bounded(char *dst, const char *src)
{
    size_t have = strlen(dst), add = strlen(src);
    if (have + add > 255) add = have < 255 ? 255 - have : 0;
    memcpy(dst + have, src, add);
    dst[have + add] = '\0';
}

/* ---- Golay and word chain (decode_ff_impl.cpp:204-259) -------------------------------- */

uint32_t ale_golay_encode(uint32_t data)
{
    data &= 0xFFF;
    return (data << 12) + ale_golay_encode_table[data];
}

uint32_t ale_golay_decode(uint32_t code, int *errors)
{
    uint32_t data = (code >> 12) & 0xFFF;
    uint32_t parity = code & 0x00000FFFL;
    uint32_t syndrome = parity ^ ale_golay_encode_table[data];
    data = data ^ (ale_golay_error_table[syndrome] & 0x0FFF);
    *errors = (int)((ale_golay_error_table[syndrome] & 0xF000) >> 12);
    return data;
}

static uint32_t modem_de_interleave_and_fec(const ale_rx *r, const int *input, int *errors)
{
    int i;
    uint32_t worda = 0, wordb = 0;
    int error_a, error_b;

    for (i = 0; i < ALE_VOTE_BUFFER_LENGTH;) {
        worda = input[i++] ? (worda << 1) + 1 : worda << 1;
        wordb = input[i++] ? (wordb << 1) + 1 : wordb << 1;
    }
    wordb = wordb ^ 0x000FFF;
    {
        uint32_t da = ale_golay_decode(worda, &error_a);
        uint32_t db = ale_golay_decode(wordb, &error_b);
        *errors = error_a > error_b ? error_a : error_b;
        if (r->flags & ALE_RX_F_VOTE_ONLY)   /* bench (R-1752.3): systematic bits as voted */
            return (((worda >> 12) & 0xFFF) << 12) + ((wordb >> 12) & 0xFFF);
        return (da << 12) + db;
    }
}

/* ---- decode_word (decode_ff_impl.cpp:336-509) ---------------------------------------- */

static void decode_word(ale_rx *r, uint32_t w, int nt, int berw)
{
    unsigned a, b, c, preamble;
    cmd_t *cp = &r->cmds[r->cmd_cnt];
    r->current_word = w;

    preamble = ALE_BF(w, 23, 21);
    a = ALE_BF(w, 20, 14);
    b = ALE_BF(w, 13, 7);
    c = ALE_BF(w, 6, 0);

    r->ascii_38_ok = (ale_ascii_set[a] == ALE_ASCII_38) && (ale_ascii_set[b] == ALE_ASCII_38)
                     && (ale_ascii_set[c] == ALE_ASCII_38);
    r->ascii_64_ok = (ale_ascii_set[a] & ALE_ASCII_64) && (ale_ascii_set[b] & ALE_ASCII_64)
                     && (ale_ascii_set[c] & ALE_ASCII_64);
    r->ascii_nl_ok = ((ale_ascii_set[a] & ALE_ASCII_64) || (ale_ascii_set[a] == ALE_ASCII_NL))
                     && ((ale_ascii_set[b] & ALE_ASCII_64) || (ale_ascii_set[b] == ALE_ASCII_NL))
                     && ((ale_ascii_set[c] & ALE_ASCII_64) || (ale_ascii_set[c] == ALE_ASCII_NL));

    if (preamble == ALE_P_CMD) {
        r->ber[nt] = berw;
        snprintf(r->cmd, sizeof(r->cmd), "%c%c%c", a, b, c);
        r->icmd = 1;
    } else if (preamble == ALE_P_DATA) {
        if (r->ascii_38_ok || (r->ascii_nl_ok && r->in_cmd
                               && (cp->cmd == ALE_CMD_AMD || cp->cmd == ALE_CMD_DTM))) {
            r->ber[nt] = berw;
            snprintf(r->data, sizeof(r->data), "%c%c%c", a, b, c);
            r->idata = 1;
        } else {
            r->activity_cnt = 0;
        }
    } else if (preamble == ALE_P_REP) {
        if (r->ascii_38_ok || (r->ascii_nl_ok && r->in_cmd
                               && (cp->cmd == ALE_CMD_AMD || cp->cmd == ALE_CMD_DTM))) {
            r->ber[nt] = berw;
            snprintf(r->rep, sizeof(r->rep), "%c%c%c", a, b, c);
            r->irep = 1;
        } else {
            r->activity_cnt = 0;
        }
    } else if (r->ascii_38_ok) {
        r->ber[nt] = berw;
        switch (preamble) {
        case ALE_P_TO:
            snprintf(r->to, sizeof(r->to), "%c%c%c", a, b, c);
            r->ito = 1;
            break;
        case ALE_P_TWAS:
            snprintf(r->twas, sizeof(r->twas), "%c%c%c", a, b, c);
            r->itwas = 1;
            break;
        case ALE_P_FROM:
            snprintf(r->from, sizeof(r->from), "%c%c%c", a, b, c);
            r->ifrom = 1;
            break;
        case ALE_P_TIS:
            snprintf(r->tis, sizeof(r->tis), "%c%c%c", a, b, c);
            r->itis = 1;
            break;
        default:
            break;
        }
    } else {
        /* AQC-ALE packed address: the reference only decodes it for its debug print */
        r->ber[nt] = berw;
    }
}

/* ---- modem_new_symbol (decode_ff_impl.cpp:261-334) ----------------------------------- */

static int modem_new_symbol(ale_rx *r, int sym, int nt)
{
    int i, j, rv;
    int majority_vote_array[ALE_VOTE_BUFFER_LENGTH];
    int bad_votes, sum, errors;
    uint32_t word;
    static const int vote_lookup[4] = {0, 0, 1, 1};

    r->nt_new = nt;

    j = r->input_buffer_pos[nt];
    r->bits[nt][j] = (sym & 4) ? 1 : 0;
    r->input_buffer_pos[nt] = (r->input_buffer_pos[nt] + 1) % ALE_VOTE_ARRAY_LENGTH;
    j = r->input_buffer_pos[nt];
    r->bits[nt][j] = (sym & 2) ? 1 : 0;
    r->input_buffer_pos[nt] = (r->input_buffer_pos[nt] + 1) % ALE_VOTE_ARRAY_LENGTH;
    j = r->input_buffer_pos[nt];
    r->bits[nt][j] = (sym & 1) ? 1 : 0;
    r->input_buffer_pos[nt] = (r->input_buffer_pos[nt] + 1) % ALE_VOTE_ARRAY_LENGTH;

    bad_votes = 0;
    for (i = 0; i < ALE_VOTE_BUFFER_LENGTH; i++) {
        j = (i + r->input_buffer_pos[nt]) % ALE_VOTE_ARRAY_LENGTH;
        sum = r->bits[nt][j];
        j = (i + r->input_buffer_pos[nt] + ALE_SYMBOLS_PER_WORD) % ALE_VOTE_ARRAY_LENGTH;
        sum += r->bits[nt][j];
        j = (i + r->input_buffer_pos[nt] + 2 * ALE_SYMBOLS_PER_WORD) % ALE_VOTE_ARRAY_LENGTH;
        sum += r->bits[nt][j];
        if (sum == 1 || sum == 2) bad_votes++;
        majority_vote_array[i] = vote_lookup[sum];
    }

    r->ber[nt] = ALE_HIGH_BER;
    rv = 0;

    if (r->word_sync[nt] == 0) {
        if (bad_votes <= ALE_BAD_VOTE_THRESHOLD) {
            word = modem_de_interleave_and_fec(r, majority_vote_array, &errors);
            if (errors <= ALE_SYNC_ERROR_THRESHOLD) {
                r->golay_errors[nt] = errors;
                decode_word(r, word, nt, bad_votes);
                r->word_sync[nt] = 1;
                r->word_sync_position[nt] = r->input_buffer_pos[nt];
                rv = 1;
            }
        }
    } else {
        if (r->input_buffer_pos[nt] == r->word_sync_position[nt]) {
            word = modem_de_interleave_and_fec(r, majority_vote_array, &errors);
            r->golay_errors[nt] = errors;
            /* Unreachable in practice, in the reference as here: the sync is dropped on the
             * next symbol (below), so every word comes through the thresholds above (README
             * quirk Q6). INVARIANT(INV-1750): a word whose Golay decode failed never reaches
             * the protocol layer, even on this path, unless reference or bench mode. */
            if (errors < 4 || (r->flags & (ALE_RX_F_REFERENCE | ALE_RX_F_VOTE_ONLY))) {
                decode_word(r, word, nt, bad_votes);
                rv = 2;
            }
        } else {
            r->word_sync[nt] = 0;
        }
    }
    return rv;
}

/* ---- log (decode_ff_impl.cpp:511-642) ------------------------------------------------- */

static void log_entry(ale_rx *r, char *cur, char *cur2, int state, int ber)
{
    int i;
    r->prev_data2[0] = '\0';

    if (r->in_cmd) {
        if (r->cmd_cnt < ALE_N_CMDS - 1) r->cmd_cnt++;   /* quirk Q4: bounded */
        r->in_cmd = r->binary = 0;
    }

    {
        /* the reference cuts the addresses at '@' in place; keep the raw copies first */
        char raw[ALE_N_CUR], raw2[ALE_N_CUR];
        memcpy(raw, cur, ALE_N_CUR);
        memcpy(raw2, cur2, ALE_N_CUR);
        raw[ALE_N_CUR - 1] = raw2[ALE_N_CUR - 1] = '\0';
        if (state != S_CMD) {
            for (i = 0; i < ALE_N_CUR; i++) if (cur[i] == '@') cur[i] = 0;
            for (i = 0; i < ALE_N_CUR; i++) if (cur2[i] == '@') cur2[i] = 0;
        }

        switch (state) {
        case S_TWAS:
        case S_TIS: {
            ale_event *ev = push_event(r, state == S_TWAS ? ALE_EV_SOUND_TWAS : ALE_EV_SOUND_TIS);
            ev->ber = ber;
            copy_str(ev->from, cur, sizeof(ev->from));
            copy_str(ev->from_raw, raw, sizeof(ev->from_raw));
            break;
        }
        case S_TO: {
            ale_event *ev = push_event(r, ALE_EV_TO);
            ev->ber = ber;
            copy_str(ev->to, cur, sizeof(ev->to));
            copy_str(ev->to_raw, raw, sizeof(ev->to_raw));
            break;
        }
        case S_CALL_EST:
            if (r->stage_num >= 1 && r->stage_num <= 3) {
                ale_event *ev = push_event(r, ALE_EV_CALL);
                ev->ber = ber;
                ev->stage = r->stage_num;
                copy_str(ev->from, cur, sizeof(ev->from));
                copy_str(ev->to, cur2, sizeof(ev->to));
                copy_str(ev->from_raw, raw, sizeof(ev->from_raw));
                copy_str(ev->to_raw, raw2, sizeof(ev->to_raw));
            }
            break;
        default:
            break;
        }
    }

    for (i = 0; i < r->cmd_cnt; i++) {
        cmd_t *cp = &r->cmds[i];
        unsigned a = (unsigned char)cp->cmd_s[0], b = (unsigned char)cp->cmd_s[1],
                 c = (unsigned char)cp->cmd_s[2];
        uint32_t w = (a << 14) | (b << 7) | c;
        unsigned d = (unsigned char)cp->cmd_s[3], e = (unsigned char)cp->cmd_s[4],
                 f = (unsigned char)cp->cmd_s[5];
        uint32_t w2 = (d << 14) | (e << 7) | f;
        ale_event *ev = push_event(r, ALE_EV_CMD);
        char *s = ev->text;
        size_t cap = sizeof(ev->text);

        ev->ber = ber;
        ev->cmd = cp->cmd;
        ev->cmd_word = cp->data;
        ev->cmd_word2 = w2;
        copy_str(ev->payload, cp->cmd_s, sizeof(ev->payload));

        if (cp->cmd == ALE_CMD_AMD || cp->cmd == ALE_CMD_DTM) {
            /* stuffed with space characters: trim trailing spaces, CR and LF */
            int n = snprintf(s, cap, "%s: \"%s", cp->cmd == ALE_CMD_AMD ? "AMD" : "DTM",
                             cp->cmd_s);
            char *end;
            if (n < 0) n = 0;
            if ((size_t)n > cap - 3) n = (int)cap - 3;   /* quirk Q3: room for '"' */
            end = s + n - 1;
            while (end >= s && (*end == ' ' || *end == '\r' || *end == '\n')) end--;
            end[1] = '"';
            end[2] = '\0';
            ev->is_message = 1;
        } else if (cp->cmd == ALE_CMD_LQA) {
            /* quirk Q5: the reference prints the fields of the following characters; the
             * structured fields below are read from the LQA word itself (Table A-XIV) */
            snprintf(s, cap, "LQA: reply=%u MP=%u SINAD=%u BER=%u", ALE_B(w2, 13),
                     ALE_BF(w2, 12, 10), ALE_BF(w2, 9, 5), ALE_BF(w2, 4, 0));
            ev->lqa_reply = (int)ALE_B(cp->data, 13);
            ev->lqa_mp = (int)ALE_BF(cp->data, 12, 10);
            ev->lqa_sinad = (int)ALE_BF(cp->data, 9, 5);
            ev->lqa_ber = (int)ALE_BF(cp->data, 4, 0);
        } else if (cp->cmd == ALE_CMD_FRQ) {
            snprintf(s, cap, "Freq: con=%d b4(0)=%d %d_%d_%d_%d_%d.%d_%d", ALE_BF(w, 13, 8),
                     ALE_B(w2, 20), ALE_BF(w2, 19, 16), ALE_BF(w2, 15, 12), ALE_BF(w2, 11, 8),
                     ALE_BF(w2, 7, 4), ALE_BF(w2, 3, 0), ALE_BF(w, 7, 4), ALE_BF(w, 3, 0));
        } else if (a == 'd') {
            snprintf(s, cap, "DTM: kd=0x%x dc=0x%x", ALE_BF(w, 13, 10), ALE_BF(w, 9, 0));
        } else if (cp->cmd == ALE_CMD_CRC) {
            snprintf(s, cap, "CRC");
        } else {
            snprintf(s, cap, "Other: %s(%02x) %s(%02x) %s(%02x)", ale_char_name((int)a), a & 0xff,
                     ale_char_name((int)b), b & 0xff, ale_char_name((int)c), c & 0xff);
        }
    }

    r->cmd_cnt = 0;
    memset(cur, '\0', ALE_N_CUR);
}

/* ---- do_modem2 (decode_ff_impl.cpp:714-924) ------------------------------------------- */

static void do_modem2(ale_rx *r)
{
    cmd_t *cp;

    /* station behaviour (README quirk Q2): a new address section ends the DATA de-duplication,
     * so the DATA word of a repeated "TIS DL1 / DATA ABC" is appended again */
    if (!(r->flags & ALE_RX_F_REFERENCE) && (r->ito2 || r->itwas2 || r->itis2 || r->ifrom2))
        r->prev_data2[0] = '\0';

    if (r->ito2) {
        if (r->state != S_START && r->state != S_TO)
            log_entry(r, r->current, r->current2, r->state, r->lastber);
        if (r->in_cmd && r->cmd_cnt < ALE_N_CMDS - 1) r->cmd_cnt++;
        r->in_cmd = r->binary = 0;
        strcpy(r->current, r->to2);
        r->state = S_TO;
        r->state_count = 0;
        r->lastber = r->bestber;
    }

    if (r->itwas2) {
        if (r->state != S_START)
            log_entry(r, r->current, r->current2, r->state, r->lastber);
        if (r->in_cmd && r->cmd_cnt < ALE_N_CMDS - 1) r->cmd_cnt++;
        r->in_cmd = r->binary = 0;
        strcpy(r->current, r->twas2);
        r->state = S_TWAS;
        r->state_count = 0;
        r->lastber = r->bestber;
    }

    if (r->itis2) {
        if (r->state != S_START && r->state != S_TO && r->state != S_TIS
            && r->state != S_CALL_EST)
            log_entry(r, r->current, r->current2, r->state, r->lastber);
        if (r->in_cmd && r->cmd_cnt < ALE_N_CMDS - 1) r->cmd_cnt++;
        r->in_cmd = r->binary = 0;
        if (r->state == S_TO) {
            memset(r->s1, 0, 4);
            memset(r->s2, 0, 4);
            strncpy(r->s1, r->current, 3);
            strncpy(r->s2, r->current2, 3);
            strcpy(r->current2, r->current);
            r->state = S_CALL_EST;
            if (r->stage_num == 0 || r->stage_num > 3) r->stage_num = 0;
            if (r->stage_num > 0) {
                if (strncmp(r->tis2, r->s2, 3) == 0 && strncmp(r->current, r->s1, 3) == 0)
                    r->stage_num++;
                else
                    r->stage_num = 1;
            } else {
                r->stage_num = 1;
            }
            r->state_count = 0;
        } else {
            r->state = S_TIS;
            r->state_count = 0;
        }
        strcpy(r->current, r->tis2);
        r->lastber = r->bestber;
    }

    if (r->ifrom2) {
        if (r->state != S_START && r->state != S_FROM)
            log_entry(r, r->current, r->current2, r->state, r->lastber);
        if (r->in_cmd && r->cmd_cnt < ALE_N_CMDS - 1) r->cmd_cnt++;
        r->in_cmd = r->binary = 0;
        strcpy(r->current, r->from2);
        r->state = S_FROM;
        r->state_count = 0;
        r->lastber = r->bestber;
    }

    if (r->icmd2 && r->in_cmd && r->cmd_cnt < ALE_N_CMDS - 1) r->cmd_cnt++;
    cp = &r->cmds[r->cmd_cnt];

    if (r->idata2) {
        char *s = r->in_cmd ? r->cmds[r->cmd_cnt].cmd_s : r->current;
        if (r->state > S_START && r->state < S_END) {
            if (strcmp(r->data2, r->prev_data2) != 0) cat_bounded(s, r->data2);
            strcpy(r->prev_data2, r->data2);
        }
        r->state_count = 0;
        r->lastber = r->bestber;
    }

    if (r->irep2) {
        char *s = r->in_cmd ? r->cmds[r->cmd_cnt].cmd_s : r->current;
        if (r->state > S_START && r->state < S_END) cat_bounded(s, r->rep2);
        r->state_count = 0;
        r->lastber = r->bestber;
    }

    if (r->icmd2) {
        unsigned a = (unsigned char)r->cmd[0], b = (unsigned char)r->cmd[1],
                 c = (unsigned char)r->cmd[2];
        uint32_t w = (a << 14) | (b << 7) | c;
        r->in_cmd = 1;
        strcpy(cp->cmd_s, r->cmd);
        if (r->ascii_64_ok) {
            cp->cmd = ALE_CMD_AMD;
        } else {
            cp->data = w;
            r->binary = 1;
            switch (a) {
            case '"': cp->cmd = ALE_CMD_ALQA; break;
            case 'a': cp->cmd = ALE_CMD_LQA; break;
            case 'b': cp->cmd = ALE_CMD_DATA_BLK; break;
            case 'c': cp->cmd = ALE_CMD_CHAN; break;
            case 'd':
                cp->cmd = ALE_CMD_DTM;
                cp->cmd_s[0] = '\0';
                r->binary = 0;
                break;
            case 'f': cp->cmd = ALE_CMD_FRQ; break;
            case 'm': cp->cmd = ALE_CMD_MODE_SEL; break;
            case 'n': cp->cmd = ALE_CMD_NOISE_RPT; break;
            case 'p': cp->cmd = ALE_CMD_PWR_CTRL; break;
            case 'r': cp->cmd = ALE_CMD_LQA_RPT; break;
            case 't': cp->cmd = ALE_CMD_SCHED; break;
            case 'v': cp->cmd = ALE_CMD_VERS_CAP; break;
            case 'x': case 'y': case 'z': case '{': cp->cmd = ALE_CMD_CRC; break;
            case '|': cp->cmd = ALE_CMD_USER_FUNC; break;
            case '~': cp->cmd = ALE_CMD_TIME_EXCH; break;
            default: cp->cmd = ALE_CMD_OTH; break;
            }
        }
        r->state_count = 0;
        r->lastber = r->bestber;
    }
}

/* ---- do_modem1 (decode_ff_impl.cpp:926-1170) ------------------------------------------ */

static void do_modem1(ale_rx *r, int eof)
{
    int ipos, n, nt, nf;
    int temppos = 0;
    const int length = ALE_HALF_FFT_SIZE;

    memset(r->to2, 0, sizeof(r->to2));
    memset(r->from2, 0, sizeof(r->from2));
    memset(r->tis2, 0, sizeof(r->tis2));
    memset(r->twas2, 0, sizeof(r->twas2));
    memset(r->data2, 0, sizeof(r->data2));
    memset(r->rep2, 0, sizeof(r->rep2));
    memset(r->cmd2, 0, sizeof(r->cmd2));

    if (eof) {
        log_entry(r, r->current, r->current2, r->state, r->lastber);
        return;
    }

    for (ipos = 0; ipos < length; ipos++) {
        float new_sample = r->inbuf[ipos];
        float old_sample = r->fft_history[r->fft_history_offset];
        int max_magnitude = 0;
        int max_freq_offset = 0;

        r->fft_history[r->fft_history_offset] = new_sample;

        /* sliding DFT, one sample per step */
        for (n = 0; n < ALE_HALF_FFT_SIZE; n++) {
            float temp_real = r->fft_out_real[n] - old_sample + new_sample;
            float temp_imag = r->fft_out_imag[n];
            float re = (temp_real * r->fft_cs_twiddle[n]) - (temp_imag * r->fft_ss_twiddle[n]);
            float im = (temp_real * r->fft_ss_twiddle[n]) + (temp_imag * r->fft_cs_twiddle[n]);
            r->fft_out_real[n] = re;
            r->fft_out_imag[n] = im;
            r->fft_mag[n] = (int)(sqrtf((re * re) + (im * im)) * 5);
        }

        for (nf = 1; nf <= ALE_HALF_FFT_SIZE - 5; nf++) {
            if (r->fft_mag[nf] > max_magnitude) {
                max_magnitude = r->fft_mag[nf];
                max_freq_offset = nf;
            }
        }

        for (nt = 0; nt < ALE_NTIME; nt++)
            r->mag_sum[nt][r->samp_count_per_symbol] += max_magnitude;

        for (nt = 0; nt < ALE_NTIME; nt++) {
            if (r->word_sync[nt] == 0) {
                max_magnitude = 0;
                for (nf = 0; nf < ALE_FFT_SIZE; nf++) {
                    if (r->mag_sum[nt][nf] > max_magnitude) {
                        max_magnitude = r->mag_sum[nt][nf];
                        r->last_sync_position[nt] = nf;
                    }
                }
            }
        }

        for (nt = 0; nt < ALE_NTIME; nt++) {
            if (r->samp_count_per_symbol == r->last_sync_position[nt])
                r->last_symbol[nt] = ale_symbol_lookup[nt][max_freq_offset];
        }

        if (r->samp_count_per_symbol == 0) {
            int sym_rv0_cnt = 0;
            r->ito = r->ifrom = r->itis = r->itwas = r->irep = r->idata = r->icmd = 0;
            r->ito2 = r->ifrom2 = r->itis2 = r->itwas2 = r->irep2 = r->idata2 = r->icmd2 = 0;
            r->bestber = ALE_HIGH_BER;
            r->activity_cnt = 0;

            for (nt = 0; nt < ALE_NTIME; nt++) {
                int sym_rv = modem_new_symbol(r, r->last_symbol[nt], nt);
                int is_best_ber, is_best_pos, best;
                if (sym_rv) { sym_rv0_cnt++; r->activity_cnt++; }

                is_best_ber = r->ber[nt] < r->bestber;
                is_best_pos = (r->bestpos == 0) || (r->bestpos == nt);
                best = is_best_ber && is_best_pos;

                if (sym_rv && (r->flags & ALE_RX_F_WORD_EVENTS)) {
                    ale_event *ev = push_event(r, ALE_EV_WORD);
                    ev->nsym = r->nsym;
                    ev->symbol = r->last_symbol[nt];
                    ev->nt = nt;
                    ev->rv = sym_rv;
                    ev->ber = r->ber[nt];
                    ev->bestber = r->bestber;
                    ev->bestpos = r->bestpos;
                    ev->best = best;
                    ev->word = r->current_word;
                    ev->preamble = (int)ALE_BF(r->current_word, 23, 21);
                    ev->golay_errors = r->golay_errors[nt];
                }

                if (best) {
                    r->bestber = r->ber[r->nt_new];
                    temppos = nt;
                }
                r->nsym++;
            }

            /* KiwiSDR: two words in the hypothesis loop before the state advances */
            if (sym_rv0_cnt >= 2) {
                r->ito2 = r->ito;     memcpy(r->to2, r->to, 3);
                r->ifrom2 = r->ifrom; memcpy(r->from2, r->from, 3);
                r->itis2 = r->itis;   memcpy(r->tis2, r->tis, 3);
                r->itwas2 = r->itwas; memcpy(r->twas2, r->twas, 3);
                r->irep2 = r->irep;   memcpy(r->rep2, r->rep, 3);
                r->idata2 = r->idata; memcpy(r->data2, r->data, 3);
                r->icmd2 = r->icmd;   memcpy(r->cmd2, r->cmd, 3);
            }

            if (r->activity_cnt >= 2 && !r->active) r->active = 1;
            if (temppos != 0) r->bestpos = temppos;
            r->nt_new = r->bestpos;

            do_modem2(r);

            /* after 2 seconds without a state change the entry is logged */
            r->state_count += ALE_FFT_SIZE;
            if (r->state_count >= 2 * ALE_SAMPLE_RATE && r->state) {
                log_entry(r, r->current, r->current2, r->state, r->lastber);
                r->state = S_START;
                r->state_count = 0;
                r->active = 0;
            }
        }

        r->fft_history_offset = (r->fft_history_offset + 1) % ALE_FFT_SIZE;
        r->samp_count_per_symbol = (r->samp_count_per_symbol + 1) % ALE_FFT_SIZE;
        if (r->samp_count_per_symbol == 0) {
            r->mag_hist_offset_per_word = (r->mag_hist_offset_per_word + 1)
                                          % ALE_SYMBOLS_PER_WORD;
            if (r->mag_hist_offset_per_word == 0) {
                if (r->flags & ALE_RX_F_REFERENCE) {
                    memset(r->mag_sum, 0, sizeof(r->mag_sum));
                } else {
                    /* station (README quirk Q9): halve instead of clearing, so the timing
                     * estimate does not restart from a few symbols in the middle of a word */
                    int t, f;
                    for (t = 0; t < ALE_NTIME; t++)
                        for (f = 0; f < ALE_FFT_SIZE; f++) r->mag_sum[t][f] /= 2;
                }
            }
        }
    }

    r->timer_samps += length;
    r->secs = r->timer_samps / ALE_SAMPLE_RATE;
}

/* ---- public API ---------------------------------------------------------------------- */

void ale_rx_reset(ale_rx *r)
{
    int i;
    r->inbuf_i = 0;
    r->activity_cnt = r->active = 0;
    r->in_cmd = r->binary = 0;
    r->cmd_cnt = 0;
    r->bestpos = 0;
    r->lastber = ALE_HIGH_BER;
    r->fft_history_offset = 0;
    r->stage_num = 0;
    r->state_count = 0;
    r->state = S_START;
    r->samp_count_per_symbol = 0;
    r->nsym = 0;
    r->mag_hist_offset_per_word = 0;
    memset(r->fft_out_real, 0, sizeof(r->fft_out_real));
    memset(r->fft_out_imag, 0, sizeof(r->fft_out_imag));
    memset(r->fft_mag, 0, sizeof(r->fft_mag));
    memset(r->fft_history, 0, sizeof(r->fft_history));
    memset(r->mag_sum, 0, sizeof(r->mag_sum));
    memset(r->bits, 0, sizeof(r->bits));
    memset(r->word_sync, 0, sizeof(r->word_sync));
    memset(r->last_symbol, 0, sizeof(r->last_symbol));
    memset(r->last_sync_position, 0, sizeof(r->last_sync_position));
    for (i = 0; i < ALE_NTIME; i++) r->ber[i] = ALE_HIGH_BER;
    memset(r->input_buffer_pos, 0, sizeof(r->input_buffer_pos));
    memset(r->word_sync_position, 0, sizeof(r->word_sync_position));
    memset(r->golay_errors, 0, sizeof(r->golay_errors));
    /* modem_init: timer and the protocol buffers start empty */
    r->timer_samps = 0;
    r->secs = 0;
    r->samples = 0;
    memset(r->current, 0, sizeof(r->current));
    memset(r->current2, 0, sizeof(r->current2));
    memset(r->cmds, 0, sizeof(r->cmds));
    memset(r->prev_data2, 0, sizeof(r->prev_data2));
    r->q_head = r->q_count = 0;
}

ale_rx *ale_rx_create(int flags)
{
    int i;
    ale_rx *r = (ale_rx *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->queue = (ale_event *)calloc(ALE_EVENT_QUEUE, sizeof(ale_event));
    if (!r->queue) {
        free(r);
        return NULL;
    }
    r->flags = flags;
    for (i = 0; i < ALE_FFT_SIZE; i++) {
        r->fft_cs_twiddle[i] = (float)cos((-2.0 * M_PI * i) / ALE_FFT_SIZE);
        r->fft_ss_twiddle[i] = (float)sin((-2.0 * M_PI * i) / ALE_FFT_SIZE);
    }
    ale_rx_reset(r);
    return r;
}

void ale_rx_destroy(ale_rx *r)
{
    if (!r) return;
    free(r->queue);
    free(r);
}

int ale_rx_process(ale_rx *r, const float *samples, int n)
{
    int i;
    if (n <= 0) {
        do_modem1(r, 1);
        return r->q_count;
    }
    for (i = 0; i < n; i++) {
        r->inbuf[r->inbuf_i++] = samples[i];
        r->samples++;
        if (r->inbuf_i == ALE_HALF_FFT_SIZE) {
            do_modem1(r, 0);
            r->inbuf_i = 0;
        }
    }
    return r->q_count;
}

int ale_rx_next_event(ale_rx *r, ale_event *ev)
{
    if (r->q_count == 0) return 0;
    *ev = r->queue[r->q_head];
    r->q_head = (r->q_head + 1) % ALE_EVENT_QUEUE;
    r->q_count--;
    return 1;
}

long ale_rx_dropped(ale_rx *r) { return r->dropped; }

int64_t ale_rx_samples(ale_rx *r) { return r->samples; }

/* ---- bench word decision (R-1755) ---------------------------------------------------- */

int32_t ale_bench_decode_word(const float *x, int mode, int *bad_votes, int *errors)
{
    int bits[ALE_VOTE_ARRAY_LENGTH], vote[ALE_VOTE_BUFFER_LENGTH];
    int k, i, n, nf;
    uint32_t worda = 0, wordb = 0, da, db;
    int ea, eb;
    double cs[ALE_HALF_FFT_SIZE][ALE_FFT_SIZE], sn[ALE_HALF_FFT_SIZE][ALE_FFT_SIZE];
    for (nf = 1; nf <= ALE_HALF_FFT_SIZE - 5; nf++)
        for (n = 0; n < ALE_FFT_SIZE; n++) {
            double ph = -2.0 * M_PI * nf * n / ALE_FFT_SIZE;
            cs[nf][n] = cos(ph);
            sn[nf][n] = sin(ph);
        }
    for (k = 0; k < ALE_SYMBOLS_PER_WORD; k++) {
        const float *s = x + k * ALE_SYMBOL_SAMPLES;
        double best = -1.0;
        int sym, best_bin = 0;
        for (nf = 1; nf <= ALE_HALF_FFT_SIZE - 5; nf++) {
            double re = 0.0, im = 0.0, m;
            for (n = 0; n < ALE_FFT_SIZE; n++) {
                re += s[n] * cs[nf][n];
                im += s[n] * sn[nf][n];
            }
            m = re * re + im * im;
            if (m > best) { best = m; best_bin = nf; }
        }
        sym = ale_symbol_lookup[6][best_bin];
        bits[3 * k] = (sym & 4) ? 1 : 0;
        bits[3 * k + 1] = (sym & 2) ? 1 : 0;
        bits[3 * k + 2] = (sym & 1) ? 1 : 0;
    }
    *bad_votes = 0;
    for (i = 0; i < ALE_VOTE_BUFFER_LENGTH; i++) {
        int sum = bits[i] + bits[i + ALE_SYMBOLS_PER_WORD] + bits[i + 2 * ALE_SYMBOLS_PER_WORD];
        if (sum == 1 || sum == 2) (*bad_votes)++;
        vote[i] = sum >= 2;
    }
    for (i = 0; i < ALE_VOTE_BUFFER_LENGTH; i += 2) {
        worda = (worda << 1) | (uint32_t)vote[i];
        wordb = (wordb << 1) | (uint32_t)vote[i + 1];
    }
    wordb ^= 0xFFF;
    da = ale_golay_decode(worda, &ea);
    db = ale_golay_decode(wordb, &eb);
    *errors = ea > eb ? ea : eb;
    if (mode == 0) return (int32_t)((((worda >> 12) & 0xFFF) << 12) | ((wordb >> 12) & 0xFFF));
    if (mode == 1 && (*bad_votes > ALE_BAD_VOTE_THRESHOLD || *errors > ALE_SYNC_ERROR_THRESHOLD))
        return -1;
    if (*errors >= 4) return -1;
    return (int32_t)((da << 12) | db);
}
