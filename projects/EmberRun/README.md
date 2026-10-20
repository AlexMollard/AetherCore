# Ember Run

A small playable Rust game running through AetherCore's shared gameplay host on desktop and WebGPU.

Collect amber embers for 30 seconds while dodging red hazards. You have three lives.
Move with WASD or arrow keys, dash with Space, and restart a round with R. Your best
completed score is saved locally. Play focuses the canvas immediately; clicking outside
releases keyboard input.

Open this folder in the editor, or publish with:

```powershell
./scripts/Run-GameplayPipeline.ps1 -Project projects/EmberRun -Operation publish -Target web
```

The export is under `Builds/Web/Ember Run/`. Serve the complete folder over localhost
or HTTPS; opening `index.html` directly displays local-server instructions.
The development Web runtime at an empty URL also selects this scene. Explicit reference
fixture URLs keep using the deterministic qualification game.

The sprite art and short audio tones are authored for this sample and may be reused.
