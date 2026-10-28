# AetherCore website

The public-facing site lives in `website/`. Edit that copy rather than the earlier temporary preview. It is plain HTML, CSS and JavaScript, with no build step or server-side runtime. Development articles live in `website/articles/`.

Development notes use image-led editorial rows rather than a card grid. The system explorer groups native disclosures with capture spaces, and articles retain implementation details and source links as prose and numbered steps rather than decorative architecture diagrams.

The hero uses a foreground gradient into the page background on desktop and mobile. Keep that vertical fade above the horizontal text-contrast shade so the bottom edge reaches the exact background colour.

### Capture placeholders

The committed website retains the approved HumanDemo hero image (`website/assets/human-demo.webp`), its fade into the background, and the AetherCore branding (`website/assets/aethercore-app-icon.png`). All other pictures use inline SVG capture placeholders, not image files. `data-capture-src` and `data-capture-image` retain intended image and zoom paths; workspace tabs preserve their selected capture path. README screenshot positions use text placeholders because GitHub does not reliably render inline image data URLs.

Other local images in `website/assets/` and source captures in ignored `logs/website/` are deliberately excluded from this commit. The FoxShowcase grass pass makes those captures outdated. Refresh the compositions, inspect their UI, then add the remaining image bundle in one separate commit and restore the active image/zoom URLs. Do not use the rejected publishing overview with buggy UI.

Earlier local capture evidence remains under `logs/website/fresh-native/` and `logs/website/fresh-publish/`. Those owned hidden-editor sessions closed with Discard and restored preferences/layouts with matching hashes. Background mode alone does not isolate preferences. The publishing-panel capture showed available controls, not a completed publish. Browser capture attempts later hung; old browser images are not fresh gameplay qualification.

## Local preview

From the repository root:

```powershell
python -m http.server 8000 --bind 127.0.0.1
```

Open <http://127.0.0.1:8000/website/>. Keeping the `/website/` prefix exercises relative asset and article URLs, as required for a GitHub Pages project site.

## Browser verification

Install the repository-pinned Playwright version into ignored local tooling:

```powershell
npm install --prefix .temp/website-checks --no-save --package-lock=false playwright@1.62.1
node .temp/website-checks/node_modules/playwright/cli.js install chromium
node tools/website/smoke.cjs http://127.0.0.1:8000/website/
```

The check exercises desktop and mobile navigation, the sticky header, editor tabs and keyboard input, image dialogs, disclosures, image loading, horizontal overflow, browser errors, and direct article navigation. It also rejects game-package downloads while browsing the landing page before Play. Screenshots go into ignored `logs/website/`. `AETHER_PLAYWRIGHT_MODULE` can point to an existing Playwright installation instead.

## GitHub Pages

Publish the **contents** of `website/` as the site root, preserving `.nojekyll` and the relative directory structure. GitHub Pages branch publishing only selects the repository root or `/docs`; it cannot select `/website` directly. A deliberate `workflow_dispatch`-only deployment can publish `website/` without mixing the engine documentation and website source. No deployment workflow is included or enabled by this migration.

The public repository is eligible for free GitHub Pages hosting. WebGPU gameplay runs on the visitor's device; Pages serves the static loader, WebAssembly, and game assets rather than renting a GPU per session. Published sites are limited to 1 GB, with a soft bandwidth limit of 100 GB per month. Large or popular demo downloads can reach those limits; GitHub may rate-limit or stop serving the site rather than provide unlimited hosting.

See [GitHub Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits) and [publishing source configuration](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

## WebGPU demos

Use self-contained published game output described in [Web export](web-export.md), not the Python development-gallery server. A static demo must load entirely over HTTPS with project-relative paths and be verified in a WebGPU-capable browser.

### FoxShowcase

FoxShowcase is the intended featured 3D project. Its existing package in `website/demos/fox-showcase/` remains local and is deliberately excluded from this commit, together with screenshot assets. The homepage marks the browser build as pending; it must not link to an absent export or download game files during ordinary browsing.

After the grass pass, publish a fresh self-contained build, verify it locally and on the target HTTPS host, then enable the homepage Play link. Keep gameplay behind an explicit visitor action and use the player’s Play gesture for keyboard focus and audio. Do not ship the development-gallery server.

The earlier local export was about 20.2 MB, but that is not the size of a future release. Recalculate the complete site and game transfer budget after publishing the replacement. Earlier evidence in `logs/website/fox-staged-export.json` is historical; it does not qualify the next export or GitHub Pages deployment.

Run the existing production-export verifier against the replacement:

```powershell
$env:AETHER_PLAYWRIGHT_MODULE = (Resolve-Path .temp/website-checks/node_modules/playwright).Path
node tools/browser-tests/verify-export.mjs --root=website/demos/fox-showcase --report=logs/website/fox-staged-export.json
```

Build updates with the normal publisher described in [Web export](web-export.md). Distribute the manifest and exactly its listed files; preserve their bytes and hashes rather than editing generated loader/runtime files. Requalify the replacement before deployment. Keep ZIPs, verification reports, toolchains, and build caches out of `website/`.

### Exported player layout

`resources/web/index.html` is the shared source for published players and development previews. Change it rather than hand-editing a generated package. Its Night Amber palette, typography and controls follow the public website while keeping the game central. Standalone exports keep relative runtime paths and do not depend on website CSS or hosted fonts. Preserve project metadata, gallery comparisons, Play/Stop/Restart, keyboard focus, fullscreen, loading and failure guidance, and diagnostics when changing the layout.

Layout verification exercised the source shell against the existing Ember Run standalone payload using a temporary browser document override, without rewriting that package or its manifest. Play, canvas focus, fullscreen, Stop, Restart and missing-WebGPU guidance were exercised. Responsive checks covered 1920, 1440, 768 and 390 pixels plus an 844 × 390 landscape viewport. This verifies the player shell, not a newly published FoxShowcase build.

### Hard requirement: free playable games and tests

Visitors should be able to launch supported AetherCore games and interactive engine tests from the public website, without installing the engine. Hosting, deployment, and ongoing operation must cost the owner nothing.

- Prefer GitHub Pages with self-contained static WebGPU exports and the free GitHub-provided domain. Compute and rendering run on the visitor's device, not a hosted GPU.
- Do not introduce paid backends, metered services, paid runners, paid storage, automatic upgrades, or a paid fallback. Keep builds local and publishing deliberate; do not enable automatic workflows.
- Load game packages only after an explicit Play action, not on landing-page load. Keep the combined published site within the 1 GB limit and account for game downloads against the 100 GB/month soft bandwidth limit.
- Before publishing each game or test, verify its real export on the target HTTPS host: asset loading, WebGPU startup, input, and gameplay/test behavior. A working local development gallery is not proof of static-host compatibility. Unsupported browsers must receive a clear message rather than a paid or server-rendered fallback.
- GitHub Pages is not a multiplayer server or native test runner. Only browser-compatible games and tests that need no paid server-side runtime are eligible.
- If a required export or deployment cannot operate entirely for free, evaluate another hosting method against its current free-tier terms and technical requirements. Do not switch providers or accept overage billing automatically.
- If no verified zero-cost option fits, omit the playable demo or remove it from hosting and retain screenshots, video, or source links. The website remains useful without live gameplay. Free hosting takes precedence over having a playable demo.

This is a bounded free-hosting requirement, not a promise of unlimited traffic or compatibility with every AetherCore game. Live demos remain unpublished until those checks pass.
