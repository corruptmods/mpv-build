#!/usr/bin/env bash
# The Dolby Vision RPU check VideoToolbox runs before libdovi (patch ffmpeg
# 0301, the "dovi p7 rpu check" region of dovi_convert.c), compiled VERBATIM
# from the final Apple series. It must take every RPU in libdovi's own test
# assets, at the dovi_tool commit versions.json records for the apple
# libdovi prebuilt, and refuse damaged and truncated ones without calling in.
# Usage: test_dovi_rpu_check.sh [patched-ffmpeg-tree [dovi_tool-checkout]]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
dovi_dir="${2:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg apple "$source_dir")
fi

read -r dovi_url dovi_ref dovi_commit < <(python3 - "$root/versions.json" <<'PY'
import json, sys
up = json.load(open(sys.argv[1]))["components"]["libdovi"]["upstream"]
print(up["url"], up["ref"], up["commit"])
PY
)
if [[ -z "$dovi_dir" ]]; then
    dovi_dir="$workdir/dovi_tool"
    git clone -q --depth 1 --branch "$dovi_ref" "$dovi_url" "$dovi_dir"
fi
head="$(git -C "$dovi_dir" rev-parse HEAD)"
if [[ "$head" != "$dovi_commit" ]]; then
    echo "dovi_tool checkout is at $head, versions.json records $dovi_commit" >&2
    exit 1
fi

python3 "$root/scripts/extract.py" region "$source_dir/libavcodec/dovi_convert.c" \
    "$workdir/dovi_rpu_check.inc" --start "dovi p7 rpu check" \
    --require "ff_dovi_p7_rpu_check"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_dovi_rpu_check.c"
"$workdir/test" "$dovi_dir"/assets/tests/*.bin
