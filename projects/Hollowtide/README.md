# Hollowtide

An idle game about keeping a parish that something is coming for.

You gather ichor by hand and buy rites that gather it for you. Owning rites draws **dread**,
and dread is the bargain at the centre of the game: it pays up to several times over, and
when the meter fills something walks toward you. Everything else in here exists to make that
one trade interesting.

See `CHANGELOG.md` for what changed and why.

## The loop

| system | what it is | why it exists |
|---|---|---|
| **Rites** | eight tiers of producer, each working on its own cadence | the spine; a rite delivers as an *event*, not a trickle |
| **Dread** | 0–1, climbs with holdings, relaxes proportionally | pays `DreadMultiplier`; fills → a visitation |
| **Wards** | insurance, priced at a quarter of the window they cover | a constant share of income at every parish size |
| **Encounters** | the meter filling names a visitor, nine seconds out | four answers; knowing beats silence beats guessing |
| **Fervour** | a decaying multiplier built by hand-gathering | makes clicking worth something without making it mandatory |
| **Communion** | prestige: give the parish back for sigils | sigils buy boons and overseers, and leave an **echo** behind |
| **Echoes** | keepers you used to be; you can shunt dread onto them | a loan, not a bin — a loaded line pulls the dark in faster |
| **Relics** | procedural loot dug up by hand-gathering | grade is drawn against *dread*, so clicking and risk are one reason |

Two things are deliberately load-bearing across all of it:

- **Idling is never punished.** Offline never starts an encounter, and an unanswered visitation
  resolves exactly as it always did. Every active mechanic is an *opportunity*, never a tax on
  the player who stepped away.
- **Colour means something.** One grey ramp, three accents — ichor, dread, sigil. Rarity is
  expressed as how much is *there*, never as a palette of its own. See `Palette.cs`.

## Where things live

```
scripts/Vigil.cs           the whole simulation - no engine reference, on purpose
scripts/Relics.cs          relic generation; a relic is a seed plus a grade, nothing more
scripts/Content.cs         rites, offerings, marks, boons, visitors, the parish's voice
scripts/VigilSaveData.cs   what a save contains and how it is applied (engine-free)
scripts/SaveSystem.cs      reading and writing the file
scripts/Layout.cs          where things stand in the parish, as pure arithmetic (engine-free)
scripts/Transcript.cs      everything the parish has said, as a ring (engine-free)
scripts/Typography.cs      how big the type is - one number (engine-free)
scripts/{Hud,Ledger,Parish,Congregation,Whispers}.cs   views onto the simulation
assets/shaders/ui_relic.slang   relics drawn from their seed; grade adds layers, not colours
tools/generate_scenes.py   scaffolds the authored chrome - read its header before running it
tools/balance/             the balance harness
```

`Vigil`, `Relics`, `Content`, `VigilSaveData`, `Palette`, `Layout`, `Typography` and
`Transcript` carry
**no `AetherCore` reference**. That is not tidiness — it is what lets the harness compile the
rules on their own and play them ten thousand times without a window, and what lets a whole
save round-trip through JSON in a test that never goes near the player's real one.

`Layout` is the newest member and joined for exactly that reason. The parish's geometry used to
live inside `Parish`, tangled up with live rect reads, and so the only way to check any of it
was to look at the screen. A claim about it that had been "verified" by working one example on
paper turned out to route every conduit straight across the stoke and ward buttons. Pulled out,
the same claim is a statement about a curve and a clearance — and the harness holds it at every
window shape and parish size. **If you find yourself reasoning about parish geometry on paper,
that is the signal to move the arithmetic here and let the harness do it instead.**

## The balance harness

```bash
dotnet run -c Release --project projects/Hollowtide/tools/balance
```

### Verifying everything at once

```bash
python projects/Hollowtide/tools/verify.py
```

Builds the scripts, plays the rules on the default seed and a couple of others, and compiles
every shader — reporting each step and **exiting non-zero if any of them broke**.

