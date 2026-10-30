#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the ale-core vector set from the two reference programs.

1. ``openale_vectors`` (unmodified openALE encoder) writes each scenario's words, 49-bit
   transmissions, symbols and 8000 Hz audio.
2. This script assembles the WAVs: silence or noise before and after, several
   transmissions in one file (handshake), AWGN at fixed seeds (SNR in a 3 kHz noise
   bandwidth), arbitrary start offsets, audio frequency offsets.
3. ``kiwi_ref -w`` (unmodified KiwiSDR ALE_2G receiver) decodes every WAV; its output with
   the ANSI colours removed is the golden ``<name>.ref.txt``.
4. ``ale_dump -w -r`` (this core, reference locked-phase behaviour) must print the same.

Usage: make_vectors.py --ref ref/build --dump build/ale_dump --out vectors
Needs numpy. Writes vectors/<name>.wav, <name>.ref.txt and vectors/manifest.json.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import wave

import numpy as np

RATE = 8000
BLOCK = 1024
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def load_raw(raw_dir, name):
    pcm = np.fromfile(os.path.join(raw_dir, name + ".s16"), dtype="<i2").astype(np.float64)
    meta = json.load(open(os.path.join(raw_dir, name + ".json")))
    return pcm, meta


def silence(seconds):
    return np.zeros(int(round(seconds * RATE)))


def awgn(x, snr_db, rng, signal_power):
    """Add white noise whose power in a 3 kHz band is signal_power / 10^(snr/10)."""
    noise_power_total = signal_power / 10 ** (snr_db / 10.0) * (RATE / 2.0) / 3000.0
    return x + rng.normal(0.0, np.sqrt(noise_power_total), len(x))


