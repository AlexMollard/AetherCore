# AetherCore website

The public-facing site lives in `website/`. Edit that copy rather than the earlier temporary preview. It is plain HTML, CSS and JavaScript, with no build step or server-side runtime. Development articles live in `website/articles/`.

The landing page flows from the hero and direct Play action into one image-led feature explorer, a compact three-entry development-note preview, and a compact resource footer with Get started. The former workspace/showcase promotion blocks, oversized closing pitch and duplicate “Follow the systems” explorer are removed.

The hero uses a factual engine description, not an advertising slogan or repeated brand labels. It uses a foreground gradient into the page background on desktop and mobile. Keep that vertical fade above the horizontal text-contrast shade so the bottom edge reaches the exact background colour.

Headers follow the landing-page order: Features, Development, then direct Play, Docs and Get started links. The feature explorer uses desktop category tabs and a native mobile selector. Panels share a CSS grid cell: the tallest content determines the space, so switching categories does not move later sections, and no fixed pixel height or nested scrolling is needed. Inactive panels are inert and hidden from accessibility APIs; without JavaScript every category remains readable in document order.

### Engine captures

The website hero is a dedicated native Sanctuary daylight capture (`website/assets/website-hero.webp`); AetherCore branding remains unchanged (`website/assets/aethercore-app-icon.png`). Every feature category, homepage note, archive entry and article body has a distinct real capture—not alternate crops or encodings passed off as new images. Expanding an image intentionally shows that same capture at full resolution. Native and browser captions describe what is actually visible, not a still image as proof of live networking or script reload.

| Asset in `website/assets/` | Pixels | Source |
| --- | --- | --- |
| `website-hero.webp` | 2560 × 1440 | Dedicated WebsiteHero scene, captured from the native renderer’s final-color attachment |
| `native-authoring.webp` | 2560 × 1440 | Native FoxShowcase meadow, selected deer and component Inspector |
| `engine-frame.webp` | 2560 × 1440 | Native FoxShowcase pond and bridge, Render Graph and texture diagnostics |
| `engine-tools.webp` | 2560 × 1440 | Native material graph, sphere preview and scene viewport together; full 16:9 editor capture |
| `engine-inspector.webp` | 2560 × 1440 | Whisper’s title canvas and actual C# ConnectScreen references and timeout |
| `engine-publish.webp` | 2560 × 1440 | The exported WebGPU game and browser-player controls; a distinct woodland view |
| `engine-assets.webp` | 2560 × 1440 | FoxShowcase asset folders, selected tree, material and mesh references |
| `editor-2d.png` | 2560 × 1440 | INKBOUND Level1, Tile World and Tile Palette |
| `launcher.png` | 866 × 537 | Launcher’s Create Project dialog; no project was created |
| `fox-showcase.webp` | 1798 × 1011 | Hardware WebGPU gameplay after correcting the lower HUD edge anchors |
| `fox-journal.webp` | 840 × 490 | Illustrated journal, cropped from the same refreshed browser build |
| `feature-animation.webp` | 2560 × 1440 | FoxShowcase deer and its SkinnedMesh component |
| `feature-physics.webp` | 2560 × 1440 | FoxShowcase bridge with mesh collider and static-body properties |
| `feature-audio.webp` | 2560 × 1440 | Temporarily configured FoxShowcase Ruin chime entity with real clip and spatial Audio Source controls |
| `feature-networking.webp` | 2560 × 1440 | Whisper Arena session Inspector; authoring, not a live connection |
| `feature-ui.webp` | 2000 × 1125 | Browser FoxShowcase journal and field atlas over the sunset scene |
| `note-csharp.webp` | 2560 × 1440 | INKBOUND Level3 with PlayerController and AetherInk fields |
| `archive-csharp.webp` | 2560 × 1440 | Whisper Arena pause canvas with session, roster and pause-menu script fields |
| `article-csharp.webp` | 2560 × 1440 | INKBOUND Sandbox NpcWraith with DialogueTrigger and Npc properties |
| `article-tools.webp` | 2560 × 1440 | FoxShowcase project folders, selected tree and native material/mesh Inspector |
| `archive-tools.webp` | 2560 × 1440 | INKBOUND Sandbox multi-platform layout and populated Tile Palette |
| `article-render.webp` | 2560 × 1440 | HumanDemo walking figure, pillar shadows and reflective sphere in the native editor |
| `archive-render.webp` | 2560 × 1440 | Separate HumanDemo sphere and pillar framing with live Render Graph diagnostics |
| `note-render.webp` | 2560 × 1440 | HumanDemo blue glowing accent and pillar with Post Processing controls |
| `note-tools.webp` | 2560 × 1440 | INKBOUND’s existing wraith sprite opened in the Pixel Art editor; no pixels edited |

