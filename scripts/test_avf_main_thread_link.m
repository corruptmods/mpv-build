// Host test for vo_avfoundation's main-thread link; see
// test_avf_main_thread_link.sh. The test's own main thread plays the host's:
// it either services the main queue or blocks the way a host blocked in
// mpv_terminate_destroy() does (joining the thread that runs the VO's code).
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <QuartzCore/QuartzCore.h>
#include <os/lock.h>

#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MP_TIME_MS_TO_NS(ms) ((ms) * INT64_C(1000000))
#define AVF_LAYER_RETIRE_DELAY_NS MP_TIME_MS_TO_NS(200)

enum avf_gravity {
    AVF_GRAVITY_ASPECT,
    AVF_GRAVITY_ASPECT_FILL,
    AVF_GRAVITY_RESIZE,
};

struct priv {
    atomic_bool shader_layer_stale;
    atomic_bool pending_layer_geometry_sync;
    atomic_bool pending_main_late;
    atomic_bool pending_presentation_resync;
    atomic_bool app_inactive;
};

struct vo {
    struct priv *priv;
};

static atomic_int wakeups;
static struct vo *poisoned_vo;

static void vo_wakeup(struct vo *vo)
{
    if (vo == poisoned_vo) {
        fprintf(stderr, "FAIL: a callback reached a detached VO\n");
        exit(1);
    }
    atomic_fetch_add(&wakeups, 1);
}

static CGFloat get_screen_scale(void)
{
    return 1.0;
}

#include "avf_main_thread_link.inc"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static void drain_main(double seconds)
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

static void *watchdog(void *arg)
{
    sleep(20);
    fprintf(stderr, "FAIL: deadlock (watchdog fired)\n");
    _exit(2);
    return NULL;
}

// What preinit's setup block does to register the link, minus the timebase
// and app-state parts this host has no use for.
static MPVLayerGeometryObserver *make_link(struct vo *vo,
                                           AVSampleBufferDisplayLayer *layer)
{
    MPVLayerGeometryObserver *link = [[MPVLayerGeometryObserver alloc] init];
    link->vo = vo;
    link->layer = [layer retain];
    NSKeyValueObservingOptions o = NSKeyValueObservingOptionOld |
                                   NSKeyValueObservingOptionNew;
    [layer addObserver:link forKeyPath:@"bounds" options:o context:NULL];
    [layer addObserver:link forKeyPath:@"contentsScale" options:o context:NULL];
    [layer addObserver:link forKeyPath:@"videoGravity" options:o context:NULL];
    link->observing = true;
    avf_link_refresh_geometry(link);
    return link;
}

static void read_cache(MPVLayerGeometryObserver *link, int *w, int *h,
                       enum avf_gravity *g)
{
    os_unfair_lock_lock(&link->lock);
    *w = link->geometry_w;
    *h = link->geometry_h;
    *g = link->geometry_gravity;
    os_unfair_lock_unlock(&link->lock);
}

struct teardown_job {
    MPVLayerGeometryObserver *link;
    AVSampleBufferDisplayLayer *osd;
    CMTimebaseRef timebase;
};

static void *teardown_thread(void *arg)
{
    struct teardown_job *job = arg;
    avf_link_teardown(job->link, job->osd, job->timebase);
    return NULL;
}

