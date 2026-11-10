// Behavioral regression for patch 0040, extracted by
// test_audio_passthrough_fallback.sh from player/audio.c: handle_ao_events(),
// fall_back_to_pcm() and reload_audio_output().
//
// What must hold: an output that cannot play its passthrough stream reports
// AO_EVENT_PASSTHROUGH_FAILED. The player closes it, decodes the track it fed
// to PCM, and decodes again from the playback position, because the failed
// output took audio it never played. The fallback belongs to that track: a
// later plain reload tries passthrough again, and a failure reported by an
// output that outlived its chain (gapless file change, track switch) only
// closes it. Before 0040 the outputs latched the format instead, and
// ao_avfoundation's latch outlived every track and player in the process.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)
#define MP_INFO(...) ((void)0)
#define MP_ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define mp_assert(x) ((void)0)
#define MP_TIME_S_TO_NS(s) ((int64_t)((s) * INT64_C(1000000000)))

struct mp_log;
struct mp_async_queue;
struct mp_pin { int disconnects; };
struct mp_decoder_wrapper { bool spdif; int reinits; bool fail_reinit; };
struct mp_codec_params { const char *codec; };
struct sh_stream { struct mp_codec_params *codec; };
struct ao_chain;
struct track {
    struct mp_decoder_wrapper *dec;
    struct sh_stream *stream;
    struct mp_pin *sink;
    struct ao_chain *ao_c;
};
struct MPOpts { bool audio_spdif_backoff; double audio_spdif_backoff_expiry; };
struct mp_filter { int resets, wakeups; };
struct mp_output_chain { int resets; };
struct ao { int events; int resets; bool closed; int format; };
struct mp_chmap { int num; };
#define AF_FORMAT_S_EAC3 100
static bool af_fmt_is_spdif(int format) { return format == AF_FORMAT_S_EAC3; }
static void ao_get_format(struct ao *ao, int *rate, int *format, struct mp_chmap *ch)
{
    *rate = 48000;
    *format = ao->format;
    ch->num = 2;
}
// The player clock, moved by the test.
static int64_t now_ns = INT64_C(1000000000000);
static int64_t mp_time_ns(void) { return now_ns; }

#include "passthrough_fallback_types.inc"

struct MPContext {
    struct MPOpts *opts;
    struct mp_filter *filter_root;
    struct ao *ao;
    struct ao_chain *ao_chain;
    struct spdif_backoff spdif_backoff[8];
    int num_spdif_backoff;
    struct ao_route_watch *spdif_route_watch;
    int64_t spdif_ok_since_ns;
    bool paused;
    enum playback_status audio_status;
};

// The route watch (ao_route_watch_*): a device change the test reports.
struct ao_route_watch { bool fired; };
static struct ao_route_watch the_watch;
static int watches_created;
static struct ao_route_watch *ao_route_watch_create(void)
{
    watches_created++;
    the_watch.fired = false;
    return &the_watch;
}
static bool ao_route_watch_fired(struct ao_route_watch *w)
{
    if (!w)
        return false;
    bool f = w->fired;
    w->fired = false;
    return f;
}

// The system's audio route, as ao_get_route_id() reports it; NULL = unknown.
static const char *current_route = "hdmi:1;";
static bool ao_get_route_id(char *buf, size_t size)
{
    if (!current_route)
        return false;
    snprintf(buf, size, "%s", current_route);
    return true;
}

// init_audio_decoder()'s decoder: the rig's, set up afresh as for a new file.
static struct mp_decoder_wrapper *next_dec;
static struct mp_decoder_wrapper *mp_decoder_wrapper_create(struct mp_filter *parent,
                                                            struct sh_stream *src)
{
    (void)parent; (void)src;
    *next_dec = (struct mp_decoder_wrapper){0};
    return next_dec;
}
static void mp_pin_disconnect(struct mp_pin *p) { p->disconnects++; }

// --- fakes: audio/out/ao.c, player/audio.c and player/misc.c, as far as the
// extracted code reaches.

static int ao_query_and_reset_events(struct ao *ao, int events)
{
    int set = ao->events & events;
    ao->events &= ~events;
    return set;
}

static void ao_reset(struct ao *ao) { ao->resets++; }