Prefer it to running the three by hand. Verifying by hand meant chaining commands with `&&`
and reading the harness's verdict by piping it to `tail`, which makes the pipeline's exit
status `tail`'s rather than the harness's: a run that printed `1 invariant(s) broken` in plain
sight reported success to the shell, and a red tree was committed. The script never pipes
anything, checks every return code directly, and runs every step even after one fails so a
single command tells you everything that is wrong.

**Run it after any change to the economy, or to the parish's layout.** It plays the real rules
at speed and checks **295 invariants**, printing PASS/FAIL and returning non-zero on a break.

Every check in it exists because the thing it checks was once broken, and *none* of them were
visible by reading the code:

- Stoking was worth **4,919,000×** a clean run at a human press rate.
- A ward cost 6% of a visitation window early and **131%** late.
- Dread physically could not move for the first eleven minutes.
- No parish under the equilibrium threshold could reach an encounter **at all**, however
  deliberately its keeper walked toward one.
- Five of eight visitors never appeared; 91% of encounters wanted the same answer.
- Ten communions left the tenth run *worse* than the first.
- A full relic loadout was worth **ten times** a bare keeper.
- At the brink, the rarest relic grade was the **commonest** of the good ones.
- A rite could be drawn taller than the room above the horizon, drawing its count off the top
  of a short window.
- The deepest rite could sit on top of the sigil, because the clearance never counted the
  rite's own width.
- Every conduit began its descent at the same *fraction* of its journey, so the longest one
  came down through the middle of the parish.
- The quietest text and the highlighted row behind it were the same grey, so a row's second
  line disappeared the instant the pointer touched it.
- Every relic drew the wrong features, because C# packed their powers with one number and the
  shader unpacked them with another.
- Wearing the best three relics duplicated one in a third of random inventories.
- A relic that lends structures paid nothing while you watched and paid in full while
  you were away, because the loop that quotes the rate and the loop that hands the ichor
  over counted different things.
- Nothing anywhere compared the rate the parish *quotes* against the ichor it actually
  *pays*, which is how the above went unnoticed; a parish parked at its own dread
  equilibrium now plays sixty workings and the two have to agree.
- The communion — the one irreversible button in the game — quoted the payout as a share of
  sigils *taken* and labelled it the gain on what you hold. At ten taken and five offered
  that read as **+50%** for a bonus that moved from x1.60 to x1.90.
- One rule of the game lived in the HUD and nowhere else: you cannot stoke while something is
  walking. Every measurement ever taken had been of a keeper who could.
- The bell's whole rule — how often it can be rung — was a float on a widget, so the largest
  multiplier in the game had never been measured, a reload cleared it, and the host had nothing
  to check an arriving ring against. Ringing on every cooldown is worth **12x** over half an
  hour, and a congregation answering each other's bells **108x** — the answered surge ran for
  22 seconds of a 25-second rope, so it was not a surge, it was the rate. Cut to **2.2x** and
  **4.6x**.
- The same press paid two different bells depending on whether the keeper's network entity had
  finished spawning.
- Shunting dread onto a *living keeper* skipped every rule the same act obeys when aimed at an
  echo — no floor, no twenty-second interval, and its own hardcoded quarter. The host relayed
  the amount unclamped and unlimited, so one client could hold a whole congregation at the
  brink. It is the one message that acts against somebody's interest, and it was the one with
  no limit.
- The tithe's ten percent was a literal in the panel that drew the button — the third of the
  congregation's three verbs to have its rule living in the view.
- Wiping the save deleted the save and left the *half-written* one beside it, which is what
  recovery reads when a save is unreadable. The one path in the game with no undo behind it
  could hand back the life the keeper had just asked twice to be rid of.
- 999,999 ichor printed as **"1000K"** — five characters in a column sized for four — because
  three significant figures round up out of the tier the magnitude was measured in. The width
  check that was supposed to catch it only ever sampled decades.
- Three colours were written as raw literals in the views, and two of them were the same dread
  meter spelled slightly differently — so tuning one would have left the HUD's bar and the
  congregation's rows showing one quantity in two colours. Nothing was checking the README's
  own claim that colour means something, because the claim is about every file *except*
  `Palette.cs`.
