/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ale-core: MIL-STD-188-141B Appendix A (2G ALE) receiver and transmitter, C99.
 *
 * Receiver: a bit-exact port of the KiwiSDR extensions/ALE_2G decoder (decode_ff_impl.cpp,
 * Copyright 2015 Milen Rangelov, GPL-3.0-or-later; derived from gr-ale and LinuxALE,
 * Copyright 2000-2001 Charles Brain and Ilkka Toivanen). Transmitter: word, address,
 * sequence and AMD encoding after openALE (MIT, Copyright 2024 PC-ALE 2.0 Contributors,
 * 2025-2026 openALE Contributors), Golay table from the receiver's reference.
 *
 * Contract "ale-c1", ABI 1 (see ale_version()). Thread model: one ale_rx per thread, no
 * global mutable state; the encoder functions are pure.
 */
#ifndef ALE_CORE_ALE_H
#define ALE_CORE_ALE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALE_ABI 1

/* Waveform (MIL-STD-188-141B A.5.1): 8-FSK, 750..2500 Hz, 250 Hz spacing, 8 ms symbols. */
#define ALE_SAMPLE_RATE 8000
#define ALE_SYMBOL_SAMPLES 64
#define ALE_SYMBOLS_PER_WORD 49
#define ALE_WORD_SAMPLES (ALE_SYMBOLS_PER_WORD * ALE_SYMBOL_SAMPLES) /* 3136 = 392 ms */

/* Preambles (A.5.2.3, decode_ff.h preamble_types_e). */
enum {
    ALE_P_DATA = 0, ALE_P_THRU = 1, ALE_P_TO = 2, ALE_P_TWAS = 3,
    ALE_P_FROM = 4, ALE_P_TIS = 5, ALE_P_CMD = 6, ALE_P_REP = 7
};

/* Receiver event kinds. */
enum {
    ALE_EV_WORD = 1,       /* one word decoded at one frequency hypothesis (trace) */
    ALE_EV_SOUND_TWAS = 2, /* sounding, "this was" */
    ALE_EV_SOUND_TIS = 3,  /* sounding, "this is" */
    ALE_EV_TO = 4,         /* TO section without a conclusion */
    ALE_EV_CALL = 5,       /* TO + TIS: stage 1 call, 2 response, 3 acknowledgement */
    ALE_EV_CMD = 6         /* command / message attached to the preceding log entry */
};

/* Command types (decode_ff.h cmd_e). */
enum {
    ALE_CMD_AMD = 0, ALE_CMD_DTM, ALE_CMD_LQA, ALE_CMD_FRQ, ALE_CMD_CRC, ALE_CMD_OTH,
    ALE_CMD_ALQA, ALE_CMD_DBA, ALE_CMD_CHAN, ALE_CMD_MODE_SEL, ALE_CMD_NOISE_RPT,
    ALE_CMD_DATA_BLK, ALE_CMD_PWR_CTRL, ALE_CMD_LQA_RPT, ALE_CMD_SCHED,
    ALE_CMD_VERS_CAP, ALE_CMD_USER_FUNC, ALE_CMD_TIME_EXCH
};

/* Receiver flags (ale_rx_create). */
/* Reference behaviour of KiwiSDR in every detail (README "Quirks", Q2, Q6, Q9): the
 * bit-exact differential tests set it. Without it (the station) a DATA word that repeats the
 * previous one is still appended after a new address section starts, so a repeated "TIS DL1 /
 * DATA ABC" sounding reads DL1ABC instead of DL1; the symbol-timing sums are halved instead of
 * cleared every 49 symbols; and a word whose Golay decode failed is never passed on
 * (INV-1750; the reference's locked-phase path that would pass it is unreachable). */
#define ALE_RX_F_REFERENCE 0x1
/* Bench only (R-1752.3): words keep the systematic data bits of the voted halves without
 * Golay correction; Golay still gates the word sync (the receiver has no other way to find
 * the word phase). Never set by a station; it exists to measure the Golay gain. */
#define ALE_RX_F_VOTE_ONLY 0x2
/* Emit ALE_EV_WORD trace events (one per word per frequency hypothesis). */
#define ALE_RX_F_WORD_EVENTS 0x4

#define ALE_ADDR_MAX 255
#define ALE_TEXT_MAX 255

typedef struct ale_event {
    int kind;             /* ALE_EV_* */
    int ber;              /* bad votes ("His BER" of the reference log) */
    int secs;             /* reference clock: whole seconds of input at the emit */
    int64_t sample;       /* samples consumed when the event was emitted */
    /* ALE_EV_WORD */
    int nsym;             /* reference symbol counter */
    int symbol;           /* tribit just shifted in */
    int nt;               /* frequency hypothesis 0..16 (125 Hz steps) */
    int rv;               /* 1 word sync found, 2 word at the locked phase */
    int bestber;          /* best bad-vote count before this hypothesis */
    int bestpos;          /* best hypothesis before this symbol */
    int best;             /* this hypothesis became the best */
    int golay_errors;     /* max errors of the two halves (0..3, 4 = failed) */
    int preamble;         /* ALE_P_* of word */
    uint32_t word;        /* 24-bit word */
    /* ALE_EV_CALL */
    int stage;            /* 1 call, 2 response, 3 acknowledgement */
    /* addresses as the reference prints them ('@' ends the string) and unmodified */
    char from[ALE_ADDR_MAX + 1];
    char to[ALE_ADDR_MAX + 1];
    char from_raw[ALE_ADDR_MAX + 1];
    char to_raw[ALE_ADDR_MAX + 1];
    /* ALE_EV_CMD */
    int cmd;              /* ALE_CMD_* */
    int is_message;       /* AMD or DTM text ("MSG" in the reference log) */
    char text[ALE_TEXT_MAX + 1];    /* message as the reference logs it */
    char payload[ALE_TEXT_MAX + 1]; /* raw command characters (AMD/DTM text) */
    uint32_t cmd_word, cmd_word2;   /* first and second 21-bit command payloads */
    int lqa_reply, lqa_mp, lqa_sinad, lqa_ber;
} ale_event;