// player/audio.c: forgets the chain's AO and closes the player's.
static void uninit_audio_out(struct MPContext *mpctx)
{
    if (mpctx->ao_chain) {
        mpctx->ao_chain->ao_queue = NULL;
        mpctx->ao_chain->queue_filter = NULL;
        mpctx->ao_chain->ao = NULL;
    }
    if (mpctx->ao)
        mpctx->ao->closed = true;
    mpctx->ao = NULL;
}

static int reinit_audio_filters(struct MPContext *mpctx) { (void)mpctx; return 1; }
static void reset_audio_state(struct MPContext *mpctx) { mpctx->audio_status = STATUS_SYNCING; }
static void mp_output_chain_reset_harder(struct mp_output_chain *c) { c->resets++; }
static void mp_filter_reset(struct mp_filter *f) { f->resets++; }
static void mp_filter_wakeup(struct mp_filter *f) { f->wakeups++; }
static void mp_wakeup_core(struct MPContext *mpctx) { (void)mpctx; }
static void mp_decoder_wrapper_set_spdif_flag(struct mp_decoder_wrapper *d, bool f) { d->spdif = f; }
static bool mp_decoder_wrapper_reinit(struct mp_decoder_wrapper *d)
{
    d->reinits++;
    return !d->fail_reinit;
}

static int track_errors;
static struct track *errored_track;
// player/misc.c: deselecting the track frees its chain.
static void error_on_track(struct MPContext *mpctx, struct track *t)
{
    track_errors++;
    errored_track = t;
    mpctx->ao_chain = NULL;
}

static int refresh_seeks;
static enum seek_precision refresh_precision;
static void issue_refresh_seek(struct MPContext *mpctx, enum seek_precision min_prec)
{
    (void)mpctx;
    refresh_seeks++;
    refresh_precision = min_prec;
}

void reload_audio_output(struct MPContext *mpctx);

#include "passthrough_fallback.inc"

// --- harness

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

struct rig {
    struct MPOpts opts;
    struct mp_codec_params codec;
    struct sh_stream stream;
    struct MPContext mpctx;
    struct ao ao;
    struct ao_chain ao_c;
    struct track track;
    struct mp_decoder_wrapper dec;
    struct mp_filter ao_filter;
    struct mp_output_chain chain;
};

// A passthrough track playing through r->ao.
static void rig_init(struct rig *r)
{
    memset(r, 0, sizeof(*r));
    track_errors = refresh_seeks = 0;
    errored_track = NULL;
    refresh_precision = MPSEEK_DEFAULT;
    r->dec.spdif = true;
    r->track.dec = &r->dec;
    // The 0040 cases run without the back-off (--audio-spdif-backoff=no).
    r->mpctx.opts = &r->opts;
    r->codec.codec = "eac3";
    r->stream.codec = &r->codec;
    r->track.stream = &r->stream;
    r->track.ao_c = &r->ao_c;
    current_route = "hdmi:1;";
    r->ao_c.mpctx = &r->mpctx;
    r->ao_c.spdif_passthrough = true;
    r->ao_c.filter = &r->chain;
    r->ao_c.ao_filter = &r->ao_filter;
    r->ao_c.track = &r->track;
    r->ao_c.ao = &r->ao;
    r->mpctx.ao = &r->ao;
    r->mpctx.ao_chain = &r->ao_c;
    r->mpctx.audio_status = STATUS_PLAYING;
}

static bool decodes_to_pcm(struct rig *r)
{
    return !r->dec.spdif && !r->ao_c.spdif_passthrough && r->ao_c.spdif_failed;
}

static bool still_passthrough(struct rig *r)
{
    return r->dec.spdif && r->ao_c.spdif_passthrough && !r->ao_c.spdif_failed;
}

