#!/usr/bin/env bash
# How VideoToolbox picks its Dolby Vision profile 7 handling (patch ffmpeg
# 0300): the HEVC decoder's dv_conversion_mode AVOption, or with it unset the
# environment, PLUET_DV_CONVERSION_MODE before PLEZY_DV_CONVERSION_MODE, read
# once at hwaccel init. The "vt dovi conversion mode" region of videotoolbox.c
# and the option's lines in hevcdec.c are compiled VERBATIM from the final
# Apple series against stub FFmpeg types, and the region again against a real
# minimal libavcodec: decoders whose priv_data has no AVClass (VP9, ProRes)
# must never have it read as one. Pass an already-patched FFmpeg tree
# for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/libavcodec/videotoolbox.c" \
    "$workdir/vt_dovi_mode.inc" --start "vt dovi conversion mode" \
    --require "videotoolbox_dovi_resolve_mode"
python3 "$root/scripts/extract.py" range "$source_dir/libavcodec/hevc/hevcdec.c" \
    "$workdir/hevc_dv_option.inc" --first '{ "dv_conversion_mode",' --last '{ NULL },'

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_vt_dovi_conversion_mode.c"
"$workdir/test"

# The same region against the real libavutil option lookup and real decoder
# contexts: every VideoToolbox hwaccel runs this resolution at init, and VP9's
# and ProRes's priv_data do not start with an AVClass. A minimal host
# libavcodec with those decoders is built from the same tree (about 2 min).
source_dir="$(cd "$source_dir" && pwd)"
build="$workdir/build"
mkdir -p "$build"
(cd "$build" && "$source_dir/configure" --disable-everything --disable-programs --disable-doc \
    --disable-network --disable-autodetect --disable-avformat --disable-avdevice \
    --disable-avfilter --disable-swscale --disable-swresample \
    --enable-decoder=hevc,vp9,prores,h264 > configure.log 2>&1) \
    || { tail -20 "$build/configure.log"; exit 1; }
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make -C "$build" -j"$jobs" libavutil/libavutil.a libavcodec/libavcodec.a > "$build/make.log" 2>&1 \
    || { tail -30 "$build/make.log"; exit 1; }
cc -O1 -g -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" -I"$build" -I"$source_dir" \
    -o "$workdir/test_real" "$root/scripts/test_vt_dovi_conversion_mode_real.c" \
    "$build/libavcodec/libavcodec.a" "$build/libavutil/libavutil.a" -lm -lpthread
"$workdir/test_real"
