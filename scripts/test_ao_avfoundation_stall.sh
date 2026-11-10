#!/usr/bin/env bash
# The compressed (AVPlayer) sink's mid-stream stall watchdog (patch mpv 0302,
# the "avp stall watchdog" region of audio/out/ao_avfoundation.m), compiled
# VERBATIM from the final Apple series and run on a fake clock: one restart
# for a playhead that stands still with stream ahead of it, the PCM fallback
# if it stays still, and nothing while the transport is not expected to move.
# The driver around it is covered by test_ao_avfoundation_fallback.sh. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/audio/out/ao_avfoundation.m" \
    "$workdir/avp_stall.inc" --start "avp stall watchdog" --require "avp_stall_check"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_ao_avfoundation_stall.c"
"$workdir/test"
