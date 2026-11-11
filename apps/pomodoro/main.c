/* main.c — the pomodoro (docs/specs/pomodoro-design.md §3-§10, plan §64.3).
 *
 * A cycle of N focuses, a short break after each and a long break at the end, run off the
 * glass. The machine is pomodoro.c (pure, host-tested): this file is the face over it, the
 * four gestures, the OS ring and the chime. Turn is a minute a click, press starts, pauses,
 * resumes and moves on over, press-and-turn chooses a phase and lands on the let-go, and a
 * hold is the OS's. The dial is a value dial, so nothing about a time the owner just set is
 * undone by walking away: no orb_dial.h, no idle return (§6.2).
 *
 * The countdown lives in the service export and is kept against the wall second, never a
 * tick count, so it outlives a reboot and one of the OS's in-place restarts (§63). The
 * service draws nothing: the ring is the only OS surface it touches, and the face is drawn
 * by the entries that mean the glass. */
#include <string.h>
#include "orb.h"
#include "orb_stage.h"
#include "pomodoro.h"

#define CX 233
#define CY 233

/* The theme's slots (§10.1): five labels and one bank of dots the app picks a frame from.
 * A theme with no time row is not this app's layout and takes the bare face. */
enum { PM_T_TIME = 8, PM_T_PHASE = 9, PM_T_COUNT = 10, PM_T_STATE = 11, PM_T_HEAD = 12,
       PM_T_DOTS = 13 };
enum { S_TIME, S_PHASE, S_COUNT, S_STATE, S_HEAD, S_DOTS, N_SLOTS };
#define N_ROWS 64

/* The theme's values (amendment B): the ring's colour per phase, and which of the theme's
 * sounds the end of each phase strikes. Slot 0 is the clock's tick loop and the projector
 * refuses a one-shot there, so a 0 names no chime (§64.7, amendment K). */
typedef struct {
    uint32_t ring_rgb[3];          /* 0xRRGGBB for focus, short break, long break */
    uint8_t  chime[3];
    uint8_t  _res;
} pm_values_t;
_Static_assert(sizeof(pm_values_t) == 16, "the theme's values are 16 bytes");
#define PM_VALUES_VER  1
#define PM_NO_CHIME    0

static orb_stage_info_t s_stage;
static bool     s_themed;
static int32_t  R[N_SLOTS];
static pm_values_t V;
static int32_t  s_cv;                          /* the bare face's canvas; 0 on a theme */
static int64_t  s_stage_retry_ms;

/* The machine, the settings that feed it and the keys that store it. */
static pm_cfg_t cfg;
static pm_t     s;
static int32_t s_key_a, s_key_b;              /* the two keys, as we last knew them */
static int      s_turned_k = -1;              /* the slot the knob made longer or shorter (§4):
                                               * whose length a settings move may not take back */
static uint32_t s_cfg_gen;
static int      s_chime_on = 1, s_chime_vol = 70, s_wake = 1, s_dir = 0, s_start = 0;
static int32_t  s_col_here = -1, s_col_away = -1;
static int64_t  s_wrote_ms;                    /* our own write: it lands late, off the tick */
static bool     s_keys_wait;                   /* a state reload that write put off */

/* The glass. */
static bool     s_visible;
static bool     s_dirty = true;
static char     s_shown[N_SLOTS][PM_TEXT];     /* what the rows already carry */
static int8_t   s_tone_out[N_SLOTS];
static int      s_frame_out = -1, s_sec_out = -1;

/* The gestures. */
#define PM_CHOICE_LOST_MS 60000               /* radar's ZOOM_LOST_MS: a let-go that never came */
#define PM_LIMIT_SAY_MS    1200               /* the stop mark on the time row, as radar's pill */
static bool     s_choosing;
static int      s_pos;
static int64_t  s_click_ms, s_limit_ms;

/* The ring: the last record sent, so a steady phase re-sets it a few times a minute. */
#define PM_REFUSE_RETRY_MS 60000
static pm_ring_t s_sent;
static bool      s_sent_valid, s_clear_sent;
static int64_t   s_ring_retry_ms;

