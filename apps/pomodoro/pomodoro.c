/* pomodoro.c — the pomodoro app's phase machine: see pomodoro.h. Pure C11, no SDK, no clock,
 * no <string.h> and no <math.h>: the whole file is integer arithmetic and hand-written text. */
#include "pomodoro.h"

/* N clamped into the cycle the settings allow, so nothing downstream can index off it. */
static int n_of(int focuses)
{
    return focuses < PM_N_MIN ? PM_N_MIN : focuses > PM_N_MAX ? PM_N_MAX : focuses;
}

int pm_cycle_len(int focuses) { return 2 * n_of(focuses); }

int pm_phase_at(int k, int focuses)
{
    const int last = pm_cycle_len(focuses) - 1;
    if (k < 0) k = 0;
    if (k > last) k = last;
    if (k == last) return PM_LONG;              /* the long break is the cycle's last slot */
    return (k & 1) ? PM_SHORT : PM_FOCUS;
}

int pm_focus_at(int k, int focuses)
{
    return pm_phase_at(k, focuses) == PM_FOCUS ? k / 2 + 1 : 0;
}

int pm_next_k(int k, int focuses)
{
    const int n = pm_cycle_len(focuses);
    return k + 1 >= n ? 0 : k + 1;
}

/* A setting is a length the machine can run: never under a minute (a 0 would make the ring's
 * denominator 0) and never over that setting's own max, whatever the caller stored for it. */
int pm_len_s(const pm_cfg_t *c, int phase)
{
    if (phase < 0 || phase > PM_LONG) return PM_MIN_S;
    int v = c->len_s[phase];
    if (v < PM_MIN_S) v = PM_MIN_S;
    if (c->cap_s[phase] >= PM_MIN_S && v > c->cap_s[phase]) v = c->cap_s[phase];
    return v;
}

/* ---- the gestures (§6) --------------------------------------------------------------- */

void pm_press(pm_t *s, const pm_cfg_t *c)
{
    switch (s->st) {
    case PM_RUN:                                /* freeze: the service keeps rem current */
        s->st = PM_PAUSE;
        s->end_s = 0;
        break;
    case PM_STOP:                               /* start at the length the knob left it at */
        if (s->len < PM_MIN_S) s->len = pm_len_s(c, pm_phase_at(s->k, c->focuses));
        s->rem = s->len;
        s->st = PM_RUN;
        s->end_s = 0;
        break;
    case PM_PAUSE:                              /* resume the kept seconds, end_s re-anchored */
        s->st = PM_RUN;
        s->end_s = 0;
        break;
    default:                                    /* WAIT, WAITED: the next phase, fresh */
        s->k = (int16_t)pm_next_k(s->k, c->focuses);
        s->len = pm_len_s(c, pm_phase_at(s->k, c->focuses));
        s->rem = s->len;
        s->st = PM_RUN;
        s->end_s = 0;
        break;
    }
}

int pm_turn(pm_t *s, const pm_cfg_t *c, int delta, int64_t now_s)
{
    const int cap = c->cap_s[pm_ring_phase(s, c->focuses)];
    int want, lim = 0;

    if (delta == 0) return PM_EV_NONE;
    if (delta > 90) delta = 90;                  /* a wild ask cannot overflow the seconds */
    if (delta < -90) delta = -90;
    /* Running, a turn moves whole minutes and keeps the seconds (12:34 -> 13:34). Stopped,
     * paused or waiting, it drops the seconds first, then moves (12:34 -> 13:00 or 11:00): a
     * time being set reads in whole minutes (owner, 2026-11-11). */
    want = (s->st == PM_RUN ? s->rem : s->rem / 60 * 60) + delta * 60;
    if (want > cap) { want = cap; lim = 1; }     /* the setting's own max, not its value */
    if (want < PM_MIN_S) { want = PM_MIN_S; lim = 1; }   /* never 0: that reads as finished */
    if (s->st == PM_RUN) {
        s->len += want - s->rem;                 /* the elapsed arc is never rewound */
        if (s->len < want) s->len = want;        /* and the denominator never trails rem */
        s->rem = want;
        s->end_s = now_s + want;
    } else {
        if (s->st == PM_WAIT || s->st == PM_WAITED) s->st = PM_PAUSE;   /* the waiting ends */
        s->len = want;                           /* STOP and PAUSE: the length is the turn */
        s->rem = want;
        s->end_s = 0;
    }
    return lim ? PM_EV_LIMIT : PM_EV_NONE;
}

int pm_at_start(const pm_t *s) { return s->len <= 0 || s->rem >= s->len; }

