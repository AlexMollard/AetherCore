"""Draw the eight rites as pixel art.

    python projects/Hollowtide/tools/generate_rite_art.py

Every rite is a 64x64 RGBA sprite in one visual language: a bone-pale STRUCTURE with a dark
interior and a small emissive CORE where the ichor gathers. The engine tints each sprite by
its rite colour and lights it with a transient 2D light, so these are drawn in greyscale and
coloured at runtime - which is also why they must read as a silhouette first and a picture
second.

Left-right symmetry is mirrored rather than drawn twice: it costs nothing, and it is what
makes a shape built from rectangles read as something made rather than something typed.
"""
import os
from PIL import Image, ImageDraw

SIZE = 64
OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "textures", "rites"))

# Greyscale roles. Colour arrives at runtime from the rite's tint.
VOID = (0, 0, 0, 0)
DARK = (26, 26, 30, 255)      # interior / negative space
EDGE = (92, 92, 98, 255)      # shadowed structure
BONE = (214, 212, 200, 255)   # lit structure
CORE = (255, 255, 255, 255)   # emissive, where the light lives


def canvas():
    im = Image.new("RGBA", (SIZE, SIZE), VOID)
    return im, ImageDraw.Draw(im)


def mirror(im):
    """Mirror the left half onto the right, so the shape is exactly symmetrical."""
    half = im.crop((0, 0, SIZE // 2, SIZE))
    im.paste(half.transpose(Image.FLIP_LEFT_RIGHT), (SIZE // 2, 0))
    return im


def grave_lantern():
    im, d = canvas()
    d.rectangle([30, 4, 33, 12], fill=EDGE)          # hook
    d.arc([24, 2, 40, 14], 200, 340, fill=BONE, width=2)
    d.polygon([(20, 18), (44, 18), (48, 24), (16, 24)], fill=BONE)   # cap
    d.rectangle([19, 24, 45, 50], fill=DARK)         # glass
    d.rectangle([19, 24, 21, 50], fill=BONE)         # posts
    d.rectangle([43, 24, 45, 50], fill=BONE)
    d.rectangle([16, 50, 48, 56], fill=BONE)         # base
    d.ellipse([28, 34, 36, 46], fill=CORE)           # flame
    d.ellipse([30, 30, 34, 38], fill=CORE)
    return im


def bone_choir():
    im, d = canvas()
    for i, (cx, cy, r) in enumerate([(16, 30, 9), (32, 24, 11), (48, 30, 9)]):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BONE)        # cranium
        d.rectangle([cx - r + 3, cy + r - 3, cx + r - 3, cy + r + 5], fill=BONE)  # jaw
        d.ellipse([cx - 5, cy - 3, cx - 1, cy + 2], fill=DARK)        # sockets
        d.ellipse([cx + 1, cy - 3, cx + 5, cy + 2], fill=DARK)
        d.rectangle([cx - 1, cy + 4, cx + 1, cy + 8], fill=CORE)      # the note
    return im


def weeping_statue():
    im, d = canvas()
    d.polygon([(32, 8), (42, 22), (40, 56), (24, 56), (22, 22)], fill=BONE)  # robe
    d.ellipse([25, 6, 39, 22], fill=BONE)                                    # bowed head
    d.polygon([(32, 14), (38, 26), (26, 26)], fill=DARK)                     # cowl shadow
    d.rectangle([30, 20, 31, 34], fill=CORE)                                 # the tear
    d.rectangle([33, 20, 34, 30], fill=CORE)
    d.rectangle([18, 56, 46, 60], fill=EDGE)                                 # plinth
    return im


def flesh_loom():
    im, d = canvas()
    d.rectangle([10, 8, 15, 56], fill=BONE)      # uprights
    d.rectangle([49, 8, 54, 56], fill=BONE)
    d.rectangle([10, 8, 54, 13], fill=BONE)      # beam
    d.rectangle([10, 51, 54, 56], fill=BONE)
    for x in range(19, 46, 5):                   # warp
        d.rectangle([x, 13, x + 1, 51], fill=EDGE)
    d.rectangle([16, 28, 48, 32], fill=CORE)     # the shuttle, mid-pass
    d.rectangle([16, 38, 48, 40], fill=EDGE)
    return im


def ossuary_engine():
    im, d = canvas()
    d.rectangle([8, 20, 56, 52], fill=EDGE)      # housing
    d.rectangle([12, 24, 52, 48], fill=DARK)
    for y in range(26, 47, 5):                   # ribs
        d.rectangle([14, y, 50, y + 2], fill=BONE)
    d.ellipse([26, 30, 38, 42], fill=CORE)       # furnace
    d.rectangle([16, 12, 22, 20], fill=BONE)     # stacks
    d.rectangle([42, 12, 48, 20], fill=BONE)
    d.rectangle([6, 52, 58, 57], fill=BONE)
    return im


def drowned_chapel():
    im, d = canvas()
    d.polygon([(32, 6), (50, 30), (14, 30)], fill=BONE)   # roof
    d.rectangle([16, 30, 48, 50], fill=EDGE)              # nave
    d.polygon([(32, 34), (38, 44), (26, 44)], fill=CORE)  # window
    d.rectangle([31, 2, 33, 10], fill=BONE)               # cross
    d.rectangle([28, 4, 36, 6], fill=BONE)
    d.rectangle([0, 46, SIZE, 52], fill=DARK)             # waterline
    d.rectangle([0, 46, SIZE, 47], fill=CORE)
    d.rectangle([0, 52, SIZE, 64], fill=DARK)
    return im


def pale_shepherd():
    im, d = canvas()
    d.polygon([(32, 10), (40, 26), (38, 58), (26, 58), (24, 26)], fill=BONE)  # figure
    d.ellipse([26, 4, 38, 18], fill=BONE)
    d.ellipse([28, 8, 31, 12], fill=CORE)                                     # eye
    d.rectangle([48, 6, 51, 58], fill=EDGE)                                   # crook
    d.arc([42, 2, 58, 16], 120, 360, fill=BONE, width=3)
    for cx in (12, 20):                                                       # the flock
        d.ellipse([cx - 5, 46, cx + 5, 56], fill=EDGE)
    return im


def hollow_mouth():
    im, d = canvas()
    d.ellipse([6, 6, 58, 58], fill=BONE)
    d.ellipse([11, 11, 53, 53], fill=DARK)
    for i in range(12):                          # teeth around the aperture
        import math
        a = i * (2 * math.pi / 12)
        x = 32 + math.cos(a) * 18
        y = 32 + math.sin(a) * 18
        d.polygon([(x, y), (x + math.cos(a) * 6, y + math.sin(a) * 6),
                   (x - math.sin(a) * 3, y + math.cos(a) * 3)], fill=BONE)
    d.ellipse([26, 26, 38, 38], fill=(0, 0, 0, 255))
    d.ellipse([29, 29, 35, 35], fill=CORE)
    return im


RITES = [
    ("grave_lantern", grave_lantern, True),
    ("bone_choir", bone_choir, False),
    ("weeping_statue", weeping_statue, True),
    ("flesh_loom", flesh_loom, True),
    ("ossuary_engine", ossuary_engine, True),
    ("drowned_chapel", drowned_chapel, True),
    ("pale_shepherd", pale_shepherd, False),
    ("hollow_mouth", hollow_mouth, False),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, draw, symmetric in RITES:
        im = draw()
        if symmetric:
            im = mirror(im)
        path = os.path.join(OUT, name + ".png")
        im.save(path)
        print("wrote", path)


if __name__ == "__main__":
    main()