static int64_t up_ms(void) { return orb_uptime_us() / 1000; }
static int32_t now_s(int64_t *sec) { int32_t f; return orb_now(sec, &f); }   /* the sync flag, §5.2 */
static int64_t wall(void) { int64_t t; now_s(&t); return t; }

static void face_forget(void);
static void redraw(void);

/* ---- the theme ---------------------------------------------------------------------- */

static void values_default(void)
{
    V.ring_rgb[PM_FOCUS] = 0xD62828;                /* Ruby: the tomato */
    V.ring_rgb[PM_SHORT] = 0x0FA36B;                /* Emerald: go */
    V.ring_rgb[PM_LONG]  = 0x2E93E4;                /* Sky: away from both */
    V.chime[PM_FOCUS] = V.chime[PM_SHORT] = V.chime[PM_LONG] = PM_NO_CHIME;
    V._res = 0;
}

/* Read when the stage opens and when it changes, so the service has the colours and the
 * slots off the glass: with no values (no theme, a short or wrong-version blob) the
 * defaults stand, and the app runs on them (amendment B). */
static void read_values(void)
{
    uint32_t ver = 0;
    pm_values_t got;
    values_default();
    const uint32_t n = s_themed ? orb_stage_values(&got, sizeof got, &ver) : 0;
    if (n == sizeof got && ver == PM_VALUES_VER) memcpy(&V, &got, sizeof V);
    else if (n) orb_logf("pomodoro: values are layout %u, %u B: the defaults ring",
                         (unsigned)ver, (unsigned)n);
    for (int i = 0; i < 3; i++) V.ring_rgb[i] &= 0x00FFFFFFu;
}

static void find_rows(void)
{
    memset(R, 0xFF, sizeof R);                      /* every row absent */
    for (int32_t i = 0; i < (int32_t)s_stage.n_rows && i < N_ROWS; i++) {
        clk_row_t r;
        if (orb_stage_row(i, &r) != ORB_OK) continue;
        switch (r.tag) {
        case PM_T_TIME:  R[S_TIME]  = i; break;
        case PM_T_PHASE: R[S_PHASE] = i; break;
        case PM_T_COUNT: R[S_COUNT] = i; break;
        case PM_T_STATE: R[S_STATE] = i; break;
        case PM_T_HEAD:  R[S_HEAD]  = i; break;
        case PM_T_DOTS:
            if (r.shape != CLK_SHAPE_TEXT) R[S_DOTS] = i;      /* the dots are art, not a label */
            break;
        default: break;
        }
    }
}

/* The stage, read afresh: at a boot the theme may not be bound while the app builds and a
 * theme may be swapped, so every arrival asks and the service asks again every 2 s while it
 * has none (radar's rule, and its reason: Bronze, 2026-11-08). No widget stands in for it:
 * the bare face is a canvas, and the stage's veto on one is radar's own lesson. */
static void stage_read(bool may_bare)
{
    const bool was = s_themed;
    s_themed = orb_stage_open(&s_stage) == ORB_OK && !s_stage.base;
    if (s_themed) find_rows();
    if (s_themed && R[S_TIME] < 0) s_themed = false;         /* not this app's layout */
    read_values();
    if (!s_themed && !s_cv && may_bare) {
        s_cv = orb_cv_create(0, CX * 2, CY * 2);
        orb_logf("pomodoro: no theme: the bare face%s", s_cv ? "" : ", but its canvas was refused");
    }
    const uint32_t beat = s_themed ? 0 : s_cv ? 1000 : 2000;
    if (orb_set_beat(beat) != ORB_OK) orb_logf("pomodoro: a %u ms beat was refused", (unsigned)beat);
    if (was != s_themed) {
        face_forget();
        orb_logf("pomodoro: %s", s_themed ? "on its theme" : "no theme bound yet: asking again");
    }
    s_stage_retry_ms = up_ms() + (may_bare ? 2000 : 300);
}

