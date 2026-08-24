# Hollowtide — Changelog

All work below landed in a single session. Every balance figure quoted was **measured** by
playing the rules in `tools/balance`, not estimated — see `README.md` for how to re-run it.

---

## Features

### The encounter
The meter filling used to resolve instantly: a ward went, or the parish worked at half pace.
It now begins a **nine-second walk**.

- Each of the eight rites has its own **visitor**, with a warning line and one true answer.
- Four answers — **ward it, offer, the bell, stand still** — all verbs the game already had.
- **Knowing beats silence beats guessing.** A correct answer turns the visitor away for free;
  a wrong one lands regardless of wards. Doing nothing resolves exactly as it always did, so
  an absent keeper is never punished.
- The visitor is drawn from **what called it** — weighted by each rite's share of the dread
  pressure — so every rite you own can summon something.
- A **bestiary** in the ledger fills in as you learn: a rumour until you meet it, a name once
  you have, and its answer only once you have turned it away yourself.
- The parish sours and holds a rising tremor while something approaches, and the rite that
  called it lights and pulses.

### Relics
Hand-gathering was worth a flat twentieth of a second of production at every parish size —
never worthless, never interesting. It now turns things up.

- **Procedurally generated**: a relic *is* a seed and a grade. Name, powers, magnitudes and
  artwork all derive from those two numbers, so the supply is endless and a save costs two
  integers per item. **15.7K possible Anointed names, 157K Hollowed.**
- **Grade is drawn against dread**, so the reason to click and the reason to ride the meter
  are one reason. `Hollowed` things exist only near the brink.
- Ladder: **Leavings → Keepsake → Anointed → Hallowed → Hollowed.** Better grades carry *more
  powers*, not bigger ones.
- **Procedural art** (`ui_relic.slang`): six silhouettes, and each grade earns another layer —
  a notch, a setting, rays, then a living core and a rim that will not settle. Only the top
  grade moves.
- **Three worn slots**, each power turning a knob the game already had.
- **A fifth ledger tab** for what is worn and what is carried.
- **Rendering**: right-click renders a relic down for ichor rather than destroying it.

### The congregation, for keepers who have none
Pushing dread onto another player is the most distinctive thing the game does, and it
required a second player online.

- A communion now leaves an **echo** — a keeper you used to be — and you can shunt dread onto
  them.
- **A loan, not a bin.** What an echo takes it keeps, and a loaded line draws the dark in
  faster: at full burden the peace between visitations halves.
- Worth **1.24×** to a keeper who answers what it brings, **1.10×** to one who does not.
- The congregation panel shows the line when nobody else is there.

### The parish carries its own yield
Every rite's output used to arrive as an orb that crossed the scene at horizon height —
straight through whatever structures stood between it and the sigil.

- Each rite now has a **visible conduit**: a run of dim pips rising from its crown, along a
  shared canopy above the parish, and down into the sigil's rim. Drawn whether anything is on
  it or not, because what makes a yield read as *carried* is that the route was there first.
- The wire lights with the rite as it works. The line is infrastructure; the bead is the light.
- **The canopy clears the tallest structure standing**, measured rather than written down — a
  rite's height is a curve on how many are owned and is capped by the spacing, so the tallest
  thing on screen depends on both the parish and the window.
- No bounce and no gait. Beads that would overlap are separated by pace, not by a wobble.

### Relic trading
- Hand a relic to another keeper; it travels as its seed and grade alone.
- Removal happens **before** anything is sent, and is restored if the send fails.
- An arriving relic always lands, displacing the worst thing carried — never something worn.

### Prestige with something to spend on
- **Six boons** (deeper wards, cold blood, the old bargain, steady hand, unsleeping, quick
  kindling), raising sigil sinks from **108 to 442**.
- The threshold reports a returning keeper's standing and offers to **begin a new vigil**.

---

## Balance

| Change | Before | After |
|---|---|---|
| Stoking, at a human press rate | **4,919,000×** a clean run | 3.3× |
| Ward cost across parish sizes | 6% → 131% of a window's income | flat **25%** |
| Aftermath | flat 30s (permanent half-pace late) | 40% of the window it follows |
| Dread relaxation | flat subtraction (dead early game) | proportional |
| Prestige, ten communions | tenth run *worse* than the first | 768M → 1.57T |
| Sigils offered, first run | 105 (against 108 to spend) | 4 |
| Relic loadout | 9.9× a bare keeper | **1.85×** |
| Rarity ladder | inverted at the brink | monotonic at every depth |
| Knowing an encounter's answer | left you *lower* on the meter | 1.45× doing nothing |

---

## Bug fixes

### Economy
- **The global multiplier was applied twice** in hand gathering and stoking, so both scaled as
  its square — compounding, worse the longer a run went.
- **The permanent multiplier read sigils *held***, so spending them made the keeper weaker and
  prestige was a treadmill.
- **Wards inverted at scale.** Priced off the rate while the window they cover shrinks, they
  grew from a twentieth of a keeper's income to more than all of it.
- **Dread could not move for the first eleven minutes** — a flat decay floor meant small
  parishes could not shift the meter at all.
- **The brink was unreachable by choice.** Proportional relaxation undid a stoke *in the same
  tick*, so no parish below the equilibrium threshold could reach an encounter however
  deliberately its keeper walked toward one.
- **A new keeper's stoke offered 0.4 ichor** against a 16-ichor lantern (regression from the
  double-multiplier fix).
- **A relic's Hand power silently stopped working** as the parish grew, reaching all of the
  by-hand figure early and none of it later.

