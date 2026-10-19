"""Run bounded desktop C#/Rust fixtures and a documented compiler repair exercise.

Each sample uses a new source/artifact directory: cold means an empty local
project build directory, not empty OS, NuGet, Cargo or GPU driver caches.
"""
from __future__ import annotations

import argparse
import json
import math
import platform
from pathlib import Path
import shutil
import statistics
import time
import uuid

from gameplay_jobs import _OwnedProcess, project_tool
from reference_fixture import ROOT


def equivalent(a, b, tolerance):
    if type(a) in (int, float) and type(b) in (int, float):
        return math.isfinite(a) and math.isfinite(b) and abs(a - b) <= tolerance
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(equivalent(x, y, tolerance) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


def compare_observations(left, right):
    def entries(state):
        result = {}
        for item in state["assertions"]:
            key = (item["entity"], item["type"], item["property"])
            if key in result:
                raise ValueError("Repeated observable assertion: " + str(key))
            result[key] = item
        return result

    a, b = entries(left), entries(right)
    failures = []
    for key in sorted(a.keys() | b.keys()):
        x, y = a.get(key), b.get(key)
        if x is None or y is None or not x["passed"] or not y["passed"]:
            failures.append(str(key) + ": missing or failed assertion")
        elif not equivalent(x["actual"], y["actual"], max(x.get("tolerance", 0), y.get("tolerance", 0))):
            failures.append(str(key) + ": observable outcomes differ")
    if not a and not b:
        failures.append("No observable assertions were compared")
    return failures


def readiness_ms(receipt, started_unix_ns, completion_ms):
    value = receipt["firstTickUnixNs"]
    if type(value) is not int:
        raise ValueError("First-tick readiness timestamp must be an integer")
    elapsed = (value - started_unix_ns) / 1_000_000
    if elapsed < 0 or elapsed > completion_ms:
        raise ValueError("First-tick readiness timestamp is outside the operation; wall clock may have changed")
    return elapsed


def run(executable, request, directory, label):
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / (label + "-request.json")
    report_path = directory / "report.json"
    source.write_text(json.dumps(request, indent=2), encoding="utf-8")
    report_path.unlink(missing_ok=True)
    ready_path = directory / "ready.json"
    ready_path.unlink(missing_ok=True)
    started_unix_ns = time.time_ns()
    started = time.perf_counter()
    with (directory / (label + "-cli.log")).open("wb") as output:
        process = _OwnedProcess([str(executable), "--request", str(source), "--report", str(report_path)], ROOT, output)
        try:
            while process.poll() is None:
                if time.perf_counter() - started > request["timeoutMs"] / 1000 + 15:
                    process.kill()
                    raise TimeoutError("Reference operation exceeded the owned process deadline")
                time.sleep(.05)
            exit_code = process.poll()
        finally:
            process.close()
    elapsed = (time.perf_counter() - started) * 1000
    report = json.loads(report_path.read_text(encoding="utf-8"))
    (directory / (label + "-report.json")).write_text(json.dumps(report, indent=2), encoding="utf-8")
    if (exit_code == 0) != (report["status"] == "passed"):
        raise RuntimeError("Native exit code and report disagree")
    ready = None
    if ready_path.is_file():
        receipt = json.loads(ready_path.read_text(encoding="utf-8"))
        ready = readiness_ms(receipt, started_unix_ns, elapsed)
        shutil.copyfile(ready_path, directory / (label + "-ready.json"))
    if report["status"] == "passed" and request["operation"] == "test" and ready is None:
        raise RuntimeError("Passing desktop test did not provide a first-tick readiness receipt")
    return report, elapsed, ready


def require_passed(report, label):
    if report["status"] != "passed":
        raise RuntimeError(label + " failed: " + json.dumps(report["diagnostics"]))


def prepare(directory, language):
    original = ROOT / "projects" / ("AgentPipeline" + language)
    shutil.copytree(original, directory, ignore=shutil.ignore_patterns("Builds", "target", "obj", "bin", "artifacts", "*.scene.bin", "*.prefab.bin"))
    if language == "Rust":
        manifest = directory / "scripts/Cargo.toml"
        text = manifest.read_text(encoding="utf-8").replace('../../../rust/aethercore', (ROOT / 'rust/aethercore').as_posix())
        manifest.write_text(text, encoding="utf-8")
    else:
        manifest = directory / "scripts/AetherGame.csproj"
        text = manifest.read_text(encoding="utf-8").replace('../../../managed/AetherCore/AetherCore.csproj', (ROOT / 'managed/AetherCore/AetherCore.csproj').as_posix())
        manifest.write_text(text, encoding="utf-8")


def sample(executable, directory, language):
    project = directory / (language + "Reference")
    prepare(project, language)
    launch = json.loads((project / "reference-test.json").read_text(encoding="utf-8"))
    request = dict(project=str(project), operation="build", ticks=90, scene="ReferenceStart", timeoutMs=120000, launchConfig=launch)
    artifacts = directory / "artifacts"
    cold, cold_ms, _ = run(executable, request, artifacts, "cold-build")
    require_passed(cold, "cold build")
    warm, warm_ms, _ = run(executable, request, artifacts, "warm-build")
    require_passed(warm, "warm build")
    request["operation"] = "test"
    baseline, baseline_ms, ready_ms = run(executable, request, artifacts, "baseline")
    require_passed(baseline, "baseline desktop fixture")
    states = {}
    for name in ("checkpoint-10", "checkpoint-20", "checkpoint-59", "state"):
        states[name] = json.loads((artifacts / (name + ".json")).read_text(encoding="utf-8"))
        shutil.copyfile(artifacts / (name + ".json"), artifacts / ("baseline-" + name + ".json"))
    if not (artifacts / "reference.png").is_file():
        raise RuntimeError("Desktop renderer did not produce its capture")

    shutil.copyfile(artifacts / "reference.png", artifacts / "baseline-reference.png")

    # Same scripted edit in both languages, with one injected unresolved symbol.
    source = project / ("scripts/src/reference.rs" if language == "Rust" else "scripts/Reference.cs")
    original = source.read_text(encoding="utf-8")
    old = "ctx.entity(), 2., 0." if language == "Rust" else "new Vector2(2, 0)"
    broken = "ctx.entity(), REFERENCE_SPEED_MISSING, 0." if language == "Rust" else "new Vector2(REFERENCE_SPEED_MISSING, 0)"
    repaired = "ctx.entity(), 2.1, 0." if language == "Rust" else "new Vector2(2.1f, 0)"
    if original.count(old) != 1:
        raise RuntimeError("Reference movement edit anchor changed")
    edit_started = time.perf_counter()
    source.write_text(original.replace(old, broken), encoding="utf-8")
    request["operation"] = "build"
    feedback, feedback_ms, _ = run(executable, request, artifacts, "compiler-feedback")
    if feedback["status"] != "failed" or not any("REFERENCE_SPEED_MISSING" in d["message"] and "source" in d for d in feedback["diagnostics"]):
        raise RuntimeError("Compiler did not return a source-located diagnostic for the injected error")
    source.write_text(original.replace(old, repaired), encoding="utf-8")
    request["operation"] = "test"
    correction_started = time.perf_counter()
    corrected, correction_ms, corrected_ready_ms = run(executable, request, artifacts, "corrected")
    require_passed(corrected, "corrected desktop fixture")
    return dict(language=language.lower(), coldBuildMs=cold_ms, warmBuildMs=warm_ms, baselineBuildToReadyMs=ready_ms,
                compilerFeedbackMs=feedback_ms, correctionBuildToReadyMs=corrected_ready_ms,
                baselineBuildToCompletionMs=baseline_ms, correctionBuildToCompletionMs=correction_ms,
                editToReadyMs=(correction_started - edit_started) * 1000 + corrected_ready_ms,
                editToCompletionMs=(time.perf_counter() - edit_started) * 1000, correctionCycles=1,
                runtime=states["state"].get("timing"), memory=states["state"].get("memory"),
                toolchains=baseline["toolchains"], artifacts=str(artifacts)), states


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--executable", type=Path, default=project_tool())
    args = parser.parse_args()
    if not 1 <= args.samples <= 20:
        parser.error("--samples must be in [1, 20]")
    # Check the shared data without consuming this command's argument parser.
    from reference_fixture import PROJECTS, assets
    for project in PROJECTS:
        for name, expected in assets().items():
            actual = (project / name).read_bytes()
            if name.endswith((".toml", ".json")):
                actual, expected = actual.replace(b"\r\n", b"\n"), expected.replace(b"\r\n", b"\n")
            if actual != expected:
                raise RuntimeError("Shared reference data changed: " + str(project / name))
    output = (args.output or ROOT / "logs/reference-comparison" / str(uuid.uuid4())).resolve()
    # Never replace a previous measurement or delete a caller-provided directory.
    output.mkdir(parents=True, exist_ok=False)
    result = dict(schemaVersion=1, status="running", machine=platform.platform(), samples=[], comparisons=[],
                  methodology="Fixed 90 ticks at 1/60s; alternating language order; fresh project build directories; then warm build, desktop test, unresolved speed symbol and correction to 2.1 units/s.",
                  limitations=["OS/package/driver caches are not cleared; cold means project-local build artifacts only.",
                               "Compiler repair is scripted; it does not measure autonomous agent productivity.",
                               "Agent audio is muted; clip/voice startup is verified, audible speaker output is not.",
                               "CPU update and wall frame intervals include native engine overhead; GPU execution timing is not measured.",
                               "Memory includes native engine, GPU driver and selected gameplay host; no language ranking is inferred.",
                               "Readiness is the first completed gameplay tick, recorded by a Unix-nanosecond receipt; out-of-range wall-clock changes fail measurement."])
    try:
        for index in range(args.samples):
            states = {}
            order = ("Rust", "CSharp") if index % 2 == 0 else ("CSharp", "Rust")
            for language in order:
                print(f"Sample {index + 1}/{args.samples}: {language}", flush=True)
                metrics, states[language] = sample(args.executable.resolve(), output / f"sample-{index + 1}" / language, language)
                metrics["sample"] = index + 1
                result["samples"].append(metrics)
            failures = []
            for checkpoint in states["Rust"]:
                failures.extend(checkpoint + ": " + message for message in compare_observations(states["Rust"][checkpoint], states["CSharp"][checkpoint]))
            result["comparisons"].append(dict(sample=index + 1, passed=not failures, failures=failures))
            if failures:
                raise RuntimeError("Observable reference outcomes differ: " + str(failures))
        result["status"] = "passed"
        result["medians"] = {language: {key: statistics.median(item[key] for item in result["samples"] if item["language"] == language)
                                     for key in ("coldBuildMs", "warmBuildMs", "editToReadyMs", "compilerFeedbackMs")}
                             for language in ("rust", "csharp")}
    except BaseException as error:
        result["status"] = "failed"
        result["error"] = str(error)
        raise
    finally:
        (output / "comparison.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print("Comparison report: " + str(output / "comparison.json"), flush=True)


if __name__ == "__main__":
    main()