/* ---- the settings, and the store (§4, §5) ------------------------------------------- *
 * The state is two keys, not four (amendment G): the slot and the run state in a byte, this
 * phase's length in thirteen of the first, and the second key the end second of a running
 * phase or the remaining seconds of any other. A run the clock cannot place is the one thing
 * two int32s cannot carry at once (31 + 13 + 13 bits past the 62 the pair holds), and
 * pm_restore's own answer for it is a held phase at its floor. */
#define PM_A_MUL  8192                              /* pm_st = (k * 8 + state) * PM_A_MUL + len */
#define PM_A_MAX  2000000                           /* (200 * 8192) + 5400, with room */
#define PM_B_MAX  5400                              /* the longest phase the other key holds */

static int32_t cfg_get(const char *id, int32_t dflt)
{
    int32_t v = dflt;
    return orb_cfg_get(id, &v) == ORB_OK ? v : dflt;
}

/* The fourteen the owner edits, in one table so a read can be compared with the last one.
 * The lengths and N reach the machine as its settings, which it applies at the next phase;
 * the ring's three and the sound's two take effect at once. Answering whether anything moved
 * is the point of the table: the app's own state write moves orb_cfg_gen too (§5.1), and a
 * gen move that changed no value is not a reason to re-send a ring or to take back a turn. */
#define PM_N_SET 14
static const struct { const char *id; int32_t dflt; } PM_SET[PM_N_SET] = {
    { "focus_len", 25 },     { "break_len", 5 },      { "long_len", 15 },
    { "focuses", 4 },        { "autostart", 0 },      { "chime", 1 },
    { "chime_vol", 70 },     { "wake", 1 },           { "ring_on", 1 },
    { "ring_op", 85 },       { "ring_dir", 0 },       { "ring_start", 0 },
    { "ring_col_here", -1 }, { "ring_col_away", -1 },
};
static int32_t s_val[PM_N_SET];                  /* the fourteen, as last read */
static bool    s_val_known;

static bool load_settings(void)
{
    const int32_t cap[3] = { 5400, 1800, 3600 };    /* app.json's maxima, in seconds */
    int32_t v[PM_N_SET], moved = 0;
    for (int i = 0; i < PM_N_SET; i++) {
        v[i] = cfg_get(PM_SET[i].id, PM_SET[i].dflt);
        if (s_val_known && v[i] != s_val[i]) moved = 1;
    }
    memcpy(s_val, v, sizeof s_val);
    s_val_known = true;
    cfg.len_s[PM_FOCUS] = 60 * v[0];
    cfg.len_s[PM_SHORT] = 60 * v[1];
    cfg.len_s[PM_LONG]  = 60 * v[2];
    memcpy(cfg.cap_s, cap, sizeof cfg.cap_s);
    cfg.focuses   = v[3];
    cfg.autostart = v[4];
    cfg.ring_on   = v[8];
    cfg.opacity   = v[9];
    s_chime_on    = v[5];
    s_chime_vol   = v[6];
    s_wake        = v[7];
    s_dir         = v[10];
    s_start       = v[11] * 90;                     /* the face's o'clock, clockwise */
    s_col_here    = v[12];
    s_col_away    = v[13];
    s_cfg_gen     = orb_cfg_gen();
    return moved != 0;
}

/* The two keys back into the four the machine reads, with the RUN phase's seconds key read
 * as its end. Nothing here is trusted: pm_restore does the clamping (§5.2). */
static void key_pair(int32_t *a, int32_t *b)
{
    *a = cfg_get("pm_st", 0);
    *b = cfg_get("pm_end", 0);
    if (*a < 0 || *a > PM_A_MAX) *a = 0;                   /* a torn key is no state */
    if (*b < 0) *b = 0;
}

static void read_raw(pm_raw_t *raw)
{
    int32_t a, b;
    key_pair(&a, &b);
    raw->st_k = (int32_t)(a / PM_A_MUL);
    raw->len  = (int32_t)(a % PM_A_MUL);
    if (raw->st_k % PM_PACK_MUL == PM_RUN) { raw->end = b; raw->rem = 0; }
    else { raw->end = 0; raw->rem = b > PM_B_MAX ? PM_B_MAX : b; }
}

