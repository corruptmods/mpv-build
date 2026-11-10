// See test_sub_ass_render_threads.sh.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MP_VERBOSE(...) ((void)0)
#define MP_WARN(...) ((void)0)

typedef struct ASS_Renderer { int dummy; } ASS_Renderer;
typedef struct ASS_Library ASS_Library;
struct mp_ass_packer;
struct mpv_global;
struct mp_log;
struct osd_style_opts;

struct mp_subtitle_opts {
    struct osd_style_opts *sub_style;
    int sub_ass_render_threads;
};
struct sd {
    void *priv;
    struct mp_subtitle_opts *opts;
    struct mpv_global *global;
    struct mp_log *log;
};
struct ass_render_state {
    ASS_Renderer *renderer;
    struct mp_ass_packer *packer;
};
struct sd_ass_priv {
    ASS_Library *ass_library;
    struct ass_render_state *renderers[2];
};

static ASS_Renderer the_renderer;
static int threads_arg = -1;
static ASS_Renderer *ass_renderer_init(ASS_Library *lib) { return &the_renderer; }
static unsigned ass_set_threads(ASS_Renderer *r, unsigned threads)
{
    threads_arg = (int)threads;
    return threads ? threads : 8;
}
static void mp_ass_configure_fonts(ASS_Renderer *r, struct osd_style_opts *o,
                                   struct mpv_global *g, struct mp_log *l) {}
static void set_renderer_cache_limits(struct sd *sd, ASS_Renderer *r) {}
static struct mp_ass_packer *mp_ass_packer_alloc(void *ta) { return NULL; }
static int destroy_renderer(void *p) { return 0; }
#define talloc_zero(ctx, type) ((type *)calloc(1, sizeof(type)))
#define talloc_set_destructor(p, d) ((void)(d))
#define talloc_free(p) free(p)

#include "get_renderer.inc"

// The option table entry, against a stand-in for mpv's option macros.
struct opt { const char *name; const char *choice; int choice_val; double min, max; int flags; };
#define OPT_CHOICE(field, ...) .choice = #__VA_ARGS__
#define M_RANGE(a, b) .min = (a), .max = (b)
#define UPDATE_SUB_HARD 1
static const struct opt table[] = {
#include "option.inc"
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int threads_for(int opt)
{
    struct mp_subtitle_opts opts = { .sub_ass_render_threads = opt };
    struct sd_ass_priv priv = {0};
    struct sd sd = { .priv = &priv, .opts = &opts };
    threads_arg = -1;
    struct ass_render_state *r = get_renderer(&sd, 0);
    CHECK(r && priv.renderers[0] == r, "no renderer");
    free(r);
    return threads_arg;
}

int main(void)
{
    // A zero-initialized option (the default) is auto: 0, as 0205 passed.
    CHECK(threads_for(0) == 0, "default passed %d", threads_for(0));
    CHECK(threads_for(1) == 1, "1 passed %d", threads_for(1));
    CHECK(threads_for(4) == 4, "4 passed %d", threads_for(4));

    CHECK(!strcmp(table[0].name, "sub-ass-render-threads"), "option name %s", table[0].name);
    CHECK(strstr(table[0].choice, "\"auto\", 0") != NULL, "auto is not 0: %s", table[0].choice);
    CHECK(table[0].min == 1 && table[0].max == 64, "range %g..%g", table[0].min, table[0].max);
    CHECK(table[0].flags == UPDATE_SUB_HARD, "a change does not reinit the subtitles");

    printf(failures ? "%d FAILED\n" : "sub ass render threads: all checks passed\n", failures);
    return failures ? 1 : 0;
}
