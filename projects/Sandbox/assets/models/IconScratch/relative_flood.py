"""
One-off keying tool for icons captured against a GRADIENT background (the live
scene's sky, not the flat black ModelPreviewService clear colour bake_icons.py
assumes). Used only for Bench/Chair/CrateLong, captured via scene.add_model +
viewport screenshot instead of the File Explorer atlas.

Region-growing flood fill: a pixel joins the background region if it is close
to an ALREADY-FILLED NEIGHBOUR's colour (not a single fixed key colour), so a
smooth gradient traces correctly step by step without needing to know its
exact colour in advance. The model's own colour is a big jump from the
gradient at every point along its silhouette, which is what stops the fill.
"""
import sys
from collections import deque

from PIL import Image

STEP_THRESHOLD = 18  # max per-channel difference between adjacent gradient steps


def relative_flood_fill(img: Image.Image):
    img = img.convert("RGBA")
    px = img.load()
    w, h = img.size
    filled = [[False] * w for _ in range(h)]
    q = deque()

    def try_seed(x, y):
        if not filled[y][x]:
            filled[y][x] = True
            q.append((x, y))

    for x in range(w):
        try_seed(x, 0)
        try_seed(x, h - 1)
    for y in range(h):
        try_seed(0, y)
        try_seed(w - 1, y)

    while q:
        x, y = q.popleft()
        r0, g0, b0, _ = px[x, y]
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h and not filled[ny][nx]:
                r, g, b, _ = px[nx, ny]
                if abs(r - r0) <= STEP_THRESHOLD and abs(g - g0) <= STEP_THRESHOLD and abs(b - b0) <= STEP_THRESHOLD:
                    filled[ny][nx] = True
                    q.append((nx, ny))

    filled_count = sum(sum(row) for row in filled)
    for y in range(h):
        for x in range(w):
            if filled[y][x]:
                px[x, y] = (0, 0, 0, 0)
    return img, filled_count, w * h


def main():
    in_path, out_path = sys.argv[1], sys.argv[2]
    im = Image.open(in_path)
    keyed, filled, total = relative_flood_fill(im)
    keyed.save(out_path)
    print(f"{in_path} -> {out_path}: filled {filled}/{total} ({filled/total:.0%})")


if __name__ == "__main__":
    main()