int main(void)
{
    struct rig r;

    // The output fails under the track it plays: AVPlayer never started, or
    // the spatial renderer refused the stream.
    rig_init(&r);
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && !r.mpctx.ao, "failure: the failed output stays open");
    CHECK(decodes_to_pcm(&r) && r.dec.reinits == 1,
          "failure: the track is not decoded to PCM (spdif %d, reinits %d)",
          r.dec.spdif, r.dec.reinits);
    CHECK(r.ao_filter.wakeups > 0 && r.chain.resets > 0,
          "failure: the chain is not reset and woken for the next output");
    CHECK(refresh_seeks == 1 && refresh_precision == MPSEEK_EXACT,
          "failure: %d refresh seek(s) at precision %d; the audio the failed output "
          "took is skipped (silent with video)", refresh_seeks, refresh_precision);
    CHECK(track_errors == 0, "failure: a successful fallback errored the track");

    // A later plain reload of the PCM output (a route or device change) tries
    // passthrough again: the fallback belongs to this track, not the player.
    struct ao pcm = {.events = AO_EVENT_RELOAD};
    r.mpctx.ao = &pcm;
    r.ao_c.ao = &pcm;
    handle_ao_events(&r.mpctx);
    CHECK(pcm.closed && still_passthrough(&r) && r.dec.reinits == 2,
          "later reload: passthrough is not tried again (spdif %d, failed %d)",
          r.dec.spdif, r.ao_c.spdif_failed);
    CHECK(refresh_seeks == 1, "later reload: a plain reload issued a refresh seek");

    // Both events pending at once: one teardown, and the failure wins.
    rig_init(&r);
    r.ao.events = AO_EVENT_RELOAD | AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.resets == 1 && r.ao.closed, "both: the output was reset %d times", r.ao.resets);
    CHECK(decodes_to_pcm(&r) && r.dec.reinits == 1 && refresh_seeks == 1,
          "both: the plain reload hid the failure (spdif %d, reinits %d, seeks %d)",
          r.dec.spdif, r.dec.reinits, refresh_seeks);

    // A plain reload of a working passthrough output leaves the decoder alone.
    rig_init(&r);
    r.ao.events = AO_EVENT_RELOAD;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && still_passthrough(&r) && r.dec.reinits == 0 && refresh_seeks == 0,
          "plain reload: the passthrough track was touched (spdif %d, reinits %d, seeks %d)",
          r.dec.spdif, r.dec.reinits, refresh_seeks);

    // No event: the output stays, as on every playloop iteration.
    rig_init(&r);
    handle_ao_events(&r.mpctx);
    CHECK(!r.ao.closed && r.ao.resets == 0 && r.mpctx.ao == &r.ao,
          "no event: the output was closed");

    // The output outlived the chain it failed for: a track switch or gapless
    // file change kept it open, and the new chain is not attached to it yet.
    // The new track tries passthrough with a fresh output.
    rig_init(&r);
    r.ao_c.ao = NULL;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && !r.mpctx.ao, "outlived chain: the failed output stays open");
    CHECK(still_passthrough(&r) && r.dec.reinits == 0 && refresh_seeks == 0,
          "outlived chain: the next track lost passthrough for the old one's failure");

    // No chain at all (the last file ended; the output was kept for the next).
    rig_init(&r);
    r.mpctx.ao_chain = NULL;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && !r.mpctx.ao && r.dec.reinits == 0 && refresh_seeks == 0,
          "no chain: the failed output is not just closed");

    // A lavfi-complex chain has no track and no decoder to switch.
    rig_init(&r);
    r.ao_c.track = NULL;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && r.dec.reinits == 0 && refresh_seeks == 0 && track_errors == 0,
          "no track: a chain without a decoder was asked to fall back");

    // The decoder cannot be reinitialized: the track errors out, which frees
    // its chain, and nothing runs on that chain afterwards.
    rig_init(&r);
    r.dec.fail_reinit = true;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(track_errors == 1 && errored_track == &r.track && !r.mpctx.ao_chain,
          "reinit failure: the track was not errored (%d)", track_errors);
    CHECK(refresh_seeks == 0, "reinit failure: a refresh seek ran for a freed chain");

    // 0303, --audio-spdif-backoff (default no; on for these cases, with the
    // expiry off unless a case sets it). After a mid-stream failure
    // on this player, the codec is decoded to PCM on this route: a later
    // output reload and the next file's decoder skip passthrough.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(decodes_to_pcm(&r) && refresh_seeks == 1, "back-off: the failure itself falls back as before");
    CHECK(r.mpctx.num_spdif_backoff == 1 && !strcmp(r.mpctx.spdif_backoff[0].codec, "eac3") &&
          r.mpctx.spdif_backoff[0].route_known &&
          !strcmp(r.mpctx.spdif_backoff[0].route, "hdmi:1;"),
          "back-off: the codec and its route are not recorded");
    struct ao pcm2 = {.events = AO_EVENT_RELOAD};
    r.mpctx.ao = &pcm2;
    r.ao_c.ao = &pcm2;
    handle_ao_events(&r.mpctx);
    CHECK(decodes_to_pcm(&r) && r.dec.reinits == 1,
          "back-off: a reload on the same route retried passthrough (spdif %d, reinits %d)",
          r.dec.spdif, r.dec.reinits);

    // The next file on the same player: its decoder starts as PCM.
    struct mp_decoder_wrapper dec2;
    next_dec = &dec2;
    r.track.dec = NULL;
    r.ao_c.spdif_passthrough = true;
    r.ao_c.spdif_failed = false;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && !dec2.spdif &&
          !r.ao_c.spdif_passthrough && !r.ao_c.spdif_failed,
          "back-off: the next file tried passthrough (spdif %d)", dec2.spdif);

    // Another codec is not affected.
    r.codec.codec = "ac3";
    r.track.dec = NULL;
    r.ao_c.spdif_passthrough = true;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && dec2.spdif && r.ao_c.spdif_passthrough,
          "back-off: AC-3 lost passthrough for an E-AC-3 failure");
    r.codec.codec = "eac3";

    // An unknown route counts as the same.
    current_route = NULL;
    r.track.dec = NULL;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && !dec2.spdif,
          "back-off: an unknown route cleared it");

    // The route changes: the entry goes and passthrough is tried again, on
    // the next decoder and on a reload alike.
    current_route = "airpods:2;";
    r.track.dec = NULL;
    r.ao_c.spdif_passthrough = true;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && dec2.spdif &&
          r.ao_c.spdif_passthrough && r.mpctx.num_spdif_backoff == 0,
          "back-off: a route change did not clear it (spdif %d, entries %d)",
          dec2.spdif, r.mpctx.num_spdif_backoff);
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    current_route = "hdmi:2;";
    struct ao pcm3 = {.events = AO_EVENT_RELOAD};
    r.mpctx.ao = &pcm3;
    r.ao_c.ao = &pcm3;
    handle_ao_events(&r.mpctx);
    CHECK(still_passthrough(&r) && r.dec.reinits == 2 && r.mpctx.num_spdif_backoff == 0,
          "back-off: a reload after a route change did not retry passthrough");

    // The same route id, but the route reported a device change since the
    // failure (the receiver was power-cycled, the cable replugged): the
    // table is cleared and passthrough is tried again.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    watches_created = 0;
    the_watch.fired = true; // a change from before the failure does not count
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.mpctx.spdif_route_watch == &the_watch && watches_created == 1,
          "route event: the failure did not start watching the route");
    struct mp_decoder_wrapper dec4;
    next_dec = &dec4;
    r.track.dec = NULL;
    r.ao_c.spdif_passthrough = true;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && !dec4.spdif,
          "route event: a change before the failure cleared it");
    the_watch.fired = true;
    r.track.dec = NULL;
    r.ao_c.spdif_passthrough = true;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && dec4.spdif &&
          r.ao_c.spdif_passthrough && r.mpctx.num_spdif_backoff == 0,
          "route event: same route id after a device change kept the back-off "
          "(spdif %d, entries %d)", dec4.spdif, r.mpctx.num_spdif_backoff);
    // And through a reload of the PCM output.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    r.mpctx.spdif_route_watch = &the_watch;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    the_watch.fired = true;
    struct ao pcm4 = {.events = AO_EVENT_RELOAD};
    r.mpctx.ao = &pcm4;
    r.ao_c.ao = &pcm4;
    handle_ao_events(&r.mpctx);
    CHECK(still_passthrough(&r) && r.mpctx.num_spdif_backoff == 0,
          "route event: a reload after a device change did not retry passthrough");
    CHECK(watches_created == 1, "route event: the player's watch was not reused");

    // Per player: a second player in the process is unaffected.
    struct rig other;
    rig_init(&other);
    other.opts.audio_spdif_backoff = true;
    struct mp_decoder_wrapper dec3;
    next_dec = &dec3;
    other.track.dec = NULL;
    CHECK(init_audio_decoder(&other.mpctx, &other.track) == 1 && dec3.spdif,
          "back-off: leaked into another player");

    // A failure at output init (no event) is not backed off: it costs nothing.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    CHECK(fall_back_to_pcm(&r.mpctx, &r.ao_c) && r.mpctx.num_spdif_backoff == 0,
          "back-off: recorded without a mid-stream failure");

    // The expiry: an entry lapses --audio-spdif-backoff-expiry after its
    // failure, on the same route.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    r.opts.audio_spdif_backoff_expiry = 600;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    struct mp_decoder_wrapper dec5;
    next_dec = &dec5;
    now_ns += MP_TIME_S_TO_NS(599);
    r.track.dec = NULL;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && !dec5.spdif,
          "expiry: lapsed early");
    now_ns += MP_TIME_S_TO_NS(1);
    r.track.dec = NULL;
    r.ao_c.spdif_passthrough = true;
    CHECK(init_audio_decoder(&r.mpctx, &r.track) == 1 && dec5.spdif &&
          r.mpctx.num_spdif_backoff == 0, "expiry: not lapsed at 600 s");

    // A passthrough output that plays for 10 s on the same route clears the
    // route's entries, of any codec; one on another route stays. Pausing
    // or a PCM output does not count.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);              // eac3 backed off on hdmi:1
    current_route = "hdmi:2;";
    r.codec.codec = "truehd";
    spdif_back_off(&r.mpctx, &r.track);      // truehd on another route
    current_route = "hdmi:1;";
    CHECK(r.mpctx.num_spdif_backoff == 2, "success: setup (%d)", r.mpctx.num_spdif_backoff);
    struct ao ac3 = { .format = AF_FORMAT_S_EAC3 };
    r.mpctx.ao = &ac3;
    r.ao_c.ao = &ac3;
    r.ao_c.spdif_passthrough = true;         // an AC-3 track passing through
    r.mpctx.audio_status = STATUS_PLAYING;
    handle_ao_events(&r.mpctx);
    now_ns += MP_TIME_S_TO_NS(5);
    r.mpctx.paused = true;
    handle_ao_events(&r.mpctx);              // a pause restarts the count
    r.mpctx.paused = false;
    handle_ao_events(&r.mpctx);
    now_ns += MP_TIME_S_TO_NS(9.9);
    handle_ao_events(&r.mpctx);
    CHECK(r.mpctx.num_spdif_backoff == 2, "success: cleared before 10 s of playing");
    now_ns += MP_TIME_S_TO_NS(0.1);
    handle_ao_events(&r.mpctx);
    CHECK(r.mpctx.num_spdif_backoff == 1 && !strcmp(r.mpctx.spdif_backoff[0].codec, "truehd"),
          "success: the route's entries were not cleared, or another route's were "
          "(%d left)", r.mpctx.num_spdif_backoff);
    struct ao pcm5 = { .format = 1 };
    r.mpctx.ao = &pcm5;
    r.ao_c.ao = &pcm5;
    current_route = "hdmi:2;";
    now_ns += MP_TIME_S_TO_NS(60);
    handle_ao_events(&r.mpctx);
    handle_ao_events(&r.mpctx);
    CHECK(r.mpctx.num_spdif_backoff == 1, "success: a PCM output counted as passthrough");
    r.codec.codec = "eac3";

    // Off (the default): nothing is recorded and nothing is skipped.
    rig_init(&r);
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(decodes_to_pcm(&r) && r.mpctx.num_spdif_backoff == 0 && !r.mpctx.spdif_route_watch,
          "off: a failure was recorded");

    // The table is bounded; the oldest goes.
    rig_init(&r);
    r.opts.audio_spdif_backoff = true;
    static char names[10][8];
    for (int i = 0; i < 10; i++) {
        snprintf(names[i], sizeof(names[i]), "c%d", i);
        r.codec.codec = names[i];
        spdif_back_off(&r.mpctx, &r.track);
    }
    CHECK(r.mpctx.num_spdif_backoff == 8 && !strcmp(r.mpctx.spdif_backoff[0].codec, "c2") &&
          !strcmp(r.mpctx.spdif_backoff[7].codec, "c9"), "back-off: table overflow");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("audio passthrough fallback harness: ok\n");
    return 0;
}
