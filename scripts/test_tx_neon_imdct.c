// See test_tx_neon_imdct.sh. The NEON inverse MDCT (FFmpeg patch 0042) and
// FFmpeg's C transform, both through the public av_tx API, on the same
// random input: the C one is what av_tx picks with the CPU flags forced to
// none. Strides of 1 float use the contiguous pre-rotation checkasm covers;
// larger strides use the gather path it does not.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/cpu.h"
#include "libavutil/mem.h"
#include "libavutil/tx.h"

static int failures;

static unsigned rng = 0x2545F491u;
static float rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / (1 << 24) - 0.5f;
}

static int run(int len, int stride_floats, int neon, const float *in, float *out)
{
    AVTXContext *tx = NULL;
    av_tx_fn fn;
    const float scale = 1.0f / len;
    av_force_cpu_flags(neon ? -1 : 0);
    int err = av_tx_init(&tx, &fn, AV_TX_FLOAT_MDCT, 1, len, &scale, 0);
    av_force_cpu_flags(-1);
    if (err < 0) {
        printf("FAIL: av_tx_init(len %d) = %d\n", len, err);
        failures++;
        return 0;
    }
    fn(tx, out, (void *)in, stride_floats * sizeof(float));
    av_tx_uninit(&tx);
    return 1;
}

int main(void)
{
    if (!(av_get_cpu_flags() & AV_CPU_FLAG_NEON)) {
        printf("FAIL: no NEON at run time\n");
        return 1;
    }
    // Frame sizes the float AAC, AC-3/E-AC-3, Opus and Vorbis decoders use,
    // and powers of two around them. av_tx's inverse MDCT reads len
    // coefficients, stride bytes apart, and writes len samples (the
    // half-length output codecs use; tx.h).
    static const int lens[] = {16, 32, 64, 120, 128, 240, 256, 480, 512, 960, 1024, 1920, 2048, 4096};
    static const int strides[] = {1, 2, 4, 8};
    int checked = 0;
    double worst = 0;
    // Transforms whose NEON output differs from C in any bit, per stride:
    // with none, av_tx picked the C codelet both times and nothing was
    // tested.
    int differs[4] = {0};
    for (size_t l = 0; l < sizeof(lens) / sizeof(lens[0]); l++) {
        int len = lens[l];
        for (size_t s = 0; s < sizeof(strides) / sizeof(strides[0]); s++) {
            int stride = strides[s];
            size_t n_in = (size_t)len * stride;
            float *in = av_malloc(n_in * sizeof(float));
            float *ref = av_malloc(len * sizeof(float));
            float *neon = av_malloc(len * sizeof(float));
            for (int rep = 0; rep < 4; rep++) {
                for (size_t i = 0; i < n_in; i++)
                    in[i] = rnd();
                memset(ref, 0, len * sizeof(float));
                memset(neon, 0x55, len * sizeof(float));
                if (!run(len, stride, 0, in, ref) || !run(len, stride, 1, in, neon))
                    continue;
                if (memcmp(ref, neon, len * sizeof(float)))
                    differs[s]++;
                for (int i = 0; i < len; i++) {
                    double d = fabs((double)ref[i] - neon[i]);
                    // checkasm's av_tx tolerance, scaled to the input's
                    // magnitude (it uses inputs in [0, 1)).
                    if (!(d <= 5e-4) || isnan(neon[i])) {
                        printf("FAIL: len %d stride %d sample %d: C %.9g NEON %.9g\n",
                               len, stride, i, ref[i], neon[i]);
                        failures++;
                        break;
                    }
                    if (d > worst)
                        worst = d;
                }
                checked++;
            }
            av_free(in);
            av_free(ref);
            av_free(neon);
        }
    }
    for (int s = 0; s < 4; s++) {
        if (!differs[s]) {
            printf("FAIL: stride %d: NEON output always bit-identical to C; "
                   "the NEON codelet was not selected\n", strides[s]);
            failures++;
        }
    }
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("NEON inverse MDCT matches C: %d transforms over 14 lengths and strides 1, 2, 4, 8 "
           "(max |diff| %.3g)\n", checked, worst);
    return 0;
}