Raw PNGs, owned-process receipts and preference restoration evidence are in ignored `logs/website/current-captures/`. Native sessions used internal render readback, including the launcher, without desktop interaction. Scene framing and panel arrangement were staged in memory; editors closed with Discard. All five backed-up preferences/layout files were restored with matching SHA-256 hashes (`restored-final.json`). The final material, asset and C# sessions also restored their project-file baselines (`material-context/`, `asset-context/`, `whisper-script/`). Whisper’s screenshot shows the real `ConnectScreen` binding in `projects/Whisper/scenes/Title.scene.toml` and class in `projects/Whisper/scripts/ConnectScreen.cs`, not a fabricated FoxShowcase C# component. Native editor captures and browser captures remain explicitly distinguished; editor idle timings are not benchmarks.

Feature captures retain their raw evidence under `logs/website/feature-captures/`; distinct editorial captures use `logs/website/unique-captures/`. The editorial pass covers HumanDemo, INKBOUND and Whisper as well as FoxShowcase. Empty or clipped tooling views and unsuitable scene framings were rejected before integration. Editor camera guides remain visible where captured; screenshots are not retouched to remove them.

The audio image stages a functional “Ruin chime” entity in memory using `project://assets/audio/ruin-chime.wav`. Runtime evidence records `Play3D` with one active voice; the hidden session was muted, so audible output was not verified. This demonstrates component authoring, not an entity saved in the shipped scene. The session closed with changes discarded. Evidence: `logs/website/unique-captures/feature-audio-source.png` and `logs/agent-editor/1d3a25d4955e40fabeb40c4ee6537199/stderr.log`.

The bottom controls and journal hint now anchor flush to their respective canvas corners while retaining internal text padding. The production Web publisher passed after that source-scene correction; the staged package matches its manifest and publisher output byte-for-byte. Fresh browser captures exercised Play, journal open/close and resizing at 1920- and 1280-pixel viewport widths. This is local evidence, not target-host verification.

The final browser images were retaken after fixing graphics-key serialization. Native TOML keys are case-insensitive, while the runtime manifest reads camelCase names such as `exposureKey` and `gtaoStrength`. The previous export passed lowercase keys through and silently used defaults for those fields. The replacement applies FoxShowcase’s authored exposure key `0.06`, exposure speed `0.8`, GTAO strength `0.65` and grading controls. Manual exposure `0.85` was already read correctly. Images were not darkened in post.

The README uses one `docs/images/showcase.webp` composite (2048 × 1776), retaining its badges and logo. A fresh browser capture at FoxShowcase’s shrine leads the image; larger supporting crops show INKBOUND’s 2D viewport and the native material graph with its sphere preview. Each capture has a padded, rounded frame with a subtle warm border and short amber accent. The outer background and gutters are transparent so the image sits naturally on light or dark README backgrounds; the frames and screenshots remain opaque. The shrine source is `logs/website/current-captures/browser/readme-shrine-angle.png`. Assembly only crops, resizes, letterboxes and frames real captures; it does not repaint scenes or fabricate UI. Update the README image URL’s content-version query when replacing the image to avoid stale cached previews.

### Dedicated hero scene

`projects/FoxShowcase/scenes/WebsiteHero.scene.toml` is an independent composition using the existing terrain, trails, landscape and fox assets. It adds a daylight environment, a warm subject fill, shorter grass, an authored camera and a sanctuary arch. The original playable scene, project startup scene and browser package are unchanged.

To reproduce the capture, open WebsiteHero in the native editor, temporarily set `graphics.autoExposure=false` and `graphics.exposure=0.45`, then Play and capture `PostProcess.FinalColor`. Exposure is a project/editor setting, not scene-serialized state: restore the prior settings afterward. The capture contains no HUD or editor chrome and has no painted lighting or post-image brightness adjustment.

Raw evidence is `logs/website/hero-scene/hero-dense.png`; the selected WebP is copied unchanged to the website. The owned capture session restored the seven baselines listed in `logs/website/hero-scene/backup.json`. The website crop keeps the fox on the right on desktop and below the text on mobile; its shorter bottom fade retains the cast shadows.

### Adding development notes

Add the article under `website/articles/` and its image/title/description entry to `website/notes.html`. The archive uses an ordinary wrapping editorial grid; entries do not depend on JavaScript. Keep only the latest three entries on the homepage. Use separate captures for the homepage teaser, archive entry and article body. Preserve the article’s source links and implementation detail; the homepage is a preview, not a second copy of the article.

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

