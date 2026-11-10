// See test_ao_avfoundation_stall.sh.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "avp_stall.inc"

#define S(x) ((int64_t)((x) * 1e9))

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct avp_stall fresh(double timeout)
{
    struct avp_stall s = { .timeout_ns = S(timeout) };
    avp_stall_reset(&s);
    return s;
}

int main(void)
{
    const int64_t t0 = S(1000);

    // A player that keeps moving is never touched, however long it plays.
    struct avp_stall s = fresh(4);
    bool quiet = true;
    for (int i = 0; i <= 600; i++) // 30 s of 50 ms ticks
        quiet &= avp_stall_check(&s, t0 + i * S(0.05), i * S(0.05), true, false) == AVP_STALL_NONE;
    CHECK(quiet, "a moving playhead was acted on");

    // Stuck: nothing before the timeout, one restart at it, then the
    // fallback a timeout later.
    s = fresh(4);
    CHECK(avp_stall_check(&s, t0, S(10), true, false) == AVP_STALL_NONE, "opening the window");
    CHECK(avp_stall_check(&s, t0 + S(4) - 1, S(10), true, false) == AVP_STALL_NONE, "before the timeout");
    CHECK(avp_stall_check(&s, t0 + S(4), S(10), true, false) == AVP_STALL_RESTART, "restart at the timeout");
    CHECK(avp_stall_check(&s, t0 + S(4.05), S(10), true, false) == AVP_STALL_NONE, "a fresh window after it");
    CHECK(avp_stall_check(&s, t0 + S(8) - 1, S(10), true, false) == AVP_STALL_NONE, "before the second timeout");
    CHECK(avp_stall_check(&s, t0 + S(8), S(10), true, false) == AVP_STALL_FALLBACK, "fallback at the second");

    // The restart worked: a move clears the count, so a later stall gets
    // its own restart.
    s = fresh(4);
    avp_stall_check(&s, t0, S(10), true, false);
    CHECK(avp_stall_check(&s, t0 + S(4), S(10), true, false) == AVP_STALL_RESTART, "restart");
    CHECK(avp_stall_check(&s, t0 + S(5), S(10.5), true, false) == AVP_STALL_NONE, "it moves");
    CHECK(avp_stall_check(&s, t0 + S(9) - 1, S(10.5), true, false) == AVP_STALL_NONE, "stuck again, early");
    CHECK(avp_stall_check(&s, t0 + S(9), S(10.5), true, false) == AVP_STALL_RESTART,
          "a new stall after a recovery restarts again rather than falling back");

    // Pause as the driver does it: hold at pause, no checks at all while
    // paused (the feed timer is idle), hold again at resume. A playhead that
    // was already still for most of a window before the pause must not be
    // restarted the moment playback resumes.
    s = fresh(4);
    avp_stall_check(&s, t0, S(10), true, false);
    CHECK(avp_stall_check(&s, t0 + S(3.9), S(10), true, false) == AVP_STALL_NONE, "still, before pause");
    avp_stall_hold(&s);                       // set_pause(true)
    avp_stall_hold(&s);                       // set_pause(false), 60 s later
    CHECK(avp_stall_check(&s, t0 + S(63.9), S(10), true, false) == AVP_STALL_NONE,
          "resume after a pause restarted at once");
    CHECK(avp_stall_check(&s, t0 + S(67.9) - 1, S(10), true, false) == AVP_STALL_NONE,
          "resume: the window is a fresh one");
    CHECK(avp_stall_check(&s, t0 + S(67.9), S(10), true, false) == AVP_STALL_RESTART,
          "a full window after resuming");

    // AVPlayer waiting to play (rebuffering): AVP_STALL_WAIT_FACTOR times
    // the allowance, then the same restart and fallback, so a wait that
    // never ends still reaches PCM.
    s = fresh(4);
    avp_stall_check(&s, t0, S(10), true, true);
    CHECK(avp_stall_check(&s, t0 + S(4), S(10), true, true) == AVP_STALL_NONE,
          "waiting player restarted at the plain timeout");
    CHECK(avp_stall_check(&s, t0 + S(16) - 1, S(10), true, true) == AVP_STALL_NONE,
          "waiting player, just before the longer allowance");
    CHECK(avp_stall_check(&s, t0 + S(16), S(10), true, true) == AVP_STALL_RESTART,
          "waiting player: restart at %d x the timeout", AVP_STALL_WAIT_FACTOR);
    CHECK(avp_stall_check(&s, t0 + S(32), S(10), true, true) == AVP_STALL_FALLBACK,
          "a wait that never ends still falls back");
    // Waiting that turns into a plain stall: the plain timeout applies from
    // the last move.
    s = fresh(4);
    avp_stall_check(&s, t0, S(10), true, true);
    CHECK(avp_stall_check(&s, t0 + S(5), S(10), true, false) == AVP_STALL_RESTART,
          "a player that stops waiting but still does not move");

    // Not expected to move (paused, interrupted, starting, starved of
    // stream): never acted on, and the time does not count.
    s = fresh(4);
    avp_stall_check(&s, t0, S(10), true, false);
    CHECK(avp_stall_check(&s, t0 + S(3), S(10), false, false) == AVP_STALL_NONE, "paused");
    CHECK(avp_stall_check(&s, t0 + S(60), S(10), false, false) == AVP_STALL_NONE, "paused for long");
    CHECK(avp_stall_check(&s, t0 + S(61), S(10), true, false) == AVP_STALL_NONE, "resumed: a fresh window");
    CHECK(avp_stall_check(&s, t0 + S(65) - 1, S(10), true, false) == AVP_STALL_NONE,
          "the paused time did not count");
    CHECK(avp_stall_check(&s, t0 + S(65), S(10), true, false) == AVP_STALL_RESTART, "a full window later");

    // A pause between the restart and the next window does not buy a
    // second restart: still stuck after it, the fallback comes.
    CHECK(avp_stall_check(&s, t0 + S(66), S(10), false, false) == AVP_STALL_NONE, "paused after the restart");
    avp_stall_hold(&s);
    CHECK(avp_stall_check(&s, t0 + S(70), S(10), true, false) == AVP_STALL_NONE, "resumed");
    CHECK(avp_stall_check(&s, t0 + S(74), S(10), true, false) == AVP_STALL_FALLBACK,
          "still stuck: the fallback, not another restart");

    // A new item starts over.
    avp_stall_reset(&s);
    avp_stall_check(&s, t0 + S(80), 0, true, false);
    CHECK(avp_stall_check(&s, t0 + S(84), 0, true, false) == AVP_STALL_RESTART, "reset clears the count");

    // A playhead that goes backwards (a new timeline) is not a move, and
    // the window keeps running.
    s = fresh(4);
    avp_stall_check(&s, t0, S(10), true, false);
    CHECK(avp_stall_check(&s, t0 + S(2), S(9), true, false) == AVP_STALL_NONE, "backwards, early");
    CHECK(avp_stall_check(&s, t0 + S(4), S(9), true, false) == AVP_STALL_RESTART, "backwards is not progress");

    // Timeout 0 turns it off.
    s = fresh(0);
    avp_stall_check(&s, t0, S(10), true, false);
    CHECK(avp_stall_check(&s, t0 + S(1000), S(10), true, false) == AVP_STALL_NONE, "off");

    printf(failures ? "%d FAILED\n" : "avp stall watchdog: all checks passed\n", failures);
    return failures ? 1 : 0;
}