int pm_pos_step(int pos, int delta, int at_start, int k, int focuses)
{
    const int n = pm_cycle_len(focuses);
    const int lo = k > 0 ? -k - 1 : -1;          /* the anticlockwise end is the first focus, k = 0 */
    const int hi = n - 1 - k;                    /* the long break ends the clockwise run */
    int p = pos;

    if (p < lo) p = lo;
    if (p > hi) p = hi;
    if (delta > 0) {
        p += delta;
        return p > hi ? hi : p;
    }
    p += delta;
    if (pos == 0 && at_start && p < 0) p -= 1;   /* -1 is where this phase already is */
    return p < lo ? lo : p;
}

void pm_land(pm_t *s, const pm_cfg_t *c, int pos)
{
    int slot;

    if (pos == 0) return;                        /* the let-go where it stands: nothing moves */
    slot = pos < 0 ? s->k + pos + 1 : s->k + pos;
    if (slot < 0) slot = 0;
    if (slot > pm_cycle_len(c->focuses) - 1) slot = pm_cycle_len(c->focuses) - 1;
    s->k = (int16_t)slot;
    s->len = pm_len_s(c, pm_phase_at(slot, c->focuses));
    s->rem = s->len;
    s->st = PM_STOP;
    s->end_s = 0;
}

/* ---- the service and the store (§5) --------------------------------------------------- */

int pm_service(pm_t *s, const pm_cfg_t *c, int64_t now_s)
{
    if (s->st != PM_RUN) return PM_EV_NONE;
    if (s->end_s == 0) s->end_s = now_s + (s->rem > 0 ? s->rem : 0);   /* the caller's now */
    if (now_s < s->end_s) {
        s->rem = (int32_t)(s->end_s - now_s);    /* derived from the clock, never accumulated */
        return PM_EV_NONE;
    }
    if (!c->autostart) {
        s->st = PM_WAIT;
        s->rem = 0;
        s->end_s = 0;
        return PM_EV_END;
    }
    s->k = (int16_t)pm_next_k(s->k, c->focuses);
    s->len = pm_len_s(c, pm_phase_at(s->k, c->focuses));
    s->rem = s->len;
    s->end_s = now_s + s->len;
    return PM_EV_END | PM_EV_START;
}

/* One clamp for every stored number that becomes a running length. */
static int clamp_len(int v, int cap)
{
    if (v < PM_MIN_S) return PM_MIN_S;
    if (cap >= PM_MIN_S && v > cap) return cap;
    return v;
}

void pm_restore(pm_t *s, const pm_cfg_t *c, const pm_raw_t *raw, int synced, int64_t now_s)
{
    const int packed = raw->st_k < 0 ? 0 : raw->st_k;
    const int n = pm_cycle_len(c->focuses);
    int st = packed % PM_PACK_MUL;
    int k = packed / PM_PACK_MUL;
    int cap, rem, len;

    if (st > PM_WAITED) st = PM_WAITED;          /* a torn key is clamped, not trusted */
    if (k > n - 1) k = n - 1;
    s->k = (int16_t)k;
    cap = c->cap_s[pm_phase_at(k, c->focuses)];
    /* The length rides in pm_st with the state, written as one word, so it is the authority;
     * pm_end is a separate write, so a restore can meet it torn (an old phase's end second, ~1.8e9,
     * beside a new stopped phase). A remaining time past the length is that tear, never real:
     * it used to raise the length to itself and both clamped to the cap, the "jumps back to 90"
     * the owner hit on Bronze (2026-11-11). */
    len = clamp_len(raw->len, cap);
    rem = clamp_len(raw->rem, cap);
    if (rem > len) rem = len;
    if (packed == 0) {                           /* no key at all: a fresh cycle, ready */
        s->st = PM_STOP;
        s->len = s->rem = pm_len_s(c, PM_FOCUS);
        s->end_s = 0;
        return;
    }
    if (st == PM_STOP) {                         /* a stopped phase is its length: pm_end is not read */
        s->st = PM_STOP;
        s->len = s->rem = len;
        s->end_s = 0;
        return;
    }
    if (st == PM_WAIT || st == PM_WAITED) {       /* silent: that boundary had its chime */
        s->st = PM_WAITED;
        s->len = len;
        s->rem = 0;
        s->end_s = 0;
        return;
    }
    if (st == PM_RUN && synced) {
        const int64_t left = (int64_t)raw->end - now_s;
        s->len = len < (int)left ? (int)left : len;
        s->end_s = raw->end;
        if (left > 0) {
            s->st = PM_RUN;
            s->rem = (int32_t)left;
            return;
        }
        s->st = PM_WAITED;                       /* ended while away: held, and silent */
        s->rem = 0;
        s->end_s = 0;
        return;
    }
    s->st = PM_PAUSE;                            /* a RUN the clock cannot place is held */
    s->len = len;
    s->rem = rem;
    s->end_s = 0;
}

int pm_raw_needs_clock(const pm_raw_t *raw)
{
    return raw && raw->st_k > 0 && raw->st_k % PM_PACK_MUL == PM_RUN;
}

