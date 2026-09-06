"""
Crops the ModelPreviewService atlas capture into per-prop icon PNGs and keys out
the render's empty-background colour to real alpha transparency.

Not a shipped asset - a throwaway tool for this one bake pass. Usage:
    python bake_icons.py <atlas.png> <out_dir> <slot_map.json>

slot_map.json: {"CrateSmall": 0, "Barrel": 1, ...}  (atlas slot index -> name)
Atlas layout (must match ModelPreviewService.cpp): 8 columns, 128px per slot.

KEY COLOUR: (0,0,0) - pure black, NOT the magenta originally planned. Verified from
source, not assumed: ModelPreviewService's forward pass (ModelPreviewService.cpp:384-392)
has no background/skybox draw - "Previews render their own small view with no lens of
their own" - so any pixel not covered by model geometry keeps the pass's clear value,
which RenderGraph.hpp:65 defaults to ClearColorValue(0,0,0,1) and nothing in
ModelPreviewService's AddDrawQueuePass call overrides. There is no exposed way to
change this to an arbitrary key colour like magenta without an engine edit, so black is
what we actually get.

KEYING IS A BORDER-CONNECTED FLOOD FILL, NOT A GLOBAL COLOUR THRESHOLD. A global
threshold answers "is this pixel dark enough", which is the wrong question: a genuinely
dark INTERIOR surface (plausible on TrashCan or a dark barrel) would punch a hole in the
middle of the icon, which reads as a rendering bug rather than a keying artefact. Flood
filling from the tile's border answers the question that's actually meant: "is this
pixel part of the connected background region". Only background-connected dark pixels
become transparent; an interior dark pixel that never touches the border through other
dark pixels survives regardless of how close to black it is. This also means the
threshold can be GENEROUS rather than tight, since it can now only ever eat background
that is actually reachable from the edge of the image.

Two known caveats, checked rather than assumed:
- A model touching the tile edge would let the fill leak inside through the contact
  point. ModelPreviewService frames each model with margin, so this should not happen -
  but it is asserted, not trusted: if the flood fill consumes more than
  LEAK_FRACTION_LIMIT of the tile, that tile is reported and NOT written, rather than
  silently saved with the model itself half-eaten.
- An enclosed background region not touching the border (e.g. visible through a handle)
  would stay opaque rather than keyed - an honest, disclosed tradeoff, not a bug. Of the
  16 catalogue props this script is meant to run on, TrashCan is the one plausible case
  (an open top could show background straight down into it, though whether that patch is
  border-connected around the rim or fully enclosed depends on the actual camera framing)
  - worth a specific visual check on that one icon rather than assuming either way.
"""
import json
import sys
from collections import deque

from PIL import Image

SLOT_SIZE = 128
ATLAS_COLS = 8
KEY_COLOR = (0, 0, 0)  # verified from source: RenderGraph.hpp's default clear value,
# unoverridden by ModelPreviewService - see module docstring.
FLOOD_THRESHOLD = 24  # generous: safe because the flood fill is connectivity-gated,
# not applied globally - see module docstring.
BORDERLINE_THRESHOLD = 48  # near-key, adjacent to real background, but not filled ->
# reported as possible fringe, never altered.
LEAK_FRACTION_LIMIT = 0.90  # if the fill consumes more of the tile than this, the
# model likely touches the tile edge and the fill leaked into it - report, don't save.


def channel_dist(pixel, key):
    return abs(pixel[0] - key[0]), abs(pixel[1] - key[1]), abs(pixel[2] - key[2])


def flood_fill_background(img: Image.Image):
    img = img.convert("RGBA")
    px = img.load()
    w, h = img.size

    def is_key_ish(x, y, threshold):
        r, g, b, a = px[x, y]
        dr, dg, db = channel_dist((r, g, b), KEY_COLOR)
        return dr <= threshold and dg <= threshold and db <= threshold

    # BFS flood fill seeded from every border pixel that is key-ish. Interior dark
    # pixels never get visited unless reachable through a chain of key-ish pixels
    # starting at the border.
    background = [[False] * w for _ in range(h)]
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            if is_key_ish(x, y, FLOOD_THRESHOLD) and not background[y][x]:
                background[y][x] = True
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if is_key_ish(x, y, FLOOD_THRESHOLD) and not background[y][x]:
                background[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h and not background[ny][nx] and is_key_ish(nx, ny, FLOOD_THRESHOLD):
                background[ny][nx] = True
                q.append((nx, ny))

    filled_count = sum(sum(row) for row in background)
    leaked = filled_count > w * h * LEAK_FRACTION_LIMIT
    if leaked:
        return img, [], filled_count, True

    # Borderline/fringe: a key-ish-at-the-wider-threshold pixel that touches the
    # filled background but was not itself filled (outside FLOOD_THRESHOLD) - a
    # real edge transition worth a human look, not silently altered.
    borderline = []
    for y in range(h):
        for x in range(w):
            if background[y][x]:
                continue
            if not is_key_ish(x, y, BORDERLINE_THRESHOLD):
                continue
            neighbours = [(x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)]
            if any(0 <= nx < w and 0 <= ny < h and background[ny][nx] for nx, ny in neighbours):
                r, g, b, a = px[x, y]
                borderline.append((x, y, r, g, b))

    for y in range(h):
        for x in range(w):
            if background[y][x]:
                px[x, y] = (0, 0, 0, 0)
    return img, borderline, filled_count, False


def crop_slot(atlas: Image.Image, slot: int) -> Image.Image:
    col = slot % ATLAS_COLS
    row = slot // ATLAS_COLS
    x0, y0 = col * SLOT_SIZE, row * SLOT_SIZE
    return atlas.crop((x0, y0, x0 + SLOT_SIZE, y0 + SLOT_SIZE))


def main():
    atlas_path, out_dir, slot_map_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(slot_map_path) as f:
        slot_map = json.load(f)
    atlas = Image.open(atlas_path)
    any_borderline = False
    any_leaked = False
    for name, slot in slot_map.items():
        tile = crop_slot(atlas, slot)
        keyed, borderline, filled_count, leaked = flood_fill_background(tile)
        if leaked:
            any_leaked = True
            frac = filled_count / (SLOT_SIZE * SLOT_SIZE)
            print(f"{name}: slot {slot} - LEAK DETECTED, fill consumed {frac:.0%} of the "
                  f"tile (limit {LEAK_FRACTION_LIMIT:.0%}). NOT saved - the model likely "
                  f"touches the tile edge and the background fill ate into it. Needs a "
                  f"re-bake with more margin/zoom-out, not a smaller icon shipped broken.")
            continue
        out_path = f"{out_dir}/{name}.png"
        keyed.save(out_path)
        if borderline:
            any_borderline = True
            sample = borderline[:5]
            print(f"{name}: slot {slot} -> {out_path} - {len(borderline)} BORDERLINE pixels "
                  f"(near background, adjacent to the filled region, not filled themselves), "
                  f"sample: {sample}")
        else:
            print(f"{name}: slot {slot} -> {out_path} - clean, no borderline pixels")
    if any_leaked:
        print("AT LEAST ONE ICON FAILED TO SAVE - see LEAK DETECTED lines above.")
    if any_borderline:
        print("FRINGING DETECTED on at least one icon - see BORDERLINE lines above. Those "
              "pixels were left as-is (not filled, not altered) so the fringe is visible "
              "rather than hidden. Report this rather than treating the bake as clean.")
    elif not any_leaked:
        print("No fringing detected across any icon.")


if __name__ == "__main__":
    main()
