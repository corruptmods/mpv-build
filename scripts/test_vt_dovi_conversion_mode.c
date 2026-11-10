// See test_vt_dovi_conversion_mode.sh.
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AV_LOG_VERBOSE 40

// Stub FFmpeg types, the fields the region reads only.
typedef struct VTContext {
    int dovi_mode;
} VTContext;

typedef struct AVCodecInternal {
    void *hwaccel_priv_data;
} AVCodecInternal;

enum AVCodecID { AV_CODEC_ID_H264 = 27, AV_CODEC_ID_HEVC = 173 };
#define AV_OPT_SEARCH_CHILDREN (1 << 0)

typedef struct AVCodecContext {
    void *priv_data;
    AVCodecInternal *internal;
    enum AVCodecID codec_id;
} AVCodecContext;

// The decoder's option, as av_opt_get_int() would find it: absent (a
// decoder without the option), or set to a value.
static int opt_present;
static int64_t opt_value;
static int opt_lookups_bad;

// Must be asked through the codec context, searching its children: that is
// what keeps libavutil from reading a priv_data without an AVClass.
static int av_opt_get_int(void *obj, const char *name, int flags, int64_t *out)
{
    AVCodecContext *avctx = obj;
    if (avctx->priv_data == obj || !(flags & AV_OPT_SEARCH_CHILDREN) ||
        avctx->codec_id != AV_CODEC_ID_HEVC)
        opt_lookups_bad++;
    if (!opt_present || strcmp(name, "dv_conversion_mode"))
        return -1;
    *out = opt_value;
    return 0;
}

static void av_log(void *ctx, int level, const char *fmt, ...)
{
    (void)ctx; (void)level; (void)fmt;
}

#include "vt_dovi_mode.inc"

// The option's lines from hevcdec.c, against an AVOption with the same
// positional layout (name, help, offset, type, default, min, max, flags, unit).
enum { AV_OPT_TYPE_INT = 1, AV_OPT_TYPE_CONST = 2 };
typedef struct AVOption {
    const char *name, *help;
    int offset, type;
    union { int64_t i64; } default_val;
    double min, max;
    int flags;
    const char *unit;
} AVOption;
struct HEVCContext { int dv_conversion_mode; };
#define OFFSET(x) offsetof(struct HEVCContext, x)
#define PAR 0
static const AVOption options[] = {
#include "hevc_dv_option.inc"
    { 0 },
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static int resolve(int present, int64_t value, const char *pluet, const char *plezy)
{
    VTContext vt = { .dovi_mode = -100 };
    AVCodecInternal in = { .hwaccel_priv_data = &vt };
    int priv;
    AVCodecContext avctx = { .priv_data = &priv, .internal = &in,
                             .codec_id = AV_CODEC_ID_HEVC };

    opt_present = present;
    opt_value = value;
    if (pluet) setenv("PLUET_DV_CONVERSION_MODE", pluet, 1);
    else unsetenv("PLUET_DV_CONVERSION_MODE");
    if (plezy) setenv("PLEZY_DV_CONVERSION_MODE", plezy, 1);
    else unsetenv("PLEZY_DV_CONVERSION_MODE");
    videotoolbox_dovi_resolve_mode(&avctx, &vt);
    // Read once: the environment changing later does not move it.
    setenv("PLUET_DV_CONVERSION_MODE", "disabled", 1);
    int got = videotoolbox_dovi_conversion_mode(&avctx);
    CHECK(got == vt.dovi_mode, "conversion_mode() does not return the resolved mode");
    return got;
}

static int64_t option_const(const char *name)
{
    for (const AVOption *o = options; o->name; o++) {
        if (o->type == AV_OPT_TYPE_CONST && !strcmp(o->name, name) &&
            o->unit && !strcmp(o->unit, "dv_conversion_mode"))
            return o->default_val.i64;
    }
    return INT64_MIN;
}

int main(void)
{
    // The option table: every named value maps onto the enum the
    // VideoToolbox code switches on, and the default leaves it unset.
    const AVOption *opt = &options[0];
    CHECK(!strcmp(opt->name, "dv_conversion_mode") && opt->type == AV_OPT_TYPE_INT,
          "first extracted line is not the option");
    CHECK(opt->default_val.i64 == -1 && opt->min == -1 && opt->max == PLEZY_DOVI_HEVC_STRIP,
          "default %lld range %g..%g", (long long)opt->default_val.i64, opt->min, opt->max);
    CHECK(option_const("unset") == -1, "unset");
    CHECK(option_const("auto") == PLEZY_DOVI_AUTO, "auto");
    CHECK(option_const("disabled") == PLEZY_DOVI_DISABLED, "disabled");
    CHECK(option_const("dv81") == PLEZY_DOVI_DV81, "dv81");
    CHECK(option_const("hevc_strip") == PLEZY_DOVI_HEVC_STRIP, "hevc_strip");

    // Nothing set: auto, as before.
    CHECK(resolve(1, -1, NULL, NULL) == PLEZY_DOVI_AUTO, "default");
    CHECK(resolve(0, 0, NULL, NULL) == PLEZY_DOVI_AUTO, "decoder without the option");

    // The option wins over either variable.
    CHECK(resolve(1, PLEZY_DOVI_HEVC_STRIP, "dv81", "disabled") == PLEZY_DOVI_HEVC_STRIP,
          "option over environment");
    CHECK(resolve(1, PLEZY_DOVI_AUTO, NULL, "disabled") == PLEZY_DOVI_AUTO,
          "an explicit auto is not unset");

    // Unset: the old variable keeps working, the new one comes first.
    CHECK(resolve(1, -1, NULL, "disabled") == PLEZY_DOVI_DISABLED, "PLEZY alone");
    CHECK(resolve(1, -1, NULL, "native") == PLEZY_DOVI_DISABLED, "PLEZY alias");
    CHECK(resolve(1, -1, "hevc_strip", NULL) == PLEZY_DOVI_HEVC_STRIP, "PLUET alone");
    CHECK(resolve(1, -1, "p7-to-p8", "disabled") == PLEZY_DOVI_DV81, "PLUET over PLEZY");
    CHECK(resolve(1, -1, "", "disabled") == PLEZY_DOVI_DISABLED, "empty PLUET falls through");
    CHECK(resolve(0, 0, "hevc", NULL) == PLEZY_DOVI_HEVC_STRIP, "no option, environment");
    CHECK(resolve(1, -1, "bogus", NULL) == PLEZY_DOVI_AUTO, "unknown value is auto");

    // Not HEVC: no lookup at all, and auto whatever is set.
    {
        VTContext vt = { .dovi_mode = -100 };
        AVCodecInternal in = { .hwaccel_priv_data = &vt };
        int priv;
        AVCodecContext h264 = { .priv_data = &priv, .internal = &in,
                                .codec_id = AV_CODEC_ID_H264 };
        opt_present = 1;
        opt_value = PLEZY_DOVI_HEVC_STRIP;
        setenv("PLUET_DV_CONVERSION_MODE", "disabled", 1);
        videotoolbox_dovi_resolve_mode(&h264, &vt);
        CHECK(vt.dovi_mode == PLEZY_DOVI_AUTO, "non-HEVC mode %d", vt.dovi_mode);
    }
    CHECK(opt_lookups_bad == 0, "%d option lookups went to priv_data, without "
          "AV_OPT_SEARCH_CHILDREN, or for a codec other than HEVC", opt_lookups_bad);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("vt dovi conversion mode: all checks passed\n");
    return 0;
}