void pm_store(const pm_t *s, pm_raw_t *raw)
{
    raw->st_k = (int32_t)s->k * PM_PACK_MUL + s->st;
    raw->len = s->len;
    raw->end = (int32_t)s->end_s;
    raw->rem = s->rem;
}

int pm_changed(const pm_raw_t *a, const pm_raw_t *b)
{
    return a->st_k != b->st_k || a->len != b->len || a->end != b->end || a->rem != b->rem;
}

/* ---- what the ring should be (§7) ----------------------------------------------------- */

int pm_ring_alpha(int base_alpha, int opacity)
{
    if (opacity < 0) opacity = 0;
    if (opacity > 100) opacity = 100;
    return base_alpha * opacity / 100;           /* rounded down: the ring dims, never brightens */
}

int pm_ring_phase(const pm_t *s, int focuses) { return pm_phase_at(s->k, focuses); }

int pm_ring_effect(const pm_t *s)
{
    switch (s->st) {
    case PM_RUN:                       return 0;  /* ORB_RING_STEADY  */
    case PM_PAUSE:                     return 2;  /* ORB_RING_BREATHE */
    case PM_WAIT: case PM_WAITED:      return 1;  /* ORB_RING_FLASH   */
    default:                           return -1; /* PM_STOP: clear   */
    }
}

int pm_fraction(const pm_t *s)
{
    if (s->len <= 0) return s->rem > 0 ? 1000 : 0;   /* a 0-length phase divides nothing */
    int f = (int)((int64_t)s->rem * 1000 / s->len);
    if (f < 0) f = 0;
    if (f > 1000) f = 1000;
    return f;
}

int pm_ring_for(const pm_t *s, const pm_cfg_t *c, pm_ring_t *out)
{
    const int eff = pm_ring_effect(s);
    const uint32_t rgb = c->col[pm_ring_phase(s, c->focuses)] & 0x00FFFFFFu;

    if (eff < 0 || !c->ring_on || c->opacity <= 0) return 0;   /* cleared, not invisible */
    /* One write of the whole record: a field the app does not name is zero, and the three asks
     * rim-1 adds are named here as plainly as the five that were always sent (rim-design §8). */
    *out = (pm_ring_t){ .fraction = pm_fraction(s),
                        .effect = eff,
                        .arc = ((uint32_t)pm_ring_alpha(PM_ARC_A, c->opacity) << 24) | rgb,
                        .track = ((uint32_t)pm_ring_alpha(PM_TRACK_A, c->opacity) << 24) | rgb,
                        .period_ms = eff == 2 ? PM_BREATHE_MS : eff == 1 ? PM_FLASH_MS : 0,
                        .dir = c->dir ? 1 : 0,                      /* the record's CCW ask     */
                        .start = ((c->start % 360) + 360) % 360,    /* the fold the OS applies  */
                        .match = c->match ? 1 : 0 };                /* off-glass, by the caller */
    return 1;
}

/* ---- the words (§10.1) ---------------------------------------------------------------- */

/* A literal into the caller's buffer, truncated rather than overrun. */
static void put(char *buf, int cap, const char *lit)
{
    int i = 0;
    if (cap <= 0) return;
    while (lit[i] && i < cap - 1) { buf[i] = lit[i]; i++; }
    buf[i] = 0;
}

static int digits(char *d, int v)                /* v 0..999, most significant first */
{
    char rev[4];
    int n = 0, i;
    do { rev[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 4);
    for (i = 0; i < n; i++) d[i] = rev[n - 1 - i];
    return n;
}

void pm_state_text(const pm_t *s, char *buf, int cap)
{
    if (s->st == PM_PAUSE) put(buf, cap, "Paused");
    else if (s->st == PM_WAIT || s->st == PM_WAITED) put(buf, cap, "Waiting");
    else put(buf, cap, "");                      /* ready and running say nothing */
}

void pm_phase_text(const pm_t *s, int focuses, char *buf, int cap)
{
    switch (pm_ring_phase(s, focuses)) {
    case PM_SHORT: put(buf, cap, "Short break"); break;
    case PM_LONG:  put(buf, cap, "Long break");  break;
    default:       put(buf, cap, "Focus");        break;
    }
}

void pm_count_text(const pm_t *s, int focuses, char *buf, int cap)
{
    char t[16];
    int n;
    const int f = pm_focus_at(s->k, focuses);

    if (f <= 0) { put(buf, cap, "break"); return; }   /* either break counts as nothing */
    n = digits(t, f);
    t[n++] = ' '; t[n++] = 'o'; t[n++] = 'f'; t[n++] = ' ';
    n += digits(t + n, n_of(focuses));
    t[n] = 0;
    put(buf, cap, t);
}