/* The state's half of the pair, the way it is carried. The comparison the write and the
 * restore trigger on is of these two words and never of the raw struct: a running phase's
 * remaining seconds are not in the store, so a struct comparison would find a change every
 * second of every focus. */
static void keys_of(const pm_t *m, int32_t *a, int32_t *b)
{
    pm_raw_t r;
    pm_store(m, &r);
    *a = r.st_k * PM_A_MUL + r.len;
    *b = r.st_k % PM_PACK_MUL == PM_RUN ? r.end : (r.rem > PM_B_MAX ? PM_B_MAX : r.rem);
}

/* Only on a state change, never on a beat (§5.1): a running phase writes nothing for its
 * whole 25 minutes, which is why an end second is stored rather than a countdown. */
/* A running phase restored before the clock synced (every boot: the app starts ~1.4 s in,
 * SNTP lands ~5 s in). Its stored end is the only record of when it finishes, so it is held
 * here and never overwritten until the clock can place it; the service finishes the restore. */
static pm_raw_t s_pending_raw;
static bool     s_restore_pending;

static void save_if_moved(void)
{
    if (s_restore_pending) return;   /* the store still holds the running phase's real end */
    int32_t a, b;
    keys_of(&s, &a, &b);
    if (a == s_key_a && b == s_key_b) return;
    (void)orb_cfg_set("pm_st", a);
    (void)orb_cfg_set("pm_end", b);
    s_key_a = a;
    s_key_b = b;                     /* what we are asking for is what the keys hold now */
    s_wrote_ms = up_ms();
}

static bool own_write_inflight(void) { return s_wrote_ms && up_ms() - s_wrote_ms < 3000; }

/* A press leaves a run unanchored until the service has put a wall second on it: reading the
 * store in between would take our own unwritten end for a phase that ended long ago. */
static bool state_settled(void) { return s.st != PM_RUN || s.end_s != 0; }

/* A settings move, from the dial or the LAN: the fourteen re-read at once when the gen moved,
 * and the state only when the keys hold something other than what we last wrote and that write
 * has landed (§5.1: the app's own write moves orb_cfg_gen too, off the tick, and lands late).
 * A move of the keys alone is not a move of the ring's or the face's name: only a settings
 * move invalidates what was sent, which is what keeps a held-over retry from re-sending. */
static void load_and_restore(bool cfg_moved)
{
    int32_t a, b, was[3];
    memcpy(was, cfg.len_s, sizeof was);
    const bool moved = cfg_moved && load_settings();      /* a gen move that moved no value is
                                                            the app's own write landing */
    key_pair(&a, &b);
    s_keys_wait = (a != s_key_a || b != s_key_b);
    if (s_keys_wait && !own_write_inflight() && state_settled()) {
        pm_raw_t got;
        int64_t t;
        read_raw(&got);
        const int32_t sync = now_s(&t);                    /* the second is read before it is used:
                                                            * a call's arguments have no order */
        pm_restore(&s, &cfg, &got, (int)sync, t);          /* §5.2, with the sync flag */
        s_key_a = a;
        s_key_b = b;
        s_keys_wait = false;
        s_dirty = true;                              /* the face read what the keys said */
        s_turned_k = got.st_k ? s.k : -1;             /* a restored length may be a turned one;
                                                      * a fresh cycle's never is */
        if (s.st == PM_STOP) save_if_moved();        /* a clamped restore names its own length */
    }
    if (moved && s.st == PM_STOP) {                  /* a phase not yet started and not turned
                                                     * takes a length the owner changed (§4) */
        const int ph = pm_phase_at(s.k, cfg.focuses);
        if (cfg.len_s[ph] != was[ph] && s_turned_k != s.k) {
            s.len = s.rem = pm_len_s(&cfg, ph);
            s_dirty = true;
        }
    }
    if (moved) {
        s_ring_retry_ms = 0;                      /* the owner may have granted the ring back */
        s_sent_valid = false;                     /* anything the record names may have moved */
        s_clear_sent = false;
        s_dirty = true;
    }
}

/* ---- the labels (§10.1): the app says what the words are and their tone ------------- */

