#!/usr/bin/env bash
# vo_avfoundation's main-thread link (the marked "avf main-thread link"
# region, patch 0300): the VO thread never blocks on the main thread at
# teardown, waits for it at most a bounded time elsewhere, reads the layer
# geometry from a cache the main thread keeps, and nothing reaches a detached
# VO. The region is compiled VERBATIM from the final Apple series against real
# Core Animation layers and the real main queue, so this needs macOS. See
# test_avf_main_thread_link.m for the scenarios. Pass an already-patched mpv
# tree for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/video/out/vo_avfoundation.m" \
    "$workdir/avf_main_thread_link.inc" --start "avf main-thread link" \
    --require "avf_link_teardown" --require "avf_link_run_on_main"

# The file is built without ARC, as mpv builds it.
cc -O1 -g -fno-objc-arc -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    -framework Foundation -framework QuartzCore -framework AVFoundation \
    -framework CoreMedia -framework AppKit \
    -o "$workdir/test" "$root/scripts/test_avf_main_thread_link.m"

"$workdir/test"
