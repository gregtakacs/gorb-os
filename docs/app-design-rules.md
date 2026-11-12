# App design rules

The rules every orb app follows, the native ones in `main/` and the SDK apps in `apps/`
alike. They are the owner's rulings gathered in one place; the plan sections named after each
rule are the ledger with the reasons (app-platform-plan §51 the knob, §55 alerts, §57
settings, §58 one page per app, §64 the ring; theme-cache-plan "Nothing of an app's display
runs off the glass"). When a rule here and a plan section disagree, the newer ruling wins and
this page is corrected. `apps/README.md` teaches the same rules with code; this page is the
rule book.

## The knob

The orb has one input: a knob that turns and presses. Four gestures, the same in every app,
so someone who has learned one app has learned them all.

| Gesture | Means | Owned by |
|---|---|---|
| **Turn** | move along the app's one dial | the app |
| **Press** | the app's one action | the app |
| **Press-and-turn** | the app's second axis, if it has one | the app (optional) |
| **Hold** | leave the app for the app list | the OS, always |

**Turn.**
- Turning never needs a press first: no modes, no "press to edit, then turn".
- Turns arrive accelerated: a slow click is ±1, a quick spin ±2 or ±4 a click.
- A click shows on the glass at once. Redraw in `orb_input`, and the OS sends a frame straight
  after every knob event.

**Press.**
- One thing, always the same thing, wherever the dial stands: the next place, start or pause.
- A press keeps the dial's position, so it compares rather than resets.
- No double press, ever: waiting for a second press would delay every first one.
- The one exception: an app the owner has let borrow the press (`orb_press_claim`, cap `press`,
  §64.13) takes it from whatever app is on the glass while its claim stands. The pomodoro does
  this, by setting, to start the next phase from anywhere.

**Press-and-turn** (hold the knob down and turn before the hold fires).
- For a second axis that is not the dial: radar's and flights' zoom, the pomodoro's phases.
- The turn shows the choice as it goes (a header line). The choice lands on the let-go
  (`orb_push(0)`), and a press or a plain turn while choosing lands it too.
- Clicks arrive one by one, unaccelerated. The ends stop.
- Never the only way to something the app is for: an owner who never discovers it still
  has a whole app.

**Hold.**
- Never the app's. It is the way out of every app, and it must always work. (A native menu
  entry may close its own group on a hold, `opt_long`; nothing else may take it.)

**Touch** is never required.

### Which way is which

Clockwise is **forward**: later, more, next, closer. Anticlockwise is back: earlier, less,
previous, wider.

- Weather turns clockwise into the coming hours, anticlockwise into the days.
- Radar turns clockwise into the forecast, anticlockwise into the past.
- Radar's and flights' press-and-turn zoom clockwise closer.
- The pomodoro's turn takes time off anticlockwise and gives it back clockwise, never past the
  phase's setting (§64.14); its press-and-turn goes to the next phase
  clockwise.

The sign comes from the knob as it is: `delta > 0` is clockwise, and no screen negates it.
An app whose ladder runs the other way subtracts the delta, as flights' range does
(§64.5), rather than reordering what it stores.

### Two kinds of dial

**A view dial** moves what the app shows. Weather, radar and flights have one.
- It is centred on a **home view**, what someone glancing at the orb wants to see. One kind
  of thing lies clockwise of home, another anticlockwise, so the wanted thing is a few clicks
  either side of home and never at the far end of a line.
- **The ends stop.** Nothing wraps, so an overshoot never jumps to the other side.
- **Home is sticky.** A fast spin stops on home, so one flick back from anywhere is home.
- **Idle goes home.** After 20 s without a turn the app shows home again, and so does coming
  back to the app.
- `apps/sdk/orb_dial.h` keeps all of this for you.

**A value dial** changes a value: the pomodoro's remaining time, a kitchen timer.
- The change stays. Nothing returns on idle, and coming back shows the value as it is.
- The ends stop at the value's own limits.
- The glass shows the value as it changes, large enough to read at arm's length.

An app has one dial, of one kind. The clock has none: a turn on the clock opens the app list.

## On the glass and off it

- **An app off the glass draws nothing and reads no card.** It is off the glass from
  `orb_suspend` until `orb_activate` or `orb_resume`.
- **Its service keeps running** (`orb_service`, every installed app). Time, data, a chime, an
  alert or a ring that must happen whatever the owner is looking at belong there.
- **A sound off the glass is a one-shot.** Loops play only on the glass. Keep a sound above
  350 Hz, because the speaker is a 2 W micro speaker.
- **State that matters survives a restart.** Every theme landing restarts the app's engine
  in place (§63), so keep a running timer as its end time on the wall clock in the app's
  storage, not as a count in RAM.

## Telling the owner something

| The owner should... | Use | Notes |
|---|---|---|
| see an ongoing state from any app (a timer, a long job) | **the ring** (§64.1) | one ring on the glass; the latest app to set it owns it |
| be told once that something happened | **an alert** (§55, `orb_attention`) | a reason the app declares; the owner chooses what it does |
| hear it | a one-shot sound (cap `sound`) | |

**The ring.**
- The OS draws it around the rim over whatever is on the glass. It is never the app's own
  pixels.
- An app sets a fraction, an arc colour and opacity, a track colour and opacity, and an
  effect (steady, flash, slow breathe), and clears it when the state ends.
- Declare the cap `ring`. The owner can revoke it per app.
- Use a colour per state, so the owner reads the state from across the room without the
  app on the glass. Flash for "something is waiting for you", breathe for "paused", steady
  for "running".
- Don't use it for a one-off event (that is an alert), and don't use it to decorate your own
  face (draw that in your theme).

## Settings

- **Declared, not drawn.** Anything that is not the dial, its press or its press-and-turn is
  a setting in `app.json`. The OS draws the setting on the app's page on the dial and on the
  hub (§57).
- **Types:** `int`, `number`, `enum`, `multi`, `toggle`, `text`, `place`, `secret`, and
  `color` (§64.2). Give every setting a label, and give it help when the label alone does
  not say what it does.
- **One page per app on the dial:** its settings, then Themes and Permissions as sub-menus
  (§58). An app never draws a settings screen of its own.
- **Units are the system's** (temperature, speed, distance, time). Read them; never offer
  your own.
- **Places are the system's:** home plus three. Read them; never ask for a location.
- **An app is installed or not.** There is no "disabled" state, and uninstalling wipes its
  settings and secrets.

## Looks

- **Every app's face is a theme**, made in the app's own editor, and the editor ships inside
  the app. Apps know nothing of each other: no app reaches into another's editor or theme.
- **Every themed app starts from the same eight looks, one name each, in this order:**
  Scope, Paper, Brass, Blueprint, Station, Celestial, Silver, Sectional, plus New (blank).
  Each look is its own: nothing borrowed between looks (§64.4). The shelf default is Scope.
- **Words are theme text**, in the theme's fonts, as labels rather than widgets.
- **A small icon (about 40 px) carries two of shape, colour and detail, never all three.**
  No coloured rims; tint the metals instead.
- The editor's start sheet, screen calibration and colour picker come from the kit
  (studio@1, theme@1), never from the app.

## Shipping

- An app's `version.txt` goes up once per push, not once per commit. The third segment is
  for fixes; the second only at a real release.
- When an app needs newer firmware, its `os_min` says so, and the hub refuses the install
  on an older orb.
- Every version bump re-bakes the app's shelf default theme (`tools/rebake_shelf.py`);
  `make check-apps` fails while one is behind.