- A relic's name got a wear vocabulary and optional parts, so the *shape* of a name varies and
  not only its words: 784 ways to name a Leavings became **312,816**, and the rarest grade —
  which had the narrowest pool of all, because a guaranteed epithet locked it out of everything
  else — became the widest. Names did not get longer; the row is 320px and that was the
  constraint the design had to buy variety within.
- The two-click confirmation on rendering a relic armed a *row*, not a relic — so wearing
  something, handing one over, pressing wear-the-best, or a visitation taking one would shift
  the satchel underneath it and the second click destroyed a relic the keeper never chose. The
  confirmation built to stop exactly that was the way it happened.
- The transcript puts the newest line at the top, so every line the parish said pushed whatever
  a scrolled-back reader was looking at down a row — about once a second when the parish is
  busy. The one panel meant for going back and reading would not hold still.

The invariants are written as **bounds, not expected values** — a rebalance is meant to move
the numbers; what must not change is the shape.

### Sweeping the seeds

**After adding any check that measures a simulated run, sweep a dozen seeds before trusting
it.** A check that passes on one sample and fails on the next is worse than no check, and this
suite has produced two of them: a first-find bound sitting at the 92nd percentile of an
exponential wait, which failed roughly one seed in twelve, and a peak-production figure quoted
as though it meant something when it spans eight orders of magnitude across seeds.

```bash
for s in 1 2 3 4 5 6 7 8 9 10 11 12; do
  dotnet run -c Release --project projects/Hollowtide/tools/balance -- --seed $s
done
```

All twelve currently pass. Twelve is enough to catch a one-in-twelve fragility about as often
as not — it is a smell test, not a proof, and a check whose bound sits anywhere near the spread
of what it measures should be rewritten to assert the underlying rate instead of a sample of
it.


```bash
dotnet run -c Release --project projects/Hollowtide/tools/balance -- --seed 3
```

Shifts every seed in the suite. **A check that passes on one sample and fails on the next is
worse than no check**, and running a few offsets is the only way to tell those apart. Sweeping
found two checks that held on the seeds they were written with and broke on others — both
because they asserted a strict rarity ladder the *bands* did not actually provide. The honest
fix was to the bands, not the checks.

A guard that keeps being loosened to keep passing is telling you something about the thing it
guards.

### Three things to know before adding checks

**Verify a check can fail.** Several here passed while testing nothing: a fixture answering an
encounter with an offering it could not afford (so the walk never resolved and one stuck
visitor was counted 4,000 times), a fuzzer whose uniform draw communed so often the parish was
wiped before dread could ever build, and a ceiling measured while wearing relics that happened
to carry none of the power in question. Reintroduce the bug and watch it go red.

**Check what your instrument counts.** The relic grade distribution was measured through
`OnFound`, which only fires for relics that make it into the satchel — and a full satchel
refuses anything worse than its worst. It reported an inversion far more dramatic than the
real one. The same trap caught the offline report, which measured a keeper's gains from the
purse — and overseers *spend* from the purse while you are away.

**Do not let luck choose the subject.** Seeding a run is fine; letting the run decide what is
being measured is not. The relic power budget wore whatever the run happened to turn up and
scored 2.51x one time and 1.54x the next, drifting toward its own failure bound. It wears a
fixed loadout now.

## Shared budgets

Several fixed-size things are shared by every system that wants them, and are where features
collide invisibly — each works perfectly alone, and nothing fails loudly when they don't:

- **the whisper feed** — six lines, nine seconds each, so about forty a minute before lines are
  pushed off unread. Pinned in the harness for the *loudest* kind of play.
- **the popup pool** — twelve slots; slots are chosen by what is worth keeping, so ordinary
  clicking cannot wipe out the relic that clicking turned up.
- **the screen flash** — a visitation, an approach and a Hollowed find all reach for it. Always
  `Max`, never assignment, or a quiet event steals the screen from a loud one.

When adding anything that talks, pops, or flashes, ask what fixed-size thing it now shares.
