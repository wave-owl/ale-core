# Changelog

## 0.1.0 (2026-10-30)

- Receiver: C99 port of KiwiSDR `extensions/ALE_2G` (gr-ale / LinuxALE lineage) at the pin of
  `ref/PINS`: sliding DFT, 17 frequency hypotheses, 49-phase word sync, vote, deinterleave,
  Golay(24,12), the reference's acceptance thresholds and protocol log; events for words,
  soundings, TO sections, call stages and commands (AMD, DTM, LQA with the Table A-XIV fields,
  frequency, CRC, other). Bit-exact with the unmodified reference on 42/42 vectors.
- Station behaviour without `ALE_RX_F_REFERENCE`: DATA de-duplication ends at a new address
  section (quirk Q2); the symbol-timing sums are halved instead of cleared every 49 symbols
  (quirk Q9).
- Encoder (`tx=1`): Golay, interleave, tribits, addresses, AMD, LQA command and the openALE
  sequences; phase-continuous 8-FSK modulator. Equal to openALE on 25 words and 12 scenarios.
- Bench: `ALE_RX_F_VOTE_ONLY`, `ale_bench_decode_word`.
- Contract `ale-c1`, ABI 1.
