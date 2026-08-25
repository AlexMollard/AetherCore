#!/usr/bin/env python3
"""Everything that has to be true before Hollowtide is committed.

Run it from anywhere:

    python projects/Hollowtide/tools/verify.py
    python projects/Hollowtide/tools/verify.py --seeds 3 7 11

WHY THIS EXISTS. Verifying by hand meant three commands chained with `&&`, and the
harness's result was read by piping it to `tail` to see the last line - which makes the
pipeline's exit status `tail`'s, not the harness's. A run that printed "1 invariant(s)
broken" in plain sight therefore reported success to the shell, the chain carried on, and
a red tree was committed and pushed. The failure was not carelessness about the output; it
was that the output and the exit code had been separated.

So this script never pipes anything. Each step's return code is checked directly, every
step runs even if an earlier one failed (so one command tells you everything that is
wrong, not just the first thing), and the summary at the end is the exit code.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(PROJECT))

GAME = os.path.join(PROJECT, "scripts", "AetherGame.csproj")
HARNESS = os.path.join(PROJECT, "tools", "balance")
SHADERS = os.path.join(PROJECT, "assets", "shaders")

# The compiler is not on PATH on either machine this is built on, so it is looked for
# where it actually installs. A missing compiler SKIPS the shader step loudly rather than
# passing it quietly - the whole point of this file is that nothing green is a guess.
SLANGC_CANDIDATES = [
    r"C:/VulkanSDK/1.4.350.0/Bin/slangc.exe",
    "/usr/bin/slangc",
    "slangc",
]


def run(label, argv, cwd=None, warnings_fail=False):
    """One step. Prints its own result and returns True if it passed.

    `warnings_fail` treats compiler warnings as a failure. Applied to the things this
    project builds, because a warning nobody is required to fix is a warning that
    accumulates until it hides the one that mattered - and the harness is what decides
    whether anything here is green, so it is the last place noise should be tolerated.
    """
    print(f"  {label} ... ", end="", flush=True)
    run.output = ""
    try:
        done = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    except FileNotFoundError:
        print("SKIPPED (not installed)")
        return None
    run.output = done.stdout + done.stderr
    if done.returncode == 0:
        noisy = [line for line in (done.stdout + done.stderr).splitlines() if "warning CS" in line]
        if warnings_fail and noisy:
            print(f"FAILED ({len(noisy)} warnings)")
            for line in noisy[:10]:
                print("      " + line.strip())
            return False
        print("ok")
        return True
    print("FAILED")
    # Only on failure, and only the tail of it: enough to see what broke without burying
    # the summary. This is the one place output is printed, and it is never what the exit
    # code is read from.
    for line in (done.stdout + done.stderr).strip().splitlines()[-25:]:
        print("      " + line)
    return False


def counted(output):
    """How many invariants the harness said it checked, or None if it did not say."""
    found = re.search(r"(\d+) invariants checked", output)
    return int(found.group(1)) if found else None


def docs_quote(count):
    """Whether the README and the changelog both quote the harness's own figure.

    The number lived in two prose files and was maintained by hand, so it documented
    whenever somebody last remembered rather than what the suite does: the changelog sat
    three commits behind the README, and both had been "updated" in the same breath. The
    harness counts its checks now, and this makes the prose answer to it - the wording wraps
    across a line break in one of the files, which is exactly how the last sed missed it, so
    the whitespace between the number and the word is deliberately anything at all.
    """
    print(f"  the docs quote {count} ... ", end="", flush=True)
    wrong = []
    for name in ("README.md", "CHANGELOG.md"):
        path = os.path.join(PROJECT, name)
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        said = re.findall(r"\*\*(\d+)\s+invariants", text)
        if not said:
            wrong.append(f"{name} quotes no figure at all")
        elif any(int(one) != count for one in said):
            wrong.append(f"{name} says {', '.join(said)}")
    if wrong:
        print("FAILED")
        for line in wrong:
            print("      " + line)
        return False
    print("ok")
    return True


def find_slangc():
    for candidate in SLANGC_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description="Verify Hollowtide.")
    parser.add_argument("--seeds", nargs="*", type=int, default=[3, 7],
                        help="extra seeds to sweep the balance harness against")
    args = parser.parse_args()

    results = []

    print("Building the game's scripts")
    results.append(("build", run("AetherGame",
                                 ["dotnet", "build", GAME, "-v", "n", "--nologo"],
                                 warnings_fail=True)))
    # Release, matching the configuration the harness is RUN in below. Building Debug here and
    # running Release meant two sets of artefacts, and on a cold output directory the two
    # collided: the run died partway through and reported a failure that was not there.
    results.append(("build harness",
                    run("balance harness (build)",
                        ["dotnet", "build", os.path.join(HARNESS, "Balance.csproj"),
                         "-c", "Release", "-v", "n", "--nologo"],
                        warnings_fail=True)))

    print("Playing the rules")
    results.append(("harness", run("balance harness",
                                   ["dotnet", "run", "-c", "Release", "--nologo", "--project", HARNESS])))
    # Only the DEFAULT seed is counted against the docs: a shifted seed deliberately skips the
    # checks that measure arithmetic rather than luck, so its total is legitimately smaller.
    total = counted(run.output)
    if total is None:
        print("  the docs quote ... SKIPPED (the harness printed no count)")
        results.append(("docs", None))
    else:
        results.append(("docs", docs_quote(total)))
    for seed in args.seeds:
        results.append((f"harness --seed {seed}",
                        run(f"seed {seed}",
                            ["dotnet", "run", "-c", "Release", "--nologo", "--project", HARNESS,
                             "--", "--seed", str(seed)])))

    print("Compiling the shaders")
    slangc = find_slangc()
    if slangc is None:
        print("  SKIPPED - no slangc found; the shaders were NOT checked")
        results.append(("shaders", None))
    else:
        # A real file, not the null device: slangc refuses to open 'nul' and reports it as a
        # compile error, which made every shader look broken the first time this ran.
        with tempfile.TemporaryDirectory() as scratch:
            for name in sorted(os.listdir(SHADERS)):
                if not name.endswith(".slang"):
                    continue
                # Compiled from the REPO ROOT: the include path is relative to it, and
                # running from anywhere else fails in a way that looks like a broken shader.
                results.append((name, run(name, [
                    slangc, os.path.join(SHADERS, name),
                    "-I", os.path.join(REPO, "src", "shaders"),
                    "-target", "spirv", "-emit-spirv-directly",
                    "-fvk-use-scalar-layout", "-matrix-layout-column-major",
                    "-o", os.path.join(scratch, name + ".spv"),
                ], cwd=REPO)))

    failed = [name for name, ok in results if ok is False]
    skipped = [name for name, ok in results if ok is None]

    print()
    if skipped:
        print("Skipped: " + ", ".join(skipped))
    if failed:
        print("BROKEN: " + ", ".join(failed))
        return 1
    print("Everything holds.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
