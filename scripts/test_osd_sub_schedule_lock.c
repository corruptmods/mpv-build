// See test_osd_sub_schedule_lock.sh.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// mpv's threading wrappers and the few macros the extracted code uses.
typedef pthread_mutex_t mp_mutex;
typedef pthread_cond_t mp_cond;
#define mp_mutex_lock pthread_mutex_lock
#define mp_mutex_unlock pthread_mutex_unlock
#define mp_cond_wait pthread_cond_wait
#define mp_cond_broadcast pthread_cond_broadcast
#define MP_NOPTS_VALUE (-0x1p63)
#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MP_PTS_OR_DEF(a, b) ((a) == MP_NOPTS_VALUE ? (b) : (a))
#define MP_PTS_MIN(a, b) MPMIN(MP_PTS_OR_DEF(a, b), MP_PTS_OR_DEF(b, a))

enum { OSDTYPE_SUB, OSDTYPE_SUB2, MAX_OSD_PARTS = 5 };

struct dec_sub {
    atomic_bool alive;       // false once the "caller" freed it
    double next_start;
    double visible_end;
    atomic_int in_query;     // threads inside a query right now
};

struct osd_object {
    struct dec_sub *sub;
    int vo_change_id;
};

struct osd_state {
    mp_mutex lock;
    struct osd_object *objs[MAX_OSD_PARTS];
    bool sub_render_busy;
    mp_cond sub_render_done;
    bool want_redraw_notification;
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A query may be held "rendering": it then blocks until released, like
// sub_get_next_event_start() in wait_render_done().
static atomic_bool hold_queries;
static atomic_int queries_blocked;
static atomic_int use_after_free;

static void query_enter(struct dec_sub *sub)
{
    if (!atomic_load(&sub->alive))
        atomic_fetch_add(&use_after_free, 1);
    atomic_fetch_add(&sub->in_query, 1);
    if (atomic_load(&hold_queries)) {
        atomic_fetch_add(&queries_blocked, 1);
        while (atomic_load(&hold_queries))
            usleep(100);
        atomic_fetch_sub(&queries_blocked, 1);
    }
}

static void query_leave(struct dec_sub *sub)
{
    if (!atomic_load(&sub->alive))
        atomic_fetch_add(&use_after_free, 1);
    atomic_fetch_sub(&sub->in_query, 1);
}

static double sub_get_next_event_start(struct dec_sub *sub, double after, int *num_events)
{
    query_enter(sub);
    *num_events = 1;
    double r = sub->next_start;
    query_leave(sub);
    return r;
}

static double sub_get_visible_end(struct dec_sub *sub, double pts)
{
    query_enter(sub);
    double r = sub->visible_end;
    query_leave(sub);
    return r;
}

#include "osd_schedule.inc"

static struct osd_state osd;
static struct osd_object obj[2];
// subs[0] and subs[1] take turns in SUB; subs[2] stays in SUB2.
static struct dec_sub subs[3];

static void init_osd(void)
{
    pthread_mutex_init(&osd.lock, NULL);
    pthread_cond_init(&osd.sub_render_done, NULL);
    for (int n = 0; n < 2; n++)
        osd.objs[OSDTYPE_SUB + n] = &obj[n];
    for (int n = 0; n < 3; n++)
        atomic_store(&subs[n].alive, true);
    subs[0].next_start = 10.0;
    subs[0].visible_end = 12.0;
    subs[1].next_start = 10.5;
    subs[1].visible_end = 12.5;
    subs[2].next_start = 11.0;
    subs[2].visible_end = 9.0;
    obj[0].sub = &subs[0];
    obj[1].sub = &subs[2];
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct sched_result { double next_start, visible_end; int next_events; atomic_bool done; };

static void *schedule_thread(void *arg)
{
    struct sched_result *r = arg;
    osd_get_sub_schedule(&osd, 5.0, 6.0, &r->next_start, &r->next_events, &r->visible_end);
    atomic_store(&r->done, true);
    return NULL;
}

struct swap_job { atomic_bool done; };

// What the player does to change a track: osd_set_sub(), then free the old
// dec_sub at once.
static void *swap_thread(void *arg)
{
    struct swap_job *j = arg;
    osd_set_sub(&osd, 0, NULL);
    atomic_store(&subs[0].alive, false); // "freed"
    atomic_store(&j->done, true);
    return NULL;
}

static atomic_bool stop_stress;
static atomic_int stress_schedules, stress_swaps;

static void *stress_schedule(void *arg)
{
    while (!atomic_load(&stop_stress)) {
        double a, b;
        int c;
        osd_get_sub_schedule(&osd, 1, 2, &a, &c, &b);
        atomic_fetch_add(&stress_schedules, 1);
    }
    return NULL;
}

// Swaps between two live dec_subs; the one swapped out is "freed" until it
// is swapped back in, which is when a query must not be using it.
static void *stress_swapper(void *arg)
{
    struct dec_sub *pool[2] = {&subs[0], &subs[1]};
    int cur = 0;
    while (!atomic_load(&stop_stress)) {
        int next = !cur;
        atomic_store(&pool[next]->alive, true);
        osd_set_sub(&osd, 0, pool[next]);
        // osd_set_sub returned: nothing may still be querying the old one.
        if (atomic_load(&pool[cur]->in_query))
            atomic_fetch_add(&use_after_free, 1);
        atomic_store(&pool[cur]->alive, false);
        cur = next;
        atomic_fetch_add(&stress_swaps, 1);
    }
    return NULL;
}

static void *stress_locker(void *arg)
{
    // The playloop's osd_set_text() and friends: take and drop the lock.
    while (!atomic_load(&stop_stress)) {
        mp_mutex_lock(&osd.lock);
        mp_mutex_unlock(&osd.lock);
    }
    return NULL;
}

int main(void)
{
    init_osd();

    // Plain answer: the earliest start, the earliest visible end.
    struct sched_result r = {0};
    schedule_thread(&r);
    CHECK(r.next_start == 10.0 && r.next_events == 1 && r.visible_end == 9.0,
          "schedule: start %g events %d end %g", r.next_start, r.next_events, r.visible_end);
    CHECK(!osd.sub_render_busy, "schedule left the subs pinned");

    // A query held by a render: osd->lock stays free for the playloop, and
    // osd_set_sub() waits until the query is done before the old dec_sub can
    // be freed.
    atomic_store(&hold_queries, true);
    struct sched_result held = {0};
    pthread_t t;
    pthread_create(&t, NULL, schedule_thread, &held);
    while (!atomic_load(&queries_blocked))
        usleep(100);
    // The old code held the lock here until the render finished; give up
    // after a second rather than hang, and let the query go.
    int64_t t0 = now_ms();
    bool got = false;
    while (!(got = !pthread_mutex_trylock(&osd.lock)) && now_ms() - t0 < 1000)
        usleep(1000);
    if (got)
        mp_mutex_unlock(&osd.lock);
    CHECK(got && now_ms() - t0 < 50, "osd->lock was held across a blocked dec_sub query");
    if (!got) {
        atomic_store(&hold_queries, false);
        pthread_join(t, NULL);
        printf("%d FAILED\n", failures);
        return 1;
    }
    struct swap_job swap = {0};
    pthread_t s;
    pthread_create(&s, NULL, swap_thread, &swap);
    usleep(50 * 1000);
    CHECK(!atomic_load(&swap.done), "osd_set_sub() swapped a dec_sub a query was still using");
    atomic_store(&hold_queries, false);
    pthread_join(t, NULL);
    pthread_join(s, NULL);
    CHECK(atomic_load(&held.done) && held.next_start == 10.0, "the held query's answer");
    CHECK(atomic_load(&swap.done) && !osd.sub_render_busy, "the swap completes after it");
    CHECK(atomic_load(&use_after_free) == 0, "a dec_sub was used after the caller freed it");

    // Stress: queries, swaps and plain lock users from several threads.
    init_osd();
    atomic_store(&use_after_free, 0);
    pthread_t th[6];
    for (int i = 0; i < 3; i++)
        pthread_create(&th[i], NULL, stress_schedule, NULL);
    pthread_create(&th[3], NULL, stress_swapper, NULL);
    pthread_create(&th[4], NULL, stress_locker, NULL);
    pthread_create(&th[5], NULL, stress_locker, NULL);
    sleep(2);
    atomic_store(&stop_stress, true);
    for (int i = 0; i < 6; i++)
        pthread_join(th[i], NULL);
    CHECK(atomic_load(&use_after_free) == 0, "stress: %d uses of a freed dec_sub",
          atomic_load(&use_after_free));
    CHECK(atomic_load(&stress_schedules) > 100 && atomic_load(&stress_swaps) > 100,
          "stress made no progress (%d schedules, %d swaps)",
          atomic_load(&stress_schedules), atomic_load(&stress_swaps));
    CHECK(!osd.sub_render_busy, "stress left the subs pinned");

    printf(failures ? "%d FAILED\n" : "osd sub schedule lock: all checks passed "
           "(%d schedules, %d swaps under stress)\n",
           failures ? failures : atomic_load(&stress_schedules), atomic_load(&stress_swaps));
    return failures ? 1 : 0;
}