def shift(x, hz):
    """Move the audio by hz (analytic signal times a complex exponential, real part)."""
    n = len(x)
    spec = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1.0
    if n % 2 == 0:
        h[n // 2] = 1.0
        h[1:n // 2] = 2.0
    else:
        h[1:(n + 1) // 2] = 2.0
    analytic = np.fft.ifft(spec * h)
    return np.real(analytic * np.exp(2j * np.pi * hz * np.arange(n) / RATE))


def to_int16(x):
    pad = (-len(x)) % BLOCK
    x = np.concatenate([x, np.zeros(pad)])
    return np.clip(np.round(x), -32768, 32767).astype("<i2")


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def scenarios(raw_dir):
    """(name, audio, description, words) of every vector."""
    names = ["sound_twas_dg6fl", "sound_tis_dl1abc", "sound_twas_long", "call_dl1abc",
             "response_dg6fl", "ack_dl1abc", "termination_dl1abc", "call_lqa", "call_amd",
             "allcall", "anycall", "netcall_hfn"]
    raw = {n: load_raw(raw_dir, n) for n in names}
    out = []
    for n in names:
        pcm, meta = raw[n]
        out.append((n, np.concatenate([silence(0.5), pcm, silence(2.5)]),
                    "openALE scenario, clean", {n: meta["words"]}))
    # three-way handshake and termination in one stream (the receiver's stage logic)
    hs = [raw["call_dl1abc"][0], silence(0.3), raw["response_dg6fl"][0], silence(0.3),
          raw["ack_dl1abc"][0]]
    out.append(("handshake", np.concatenate([silence(0.5)] + hs + [silence(2.5)]),
                "call, response, acknowledgement",
                {k: raw[k][1]["words"] for k in ("call_dl1abc", "response_dg6fl", "ack_dl1abc")}))
    out.append(("link_terminate", np.concatenate(
        [silence(0.5)] + hs + [silence(1.0), raw["termination_dl1abc"][0], silence(2.5)]),
        "handshake then termination",
        {k: raw[k][1]["words"] for k in ("call_dl1abc", "response_dg6fl", "ack_dl1abc",
                                         "termination_dl1abc")}))
    # noise, start offsets and frequency offsets (fixed seeds)
    rng = np.random.default_rng(20261030)
    level = 2000.0 / 8192.0          # openALE TX_AMPLITUDE 0.25 -> peak 2000 under noise
    for base in ("sound_twas_dg6fl", "call_amd", "handshake"):
        clean = dict((o[0], o) for o in out)[base]
        sig = clean[1] * level
        p_sig = float(np.mean(raw[base][0] ** 2)) * level ** 2 if base in raw else \
            float(np.mean(raw["call_dl1abc"][0] ** 2)) * level ** 2
        for snr in (10, 0, -4, -7):
            lead = int(rng.integers(0, 3136))
            x = np.concatenate([np.zeros(lead), sig])
            out.append((f"{base}_snr{snr:+d}".replace("+", "p").replace("-", "m"),
                        awgn(x, snr, rng, p_sig),
                        f"{base} with AWGN at {snr} dB in 3 kHz, {lead} samples later",
                        clean[3]))
    for hz in (125, -250, 60):
        sig = dict((o[0], o) for o in out)["sound_twas_dg6fl"][1]
        out.append((f"sound_twas_dg6fl_shift{hz:+d}".replace("+", "p").replace("-", "m"),
                    shift(sig, hz), f"sound_twas_dg6fl moved by {hz} Hz",
                    {"sound_twas_dg6fl": raw["sound_twas_dg6fl"][1]["words"]}))
    out.append(("noise_only", rng.normal(0.0, 3000.0, 20 * RATE), "20 s of white noise", {}))
    # random mixtures: scenario, SNR, start offset and audio offset drawn from a fixed seed
    pool = names + ["handshake"]
    clean = dict((o[0], o) for o in out)
    for i in range(12):
        base = pool[int(rng.integers(0, len(pool)))]
        snr = float(rng.uniform(-9.0, 6.0))
        hz = float(rng.choice([0.0, 125.0, -125.0, 250.0, -375.0]))
        lead = int(rng.integers(0, 3136))
        sig = clean[base][1] * level
        p_sig = float(np.mean(raw["call_dl1abc"][0] ** 2)) * level ** 2
        x = np.concatenate([rng.normal(0.0, 1.0, lead), sig])
        if hz:
            x = shift(x, hz)
        out.append((f"mix{i:02d}_{base}", awgn(x, snr, rng, p_sig),
                    f"{base}, AWGN {snr:.2f} dB in 3 kHz, {hz:+.0f} Hz, {lead} samples later",
                    clean[base][3]))
    return out


def write_tx_tables(raw_dir, out_dir):
    """tx_words.txt (word, tx49, 49 symbols) and tx_sequences.txt (scenario: words) for the
    C unit test of the encoder."""
    words, seqs = {}, []
    for fn in sorted(os.listdir(raw_dir)):
        if not fn.endswith(".json"):
            continue
        meta = json.load(open(os.path.join(raw_dir, fn)))
        seqs.append(meta["name"] + " " + " ".join("%06x" % w["word"] for w in meta["words"]))
        for w in meta["words"]:
            words[w["word"]] = (w["tx49"], w["symbols"])
    with open(os.path.join(out_dir, "tx_words.txt"), "w") as f:
        for w in sorted(words):
            tx49, sym = words[w]
            f.write("%06x %s %s\n" % (w, tx49, " ".join(str(v) for v in sym)))
    with open(os.path.join(out_dir, "tx_sequences.txt"), "w") as f:
        f.write("\n".join(seqs) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    manifest = []
    failures = 0
    with tempfile.TemporaryDirectory() as raw_dir:
        subprocess.run([os.path.join(a.ref, "openale_vectors"), raw_dir], check=True)
        write_tx_tables(raw_dir, a.out)
        for name, audio, desc, words in scenarios(raw_dir):
            wav = os.path.join(a.out, name + ".wav")
            write_wav(wav, to_int16(audio))
            ref = subprocess.run([os.path.join(a.ref, "kiwi_ref"), "-w", wav], check=True,
                                 capture_output=True, text=True).stdout
            ref = ANSI.sub("", ref)
            open(os.path.join(a.out, name + ".ref.txt"), "w").write(ref)
            core = subprocess.run([a.dump, "-w", "-r", wav], check=True, capture_output=True,
                                  text=True).stdout
            same = core == ref
            failures += not same
            log_lines = [ln for ln in ref.splitlines() if ln.startswith(">>>>")]
            manifest.append({
                "name": name, "description": desc, "words": words,
                "wav_sha256": hashlib.sha256(open(wav, "rb").read()).hexdigest(),
                "ref_lines": len(ref.splitlines()), "log": log_lines, "core_equal": same})
            print(f"{'OK  ' if same else 'DIFF'} {name}: {len(log_lines)} log lines")
    json.dump({"generator": "scripts/make_vectors.py", "pins": open(
        os.path.join(os.path.dirname(__file__), "..", "ref", "PINS")).read(),
        "vectors": manifest}, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
