#!/usr/bin/env bash
# --sub-ass-render-threads (patch 0305): get_renderer() in sub/sd_ass.c,
# compiled VERBATIM from the final Apple series against fakes, hands libass's
# ass_set_threads() 0 (auto, 0205's behaviour) by default and the option's
# value otherwise; the option's table entry is checked too. Pass an
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

E="$root/scripts/extract.py"
python3 "$E" symbol "$source_dir/sub/sd_ass.c" "$workdir/get_renderer.inc" \
    --fn get_renderer --return "struct ass_render_state *"
python3 "$E" range "$source_dir/options/options.c" "$workdir/option.inc" \
    --first '{"sub-ass-render-threads"' --last 'M_RANGE(1, 64), .flags = UPDATE_SUB_HARD}' \
    --last-inclusive
grep -q '\.sub_ass_render_threads' "$source_dir/options/options.c" && {
    echo "unexpected explicit default for sub_ass_render_threads; check the test" >&2; exit 1; }

cc -O1 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_sub_ass_render_threads.c"
"$workdir/test"
