# Private desktop support and release policy

Owner decision, 2026-10-31: keep AetherCore private; defer the engine distribution
license. The first production platform targets are Windows and Linux desktop.
This policy defines release gates, not a claim that every gate has passed.

## Support contract

Each private release must name its exact engine commit, version, build tier,
compiler, operating system, GPU and driver versions, managed runtime and gameplay
SDK versions. Only combinations with attached passing evidence are qualified.
Windows and Linux need separate native builds, CTest runs, GPU/lifecycle runs and
delivered-package tests. A Windows Clang build does not qualify Linux.

The native manifest requires Windows 10 version 1903 or later to select the UTF-8
process code page through `activeCodePage`. This is a technical floor, not a promise
that every Windows version from 1903 onward is supported. Each release must use the
intersection of the engine, bundled .NET runtime, tools and GPU driver's supported
OS versions, then qualify the exact combination. See [Microsoft's UTF-8 process code
page documentation](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)
and the [.NET 10 supported OS matrix](https://github.com/dotnet/core/blob/main/release-notes/10.0/supported-os.md).
Linux and additional GPU/driver combinations remain untested until their separate
qualification evidence is attached.

Offline projects and trusted local development are the current qualification scope.
Public multiplayer is unqualified: bounded loopback tests do not establish Internet
hosting, authentication, abuse resistance or an operational service commitment.

The current Vulkan backend still requires Vulkan 1.4 and its declared mandatory
device features. Host image copy is optional. Device rejection must list missing
requirements; an untested card is not supported solely because it is six years old
or belongs to a supported vendor. Record discrete/integrated AMD, NVIDIA and Intel
coverage before making a vendor-wide compatibility promise. Both texture upload
paths must pass validation/readback on a device that exposes host image copy.

Web profiles remain experimental and retain their separate documented limits.
Editor keyboard/accessibility, undo/recovery coverage and extension stability must
be qualified per operation before promising complete editor parity with another engine.

## Versions and migration

During 0.x development, minor releases may change gameplay APIs and stored formats.
Patch releases should contain compatible fixes. Every incompatible change requires
a migration note stating affected formats/APIs, detection, backup, conversion and
rollback steps. Never silently overwrite the only copy of an older project. Test
upgrade with a copy of the previous supported project and retain the original.

The latest qualified private release receives fixes. Older releases have no promised
backport window until the owner explicitly adopts one. A release manifest records
the supported baseline and known issues; unfinished gates block a production label.

## Reports and trusted local control

Report ordinary defects through the existing private repository issue tracker with
the release manifest, reproduction and redacted logs. Security-sensitive reports go
directly to the repository owner through an existing private channel; no public
reporting address or response-time commitment has been established.

The editor control endpoint is a trusted-local development interface on loopback.
Do not expose or forward it to an untrusted network. Treat local accounts that can
connect as trusted; authentication between mutually untrusted local processes is
not currently a promised boundary. Packaged workflows must explicitly choose whether
control is enabled. Disable it with `AETHER_CONTROL_PORT=off` when it is unnecessary.

## Required evidence

- All expected CTest suites present and passing, plus the full GPU gauntlet.
- Strict Clang and Windows MSVC builds; separate Linux qualification.
- Debug/Dev and shipping Release/Retail checks, with ASAN where supported.
- Graceful Full/UiShell/runtime shutdown under deadlines, repeated lifecycle tests
  and a recorded long-running soak; forced termination is a failure.
- Isolated staged-product create/import/compile/play/save/reopen/publish and copied
  game launch, including spaces/Unicode paths. Clean standard-user VM installation,
  upgrade and uninstall must preserve user projects and settings.
- Artifact hashes, source/tool/runtime/dependency provenance, separate matching
  symbols, dependency notices and explicit signature status. An unsigned private
  artifact must identify itself as unsigned; no production signing claim without
  a verified signature and owner-managed credentials.

Use `AETHER_USER_DATA_DIR` with an absolute path for isolated app state. Invalid
overrides disable persistence instead of falling back to the real user's settings.
This isolates engine config; it does not turn a developer machine into a clean VM.

The implementation ledger is
[the production-readiness plan](superpowers/plans/2026-10-31-production-readiness.md).
