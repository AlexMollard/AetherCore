"""Bounded subprocess evidence for the real Rust SDK provider callback boundary.

Build the fixture provider_probe binary in dev and abort profiles, then run this
script. Recoverable failures must remain callable for cleanup; aborting panics
must terminate a separate child. No engine/GPU or third-party Python modules needed.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def run_case(name: str, executable: Path, mode: str, aborts: bool, timeout: float, output: Path):
    started = time.monotonic()
    env = dict(os.environ)
    env.pop("AETHER_RUST_FIXTURE_FAILURE", None)
    try:
        process = subprocess.run(
            [str(executable.resolve()), mode], capture_output=True,
            timeout=timeout, env=env, cwd=ROOT, check=False,
        )
        text = (process.stdout + process.stderr).decode("utf-8", errors="replace")
        output.write_text(text, encoding="utf-8")
        entered = f"PROBE_ENTER_UPDATE {mode}" in text
        finished = f"PROBE_PASSED {mode}" in text
        passed = entered and ((process.returncode != 0 and not finished) if aborts else (process.returncode == 0 and finished))
        code = "RUST_PROCESS_ABORT" if aborts else "RUST_PROVIDER_CONTAINED"
        if not passed:
            code = "RUST_PROCESS_EXPECTATION_FAILED"
        message = f"{name}: child exit={process.returncode}, entered_update={entered}, completed={finished}"
    except subprocess.TimeoutExpired as error:
        partial = (error.stdout or b"") + (error.stderr or b"")
        output.write_text(partial.decode("utf-8", errors="replace"), encoding="utf-8")
        passed, code, message = False, "RUST_PROCESS_TIMEOUT", f"{name}: child exceeded {timeout:g}s and was killed"
    except OSError as error:
        output.write_text(str(error), encoding="utf-8")
        passed, code, message = False, "RUST_PROCESS_START_FAILED", f"{name}: {error}"
    return (
        {"name": name, "status": "passed" if passed else "failed"},
        {"code": code, "severity": "info" if passed else "error", "message": message},
        int((time.monotonic() - started) * 1000),
    )


def main() -> int:
    suffix = ".exe" if os.name == "nt" else ""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=ROOT / f"artifacts/task5/fixture/debug/provider_probe{suffix}")
    parser.add_argument("--panic-abort-probe", type=Path, default=ROOT / f"artifacts/task5/abort/abort/provider_probe{suffix}")
    parser.add_argument("--report", type=Path, default=ROOT / "logs/task5/rust-provider-process-report.json")
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be greater than zero and at most 60 seconds")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    checks, diagnostics, artifacts = [], [], []
    cases = [(mode, args.probe, mode, mode == "abort") for mode in ("normal", "error", "unwind", "abort")]
    cases.append(("panic-abort", args.panic_abort_probe, "unwind", True))
    for name, executable, mode, aborts in cases:
        output = args.report.parent / f"rust-provider-{name}.log"
        check, diagnostic, _ = run_case(name, executable, mode, aborts, args.timeout, output)
        checks.append(check)
        diagnostics.append(diagnostic)
        artifacts.append({"path": str(output.resolve()), "bytes": output.stat().st_size})
        print(diagnostic["message"], flush=True)
    passed = all(check["status"] == "passed" for check in checks)
    report = {
        "schemaVersion": 1, "jobId": "rust-provider-process", "status": "passed" if passed else "failed",
        "phase": "test", "target": f"{platform.system().lower()}-{platform.machine().lower()}",
        "language": "rust", "profile": "desktop", "diagnostics": diagnostics,
        "toolchains": [], "artifacts": artifacts, "requiredChecks": checks,
        "durationMs": int((time.monotonic() - start) * 1000), "exitCode": 0 if passed else 1,
    }
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report["exitCode"]


if __name__ == "__main__":
    raise SystemExit(main())
