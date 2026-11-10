# D43 §4.5 (P5e), as ruled by the owner 2026-11-10, with the owner amendments of the same day

This is a verbatim copy of `docs/composer-stack-plan.md` §4.5 from the owner's checkout (not yet committed there), followed by the two binding amendments. The specs `p5e-*.md` build it.

### 4.5 A sub-dial is a face, its hands are pointers (P5e, owner 2026-11-10)

**The fault.** §2 says a sub-dial is "a still row or a fold" and P5c says "every other hand
ships as a pointer row". The bake did neither for a sub-dial. `compPlan3` made a sub-dial
with a hand a **value row**: one plate per value of its scale (31 for a date), each plate
the whole sub-dial with *every* hand it hosts painted in at that value
(`compPass3`'s `handsOf`). Only a seconds or hours sub-dial that hosted nothing else
became a pointer (`pointerDefault3`, `COMP3_RING_FULL`). Three consequences on Gold:

- **A second hand on a sub-dial cannot be drawn.** An hour hand under the date hand on a
  date sub-dial froze at whatever hour each date's render used.
- **A wheel hosted on the sub-dial was painted into the sub-dial's plates** at the
  sub-dial's values, over the wheel's own plates. Triple calendar's moon read January's
  phase for the day of the month (patched 2026-11-10 by punching hosted wheels' cutouts in
  `F.compHoles`; this slice removes the path that needed it).
- **A large hand sweeping under the sub-dial** had no depth to pass under: the sub-dial
  was a value row, not a face.

**Rulings (owner, 2026-11-10).**

1. **Every hand on a sub-dial is its own pointer row.** It reads its own `shows`
   (second, minute, hour, 24-hour, GMT, day of week, date, month), turns about its own
   pivot, and stacks in Layers order among its siblings. The pivot is the author's
   setting: the sub-dial's centre is one choice, anywhere else is as legal (owner,
   2026-11-10: "it can turn wherever it wants. That is a user setting"). Two, three or more
   hands on one sub-dial are legal, under R6 and the pointer cap.
2. **The sub-dial's face is a face, at its Layers depth.** It carries no values. It
   folds into the plate unless something moving below it would pass under it (R5): a
   layer lower in the stack whose reach (a hand's swept disc about its pivot, a wheel's
   visible disc: what its cutouts show, or the whole disc of an exposed wheel) overlaps the
   face's ink. A small hand beside it and below it in Layers that never
   reaches under it does not lift it; the face folds into the base dial. When something
   does pass under, the face ships as a still row at its depth and that layer passes under
   the face *and* everything the face hosts. A face is never baked into the plate over a
   hand that sweeps beneath it.
3. **A wheel is a wheel.** A moon, date, day or month wheel hosted on a sub-dial is
   handled exactly like one hosted on the dial: snapping plates cut to its cutouts, or a
   turning row under its cover. Only its face paint and its turn differ. Hosted on a
   lifted face, the wheel's rows sit just under that face's still row and above whatever
   passes beneath.
4. **The plate-per-value sub-dial is not a thing.** It goes, and with it the refusals that
   existed only because of it: "a date ring does not close under its own hand"
   (`COMP3_RING_FULL`), "a sub-dial that hosts another indication cannot be a pointer"
   (`pointerPlan3`), and "a wheel turns under a lifted face" (`clock-logic.js` near
   `lifted3`).

**The wire is unchanged.** The clock already drives any row from any source with any
cadence about its own pivot (`apps/clock/main.c`: `comp_value`, `drive_units`,
`drive_t`, `row_pose`). A date pointer is `src date, vmin 1, vmax 32, span 3600, cadence
jump`: 31 steps a whole turn, the 31st one step short of the 1st, so the ring closes. A
weekday is 7 (`vmin 0, vmax 7`), a month 12. Time hands take the hand's own motion, as the
centre hands do. The pivot is the hand's own authored pivot as the engine lays it out
(the sub-dial's centre by default), never derived from the hand's pixels. No firmware change is expected; if one is needed, it is a finding
to stop and report, not to make.

**Cost.** A date sub-dial drops from 31 full sub-dial plates (≈1.4 MB at 62 px) to one face
(folded or one still row) plus one small plate per hand and its shadow twin. Each hand is
one pointer row against `CLK_AUX_MAX` (8) and R6.

**Proof.**
- Host: a plan test per ruling — two hands on one date sub-dial give two pointer rows with
  their own sources and each hand's authored pivot (one moved off the sub-dial's centre
  keeps its own); a face with a lower hand that never reaches under it still folds; no sub-dial ever appears in
  `compPlan3`'s rows; a moon wheel hosted on a sub-dial plans exactly as one hosted on the
  dial; a centre hand under a sub-dial lifts the face to a still row with the hosted

**Owner amendments (2026-11-10), binding.**

1. **Pivot is the author's setting, not the sub-dial's centre.** A sub-dial hand turns about
   its own authored pivot as the engine lays it out (the sub-dial's centre is only the
   default; anywhere else is legal). The pointer row's pivot comes from the engine's layout
   for that hand, never from the hand's pixels and never forced to the sub-dial centre.
   Test: a hand whose pivot is moved off the sub-dial's centre keeps its own pivot in the
   planned row.
2. **A face lifts only when something below it would actually pass under it.** It folds
   into the plate unless a moving layer lower in the stack has a reach that overlaps the
   face's ink: a hand's swept disc about its pivot, a wheel's disc. A small hand beside the
   face and below it in Layers that never reaches under it does NOT lift it; the face folds
   into the base dial. Test: a face with a lower hand that never reaches under it still
   folds; a centre hand long enough to sweep under it lifts it to a still row.

**Owner refinement to amendment 2 (2026-11-10), binding.** A wheel's reach is its VISIBLE
disc, not its whole disc: the area its cutouts show, or the whole disc only for an exposed
wheel. A wheel lower in the stack lifts a face only if that visible area overlaps the face's
ink; a wheel whose hidden part lies under the face but whose cutout window does not does NOT
lift it. A test for each case.