struct run_job {
    MPVLayerGeometryObserver *link;
    bool ran;
    atomic_bool block_ran;
    int64_t took_ns;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static void *run_thread(void *arg)
{
    struct run_job *job = arg;
    int64_t t0 = now_ns();
    job->ran = avf_link_run_on_main(job->link, ^{
        atomic_store(&job->block_ran, true);
    });
    job->took_ns = now_ns() - t0;
    return NULL;
}

static void test_geometry_cache(void)
{
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    AVSampleBufferDisplayLayer *layer = [[AVSampleBufferDisplayLayer alloc] init];
    layer.contentsScale = 2.0;
    layer.bounds = CGRectMake(0, 0, 100, 50);
    MPVLayerGeometryObserver *link = make_link(&vo, layer);

    int w, h;
    enum avf_gravity g;
    read_cache(link, &w, &h, &g);
    CHECK(w == 200 && h == 100 && g == AVF_GRAVITY_ASPECT,
          "initial cache %dx%d gravity %d", w, h, g);

    // A resize on the main thread reaches the cache before the VO's flag.
    atomic_store(&wakeups, 0);
    layer.bounds = CGRectMake(0, 0, 640, 360);
    read_cache(link, &w, &h, &g);
    CHECK(w == 1280 && h == 720, "after resize %dx%d", w, h);
    CHECK(atomic_load(&p.pending_layer_geometry_sync), "resize not flagged");
    CHECK(atomic_load(&p.shader_layer_stale), "resize did not stale shaders");
    CHECK(atomic_load(&wakeups) == 1, "resize woke %d times", atomic_load(&wakeups));

    // Gravity alone stales the shaders, not the OSD geometry.
    atomic_store(&p.pending_layer_geometry_sync, false);
    atomic_store(&p.shader_layer_stale, false);
    layer.videoGravity = AVLayerVideoGravityResizeAspectFill;
    read_cache(link, &w, &h, &g);
    CHECK(g == AVF_GRAVITY_ASPECT_FILL, "gravity %d", g);
    CHECK(!atomic_load(&p.pending_layer_geometry_sync), "gravity flagged a resize");
    CHECK(atomic_load(&p.shader_layer_stale), "gravity did not stale shaders");

    // Setting the same value is not a change.
    atomic_store(&wakeups, 0);
    layer.bounds = CGRectMake(0, 0, 640, 360);
    CHECK(atomic_load(&wakeups) == 0, "a no-op set woke the VO");

    // A change made off the main thread reaches the cache on the main queue,
    // before the VO is flagged.
    atomic_store(&p.pending_layer_geometry_sync, false);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        [CATransaction begin];
        layer.bounds = CGRectMake(0, 0, 320, 180);
        [CATransaction commit];
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
    CHECK(!atomic_load(&p.pending_layer_geometry_sync),
          "an off-main change flagged the VO before the cache refresh");
    drain_main(0.2);
    read_cache(link, &w, &h, &g);
    CHECK(w == 640 && h == 360, "after off-main resize %dx%d", w, h);
    CHECK(atomic_load(&p.pending_layer_geometry_sync), "off-main resize not flagged");

    // A queued refresh picks up what KVO cannot see (no notification is sent
    // for a direct ivar change, so stand in with a cache that went stale).
    os_unfair_lock_lock(&link->lock);
    link->geometry_w = 1;
    os_unfair_lock_unlock(&link->lock);
    avf_link_queue_refresh(link);
    avf_link_queue_refresh(link); // coalesced
    drain_main(0.1);
    read_cache(link, &w, &h, &g);
    CHECK(w == 640, "queued refresh left %d", w);

    avf_link_teardown(link, nil, NULL);
    drain_main(0.1);
    [layer release];
}

// The host destroys the player on its main thread: the main thread joins the
// thread running uninit and services nothing meanwhile.
static void test_teardown_while_main_blocked(void)
{
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    AVSampleBufferDisplayLayer *layer = [[AVSampleBufferDisplayLayer alloc] init];
    AVSampleBufferDisplayLayer *osd = [[AVSampleBufferDisplayLayer alloc] init];
    layer.bounds = CGRectMake(0, 0, 100, 100);
    MPVLayerGeometryObserver *link = make_link(&vo, layer);

    CMTimebaseRef timebase = NULL;
    CMTimebaseCreateWithSourceClock(kCFAllocatorDefault, CMClockGetHostTimeClock(),
                                    &timebase);
    layer.controlTimebase = timebase;
    osd.controlTimebase = timebase;

    // An OSD layer the main thread made that the VO never adopted.
    AVSampleBufferDisplayLayer *orphan = [[AVSampleBufferDisplayLayer alloc] init];
    CALayer *parent = [CALayer layer];
    [parent addSublayer:orphan];
    link->pending_osd_layer = [orphan retain]; // the link's; the test keeps its own

    [link retain]; // the test's own, to look at it afterwards
    struct teardown_job job = {link, osd, (CMTimebaseRef)CFRetain(timebase)};
    pthread_t t;
    pthread_create(&t, NULL, teardown_thread, &job);
    pthread_join(t, NULL); // returns only if teardown never waits for main

    // Detached at once: a callback before the main queue runs finds no VO.
    poisoned_vo = &vo;
    layer.bounds = CGRectMake(0, 0, 200, 200);
    CHECK(link->vo == NULL, "not detached");

    drain_main(0.5);
    poisoned_vo = NULL;
    CHECK(!link->observing, "KVO still registered after teardown");
    CHECK(link->layer == nil, "teardown kept the display layer");
    CHECK(layer.controlTimebase == NULL, "display layer kept the timebase");
    CHECK(osd.controlTimebase == NULL, "OSD layer kept the timebase");
    CHECK(link->pending_osd_layer == nil, "orphan OSD layer left in the link");
    CHECK(orphan.superlayer == nil, "orphan OSD layer left in the tree");
    CHECK([orphan retainCount] == 1, "orphan retain count %lu",
          (unsigned long)[orphan retainCount]);
    [orphan release];
    CHECK(CFGetRetainCount(timebase) == 1 + (layer.controlTimebase ? 1 : 0),
          "timebase retain count %ld", (long)CFGetRetainCount(timebase));
    CHECK([link retainCount] == 1, "link retain count %lu after teardown",
          (unsigned long)[link retainCount]);

    // With KVO gone, a resize reaches nothing.
    atomic_store(&wakeups, 0);
    layer.bounds = CGRectMake(0, 0, 300, 300);
    CHECK(atomic_load(&wakeups) == 0, "a resize after teardown woke the VO");

    [link release];
    CFRelease(timebase);
    [osd release];
    [layer release];
}

// A later VO on the same layer set its own timebase first: teardown must
// leave it alone.
static void test_teardown_keeps_a_newer_timebase(void)
{
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    AVSampleBufferDisplayLayer *layer = [[AVSampleBufferDisplayLayer alloc] init];
    MPVLayerGeometryObserver *link = make_link(&vo, layer);
    CMTimebaseRef old_tb = NULL, new_tb = NULL;
    CMTimebaseCreateWithSourceClock(kCFAllocatorDefault, CMClockGetHostTimeClock(), &old_tb);
    CMTimebaseCreateWithSourceClock(kCFAllocatorDefault, CMClockGetHostTimeClock(), &new_tb);
    layer.controlTimebase = new_tb;
    avf_link_teardown(link, nil, (CMTimebaseRef)CFRetain(old_tb));
    drain_main(0.1);
    CHECK(layer.controlTimebase == new_tb, "teardown cleared a timebase it did not own");
    CHECK(CFGetRetainCount(old_tb) == 1, "old timebase retain count %ld",
          (long)CFGetRetainCount(old_tb));
    layer.controlTimebase = NULL;
    CFRelease(old_tb);
    CFRelease(new_tb);
    [layer release];
}

static void test_bounded_wait(void)
{
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    AVSampleBufferDisplayLayer *layer = [[AVSampleBufferDisplayLayer alloc] init];
    MPVLayerGeometryObserver *link = make_link(&vo, layer);

    // Main thread serviced: the block runs and the caller sees it ran.
    struct run_job ok = {.link = link};
    pthread_t t;
    pthread_create(&t, NULL, run_thread, &ok);
    for (int i = 0; i < 100 && !atomic_load(&ok.block_ran); i++)
        drain_main(0.02);
    pthread_join(t, NULL);
    CHECK(ok.ran && atomic_load(&ok.block_ran), "serviced main: ran=%d", ok.ran);
    CHECK(!atomic_load(&p.pending_main_late), "serviced main set pending_main_late");

    // Main thread blocked: the caller gives up after the bound, the block
    // runs later and tells the VO.
    struct run_job late = {.link = link};
    atomic_store(&wakeups, 0);
    pthread_create(&t, NULL, run_thread, &late);
    pthread_join(t, NULL);
    CHECK(!late.ran, "blocked main: the caller thinks the block ran");
    CHECK(late.took_ns >= AVF_MAIN_WAIT_NS - MP_TIME_MS_TO_NS(5) &&
          late.took_ns < AVF_MAIN_WAIT_NS + MP_TIME_MS_TO_NS(400),
          "bounded wait took %lld ns", (long long)late.took_ns);
    CHECK(!atomic_load(&late.block_ran), "block ran while main was blocked");
    drain_main(0.1);
    CHECK(atomic_load(&late.block_ran), "late block never ran");
    CHECK(atomic_load(&p.pending_main_late), "late block did not set pending_main_late");
    CHECK(atomic_load(&wakeups) == 1, "late block woke %d times", atomic_load(&wakeups));

    // Late and the VO gone by then: nothing reaches it.
    atomic_store(&p.pending_main_late, false);
    struct run_job gone = {.link = link};
    pthread_create(&t, NULL, run_thread, &gone);
    pthread_join(t, NULL);
    avf_link_detach(link);
    poisoned_vo = &vo;
    drain_main(0.1);
    poisoned_vo = NULL;
    CHECK(atomic_load(&gone.block_ran), "late block never ran after detach");
    CHECK(!atomic_load(&p.pending_main_late), "a detached VO was flagged");

    avf_link_teardown(link, nil, NULL);
    drain_main(0.1);
    [layer release];
}

// The OSD layer the main thread makes waits in the link; one dropped while
// it was being made is retired instead.
static void test_osd_layer_handoff(void)
{
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    AVSampleBufferDisplayLayer *layer = [[AVSampleBufferDisplayLayer alloc] init];
    MPVLayerGeometryObserver *link = make_link(&vo, layer);

    AVSampleBufferDisplayLayer *got = nil;
    bool orphaned = false;
    os_unfair_lock_lock(&link->lock);
    link->osd_layer_creating = true;
    os_unfair_lock_unlock(&link->lock);
    CHECK(avf_link_adopt_osd_layer(link, &got, &orphaned) && !got,
          "an in-flight layer must make the VO wait, not start another");

    AVSampleBufferDisplayLayer *made = [[AVSampleBufferDisplayLayer alloc] init];
    os_unfair_lock_lock(&link->lock);
    link->osd_layer_creating = false;
    link->pending_osd_layer = made;
    link->osd_layer_orphaned = true;
    os_unfair_lock_unlock(&link->lock);
    CHECK(avf_link_adopt_osd_layer(link, &got, &orphaned) && got == made && orphaned,
          "adopt: got %p orphaned %d", (void *)got, orphaned);
    CHECK(!avf_link_adopt_osd_layer(link, &got, &orphaned), "adopted twice");

    // Dropped while in flight: the flag tells the maker to retire it.
    os_unfair_lock_lock(&link->lock);
    link->osd_layer_creating = true;
    os_unfair_lock_unlock(&link->lock);
    avf_link_discard_osd_layer(link);
    CHECK(link->osd_layer_discard, "discard while creating did not mark it");
    // Wanted again before it arrived: keep it after all.
    avf_link_adopt_osd_layer(link, &got, &orphaned);
    CHECK(!link->osd_layer_discard, "re-wanted layer still marked for discard");

    // A pending one is retired: out of the tree after the delay.
    AVSampleBufferDisplayLayer *extra = [[AVSampleBufferDisplayLayer alloc] init];
    CALayer *parent = [CALayer layer];
    [parent addSublayer:extra];
    os_unfair_lock_lock(&link->lock);
    link->osd_layer_creating = false;
    link->pending_osd_layer = [extra retain];
    os_unfair_lock_unlock(&link->lock);
    avf_link_discard_osd_layer(link);
    drain_main(0.05);
    CHECK(extra.hidden, "retired layer not hidden first");
    CHECK(extra.superlayer == parent, "retired layer left the tree before the delay");
    drain_main(0.4);
    CHECK(extra.superlayer == nil, "retired layer still in the tree");
    CHECK([extra retainCount] == 1, "retired layer leaked or over-released");
    [extra release];

    [made release];
    avf_link_teardown(link, nil, NULL);
    drain_main(0.1);
    [layer release];
}

int main(void)
{
    @autoreleasepool {
        pthread_t dog;
        pthread_create(&dog, NULL, watchdog, NULL);
        pthread_detach(dog);

        test_geometry_cache();
        test_teardown_while_main_blocked();
        test_teardown_keeps_a_newer_timebase();
        test_bounded_wait();
        test_osd_layer_handoff();
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_avf_main_thread_link: all passed\n");
    return 0;
}
