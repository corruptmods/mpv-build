#!/usr/bin/env bash
# The HEVC decoder's software tables under a hwaccel (patches ffmpeg 0030
# and 0302). 0030 leaves a hwaccel frame's motion vector field NULL; 0302
# refuses software slice decoding of such a frame instead of dereferencing
# it. Nothing here can decode a stream, so this checks structure, on the
# final Apple series:
#   - decode_slice_data() checks cur_frame->tab_mvf after the hwaccel
#     return and before its first software-path statement;
#   - every function in libavcodec/hevc that reads a frame's tab_mvf is on
#     the reviewed list below, all of them reached only through that
#     software path (or NULL-checking, like the collocated reference read).
#     A pin bump that adds a reader fails here until it is reviewed.
# Pass an already-patched FFmpeg tree for offline use; otherwise fetch and
# patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg apple "$source_dir")
fi

python3 "$root/scripts/extract.py" symbol "$source_dir/libavcodec/hevc/hevcdec.c" \
    "$workdir/decode_slice_data.c" --fn decode_slice_data --return int

python3 -I - "$workdir/decode_slice_data.c" "$source_dir/libavcodec/hevc" <<'PY'
import re, sys
from pathlib import Path

fn = Path(sys.argv[1]).read_text().splitlines()
hevc = Path(sys.argv[2])
failures = []

def first(pred, start=0):
    for i in range(start, len(fn)):
        if pred(fn[i]):
            return i
    return -1

hw = first(lambda l: "FF_HW_CALL(s->avctx, decode_slice" in l)
guard = first(lambda l: "!s->cur_frame->tab_mvf" in l)
# The first statement of the software path: the SCC refusal.
sw = first(lambda l: "AV_PROFILE_HEVC_SCC" in l)
if hw < 0 or guard < 0 or sw < 0:
    failures.append(f"decode_slice_data: hwaccel return {hw}, guard {guard}, software path {sw}")
elif not (hw < guard < sw):
    failures.append(f"decode_slice_data: guard at {guard} is not between the hwaccel "
                    f"return ({hw}) and the software path ({sw})")
elif "return AVERROR_BUG" not in "\n".join(fn[guard:sw]):
    failures.append("decode_slice_data: the guard does not return an error")

# Reviewed users of a frame's motion vector field, by file and the function
# the scan attributes them to (a macro defined between functions counts for
# the one above it). Software slice decoding, reached only past the guard:
# prediction units, intra prediction, MV derivation, deblocking strengths.
# The collocated reference read (temporal_luma_motion_vector and the
# TAB_MVF macro under derive_temporal_colocated_mvs) checks for NULL itself.
# Bookkeeping that copies, frees or allocates the reference: hevc_ref_frame,
# ff_hevc_unref_frame, alloc_frame. And the guard itself, decode_slice_data.
reviewed = {
    ("filter.c", "ff_hevc_deblocking_boundary_strengths"),
    ("mvs.c", "derive_temporal_colocated_mvs"),
    ("mvs.c", "temporal_luma_motion_vector"),
    ("mvs.c", "derive_spatial_merge_candidates"),
    ("mvs.c", "dist_scale"),
    ("mvs.c", "mv_mp_mode_mx"),
    ("mvs.c", "mv_mp_mode_mx_lt"),
    ("mvs.c", "ff_hevc_luma_mv_mvp_mode"),
    ("hevcdec.c", "hls_prediction_unit"),
    ("hevcdec.c", "luma_intra_pred_mode"),
    ("hevcdec.c", "intra_prediction_unit_default_value"),
    ("hevcdec.c", "decode_slice_data"),
    ("hevcdec.c", "hevc_ref_frame"),
    ("pred_template.c", "FUNC"),
    ("refs.c", "ff_hevc_unref_frame"),
    ("refs.c", "alloc_frame"),
}
func_re = re.compile(r"^(?:static\s+)?(?:av_always_inline\s+|inline\s+)?[\w\s\*]+?\b(\w+)\s*\(")
found = set()
for path in sorted(hevc.glob("*.c")):
    current = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = func_re.match(line)
        if m and not line.rstrip().endswith(";") and not line.startswith((" ", "\t", "#")):
            current = m.group(1)
        if re.search(r"\btab_mvf\b", line) and "tab_mvf_pool" not in line:
            found.add((path.name, current))
new = sorted(found - reviewed)
if new:
    failures.append("unreviewed readers of tab_mvf: " +
                    ", ".join(f"{f}:{fn_}" for f, fn_ in new))
gone = sorted(reviewed - found)
if gone:
    print("note: reviewed readers no longer present: " +
          ", ".join(f"{f}:{fn_}" for f, fn_ in gone))

for f in failures:
    print("FAIL:", f)
if failures:
    sys.exit(1)
print(f"hevc hwaccel tables: guard in place, {len(found)} tab_mvf readers all reviewed")
PY
