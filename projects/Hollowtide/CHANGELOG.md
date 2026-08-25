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

### A relic is made of what it does
Relics carried one power at the two grades a keeper sees most, so the whole system read as
"a relic does a thing" rather than as a loadout.

- **One power per grade**: Leavings do one thing, a Hollowed relic does **five**. Magnitudes
  came down to pay for it — a better relic is more *interesting*, not a bigger number.
- **The art is built from the powers.** Each one adds its own feature to the object: a grip, a
  hung weight, a ward ring, rays, an alms bowl, a standing foot, structures at the base, a
  struck spark, a chamber with something kept in it. A relic that does five things has five
  things on it, and two relics with different powers are different **objects** rather than the
  same object in a higher tier of jewellery.
- **Foundation** — while worn, the keeper holds copies of one rite they never bought. They
  stand in the parish and produce, and they are gone the moment it comes off. Kept off the
  books: no milestone credit, no discount on the next purchase. A loan of production, not of
  progress.
- **Kindling** shortens the wait to stoke; **Reliquary** makes the ground give things up more
  often — a relic that finds relics. Nine kinds in all.
- **An inspector**: hover any relic for the drawing at a size where its features can be told
  apart, every power on its own line, what rendering it pays, and two sentences of history
  from its own seed.
- **A wear-the-best button** that works on worn and carried together, so it is idempotent and
  can only improve the hand — lit only when it would change something.
- **Rendering takes two right-clicks**, and says what it destroyed and for how much.

### The parish keeps its voice
The whisper feed shows a line for nine seconds and then loses it forever.

- A **VOICES tab** holds the last 160 lines, newest first, in the colour each was said in.
- Recorded where the parish **speaks**, not where it is drawn, so nothing said while the feed
  was full is missed — which is exactly the busy stretch a keeper looks away during.
- **Saved**, and carried through a communion. A record that empties on quit is still gone.

### Consecration
Every other choice in a vigil is a purchase, and a purchase is not a decision you live with.

- Once per vigil, give the run to one rite: it is worth **three times** as much and every other
  rite gives up a fifth.
- Refused a second time, and refused for a rite the keeper does not hold.
- Cleared by a communion, so the question is asked again rather than answered once, ten runs
  ago.

### Offerings that do not run out
Three per rite ran out at fifty copies, and then the tab was empty and told a keeper to buy
more of a rite they owned hundreds of.

- **86 offerings**, on a deep ladder reaching to six hundred copies.
- **Muffled Bell** — a third less dread from a rite for a seventh less yield. The first
  offering that is a *bargain* rather than a bonus.
- **Unhooded Lantern** — its mirror: two fifths more taken for half again the dread. Two
  offerings pointing opposite ways on one axis put the game's bargain in the keeper's hands.

### The dark takes something
A visitation that landed cost dread and half a window's production and nothing else — odd for
a game about a thing that comes to take from you, and it left a mark called **Bereaved**
describing an event where nothing was lost.

- It now takes **the best thing loose in your satchel**, and never anything worn. What is on
  your hands is yours; what is in the bag is the parish's if it comes for it.
- The visitation panel **names the relic at risk** while the clock runs, so putting it on is a
  decision rather than a rule learned by losing something.
- A held ward and a right answer still cost nothing.

### A visitor says it three ways
The encounter is the most dramatic moment in the game and each visitor announced itself with
the same sentence forever, which turns a warning into a label.

- **24 approach lines** across eight visitors, and **24 more** for what the parish says when
  something is lost.
- **Every variant keeps the tell.** The line is the only thing a keeper has to work out what a
  visitor wants, so varying the wording is flavour and varying the meaning would be cheating.
- One line is drawn **per approach and shared** by the panel, the whisper and the transcript —
  picking independently would have the parish warn about one thing while the panel described
  another. The bestiary still shows the canonical line, so what you *study* stays stable.

### Marks you can see yourself approaching
**24 marks**, nine of them added here, covering relics, consecration, a full board of offerings
and the prestige ladder.
Twenty-three marks showed a name, a sentence and nothing else — a list of things you have not
done rather than a track.

