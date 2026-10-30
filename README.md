# ale-core

A C99 library that receives and transmits **2G ALE** (Automatic Link Establishment,
MIL-STD-188-141B Appendix A / FED-STD-1045): audio in, ALE words, soundings, calls, LQA and
AMD messages out; addresses and messages in, 8-FSK audio out. The receiver is a line-by-line
port of the KiwiSDR `extensions/ALE_2G` decoder (the gr-ale / LinuxALE lineage that runs on
the public KiwiSDR network); on the reference build it prints the reference's log and word
trace byte for byte, clean, in noise, off frequency and on pure noise (42/42 vectors, see
[Verification](#verification)). The encoder follows openALE and reproduces its words,
49-bit transmissions and tribit symbols exactly. It is the ALE implementation of
[wave-owl](https://github.com/wave-owl/wave-owl) (change `complete-ale`).

| | |
|---|---|
| **Is** | A single-channel 2G ALE **receiver** at 8000 Hz: sliding 64-point DFT, 17 frequency hypotheses (+-1 kHz in 125 Hz steps), 64 sub-symbol timing offsets, 49-phase word sync, 2-of-3 vote with bad-vote count, deinterleave, Golay(24,12) on both halves, the reference's word acceptance and its protocol log (soundings TWAS/TIS, TO, call / response / acknowledgement, commands: AMD, DTM, LQA, frequency, CRC, other). |
| **Is** | The matching **encoder and modulator** (tag `tx=1`): Golay, interleave, stuff bit, triple-redundant tribits, addresses (basic ASCII-38, `@` padding, DATA/REP extension words), AMD (ASCII-64), LQA command, and the openALE sequences (sounding, scanning + leading call, response, acknowledgement, termination); phase-continuous 8-FSK at 8000 Hz. |
| **Is not** | An ALE controller: no scanning, no timing of a handshake, no automatic answers, no radio control. A host decides what is sent and when. No 3G (Appendix C), 4G/WALE (188-141D), AQC-ALE decoding (AQC words are recognised and dropped as the reference does), DBM/DTM data transfer, or linking protection. |
| | No I/O, no threads, no global mutable state; memory is allocated in `*_create()` only. |

## Standard and reference table (task 1.4 of wave-owl `complete-ale`)

| Item | MIL-STD-188-141B App. A | Reference (KiwiSDR `decode_ff*.{h,cpp}`, openALE) | ale-core |
|---|---|---|---|
| Tones | 8 tones 750..2500 Hz, 250 Hz apart, 8 ms per tribit, 125 symbols/s (A.5.1) | `decode_ff_impl.h:37-44` (comment), `:62-71`; openALE `include/FSK/ale_waveform.h` `TONE_FREQS_HZ` | `ALE_SYMBOL_SAMPLES` 64 at 8000 Hz |
| Tone to tribit | Gray order 750=000, 1000=001, 1250=011, 1500=010, 1750=110, 2000=111, 2250=101, 2500=100 (A.5.1.2) | `decode_ff.h` `g_symbol_lookup`; openALE `FREQ_TO_SYMBOL {0,1,3,2,6,7,5,4}` | `ale_symbol_lookup`, `ale_symbol_tone_hz` |
| Word | 24 bits: preamble 3 + three 7-bit characters (A.5.2.1) | `decode_ff_impl.cpp` `decode_word` `bf(w,23,21)`..`bf(w,6,0)` | the same |
| Preambles | DATA 0, THRU 1, TO 2, TWAS 3, FROM 4, TIS 5, CMD 6, REP 7 (Table A-VII) | `decode_ff.h:35` `preamble_types_e`; openALE `WORD_TYPE_NAMES` | `ALE_P_*` |
| Characters | basic ASCII-38 (A-Z, 0-9, `@`, `?`) for addresses, ASCII-64 (0x20..0x5F) for AMD, ASCII-128 for DTM (A.5.2.4, A.5.7.2) | `decode_ff.h` `ASCII_Set`; openALE `ASCII_SET` | `ale_ascii_set` (receiver), `ale_word_make` (encoder) |
| FEC | each 12-bit half Golay(24,12), second half's check bits inverted (A.5.2.2) | `golay_encode` / `golay_decode`, `wordb ^ 0xFFF`; openALE `flip_coder_b_check_bits` | `ale_golay_*`, table from `decode_ff.h` |
| Interleave | A and B bits alternate, MSB first, plus one stuff bit = 49 bits (A.5.2.2.3) | `modem_de_interleave_and_fec`; openALE `WordInterleaver::interleave` | `ale_word_tx49` |
| Redundancy | each 49-bit word sent three times = 49 tribits (392 ms) (A.5.2.2.4) | `modem_new_symbol` vote over `+49`, `+98`; openALE `encode_tx49` | `ale_tx49_symbols` |
| Acceptance | majority vote; word accepted below error thresholds (A.5.2.2.5) | `BAD_VOTE_THRESHOLD 25`, `SYNC_ERROR_THRESHOLD 1` (`decode_ff_impl.h:62-63`) | the same |
| LQA command | CMD, `a`, KA1, MP 3 bits, SINAD 5 bits, BER 5 bits (Table A-XIV) | openALE `encode_lqa_cmd` (`src/LQA/lqa_metrics.cpp`) | `ale_lqa_word`; receiver event fields `lqa_*` |
| AMD | CMD word with ASCII-64 characters, then DATA/REP alternating, space padding, at most 90 characters (A.5.7.2) | openALE `encode_amd`; KiwiSDR `AMD` path | `ale_seq_amd` |

## Signal chain (receiver)

`decode_ff_impl.cpp` at the pin; this port keeps names and order (`src/ale_rx.c`).

1. **Sliding DFT** (`do_modem1`, lines 926-1030): one 64-point DFT per input sample by the
   recursive update `X[n] = (X[n] - x_old + x_new) * W^n`, float, bins 0..31; magnitude
   `(int)(sqrt(re^2 + im^2) * 5)`; the strongest of bins 1..27 is the tone candidate.
2. **Symbol timing** (lines 1031-1070): the winning magnitude is summed per sample offset
   (0..63) over 49 symbols; each hypothesis takes the symbol at the offset with the largest
   sum while it is not synchronised; the sums restart every 49 symbols.
3. **Frequency hypotheses**: 17 rows of `g_symbol_lookup` map bins `nt..nt+15` to tribits,
   i.e. tone sets from 750 - 6x125 Hz upwards in 125 Hz steps (hypothesis 6 = nominal).
4. **Vote and word** (`modem_new_symbol`, lines 261-334): 147-bit ring per hypothesis, vote
   of bits `i`, `i+49`, `i+98`, bad votes counted; deinterleave, invert B's check bits,
   Golay both halves; accepted when `bad_votes <= 25` and both halves have at most 1 error.
5. **Gate** (lines 1111-1121): the word state advances only when at least two hypotheses
   decoded a word at this symbol ("KiwiSDR fix").
6. **Protocol log** (`do_modem2`, `log`): TO / TWAS / TIS / FROM sections, DATA/REP
   appended, call stages (TO x + TIS y = call, then response, then acknowledgement), commands
   collected and logged with the section; 2 s without a new word ends the entry.

## Quirks

Kept on purpose (bit-exactness, the D-918 pattern) unless noted.

| | Reference behaviour | ale-core |
|---|---|---|
| Q1 | KiwiSDR builds with `-Ofast`. | The core and the reference programs of the tests build with `-O2 -ffp-contract=off -fno-fast-math`; the float sliding DFT then rounds identically. |
| Q2 | A DATA word equal to the previous DATA word is dropped until the next log entry, so a repeated "TIS DL1 / DATA ABC" sounding reads `DL1`. | Kept with `ALE_RX_F_REFERENCE`; without it (the station) a new address section ends the de-duplication and the sounding reads `DL1ABC`. |
| Q3 | Message and address buffers are 256 bytes and `strcat` overflows them (undefined behaviour). | Appending stops at 255 characters. |
| Q4 | More than 16 commands in one entry overflow `cmds[16]` (undefined behaviour). | The 16th slot is reused. |
| Q5 | The LQA log line prints the fields of the three bytes after the command's own characters (`cmd_s[3..5]`), which are zero or stale; the LQA values themselves are in the command word (Table A-XIV). | The log text equals the reference (zero-initialised receiver, as KiwiSDR's static `ale_2g_chan[]`); the event's `lqa_reply/mp/sinad/ber` are read from the command word. |
| Q6 | The "locked" branch of `modem_new_symbol` is unreachable: synchronisation is dropped on the next symbol, so every word passes the thresholds of step 4. | Same; the branch additionally never passes a failed Golay word (INV-1750 of wave-owl). |
| Q7 | `cdeco()` prints a name table of the Kiwi runtime. | `ale_char_name()`: the character, or `ctl`; the reference build shim uses the same table. |
| Q8 | The `active` flag and the frequency/slot messages to the Kiwi web UI. | Not ported (no I/O). |
| Q9 | The per-offset magnitude sums of the symbol timing are cleared every 49 symbols counted from the start of the input, not from a word: a word that straddles the clearing is sampled at a timing estimated from a few symbols. About 4 % of start offsets lose a clean sounding (448 offsets tried, 19 lost). | Kept with `ALE_RX_F_REFERENCE`; without it (the station) the sums are halved instead of cleared, which keeps the estimate: 0 of the same 448 offsets lost, and no false decode in 60 s of noise. |

## Golay gain (wave-owl `scripts/ale_golay_bench.py`, seed 20261030)

AWGN, SNR in a 3 kHz noise bandwidth, 2000 random words per step at the symbol timing:
1 % word error rate at -3.1 dB with voting only, -4.3 dB with the receiver's acceptance
(Golay, at most one error per half, at most 25 bad votes; no false word down to -12 dB) and
-5.7 dB with full Golay (up to 3 errors per half; 3-6 % false words below -6 dB): 2.5 dB
gain of Golay over voting. A whole sounding (receiver with sync, station behaviour) is lost
0 % of the time at -2 dB, 1 % at -4 dB and 26 % at -6 dB; the vote-only receiver loses 50 %
at -6 dB.

## API

`include/ale.h`: `ale_rx_create(flags)`, `ale_rx_process(rx, samples, n)` (8000 Hz, int16
scale; `n == 0` flushes at end of input), `ale_rx_next_event(rx, &ev)`, `ale_rx_reset`,
`ale_rx_destroy`; events `ALE_EV_WORD` (with `ALE_RX_F_WORD_EVENTS`), `ALE_EV_SOUND_TWAS`,
`ALE_EV_SOUND_TIS`, `ALE_EV_TO`, `ALE_EV_CALL` (stage 1/2/3), `ALE_EV_CMD`. Encoder:
`ale_golay_*`, `ale_word_tx49`, `ale_tx49_symbols`, `ale_word_make`, `ale_seq_*`,
`ale_lqa_word`, `ale_tx_render`. Bench: `ALE_RX_F_VOTE_ONLY`, `ale_bench_decode_word`.

## Contract and versioning

`ale_version()` = `ale-core <x.y.z> contract=ale-c1 abi=1 tx=1`; `ale_sizeof(0)` =
`sizeof(ale_event)` for bindings. ABI 1 changes only with a new contract name.

## Verification

* `make test`: Golay (every data word; 1 and 3 errors corrected, 4 detected; minimum
  distance 8), character sets, the encoder against openALE (25 distinct words: 49-bit
  transmissions and all 49 tribits; 12 scenarios word for word), TX -> RX loopback (clean,
  in noise, vote-only), 60 s of noise without a log entry, and `scripts/check_vectors.sh`.
* **Vectors** (`vectors/`, `scripts/make_vectors.py`): 42 WAVs made from openALE's
  transmitter (12 scenarios, handshake, handshake + termination, AWGN at +10/0/-4/-7 dB in
  3 kHz with random start offsets, audio offsets +125/-250/+60 Hz, 20 s of noise, 12 random
  mixtures down to -9 dB). The unmodified KiwiSDR receiver (`ref/build_ref.sh`) decodes each;
  its output with colours removed is `<name>.ref.txt`. `ale_dump -w -r` prints the same
  42/42: every word trace line (symbol counter, hypothesis, bad votes, best position, word)
  and every log line.
* `make asan`: the same under AddressSanitizer and UndefinedBehaviorSanitizer.

## Performance and memory

Apple M4, one channel: 340 s of audio in 1.4 s including process start-up, about 0.4 % of
one core (budget for wave-owl R-1751.5: 5 % per channel). Memory per receiver: about 25 KB
of state plus a 256-event queue (420 KB); nothing is allocated after `ale_rx_create`.

## Building

`make` (shared `libale.{dylib,so}`, static `libale.a`, `ale_dump`), `make test`,
`make asan`. Reference programs (tests only, never shipped): `make ref` checks out the pins
of `ref/PINS` (KiwiSDR and openALE, from the wave-owl mirrors or `$ALE_REF_SRC`) and needs
`fftw3f` for the KiwiSDR build; `make vectors` regenerates `vectors/`.

## License and credits

GPL-3.0-or-later (`LICENSE`). The receiver is a port of KiwiSDR `extensions/ALE_2G`
(`decode_ff_impl.cpp`, `decode_ff.h`; Copyright 2015 Milen Rangelov, GPL-3.0-or-later; John
Seamons ZL4VO/KF6VO for KiwiSDR), which states that significant portions are based on
LinuxALE (Copyright 2000-2001 Charles Brain G4GUO and Ilkka Toivanen, GNU GPL). The encoder
follows openALE (MIT; Copyright 2024 PC-ALE 2.0 Contributors, Alex Pennington AAM402/KY4OLB;
2025-2026 openALE Contributors, dl3hc). MIL-STD-188-141B is a public US DoD standard.