### Long uptime
- **The parish animation clock froze permanently after 6.1 days.** A float accumulating a
  frame delta stops advancing once its own spacing exceeds one.
- **Up to 65 consecutive relics rendered identical art.** Shader parameters are float32, exact
  only to 2²⁴; relic seeds run to a billion.
- **Every keeper's fifth find was the same relic**, since all seed counters started at zero.

### What the game told the player
- **The offline report understated gains by up to 82×.** It measured the purse — which
  overseers spend from while you are away — instead of what was produced.
- **The ledger and the bar disagreed by 2×** during an aftermath.
- **Communing greedily is 24 million times worse** than waiting, and the button said only
  `+4 sigils`.

### Multiplayer
- **Tithing was dead in multiplayer.** The echo panel hid the button and the live path never
  showed it again; `Net.IsConnected` is false for the first frames of every session.
- **Fresh echoes were evicted the instant they were added**, so the line showed the same four
  names forever.

### The parish's layout
All four found by making the geometry checkable, not by looking at it.

- **A rite could be drawn 60% of the window height** when only 62% exists above the horizon —
  so on a short window a tall structure drew its count off the top, and the canopy, having
  nowhere to go, clamped *below* the crowns and sent every conduit through the structures it
  exists to clear.
- **The deepest rite could overlap the sigil.** The clearance reserved a flat 0.03 and never
  counted the rite's own half-width, which on a wide window is nearer 0.08.
- **The conduits' descent was anchored to the wrong variable.** A cubic's height profile
  depends only on its endpoints, not its length, so every wire began dropping at the same
  *fraction* of its journey — meaning the one from the far-left rite started down over the
  middle of the parish. Two attempts were made at capping structure sizes to buy it room
  before it was clear the descent has to be anchored in *position*.
- **Clicking the sigil shuffled the whole parish sideways.** The rite spacing was measured from
  the sigil's animated left edge, which slides outward every time it is struck.

### The parish's presentation
- **The weeping statue was crooked.** Its robe, shoulders, neck and head each sat a little
  further right than the last, on the theory that offsetting the upper body reads as a bow.
  Against a plinth that is dead centre it reads as one thing: not straight.
- **A second purchase reset the first purchase's wave** instead of sending another, so a front
  halfway across the floor snapped back to the horizon and set off again — one purchase looked
  exactly like five. Three fronts can now be in flight.
- **Fast clicking stacked every floating figure in one column,** each hiding the one before it.
- **The whole interface was too small to read** at a short window.
- **A getter was mutating the field it returned arithmetic from.** The sigil's resting size was
  measured as a side effect of reading the rite layout, so everything else needing it quietly
  depended on the layout having been computed first.

### Presentation budgets
- **The whisper feed ran at 41 lines a minute** against a display showing ~40, so lines were
  pushed off unread.
- **Ordinary clicking evicted relic-find popups** above 8 clicks/s — the feedback destroyed by
  the behaviour it rewarded.
- **The screen flash was assigned, not maxed**, so a quiet event stole the screen from a loud
  one still playing out.

### Persistence
- **A corrupt save could hand a keeper negative sigils.** Everything read off the file is now
  clamped, including NaN and infinity in the doubles.

---

## Made visible

Mechanics that existed and worked, but that no player could see:

- **Fervour** — a 1.60× multiplier, drained in 12 seconds, drawn nowhere.
- **What dread pays** — the game's central bargain, referenced by no game code.
- **The parish's voice** — ten minutes of a first session were six lines, every one a receipt.
- **Finding a relic** — the payoff for clicking arrived in a panel you might not have open.
- **Which marks need a congregation** — two of fifteen, presented as ordinary.
- **Which relic is next to give**, and what rendering one pays.
- **The bargain relic's true effect** — reworded, not rebalanced.

---

## Tooling

- **The balance harness** (`tools/balance`) — plays the real rules at speed and checks **125
  invariants**. Every check exists because the thing it checks was once broken.
- **The parish's layout is checked too.** The geometry was split into an engine-free `Layout`
  and the harness now lays the parish out at **336 sizes across six window shapes**, asserting
  that nothing reaches the button row, leaves the screen, crosses a structure or overlaps the
  sigil. It was written after a claim "verified" by working one example on paper turned out to
  route every conduit across the stoke and ward buttons, and it found four faults immediately.
- **A fuzzer** — 750k legal actions in illegal-looking orders across three seeds, with frames
  up to five seconds.
- **`--seed N`** — shifts every seed, so a check that passes on one sample and fails on the
  next can be found. It found two.
- **The save is testable** — the data model was split from the file IO, so a whole vigil
  round-trips through JSON in memory without touching the player's real save.

## What is still only checked by eye

Named because the rest of this document is measured, and the difference matters.

- **Whether any of it looks right.** Every visual change here was verified by build, by shader
  compile and by contract — never by looking. A single brief playtest found three problems that
  dozens of simulated iterations never surfaced, which is the whole signal.
- **Type outgrowing its boxes.** The layout invariants pass at *any* type scale, tested at six
  times the current one — they have to, because the rite height cap subtracts the space the
  labels need, so a larger label just makes the structures shorter and the geometry stays
  consistent with itself. What breaks first is text clipping its container, and nothing here
  can see it.
- **The two type scales staying in step.** Most of the game's type is authored into the scene
  files by a Python generator, and the rest is built at runtime in C#. They hold the same
  number and it is named on both sides, but nothing enforces it.

## Documentation

- `README.md` — the loop, where things live, the load-bearing design rules, and the three ways
  this harness has lied.