static void face_forget(void)
{
    for (int i = 0; i < N_SLOTS; i++) s_shown[i][0] = 0;
    s_tone_out[S_TIME] = s_tone_out[S_PHASE] = s_tone_out[S_COUNT] = 0;
    s_tone_out[S_STATE] = s_tone_out[S_HEAD] = 0;
    s_frame_out = -1;
    s_sec_out = -1;
}

static void pose(int32_t row)
{
    if (row >= 0) orb_stage_pose(row, 0, 0, 0);          /* the look baked where it wanted it */
}

static void hide(int32_t row)
{
    if (row >= 0) orb_stage_pose(row, ORB_STAGE_HIDDEN, 0, 0);
}

/* A row is sent when its words or its tone moved: the face repaints on every gesture and on
 * every beat, and this glass is nearly still. */
static void say(int slot, const char *txt, int tone)
{
    const int32_t row = R[slot];
    if (row < 0) return;
    if (strcmp(s_shown[slot], txt) || s_tone_out[slot] != (int8_t)tone) {
        snprintf(s_shown[slot], sizeof s_shown[slot], "%s", txt);
        s_tone_out[slot] = (int8_t)tone;
        orb_stage_text(row, txt, tone);
    }
    if (txt[0]) pose(row);
    else hide(row);
}

/* mm:ss and never h:mm:ss: the longest phase is 90 minutes. Over and waiting the machine
 * holds 0, which is the figure a finished phase shows while it waits. */
static void time_text(char *b, int n)
{
    const int r = s.rem < 0 ? 0 : s.rem;
    snprintf(b, n, "%02d:%02d", r / 60, r % 60);
}

/* The choice's name. pm_land on a copy of the state is how pos becomes a slot: the mapping
 * is the let-go's own rule, and lives in pomodoro.c rather than twice. */
static void head_text(char *b, int n)
{
    pm_t to = s;
    pm_land(&to, &cfg, s_pos);                          /* pos to slot, as the let-go does */
    pm_phase_text(&to, cfg.focuses, b, n);
    const int f = pm_focus_at(to.k, cfg.focuses);
    int len = (int)strlen(b);
    if (f && len + 4 < n) len += snprintf(b + len, (size_t)(n - len), " %d", f);
    if (s_pos == -1 && len + 14 < n) snprintf(b + len, (size_t)(n - len), "%s", " at its start");
}

/* ---- the OS ring (§7): one path, from the service ----------------------------------- */

/* Which colour the ring wears (amendment H): the owner's setting for here or for away, and
 * a negative special ("From the look", and "Match the app" until the overlay slice gives the
 * record a flag: §64.9) the theme's own colour for the phase in hand. */
static void pick_colours(void)
{
    const int32_t sel = s_visible ? s_col_here : s_col_away;
    for (int i = 0; i < 3; i++) cfg.col[i] = sel >= 0 ? (uint32_t)sel : V.ring_rgb[i];
}

/* PM_STEP_PCT is the core's threshold and the only throttle here: the OS clamps a steady
 * fraction send at its own 5 per mille and repaints on its own 500 ms grid (ring-design §6),
 * so a second app-side clamp would only duplicate a clamp the OS already owns. */
static bool ring_moved(const pm_ring_t *r)
{
    if (!s_sent_valid) return true;
    if (s_sent.effect != r->effect || s_sent.period_ms != r->period_ms) return true;
    if (s_sent.arc != r->arc || s_sent.track != r->track) return true;   /* colour, both alphas */
    const int d = s_sent.fraction > r->fraction ? s_sent.fraction - r->fraction
                                                : r->fraction - s_sent.fraction;
    return d >= PM_STEP_PCT || (r->fraction == 0) != (s_sent.fraction == 0);
}

