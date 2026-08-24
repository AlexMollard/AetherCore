# Hollowtide

An idle game about keeping a parish that something is coming for.

You gather ichor by hand and buy rites that gather it for you. Owning rites draws **dread**,
and dread is the bargain at the centre of the game: it pays up to several times over, and
when the meter fills something walks toward you. Everything else in here exists to make that
one trade interesting.

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
scripts/{Hud,Ledger,Parish,Congregation,Whispers}.cs   views onto the simulation
assets/shaders/ui_relic.slang   relics drawn from their seed; grade adds layers, not colours
tools/generate_scenes.py   scaffolds the authored chrome - read its header before running it
tools/balance/             the balance harness
```

`Vigil`, `Relics`, `Content` and `VigilSaveData` carry **no `AetherCore` reference**. That is
not tidiness — it is what lets the harness compile the rules on their own and play them ten
thousand times without a window, and what lets a whole save round-trip through JSON in a test
that never goes near the player's real one.

## The balance harness

```bash
dotnet run -c Release --project projects/Hollowtide/tools/balance
```

**Run it after any change to the economy.** It plays the real rules at speed and checks ~100
invariants, printing PASS/FAIL and returning non-zero on a break.

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

The invariants are written as **bounds, not expected values** — a rebalance is meant to move
the numbers; what must not change is the shape.

### Two things to know before adding checks

**Verify a check can fail.** Several here passed while testing nothing: a fixture answering an
encounter with an offering it could not afford (so the walk never resolved and one stuck
visitor was counted 4,000 times), a fuzzer whose uniform draw communed so often the parish was
wiped before dread could ever build, and a ceiling measured while wearing relics that happened
to carry none of the power in question. Reintroduce the bug and watch it go red.

**Check what your instrument counts.** The relic grade distribution was measured through
`OnFound`, which only fires for relics that make it into the satchel — and a full satchel
refuses anything worse than its worst. It reported an inversion far more dramatic than the
real one.

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
