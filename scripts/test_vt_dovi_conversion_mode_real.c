// See test_vt_dovi_conversion_mode.sh: the "vt dovi conversion mode" region
// against the real libavutil AVOption lookup and real, opened decoder
// contexts. ff_videotoolbox_common_init() runs it for every VideoToolbox
// hwaccel (H.264, HEVC, VP9, AV1, ProRes, MPEG-1/2/4), by which time the
// decoder has initialized its private state. A decoder without a priv_class
// has no AVClass at the start of priv_data; there the test fills the first
// pointer with an address nothing maps, as initialized state would (ProRes's
// first member is a table of DSP function pointers). Reading it as an
// AVClass, as av_opt_get_int(avctx->priv_data, ...) does, faults.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/internal.h"
#include "libavutil/opt.h"

typedef struct VTContext {
    int dovi_mode;
} VTContext;

#include "vt_dovi_mode.inc"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static AVCodecContext *open_decoder(const char *name, int poison)
{
    const AVCodec *codec = avcodec_find_decoder_by_name(name);
    if (!codec) {
        printf("FAIL: decoder %s not built\n", name);
        failures++;
        return NULL;
    }
    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx || avcodec_open2(avctx, codec, NULL) < 0) {
        printf("FAIL: cannot open %s\n", name);
        failures++;
        avcodec_free_context(&avctx);
        return NULL;
    }
    if (poison) {
        if (codec->priv_class) {
            printf("FAIL: %s has a priv_class; it cannot model the bug\n", name);
            failures++;
        } else {
            uintptr_t wild = (uintptr_t)0x10;  // the zero page: never mapped
            memcpy(avctx->priv_data, &wild, sizeof(wild));
        }
    }
    return avctx;
}

static int resolve(AVCodecContext *avctx)
{
    VTContext vt = { .dovi_mode = -100 };
    videotoolbox_dovi_resolve_mode(avctx, &vt);
    return vt.dovi_mode;
}

int main(void)
{
    unsetenv("PLUET_DV_CONVERSION_MODE");
    unsetenv("PLEZY_DV_CONVERSION_MODE");

    // HEVC: its priv_class carries the option.
    AVCodecContext *hevc = open_decoder("hevc", 0);
    if (hevc) {
        CHECK(resolve(hevc) == PLEZY_DOVI_AUTO, "hevc default: %d", resolve(hevc));
        CHECK(av_opt_set(hevc, "dv_conversion_mode", "hevc_strip", AV_OPT_SEARCH_CHILDREN) >= 0,
              "hevc: the option cannot be set through the codec context");
        CHECK(resolve(hevc) == PLEZY_DOVI_HEVC_STRIP, "hevc set: %d", resolve(hevc));
        av_opt_set(hevc, "dv_conversion_mode", "unset", AV_OPT_SEARCH_CHILDREN);
        setenv("PLUET_DV_CONVERSION_MODE", "dv81", 1);
        CHECK(resolve(hevc) == PLEZY_DOVI_DV81, "hevc env: %d", resolve(hevc));
        unsetenv("PLUET_DV_CONVERSION_MODE");
        avcodec_free_context(&hevc);
    }

    // H.264 has a priv_class without the option; VP9 and ProRes have none,
    // and their priv_data must not be read. All resolve to auto, even with
    // the environment set (it only ever applied to HEVC).
    setenv("PLUET_DV_CONVERSION_MODE", "disabled", 1);
    static const struct { const char *name; int poison; } others[] = {
        {"h264", 0}, {"vp9", 1}, {"prores", 1},
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        AVCodecContext *avctx = open_decoder(others[i].name, others[i].poison);
        if (!avctx)
            continue;
        int mode = resolve(avctx); // faults on a priv_data AVClass read
        CHECK(mode == PLEZY_DOVI_AUTO, "%s: mode %d", others[i].name, mode);
        if (others[i].poison) // restore what the decoder's close may use
            memset(avctx->priv_data, 0, sizeof(uintptr_t));
        avcodec_free_context(&avctx);
    }
    unsetenv("PLUET_DV_CONVERSION_MODE");

    printf(failures ? "%d FAILED\n" : "vt dovi conversion mode (real libavcodec): all checks passed\n",
           failures);
    return failures ? 1 : 0;
}
