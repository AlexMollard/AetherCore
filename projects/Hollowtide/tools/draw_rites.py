"""Author the eight rite sprites in the engine's own pixel-art editor.

    (editor running with AETHER_CONTROL_PORT set)
    python projects/Hollowtide/tools/draw_rites.py

Every sprite is composed here and then pushed through the engine's PixelArtDocument via the
`pixel.*` control methods, so the canvas the panel opens is the canvas this wrote and the PNG
lands in the project through the engine's own file path. Open any of them in the Pixel Art
panel afterwards and keep drawing - this script is a starting point, not an owner.

The art is deliberately greyscale. Each rite is tinted at runtime by its own colour and
soured toward rust as dread rises, so a sprite that carried its own hue would fight both.
"""
import json
import os
import subprocess
import sys

CTL = os.path.join("build", "default", "tools", "control-client", "RelWithDebInfo", "aether-ctl.exe")
SIZE = 48
OUT_DIR = "project://assets/textures/rites"

# A five-tone ramp, which is all a silhouette at this size can carry. Anything more and the
# shape stops reading once it is tinted and lit.
LINE = (28, 26, 32, 255)      # outline, almost black
DARK = (74, 70, 78, 255)      # shadowed interior
MID = (150, 146, 140, 255)    # body
LIT = (214, 212, 200, 255)    # struck by the light
CORE = (255, 250, 235, 255)   # the ichor itself