static void ring_send(void)
{
    pm_ring_t r;
    if (up_ms() < s_ring_retry_ms) return;                /* refused: not asked every pass */
    pick_colours();
    if (!pm_ring_for(&s, &cfg, &r)) {                     /* pm_ring_effect's -1, folded in */
        if (s_clear_sent) return;                         /* once, never once a second */
        if (orb_ring_clear() == ORB_ERR_NOT_ALLOWED) {
            s_ring_retry_ms = up_ms() + PM_REFUSE_RETRY_MS;   /* the cap is the owner's (§64.1) */
            return;
        }
        s_clear_sent = true;
        s_sent_valid = false;
        return;
    }
    if (!ring_moved(&r)) return;
    orb_ring_t rec;
    memset(&rec, 0, sizeof rec);
    rec.fraction = (uint16_t)r.fraction;
    rec.effect = (uint8_t)(r.effect == 2 ? ORB_RING_BREATHE : r.effect == 1 ? ORB_RING_FLASH
                                                                            : ORB_RING_STEADY);
    rec.arc = r.arc;
    rec.track = r.track;
    rec.period_ms = (uint16_t)r.period_ms;
    /* s_dir and s_start wait on the record's flags and start (amendment J): this base's
     * orb_ring_t still reserves both words, so the ring drains clockwise from twelve. */
    if (orb_ring_set(&rec) == ORB_ERR_NOT_ALLOWED) {
        s_ring_retry_ms = up_ms() + PM_REFUSE_RETRY_MS;
        return;
    }
    s_sent = r;
    s_sent_valid = true;
    s_clear_sent = false;
}

/* ---- the chime and the wake (§9, amendment 2), both from the service ---------------- */

/* One strike on the pass that found the boundary, and never a second one: a WAIT still owes
 * its chime, and is spent here before this returns, so a restart over a theme landing cannot
 * sound it again. Under autostart the next phase has already started and the machine answers
 * that boundary exactly once, so the event itself is the once. The start, a pause and a
 * landed choice are silent (§9), and chime_vol at 0 is silence. The level rides the play's
 * flags (ORB_SOUND_VOL), which works off the glass too, and the OS never lets it pass the master. */
static void chime(int ended)
{
    const int slot = V.chime[ended];
    if (s_chime_on && s_chime_vol > 0 && slot > PM_NO_CHIME)
        (void)orb_sound_play((uint32_t)slot, ORB_SOUND_VOL(s_chime_vol), 0);   /* a share of the master (§64.6) */
    if (s.st == PM_WAIT) s.st = PM_WAITED;
}

/* Light a dimmed glass for a glance, never taking it (a timer that ends unseen has failed).
 * The OS's own minute, its quiet hours and a display the owner switched off all answer, and
 * all are the answer: the ring is still flashing. */
static void wake_glass(void)
{
    if (s_wake) (void)orb_wake(5);
}

/* ---- the face ----------------------------------------------------------------------- */

static void face_rows(void)
{
    char t[PM_TEXT], h[48];
    const bool over = s.st == PM_WAIT || s.st == PM_WAITED;
    time_text(t, sizeof t);
    say(S_TIME, t, over || s_limit_ms ? ORB_TONE_WARNING : ORB_TONE_NORMAL);
    pm_phase_text(&s, cfg.focuses, t, sizeof t);
    say(S_PHASE, t, pm_ring_phase(&s, cfg.focuses) == PM_FOCUS ? ORB_TONE_NORMAL : ORB_TONE_ACCENT);
    pm_count_text(&s, cfg.focuses, t, sizeof t);
    say(S_COUNT, t, ORB_TONE_QUIET);
    pm_state_text(&s, t, sizeof t);
    say(S_STATE, t, over ? ORB_TONE_WARNING : ORB_TONE_QUIET);
    if (s_choosing) { head_text(h, sizeof h); say(S_HEAD, h, ORB_TONE_ACCENT); }
    else say(S_HEAD, "", ORB_TONE_ACCENT);              /* the header is the choice's row */
    if (R[S_DOTS] >= 0) {
        const int f = pm_focus_at(s.k, cfg.focuses);     /* 0 filled on either break */
        if (f != s_frame_out) { orb_stage_frame(R[S_DOTS], f); s_frame_out = f; pose(R[S_DOTS]); }
    }
    s_sec_out = s.rem;
}