The check exercises desktop and mobile navigation, sticky headers, category selection and keyboard navigation, stable content positions when switching features, image dialogs, image loading, overflow, browser errors, and article/archive navigation. All feature content remains available without JavaScript. It also rejects game-package downloads during ordinary homepage browsing before Play. Screenshots go into ignored `logs/website/`. `AETHER_PLAYWRIGHT_MODULE` can point to an existing Playwright installation instead.

## GitHub Pages

`.github/workflows/pages.yml` publishes the **contents** of `website/` as the site root, preserving `.nojekyll` and relative URLs. It runs only through `workflow_dispatch`: pushes, pull requests and schedules do not deploy. The job uses a standard Ubuntu runner and runs only while this repository is public.

After committing and pushing the workflow to `master`, open **Actions → Publish website → Run workflow**, select `master`, and run it. Alternatively:

```powershell
gh workflow run pages.yml --ref master
```

Repository **Settings → Pages → Source** must be **GitHub Actions**; this was already configured when the workflow was added. The deployment URL is <https://alexmollard.github.io/AetherCore/>. The workflow uses the built-in token and the `github-pages` environment; no personal access token or paid service is required.

The owner approved one narrow storage exception: the Pages deployment artifact, retained for one day. No general artifacts, dependency caches, engine builds or local untracked captures are uploaded. Checkout includes only committed files, so the WIP game package stays local until deliberately added. Adding this workflow does not publish the site; dispatch it explicitly and verify the resulting live URL.

The public repository is eligible for free GitHub Pages hosting. WebGPU gameplay runs on the visitor's device; Pages serves the static loader, WebAssembly, and game assets rather than renting a GPU per session. Published sites are limited to 1 GB, with a soft bandwidth limit of 100 GB per month. Large or popular demo downloads can reach those limits; GitHub may rate-limit or stop serving the site rather than provide unlimited hosting.

See [GitHub Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits) and [publishing source configuration](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

## WebGPU demos

Use self-contained published game output described in [Web export](web-export.md), not the Python development-gallery server. A static demo must load entirely over HTTPS with project-relative paths and be verified in a WebGPU-capable browser.

### FoxShowcase

FoxShowcase's self-contained package is tracked in `website/demos/fox-showcase/`. It is copied byte-for-byte from the verified local publisher output, including the manifest and exactly its listed files. The homepage links to [the published player](https://alexmollard.github.io/AetherCore/demos/fox-showcase/) without downloading the game during ordinary homepage browsing.

The package includes the completed grass, direct third-person camera and illustrated journal work. Manual Pages run `40637287836` deployed the authored-graphics fix. The actual HTTPS player was then exercised on hardware WebGPU (NVIDIA, non-fallback): Play, keyboard movement, journal open/close, Stop and Restart all worked with no browser runtime errors or engine diagnostics. Hosted `project.js` exposed the authored `exposureKey: 0.06` and `gtaoStrength: 0.65`. Evidence images are `logs/website/current-captures/browser/hosted-journal.png` and `hosted-movement.png`. Replacement packages still require local export and nested-path/iframe checks followed by target-host verification.

Keep gameplay behind an explicit visitor action and use the player's Play gesture for keyboard focus and audio. Do not ship the development-gallery server. The staged game is approximately 28 MB; measure the complete site again after changing media or packages rather than treating this as a future release budget.

Run the existing production-export verifier against the replacement:

```powershell
$env:AETHER_PLAYWRIGHT_MODULE = (Resolve-Path .temp/website-checks/node_modules/playwright).Path
node tools/browser-tests/verify-export.mjs --root=website/demos/fox-showcase --report=logs/website/fox-staged-export.json
```

Build updates with the normal publisher described in [Web export](web-export.md). Distribute the manifest and exactly its listed files; preserve their bytes and hashes rather than editing generated loader/runtime files. Requalify the replacement before deployment. Keep ZIPs, verification reports, toolchains, and build caches out of `website/`.

`website/demos/**` disables Git text conversion: generated HTML and JavaScript are covered by manifest hashes just like the binary payloads. Keep that byte-preservation rule when adding another staged export.

### Exported player layout

`resources/web/index.html` is the shared source for published players and development previews. Change it rather than hand-editing a generated package. Its Night Amber palette, typography and controls follow the public website while keeping the game central. Standalone exports keep relative runtime paths and do not depend on website CSS or hosted fonts. Preserve project metadata, gallery comparisons, Play/Stop/Restart, keyboard focus, fullscreen, loading and failure guidance, and diagnostics when changing the layout.

The player heading omits gallery category and qualification badges; those belong in the gallery, not above the game. Live `?scene=` previews read the source template immediately. `/projects/` links serve existing exported packages unchanged, so those pages receive layout updates only after re-exporting.

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