/* Version and contract: "ale-core <ver> contract=ale-c1 abi=1 tx=1". */
const char *ale_version(void);
int ale_abi(void);
/* Size of the public structures for binding checks: 0 = ale_event. */
size_t ale_sizeof(int which);

/* ---- receiver ---------------------------------------------------------------------- */
typedef struct ale_rx ale_rx;

/* New receiver for 8000 Hz audio; NULL on allocation failure. */
ale_rx *ale_rx_create(int flags);
void ale_rx_destroy(ale_rx *rx);
/* Back to the state after create (reference modem_reset), keeping the flags. */
void ale_rx_reset(ale_rx *rx);
/* Feed n samples (int16 scale, i.e. full scale +-32767). n == 0 flushes the pending log
 * entry (end of input). Returns the number of queued events. */
int ale_rx_process(ale_rx *rx, const float *samples, int n);
/* Copy the oldest queued event into *ev; 1 if there was one, else 0. */
int ale_rx_next_event(ale_rx *rx, ale_event *ev);
/* Events dropped because the queue was full. */
long ale_rx_dropped(ale_rx *rx);
/* Samples consumed so far. */
int64_t ale_rx_samples(ale_rx *rx);

/* Bench only (R-1755): decide one word from ALE_WORD_SAMPLES samples aligned to its first
 * symbol at the nominal tones (frequency hypothesis 6): per symbol the strongest of DFT bins
 * 1..27 mapped like the receiver, 2-of-3 vote with bad-vote count, deinterleave, then
 *   mode 0  vote only: the systematic bits of the voted halves, always accepted
 *   mode 1  the receiver's acceptance: Golay, at most 1 error per half, <= 25 bad votes
 *   mode 2  full Golay: up to 3 errors per half corrected, 4 (detected) rejected
 * Returns the 24-bit word, or -1 when rejected; *bad_votes and *errors are set. */
int32_t ale_bench_decode_word(const float *samples, int mode, int *bad_votes, int *errors);

/* ---- word coding (pure) ------------------------------------------------------------ */
uint32_t ale_golay_encode(uint32_t data12);
/* 12 data bits; *errors = 0..3 corrected, 4 = detected uncorrectable (data unchanged). */
uint32_t ale_golay_decode(uint32_t code24, int *errors);
/* 24-bit word -> 49 transmitted bits (A: Golay of the upper 12 bits, B: Golay of the lower
 * 12 bits with inverted check bits, interleaved A0 B0 A1 B1 ..., bit 48 = stuff bit 0). */
uint64_t ale_word_tx49(uint32_t word24);
/* 49 transmitted bits -> 49 tribit symbols carrying them three times. */
void ale_tx49_symbols(uint64_t tx49, unsigned char symbols[ALE_SYMBOLS_PER_WORD]);
/* Tone of a tribit (Hz). */
int ale_symbol_tone_hz(int symbol);
/* One word from a preamble and three characters (basic ASCII-38 except DATA/REP, which take
 * ASCII-64; CMD takes any 7-bit code); -1 on an invalid character. */
int32_t ale_word_make(int preamble, const char chars[3]);

/* ---- sequences (openALE ALESequenceBuilder) ----------------------------------------- */
/* Every builder writes at most max words and returns the count, or -1 when an address or
 * text is invalid or max is too small. */
int ale_seq_address(const char *addr, int first_preamble, uint32_t *words, int max);
int ale_seq_amd(const char *text, uint32_t *words, int max);
uint32_t ale_lqa_word(int ka1, int mp, int sinad, int ber);
/* Sounding: the conclusion (TIS, or TWAS when twas) repeated reps times. */
int ale_seq_sounding(const char *self, int twas, int reps, uint32_t *words, int max);
/* Individual call: scanning call (first TO word x 2 x scan_channels), leading call (TO
 * address twice), optional FROM self + LQA command + AMD, conclusion TIS self. lqa < 0 =
 * no LQA command, amd NULL/"" = no message. */
int ale_seq_call(const char *dest, const char *self, int scan_channels, int32_t lqa,
                 const char *amd, uint32_t *words, int max);
/* Response (TO caller x2 + TIS self) or rejection (TWAS self). */
int ale_seq_response(const char *caller, const char *self, int reject, uint32_t *words, int max);
/* Acknowledgement (TO peer x2 + TIS self, or TWAS self when no_link). */
int ale_seq_ack(const char *peer, const char *self, int no_link, uint32_t *words, int max);
/* Termination (TO peer x2 + TWAS self; TWAS self alone when peer is empty). */
int ale_seq_termination(const char *peer, const char *self, uint32_t *words, int max);

/* ---- modulator --------------------------------------------------------------------- */
/* Phase-continuous 8-FSK at 8000 Hz: every word is 49 symbols of 64 samples. The phase
 * continues across calls until ale_tx_reset(). Writes n_words * ALE_WORD_SAMPLES samples
 * (amplitude * sin) and returns that count, or -1 when max_samples is too small. */
typedef struct ale_tx ale_tx;
ale_tx *ale_tx_create(void);
void ale_tx_destroy(ale_tx *tx);
void ale_tx_reset(ale_tx *tx);
int ale_tx_render(ale_tx *tx, const uint32_t *words, int n_words, float amplitude,
                  float *out, int max_samples);

#ifdef __cplusplus
}
#endif
#endif