- The eleven that **count** toward something now draw a bar, on the same widget the relics use
  for rarity and the rites use for a working.
- Null where a mark cannot be partly done: lighting a first lantern is not 40% complete.

### The parish notices
- **24 ambient lines** where there were eight, and **twelve more** the parish says only when
  they are true of this vigil — a rite consecrated, structures standing that were never bought,
  a ledger already in your handwriting throughout.
- Each dread band keeps a voice **after its lesson is done**. The lessons are still said twice
  and never again; the meter used to go permanently silent behind them.
- An **echo remembers which keeper it was** — the rite that run was given to, and how deep it
  got. The line reads as people you can tell apart rather than four names with four numbers.

### Relic trading
- Hand a relic to another keeper; it travels as its seed and grade alone.
- Removal happens **before** anything is sent, and is restored if the send fails.
- An arriving relic always lands, displacing the worst thing carried — never something worn.

### Prestige with something to spend on
- **Six boons** (deeper wards, cold blood, the old bargain, steady hand, unsleeping, quick
  kindling), raising sigil sinks from **108 to 442**.
- **A sink that never fills.** 442 was still nothing: a fortnight of hard play earns 124
  thousand sigils, so every boon maxed long before then and prestige decayed back into a number
  that only accumulates — the very fault the boons were added to fix, arriving further out. **The
  Old Bargain** now runs to two hundred levels instead of three, taking total spending to **179
  billion**. Its cost multiplies while its effect adds, so what a keeper gets grows with the
  *logarithm* of what they have earned: the sink never fills and never runs away. It is that
  boon rather than a new system because it is the game's own sentence — dread pays, and it is
  coming, so buying more of the first half is choosing to want the meter higher.
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
- **Lent structures only paid while you were away.** A relic that lends copies of a rite was
  taught to count toward the quoted rate — which is what the ward is priced off, and what the
  offline catch-up settles against — but the loop that actually hands ichor over stepped its
  cadence off the *bought* count. So the structures stood in the parish, were charged for in
  the ward's price, and earned **nothing at all** for as long as the keeper was watching. The
  invariant that was supposed to cover this asked `Rate`, the quoted number, rather than
  playing the parish; it now plays sixty of the rite's workings and compares what arrived
  against what was quoted. Their conduits and the canopy above them were counting the bought
  copies too, so a lent structure had no wire running from it.

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

### Things nobody could see
- **The second line of every ledger row vanished under the pointer.** `TextFaint` and `RowHot`
  were the same step of the grey ramp — a contrast ratio of **1.04:1**. Not dim: gone. Faint
  text on a normal row was only 1.50:1, which is why the interface read as unreadable
  generally.
- **A row's description ran underneath its own price.** The two column widths were hardcoded
  against a figures column inset from the right and overlapped by **70px**; raising the type
  made it worse, because the inset scaled and the text widths did not.
- **Every relic in the game drew the wrong features.** The power mask is packed in C# by
  `1 << PowerKinds` and unpacked in the shader by a number typed out in the shader; adding a
  seventh power moved one and not the other, halving every mask. Silent, because a
  wrong-but-plausible object looks like art.
- **A relic that lent structures put them in the parish and produced nothing** — the rate loop
  guarded on what was *bought* rather than what was *standing*.
- **Wearing the best three churned the hand.** A low relic in slot 0 and a high one in slot 1
  would swap, changing both slots to reach an identical hand. The first fix for that
  **duplicated relics** — a demoted relic stayed worn *and* went back to the satchel — in 962
  of 3000 random inventories.
- **A second purchase reset the first purchase's wave** instead of sending another, so a front
  halfway across the floor snapped back and set off again. One purchase looked like five.
- **The weeping statue was crooked** — its robe, shoulders, neck and head each sat a little
  further right than the last.

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
- **The offerings and marks tables are save formats.** Both are stored as flags indexed by
  position, so reordering either silently re-points every flag in every existing save — a
  keeper loads their game and owns things they never bought, or holds a record of things they
  never did. Everything new is appended, and both prefixes are now frozen by a check.
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