def ctl(method, params=None):
    args = [CTL, method] + ([json.dumps(params)] if params else [])
    out = subprocess.run(args, capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout.strip():
        raise SystemExit(f"{method} failed: {out.stdout}{out.stderr}")
    return json.loads(out.stdout)


class Sprite:
    """A grid of RGBA tuples, composed with the same primitives the panel offers."""

    def __init__(self, size=SIZE):
        self.n = size
        self.px = {}

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if 0 <= x < self.n and 0 <= y < self.n:
                    self.px[(x, y)] = c

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            if 0 <= x0 < self.n and 0 <= y0 < self.n:
                self.px[(x0, y0)] = c
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def disc(self, cx, cy, r, c):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    if 0 <= x < self.n and 0 <= y < self.n:
                        self.px[(x, y)] = c

    def ring(self, cx, cy, r, c, thickness=1):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                d2 = (x - cx) ** 2 + (y - cy) ** 2
                if (r - thickness) ** 2 < d2 <= r * r:
                    if 0 <= x < self.n and 0 <= y < self.n:
                        self.px[(x, y)] = c

    def mirror(self):
        """Mirror the left half onto the right - what makes a built thing look built."""
        half = self.n // 2
        for (x, y), c in list(self.px.items()):
            if x < half:
                self.px[(self.n - 1 - x, y)] = c

    def outline(self):
        """Trace a dark edge around everything drawn. A silhouette needs a border to read
        against a dark parish, and doing it last means no shape has to draw its own."""
        edge = {}
        for (x, y) in self.px:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                n = (x + dx, y + dy)
                if n not in self.px and 0 <= n[0] < self.n and 0 <= n[1] < self.n:
                    edge[n] = LINE
        self.px.update(edge)

    def by_colour(self):
        """Texels grouped by tone. Pick a colour, lay down every texel that uses it - which
        is how the panel is used by hand, and keeps each request well inside the command-line
        length limit that a single flat list of two thousand coloured texels blows past."""
        groups = {}
        for (x, y), c in sorted(self.px.items()):
            groups.setdefault(c, []).append({"x": x, "y": y})
        return groups


def grave_lantern():
    s = Sprite()
    s.line(23, 3, 23, 9, MID)                       # hook
    s.rect(14, 10, 23, 13, LIT)                     # cap
    s.rect(12, 13, 23, 15, MID)
    s.rect(13, 16, 23, 36, DARK)                    # glass
    s.rect(13, 16, 15, 36, LIT)                     # post
    s.rect(11, 37, 23, 41, MID)                     # base
    s.rect(13, 41, 23, 43, LIT)
    s.disc(23, 27, 5, CORE)                         # flame
    s.rect(22, 20, 23, 27, CORE)
    s.mirror()
    s.outline()
    return s


def bone_choir():
    s = Sprite()
    for cx, cy, r in ((11, 26, 7), (24, 20, 9), (37, 26, 7)):
        s.disc(cx, cy, r, LIT)
        s.rect(cx - r + 2, cy + r - 2, cx + r - 2, cy + r + 3, MID)   # jaw
        s.rect(cx - 4, cy - 2, cx - 2, cy + 1, LINE)                  # sockets
        s.rect(cx + 2, cy - 2, cx + 4, cy + 1, LINE)
        s.rect(cx - 1, cy + 3, cx, cy + 5, CORE)                      # the note
    s.outline()
    return s


def weeping_statue():
    s = Sprite()
    s.rect(18, 34, 23, 42, MID)                     # robe
    s.rect(16, 22, 23, 34, LIT)
    s.rect(14, 42, 23, 44, MID)
    s.disc(21, 15, 7, LIT)                          # bowed head
    s.rect(17, 12, 21, 18, DARK)                    # cowl shadow
    s.rect(20, 17, 21, 27, CORE)                    # the tear
    s.rect(12, 44, 23, 46, DARK)                    # plinth
    s.mirror()
    s.outline()
    return s


def flesh_loom():
    s = Sprite()
    s.rect(6, 6, 10, 42, LIT)                       # upright
    s.rect(6, 6, 41, 9, MID)                        # beams
    s.rect(6, 39, 41, 42, MID)
    for x in range(13, 36, 4):                      # warp
        s.rect(x, 10, x + 1, 38, DARK)
    s.rect(9, 22, 38, 25, CORE)                     # shuttle mid-pass
    s.rect(9, 30, 38, 31, MID)
    s.mirror()
    s.outline()
    return s


def ossuary_engine():
    s = Sprite()
    s.rect(5, 14, 23, 38, MID)                      # housing
    s.rect(8, 17, 23, 35, DARK)
    for y in range(19, 35, 4):                      # ribs
        s.rect(9, y, 23, y + 1, LIT)
    s.rect(10, 8, 14, 14, MID)                      # stack
    s.rect(3, 38, 23, 42, LIT)                      # bed
    s.mirror()
    s.disc(24, 26, 5, CORE)                         # furnace
    s.outline()
    return s


def drowned_chapel():
    s = Sprite()
    for i in range(14):                             # roof
        s.rect(23 - i, 8 + i, 23, 9 + i, LIT if i % 3 else MID)
    s.rect(9, 22, 23, 36, MID)                      # nave
    s.rect(12, 25, 23, 34, DARK)
    s.rect(22, 2, 23, 8, LIT)                       # cross
    s.rect(20, 4, 23, 5, LIT)
    s.mirror()
    s.rect(19, 27, 28, 36, CORE)                    # window
    s.rect(0, 37, 47, 40, DARK)                     # waterline
    s.rect(0, 37, 47, 37, CORE)
    s.rect(0, 41, 47, 47, DARK)
    s.outline()
    return s


def pale_shepherd():
    s = Sprite()
    s.rect(18, 18, 27, 44, LIT)                     # figure
    s.rect(20, 24, 25, 44, MID)
    s.disc(22, 13, 6, LIT)
    s.rect(20, 11, 22, 14, CORE)                    # eye
    s.rect(36, 6, 38, 44, MID)                      # crook
    s.ring(36, 8, 5, MID, 2)
    s.disc(9, 38, 4, DARK)                          # flock
    s.disc(14, 41, 3, DARK)
    s.outline()
    return s


def hollow_mouth():
    s = Sprite()
    s.disc(24, 24, 20, MID)
    s.disc(24, 24, 16, DARK)
    for i in range(16):                             # teeth
        import math
        a = i * (2 * math.pi / 16)
        x, y = 24 + math.cos(a) * 14, 24 + math.sin(a) * 14
        s.line(int(round(24 + math.cos(a) * 19)), int(round(24 + math.sin(a) * 19)),
               int(round(x)), int(round(y)), LIT)
    s.disc(24, 24, 7, LINE)
    s.disc(24, 24, 3, CORE)
    s.outline()
    return s


def bearer():
    """The wisp that carries a rite's yield to the keeper. Small, because a dozen of them
    are in the air at once and they must read as traffic rather than as objects."""
    s = Sprite(16)
    s.disc(7, 7, 5, MID)
    s.disc(7, 7, 3, LIT)
    s.disc(7, 7, 1, CORE)
    s.outline()
    return s


RITES = [
    ("grave_lantern", grave_lantern), ("bone_choir", bone_choir),
    ("weeping_statue", weeping_statue), ("flesh_loom", flesh_loom),
    ("ossuary_engine", ossuary_engine), ("drowned_chapel", drowned_chapel),
    ("pale_shepherd", pale_shepherd), ("hollow_mouth", hollow_mouth),
    ("bearer", bearer),
]


def main():
    for name, build in RITES:
        sprite = build()
        ctl("pixel.new", {"width": sprite.n, "height": sprite.n})
        strokes = 0
        for colour, texels in sprite.by_colour().items():
            ctl("pixel.set_color", {"color": list(colour)})
            # Chunked: even one tone of a 48x48 sprite can outrun the argument limit.
            for i in range(0, len(texels), 180):
                ctl("pixel.set", {"pixels": texels[i:i + 180]})
                strokes += 1
        ctl("pixel.save", {"path": f"{OUT_DIR}/{name}.png"})
        print(f"drew {name}: {len(sprite.px)} texels in {strokes} strokes")
    print("done - open any of them in the Pixel Art panel to keep editing")


if __name__ == "__main__":
    main()
