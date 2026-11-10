#!/usr/bin/env bash
# The hardware decoder's mid-stream error recovery (patch 0025, made
# configurable by 0306): handle_err() in video/decode/vd_lavc.c, compiled
# VERBATIM from the final Apple series, against the option defaults read from
# the same file. With the defaults a hwdec that has delivered frames gets two
# keyframes, or 240 errored frames, before the software fallback; while
# probing it falls back at --hwdec-software-fallback; --hwdec-recover-keyframes=0
# is upstream mpv's behaviour. Pass an already-patched mpv tree for offline
# use; otherwise fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

E="$root/scripts/extract.py"
vd="$source_dir/video/decode/vd_lavc.c"
python3 "$E" symbol "$vd" "$workdir/handle_err.inc" --fn handle_err --return void
# The shipped defaults, as initializers the test reuses.
python3 "$E" range "$vd" "$workdir/defaults.inc" \
    --first '        .recover_keyframes = ' --last '        .recover_max_frames = ' --last-inclusive
python3 "$E" range "$vd" "$workdir/defaults.inc" --append \
    --first '        .software_fallback = 3,' --last '        .software_fallback = 3,' --last-inclusive

cc -O1 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_hwdec_recovery.c"
"$workdir/test"
