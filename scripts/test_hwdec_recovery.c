// See test_hwdec_recovery.sh.
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>

#define MP_WARN(...) ((void)0)

struct hwdec_opts {
    int software_fallback;
    int recover_keyframes;
    int recover_max_frames;
};
typedef struct lavc_ctx {
    struct hwdec_opts *hwdec_opts;
    bool use_hwdec, hwdec_notified, hwdec_failed;
    int hwdec_fail_count, hwdec_err_keyframes;
} vd_ffmpeg_ctx;
struct mp_filter { void *priv; };

#include "handle_err.inc"

static const struct hwdec_opts defaults = {
#include "defaults.inc"
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Feed errored frames, with a keyframe every `gop` frames (send_packet()'s
// count: a keyframe sent while a burst is active); returns how many errored
// frames it took to fall back, or -1 if it never did within `limit`.
static int frames_to_fallback(struct hwdec_opts opts, bool notified, int gop, int limit)
{
    vd_ffmpeg_ctx ctx = { .hwdec_opts = &opts, .use_hwdec = true, .hwdec_notified = notified };
    struct mp_filter vd = { .priv = &ctx };
    for (int n = 1; n <= limit; n++) {
        if (gop && ctx.hwdec_fail_count > 0 && n % gop == 0)
            ctx.hwdec_err_keyframes++;
        handle_err(&vd);
        if (ctx.hwdec_failed)
            return n;
    }
    return -1;
}

int main(void)
{
    CHECK(defaults.software_fallback == 3 && defaults.recover_keyframes == 2 &&
          defaults.recover_max_frames == 240,
          "defaults %d/%d/%d are not 0025's 3/2/240", defaults.software_fallback,
          defaults.recover_keyframes, defaults.recover_max_frames);

    // Probing (no frame delivered yet): --hwdec-software-fallback errors.
    CHECK(frames_to_fallback(defaults, false, 0, 1000) == 3, "probing");

    // Delivered frames, keyframe every 24: two keyframes into the burst,
    // as 0025 behaves today.
    CHECK(frames_to_fallback(defaults, true, 24, 1000) == 48,
          "two keyframes: %d", frames_to_fallback(defaults, true, 24, 1000));
    // No keyframes: the 240-frame ceiling.
    CHECK(frames_to_fallback(defaults, true, 0, 1000) == 240, "ceiling");

    // --hwdec-recover-keyframes=0: upstream mpv, at the fallback count.
    struct hwdec_opts up = defaults;
    up.recover_keyframes = 0;
    CHECK(frames_to_fallback(up, true, 24, 1000) == 3, "recover-keyframes=0");

    // Other settings.
    struct hwdec_opts one = defaults;
    one.recover_keyframes = 1;
    CHECK(frames_to_fallback(one, true, 24, 1000) == 24, "one keyframe");
    struct hwdec_opts low = defaults;
    low.recover_max_frames = 30;
    CHECK(frames_to_fallback(low, true, 0, 1000) == 30, "max-frames=30");

    // --hwdec-software-fallback=no still never falls back.
    struct hwdec_opts never = defaults;
    never.software_fallback = INT_MAX;
    CHECK(frames_to_fallback(never, true, 24, 5000) == -1, "software-fallback=no");

    printf(failures ? "%d FAILED\n" : "hwdec recovery: all checks passed\n", failures);
    return failures ? 1 : 0;
}
