#!/usr/bin/env bash
# osd_get_sub_schedule() (sub/osd.c, patches 0011 and 0304) must not hold
# osd->lock while its dec_sub queries wait for a subtitle render, and must
# still keep the dec_subs from being swapped or freed under it. Compiles
# wait_sub_render_done(), osd_set_sub() and osd_get_sub_schedule() VERBATIM
# from the final Apple series against a fake osd_state and fake dec_sub
# queries that block like a render in progress, and stresses them from
# several threads. Pass an already-patched mpv tree for offline use;
# otherwise fetch and patch the pinned source.
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
osd="$source_dir/sub/osd.c"
python3 "$E" symbol "$osd" "$workdir/osd_schedule.inc" --fn wait_sub_render_done --return void
python3 "$E" symbol "$osd" "$workdir/osd_schedule.inc" --append --fn osd_set_sub --return void
python3 "$E" symbol "$osd" "$workdir/osd_schedule.inc" --append --fn osd_get_sub_schedule --return void

cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -pthread -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_osd_sub_schedule_lock.c" -lm
"$workdir/test"