- **One command that verifies everything** (`tools/verify.py`) — builds, plays the rules on
  several seeds, and compiles every shader, exiting non-zero if any of it broke. It exists
  because verifying by hand meant piping the harness to `tail`, which hands the pipeline
  `tail`'s exit status: a run printing `1 invariant(s) broken` reported success to the shell
  and a red tree was committed. It never pipes, checks every return code directly, keeps going
  after a failure so one run reports everything, and skips the shaders **loudly** when no
  compiler is present.
- **The balance harness** (`tools/balance`) — plays the real rules at speed and checks **295
  invariants**. Every check exists because the thing it checks was once broken. It counts them
  itself, and `verify.py` holds this sentence and the README's to that count — the figure was
  maintained by hand in two files and had already gone stale in one of them.
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

## Boundaries, now read rather than trusted

Constants written on both sides of a language boundary caused the worst bug of the project:
C# packed a relic's powers with one number, a shader unpacked them with another, and every
relic in the game drew the wrong features — silently, because a wrong-but-plausible object
looks like art. Every such pairing is now **verified by reading the other side**:

- The horizon, the relic power divisor, and the three numbers a rolling front is packed with,
  parsed out of the shader source. The horizon had said *"the two MUST agree"* in a comment
  since it was written, with nothing enforcing it.
- **Every element name a script binds** — 77 of them — against the generated scene, because
  `Scene.Find` answers a rename with an invalid entity rather than an error, and the feature
  simply stops existing with nothing in the log.
- The type scale and the ledger width, read out of the Python scene generator.

A missing file or an unmatched line **fails** rather than passing quietly: a verifier that
silently stops verifying keeps reporting green.

## Eight things the checks themselves got wrong

Each was a *check* failing rather than the game, and each taught something the next one assumes.

- **A check that compared C# to C#.** It verified a packed value against the constant that
  packed it — both sides of the comparison were the same side — and passed happily while every
  relic drew the wrong features.
- **A fixture that did not know about a field**, reporting six earnable marks as impossible.
- **A bound tighter than its own distribution.** One sample of an exponential wait, bounded at
  the 92nd percentile: it would have failed one seed in twelve, forever, at random.
- **A fixture that resolved nothing.** It answered a visitation with a call that returns early
  for "no answer", then asserted the consequences against a visitation still walking. Two checks
  passed that way, finding an intact satchel because nothing had happened to it yet.
- **A progress check that could not see a wrong denominator.** It compared every bar against its
  mark at an empty keeper and a maximal one — where every bar is empty and then full whatever it
  was divided by. A bar filling at 25 for a mark needing 50 is only visible in between.

- **A sentinel that poisoned arithmetic.** "No ceiling" was first written as `MaxLevel = -1`,
  and `MaxLevel` is arithmetic in eight other places: the ledger read the boon as already maxed,
  printed `3/-1`, drew a progress bar of **negative width**, and a fixture divided by zero and
  **crashed the whole suite** — a run that then reported "0 failures" because it aborted before
  most checks ran. Replaced by a ladder that is simply long, which breaks nothing and says the
  same thing.
- **An `int` that could not hold the thing it measured.** The check for "a first communion
  cannot buy the game out" sums every level of every boon; with the sink in place that is 179
  billion, which wrapped to **−734,687,863**. The sink working is what broke the arithmetic
  measuring it.

- **A figure quoted as if it meant something.** The long-run check reported the peak
  production it reached, and that number was used to describe the game. Measured across three
  seeds it spans **669 K/s to 127 T/s** — eight orders of magnitude — because wearing a relic
  changes the find chance, a find consumes an extra random draw, and from there two runs see
  different luck forever, which an economy built on repeated doublings compounds without limit.
  The check asserts only that the arithmetic stays arithmetic; the peak beside it is a sample,
  not a property.

And one **assertion that could not fail**: `lentRate > 0.0 && costBefore == 0`, where
`costBefore` was declared zero and never assigned. Half always true, half a repeat of the check
above it — and still counted in the total.

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