/* No theme, or one that is not this app's layout (§10.3): the words in the system fonts on a
 * canvas, the figure in the phase's own colour. The ring, the chime, the knob and the state
 * all still work; only the face is plain. */
#define BARE_INK  0xEDEDF3                              /* Pearl, the OS's own light grey */
#define BARE_QUIET 0xA8A8A8
static void bare_draw(void)
{
    char t[PM_TEXT], line[48], p[PM_TEXT], c[PM_TEXT], q[PM_TEXT];
    const bool over = s.st == PM_WAIT || s.st == PM_WAITED;
    orb_cv_fill(s_cv, 0x000000);
    time_text(t, sizeof t);
    orb_cv_text(s_cv, 0, CY - 44, CX * 2, ORB_FONT_48 | ORB_CV_CENTER,
                over || s_limit_ms ? 0xFFC300u : cfg.col[pm_ring_phase(&s, cfg.focuses)], t);
    if (s_choosing) head_text(line, sizeof line);        /* the choice takes the line's place */
    else {
        pm_phase_text(&s, cfg.focuses, p, sizeof p);
        pm_count_text(&s, cfg.focuses, c, sizeof c);
        pm_state_text(&s, q, sizeof q);
        snprintf(line, sizeof line, "%s  %s%s%s", p, c, q[0] ? "  " : "", q);
    }
    orb_cv_text(s_cv, 0, CY + 30, CX * 2, ORB_FONT_18 | ORB_CV_CENTER, BARE_QUIET, line);
    orb_cv_flush(s_cv);
    s_sec_out = s.rem;
}

/* The one drawing entry. Off the glass it does nothing but keep the flag, which is what
 * carries a change made three apps away to orb_resume. */
static void redraw(void)
{
    if (!s_visible) { s_dirty = true; return; }
    s_dirty = false;
    if (s_themed) face_rows();
    else if (s_cv) bare_draw();
}

/* The gathered choice lands: on the let-go, and on the safety timer for an OS that sends
 * none (an end that never arrives would otherwise hold the countdown still for good). */
static void choice_step(void)
{
    if (!s_choosing || (s_click_ms && up_ms() - s_click_ms < PM_CHOICE_LOST_MS)) return;
    pm_land(&s, &cfg, s_pos);
    s_choosing = false;
    s_pos = 0;
    s_click_ms = 0;
    s_turned_k = -1;                    /* a landed phase is at its setting's length */
    redraw();
    save_if_moved();
}

/* ---- entry points ------------------------------------------------------------------ */

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    int64_t t;
    orb_knob_dial();                               /* every turn is the dial's, from the first click */
    load_settings();
    stage_read(false);
    pick_colours();
    key_pair(&s_key_a, &s_key_b);                          /* what we last knew the store held */
    pm_raw_t raw;
    read_raw(&raw);
    const int32_t sync = now_s(&t);                        /* read first: §5.2, and no order in a
                                                            * call's arguments */
    pm_restore(&s, &cfg, &raw, (int)sync, t);
    if (!sync && pm_raw_needs_clock(&raw)) {               /* held as paused until the clock syncs */
        s_pending_raw = raw;
        s_restore_pending = true;
    }
    s_turned_k = raw.st_k ? s.k : -1;                      /* §4: a stored length may be turned */
    ring_send();
    save_if_moved();                    /* a fresh cycle: this write names its length */
    orb_logf("pomodoro ready: %s, %d of %d, %d min, ring %s from %d",
             s_themed ? "themed" : "no theme", pm_focus_at(s.k, cfg.focuses), (int)cfg.focuses,
             (int)(s.len / 60), s_dir ? "anticlockwise" : "clockwise", s_start);
    return ORB_OK;
}

static void on_glass(void)
{
    s_visible = true;
    if (!s_cv) stage_read(false);                  /* a theme may have bound while we were away */
    face_forget();                                 /* what the rows carry now is the theme's */
    s_sent_valid = false;                          /* here, the ring may wear another colour */
    s_clear_sent = false;
    redraw();
    orb_logf("pomodoro on the glass: %s, %d of %d, state %d", s_themed ? "themed" : "bare",
             pm_focus_at(s.k, cfg.focuses), (int)cfg.focuses, (int)s.st);
}

