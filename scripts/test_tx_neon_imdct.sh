#!/usr/bin/env bash
# The AArch64 NEON inverse MDCT (patch ffmpeg 0042) against FFmpeg's C
# transform, on an aarch64 host (Apple Silicon macOS, or arm64 Linux):
#   - FFmpeg's own checkasm av_tx test, which compares float_imdct with the
#     C reference at its lengths, contiguous input (stride sizeof(float));
#   - test_tx_neon_imdct.c, which covers the strided pre-rotation path
#     checkasm does not reach: input strides 2, 4 and 8 floats (Opus
#     interleaves its short blocks), at codec lengths, on random input.
# Configures and builds a minimal libavutil and checkasm from the final apple
# series (a few minutes). Elsewhere there is no NEON to test and it says so.
# Pass an already-patched FFmpeg tree for offline use; otherwise fetch and
# patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

case "$(uname -m)" in
    arm64|aarch64) ;;
    *) echo "test_tx_neon_imdct: $(uname -m) has no NEON; nothing to test"; exit 0 ;;
esac

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg apple "$source_dir")
fi
source_dir="$(cd "$source_dir" && pwd)"

build="$workdir/build"
mkdir -p "$build"
(cd "$build" && "$source_dir/configure" --disable-everything --disable-programs --disable-doc \
    --disable-network --disable-autodetect --disable-avcodec --disable-avformat \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --enable-neon > configure.log 2>&1) || { tail -20 "$build/configure.log"; exit 1; }
grep -q '^#define HAVE_NEON 1' "$build/config.h" || { echo "configure did not enable NEON" >&2; exit 1; }
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make -C "$build" -j"$jobs" libavutil/libavutil.a tests/checkasm/checkasm > "$build/make.log" 2>&1 \
    || { tail -30 "$build/make.log"; exit 1; }

out="$("$build/tests/checkasm/checkasm" --test=av_tx 2>&1)" || { echo "$out"; exit 1; }
echo "$out" | grep -q 'av_tx.float_imdct.*\[OK\]' \
    || { echo "$out"; echo "checkasm did not test the NEON float_imdct" >&2; exit 1; }
echo "checkasm av_tx: $(echo "$out" | tail -n 1)"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$build" -I"$source_dir" \
    -o "$workdir/strided" "$root/scripts/test_tx_neon_imdct.c" "$build/libavutil/libavutil.a" \
    -lm -lpthread
"$workdir/strided"
