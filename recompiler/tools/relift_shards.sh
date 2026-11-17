#!/bin/bash
# Relift cohort shards (WSL/Linux) with the accepted toolchain, optionally with
# the poll instrumenter's --speed profile, and compile them for Windows.
#
#   relift_shards.sh OUT HEADS_MAP ALL_HEADS IMAGE IMAGE_START POLL MODE SHARD...
#
#   HEADS_MAP   lines "shard-NNNNN.o <hex head>" (nm of the cohort objects)
#   ALL_HEADS   every cohort head, one "0x..." per line (sha256 d7fa7793...)
#   IMAGE       flat executable image the cohort was lifted from
#   MODE        "default" (reproduces the cohort object) or "speed"
# OPT overrides the clang level. It defaults to -O2 for "default" and -O1 for
# "speed": with --static-allocas, clang 18 -O2 emits reloads of stack slots it
# never spilled in sub_7085f44c (shard 295), which crashes at guest PC
# 0x7085fc5c; -O1 output has no such reloads and runs.
# KEEP_IR=0 deletes raw/polled IR after compiling (a full batch is ~180 GB of IR).
# Lifter/semantics come from $TOOLCHAIN (lifter 3daa9c8d..., semantics 5a9c6807...).
set -euo pipefail
OUT=$1 MAP=$2 HEADS=$3 IMAGE=$4 START=$5 POLL=$6 MODE=$7; shift 7
: "${TOOLCHAIN:?set TOOLCHAIN to the accepted lifter directory}"
CLANG=${CLANG:-/usr/lib/llvm-18/bin/clang}
mkdir -p "$OUT"
one() {
  local s=${1%.o} d="$OUT/${1%.o}" flag=()
  [[ $MODE == speed ]] && flag=(--speed)
  # POLL_EXTRA adds instrumenter flags, e.g. --inline-poll (needs poll-instrument v19+).
  flag+=(${POLL_EXTRA:-})
  mkdir -p "$d"
  awk -v o="$1" '$1==o{print "0x"$2}' "$MAP" | sort -u > "$d/entries.txt"
  [[ -s $d/entries.txt ]] || { echo "$1: no heads in map" >&2; return 1; }
  "$TOOLCHAIN/remill-lift" --arch aarch64 --os linux --semantics_dir "$TOOLCHAIN" --bytes_file "$IMAGE" \
    --address "$START" --entry_addresses_file "$d/entries.txt" --trace_heads_file "$HEADS" \
    --guest_call_transfer=true --guest_call_transfer_stats_out "$d/lowering-stats.json" --ir_out "$d/raw.ll" > "$d/lift.log" 2>&1
  "$POLL" "${flag[@]}" "$d/raw.ll" "$d/polled.ll" > "$d/poll.log" 2>&1
  local opt=${OPT:-$([[ $MODE == speed ]] && echo -O1 || echo -O2)}
  "$CLANG" -target x86_64-w64-windows-gnu "$opt" -Wno-override-module -c "$d/polled.ll" -o "$d/$s.o" > "$d/compile.log" 2>&1
  local defs; defs=$(grep -c '^define .*@sub_' "$d/polled.ll")
  [[ ${KEEP_IR:-1} == 1 ]] || rm -f "$d/raw.ll" "$d/polled.ll"
  echo "$s $(wc -l < "$d/entries.txt") $defs $(sha256sum "$d/$s.o" | cut -c1-64)"
}
export -f one; export OUT MAP HEADS IMAGE START POLL MODE TOOLCHAIN CLANG OPT POLL_EXTRA
for s in "$@"; do echo "$s"; done | xargs -P "${JOBS:-12}" -I{} bash -c 'one {}'