ORB_EXPORT("orb_activate") void orb_activate(void) { on_glass(); }
ORB_EXPORT("orb_resume")   void orb_resume(void)   { on_glass(); }
ORB_EXPORT("orb_show")     void orb_show(void)     { on_glass(); }

/* Off the glass: nothing is freed and nothing is forgotten. The phase is the wall clock's,
 * and the ring the service sets is drawn over whatever app the owner went to. */
ORB_EXPORT("orb_suspend") void orb_suspend(void)
{
    s_visible = false;
    s_sent_valid = false;
    s_clear_sent = false;
    s_dirty = true;
}

/* The beat, on the glass only: the seconds the service keeps are what the figure shows, so
 * the face moves when that figure moved and not when the beat did. */
ORB_EXPORT("orb_tick") void orb_tick(uint32_t now)
{
    (void)now;
    if (s.rem != s_sec_out) redraw();
}

/* The one pass that runs whether or not the glass is on: the wall clock, the chime, the ring
 * and the store. Drawing is redraw()'s alone, which returns at once off the glass, so nothing
 * here reaches the display by that name or another's (§8). */
ORB_EXPORT("orb_service") void orb_service(void)
{
    const uint32_t gen = orb_cfg_gen();
    if (s_restore_pending) {                                /* the clock synced: place the running phase */
        int64_t t;
        if (now_s(&t)) {
            pm_restore(&s, &cfg, &s_pending_raw, 1, t);
            s_restore_pending = false;
            s_dirty = true;
            orb_logf("pomodoro: clock synced, the running phase restored (%d s left)", (int)s.rem);
        }
    }
    load_and_restore(gen != s_cfg_gen);
    if (s_limit_ms && up_ms() - s_limit_ms >= PM_LIMIT_SAY_MS) { s_limit_ms = 0; s_dirty = true; }
    if (!s_choosing) {                                  /* the countdown holds while choosing (§6.3) */
        const int ended = pm_ring_phase(&s, cfg.focuses);
        const int32_t ev = pm_service(&s, &cfg, wall());
        if (ev & PM_EV_END) { chime(ended); wake_glass(); s_turned_k = -1; s_dirty = true; }
    }
    choice_step();
    ring_send();
    save_if_moved();
    if (!s_themed && s_visible && up_ms() >= s_stage_retry_ms) stage_read(true);
    if (s_dirty) redraw();
}

/* Press: start, pause, resume, and over and waiting the next phase. A turn is minutes.
 * Either while a choice is in flight is that choice's let-go and nothing else (§6.3), which
 * is why the gesture ends here rather than being honoured on top of it. */
ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (delta || pressed) s_restore_pending = false;      /* the owner acted on what the glass shows */
    if (s_choosing) {
        s_click_ms = 0;
        choice_step();
        return;
    }
    const int was_st = s.st;
    if (pressed) {
        pm_press(&s, &cfg);
        if (was_st == PM_WAIT || was_st == PM_WAITED) s_turned_k = -1;  /* a new phase takes its
                                                                          * setting, §4 */
    }
    if (delta) {
        if (pm_turn(&s, &cfg, (int)delta, wall()) & PM_EV_LIMIT) s_limit_ms = up_ms();
        s_turned_k = s.k;                             /* the turn is this phase's, never the
                                                       * setting's (§4) */
    }
    redraw();
    save_if_moved();
}

/* Press-and-turn: the phases, one a click, clockwise forward and anticlockwise back, the
 * ends stopping inside one cycle. The clicks only show the choice; it lands on the let-go. */
ORB_EXPORT("orb_push") void orb_push(int32_t delta)
{
    s_restore_pending = false;                            /* the owner acted: what the glass shows stands */
    if (!delta) {
        s_click_ms = 0;
        choice_step();
        return;
    }
    s_pos = pm_pos_step(s_choosing ? s_pos : 0, (int)delta, pm_at_start(&s), s.k, cfg.focuses);
    s_choosing = true;
    s_click_ms = up_ms();
    redraw();
}
