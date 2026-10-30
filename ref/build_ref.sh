#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the two reference programs of the differential tests (bench tools, never shipped):
#   ref/build/kiwi_ref         KiwiSDR extensions/ALE_2G receiver (unmodified) + kiwi_driver.cpp
#   ref/build/openale_vectors  openALE word/sequence/tone encoder (unmodified) + openale_vectors.cpp
# Sources are checked out at the commits of ref/PINS from $ALE_REF_MIRROR/<name>.git (default:
# the intended wave-owl mirrors), or used as they are from $ALE_REF_SRC/<name> when set.
# Compiler flags equal the core's (no fast math, no FP contraction) so the float sliding DFT
# rounds identically; KiwiSDR itself builds with -Ofast (README quirk Q1).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/build"
SRC="${ALE_REF_SRC:-$OUT/src}"
MIRROR="${ALE_REF_MIRROR:-https://github.com/wave-owl}"
CXX="${CXX:-c++}"
FLAGS="-O2 -ffp-contract=off -fno-fast-math -w"
mkdir -p "$OUT"

fetch() { # name commit
    if [ -n "${ALE_REF_SRC:-}" ]; then return; fi
    if [ ! -d "$SRC/$1/.git" ]; then
        git clone -q "$MIRROR/$1.git" "$SRC/$1"
    fi
    git -C "$SRC/$1" -c advice.detachedHead=false checkout -q "$2"
}

grep -v '^#' "$HERE/PINS" | while read -r name _up _mirror commit _lic; do
    [ -n "$name" ] && fetch "$name" "$commit"
done

FFTW_CFLAGS=$(pkg-config --cflags fftw3f 2>/dev/null || echo "-I/opt/homebrew/include -I/usr/local/include")
FFTW_LIBS=$(pkg-config --libs fftw3f 2>/dev/null || echo "-L/opt/homebrew/lib -L/usr/local/lib -lfftw3f")
K="$SRC/KiwiSDR"
A="$K/extensions/ALE_2G"
KI="-I$A/standalone_test -I$A -I$K -I$K/extensions/DRM/dream/resample $FFTW_CFLAGS"
$CXX $FLAGS -include "$HERE/kiwi_shim.h" $KI -Dmain=kiwi_standalone_main -c "$A/decode_ff_impl.cpp" -o "$OUT/decode_ff_impl.o"
$CXX $FLAGS -include "$HERE/kiwi_shim.h" $KI -DSTANDALONE_TEST -c "$HERE/kiwi_driver.cpp" -o "$OUT/kiwi_driver.o"
$CXX "$OUT/decode_ff_impl.o" "$OUT/kiwi_driver.o" $FFTW_LIBS -lm -o "$OUT/kiwi_ref"

O="$SRC/openALE"
OSRC="$O/src/Word/ale_word.cpp $O/src/Word/address_encoder.cpp $O/src/Word/ale_sequence.cpp
      $O/src/FEC/golay.cpp $O/src/FEC/ale_fec_codec.cpp $O/src/FEC/word_interleaver.cpp
      $O/src/Codec/ale_encoder.cpp $O/src/FSK/tone_generator.cpp
      $O/src/Protocol/Message/ale_orderwire_protocols.cpp $O/src/LQA/lqa_metrics.cpp
      $O/src/LQA/lqa_report.cpp $O/src/LQA/lqa_database.cpp"
$CXX -std=c++17 $FLAGS -I"$O/include" $OSRC "$HERE/openale_vectors.cpp" -o "$OUT/openale_vectors"
echo "reference programs: $OUT/kiwi_ref $OUT/openale_vectors"
