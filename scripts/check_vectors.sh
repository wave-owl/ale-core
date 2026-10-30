#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Decode every vectors/*.wav with ale_dump -w -r and compare with the reference golden.
set -u
DUMP="$1"
DIR="${2:-vectors}"
fail=0
n=0
for wav in "$DIR"/*.wav; do
    n=$((n + 1))
    if ! "$DUMP" -w -r "$wav" | cmp -s - "${wav%.wav}.ref.txt"; then
        echo "DIFF $wav"
        fail=$((fail + 1))
    fi
done
echo "vectors: $((n - fail))/$n equal to the reference"
[ "$fail" -eq 0 ]
