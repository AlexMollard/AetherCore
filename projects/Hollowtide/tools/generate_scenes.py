"""Scaffold Hollowtide's authored UI chrome into its two scene files.

    python projects/Hollowtide/tools/generate_scenes.py --force

WHAT THIS IS FOR. The .scene.toml files are the source of truth: the chrome is authored, and
the point of authoring it is that layout is dragged in the editor rather than recompiled. This
script is the SCAFFOLDER that produced those files in the first place, kept because laying out
eighty elements by hand in TOML is not something anyone should have to do twice - adding a
batch of new elements, or resetting a screen after an experiment, is far quicker from here.

WHAT IT COSTS. Running it REPLACES both scenes wholesale. Anything moved, recoloured or added
in the editor since the last run is gone. That is why --force exists: without it the script
refuses to touch a file that already exists, so nobody loses an afternoon to muscle memory.
If you have edited a scene and still want to regenerate, port your change into the layout
below first - that is the whole discipline this file asks for.

Positions here mirror scripts/Palette.cs and the Bind() calls in the view classes. A name
changed in one place has to change in the other: the scripts find these entities by name.
"""

import argparse
import hashlib
import io
import os
import sys

import hashlib
import io
import os
import re
import sys

PALETTE_CS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts", "Palette.cs")

# ── Palette ─────────────────────────────────────────────────────────────────────────
#
# READ from scripts/Palette.cs, not mirrored from it. It used to be a hand-copied block under a
# comment saying "mirrors scripts/Palette.cs", and it had stopped: seven of its colours differed
# from the ones the same names have in C#, and BONE - the colour most of the authored text is
# drawn in - was a warm near-white against a cool mid-grey, left behind when the C# ramp was
# reworked to move text off the surface steps. So every label this file authored was a different
# colour from every label the game builds at runtime, both of them claiming the same name.
#
# Nothing here can drift now, because there is only one set of numbers. The derived colours are
# computed by the same two operations Palette.cs uses, spelled out below.
def _palette():
    text = io.open(PALETTE_CS, encoding="utf-8").read()
    base = {}
    for name, digits in re.findall(r"Vector4 (\w+) = Hex\(0x([0-9A-Fa-f]{6})\)", text):
        base[name] = tuple(int(digits[i:i + 2], 16) / 255.0 for i in (0, 2, 4)) + (1.0,)
    missing = [n for n in ("Ink", "Pitch", "Slate", "Stone", "Ash", "Bone", "Pale", "Chalk",
                           "Ichor", "IchorDim", "Dread", "DreadDeep", "Sigil") if n not in base]
    if missing:
        raise SystemExit("Palette.cs is missing " + ", ".join(missing) + " - has the ramp been renamed?")
    return base


def fade(c, a):
    return (c[0], c[1], c[2], a)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(4))


P = _palette()

# The ramp, under the names this file already used for them.
VOID = P["Ink"]
CLEAR = (0.0, 0.0, 0.0, 0.0)

# Panels and rows, at the same weights Palette.cs fades them to.
PANEL = fade(P["Slate"], 0.94)
PANEL_DEEP = fade(P["Pitch"], 0.96)
ROW = fade(P["Stone"], 0.90)
ROW_HOT = fade(mix(P["Stone"], P["Ash"], 0.55), 0.97)

# Text. These are Palette.TextBright / TextBody / TextDim, which is what the authored labels are
# actually for - the old numbers here were the ramp's own steps, from before text was moved off
# the surface colours because a row highlight and the faintest text were the same grey.
BONE = P["Chalk"]
BONE_DIM = P["Pale"]
BONE_FAINT = mix(P["Bone"], P["Pale"], 0.55)

ICHOR = P["Ichor"]
ICHOR_DIM = P["IchorDim"]
DREAD = P["Dread"]
DREAD_DEEP = P["DreadDeep"]
SIGIL = P["Sigil"]

# Every authored face is multiplied by this. One number, because the complaint was about the
# interface as a whole rather than any one label: at a short window the whole thing read small.
# Boxes that were sized tightly around their old text are grown alongside it below - a bigger
# face in an unchanged box clips, which is worse than a small one.
#
# READ from Typography.Authored rather than pasted beside it. The comment here used to say the
# two had to be moved together, "move both or neither, or half the interface grows and half
# stays where it was" - which is an instruction to a person, and the palette three lines up is
# what an instruction to a person is worth over time.
def _authored_scale():
    text = io.open(os.path.join(os.path.dirname(PALETTE_CS), "Typography.cs"), encoding="utf-8").read()
    found = re.search(r"float Authored = ([0-9.]+)f", text)
    if found is None:
        raise SystemExit("Typography.cs no longer states the scale the scenes are authored at")
    return float(found.group(1))


TYPE = _authored_scale()


# How far the keeper may take the type, read from the same file. The authored layout has to
# leave room for the LARGEST of them, or the slider's top end runs one line into the next.
def _type_bound(name):
    text = io.open(os.path.join(os.path.dirname(PALETTE_CS), "Typography.cs"), encoding="utf-8").read()
    found = re.search(r"float " + name + r" = ([0-9.]+)f", text)
    if found is None:
        raise SystemExit("Typography.cs no longer states " + name)
    return float(found.group(1))


SMALLEST_TYPE = _type_bound("Smallest")
LARGEST_TYPE = _type_bound("Largest")

# One settings row's vertical step, with room for the largest type. Forty-four was fine for
# type authored at 1.30 and ran the rows into each other above it.
SETTINGS_STEP = 15.0 * LARGEST_TYPE + 12.0

# The three faces, read for the same reason the colours are. A font name that drifts does not
# look wrong, it looks MISSING: the renderer falls back, and one half of the interface quietly
# renders in something nobody chose.
def _font(name):
    found = re.search(r'string ' + name + r' = "([^"]+)"', io.open(PALETTE_CS, encoding="utf-8").read())
    if found is None:
        raise SystemExit("Palette.cs no longer names a font called " + name)
    return found.group(1)


BODY = _font("Body")
WHISPER = _font("Whisper")
DISPLAY = _font("Display")

LEDGER_W = 760.0  # paired with Hud.LedgerWidth
BAR_H = 104.0


def node_id(name):
    """Stable per-name node id, so regenerating does not churn every id in the file."""
    digest = hashlib.sha256(name.encode()).digest()
    value = int.from_bytes(digest[:8], "little", signed=True)
    return value if value != 0 else 1


class Scene:
    def __init__(self, features="[ 'sprites', 'tilemaps', 'physics_2d' ]"):
        self.entities = []
        self.features = features

    def add(self, name, parent=None, components=None, transform=True):
        index = len(self.entities)
        self.entities.append(
            {
                "name": name,
                "parent": parent,
                "components": components or [],
                "transform": transform,
            }
        )
        return index

    # ── component helpers ───────────────────────────────────────────────────────────
    @staticmethod
    def rect(anchor_min, anchor_max, offset_min, offset_max, pivot=(0.0, 0.0)):
        return (
            "ui_rect",
            [
                ("anchor_max", list(anchor_max)),
                ("anchor_min", list(anchor_min)),
                ("offset_max", list(offset_max)),
                ("offset_min", list(offset_min)),
                ("pivot", list(pivot)),
            ],
        )

    @classmethod
    def box(cls, x, y, w, h, pivot=(0.0, 0.0), anchor=(0.0, 0.0)):
        """A fixed box placed from an anchor point, honouring the pivot - the same maths
        Ui.SetRect does, so authored and script-built elements land identically."""
        return cls.rect(
            anchor,
            anchor,
            (x - pivot[0] * w, y - pivot[1] * h),
            (x + (1.0 - pivot[0]) * w, y + (1.0 - pivot[1]) * h),
            pivot,
        )

    @classmethod
    def stretch(cls, anchor_min, anchor_max, offset_min, offset_max):
        return cls.rect(anchor_min, anchor_max, offset_min, offset_max, (0.0, 0.0))

    @staticmethod
    def image(color, corner_radius=0.0, texture="", pixel_art=False):
        return (
            "ui_image",
            [
                ("color", list(color)),
                ("corner_radius", corner_radius),
                ("pixel_art", pixel_art),
                ("texture", texture),
            ],
        )

    @staticmethod
    def text(value, font=BODY, size=17.0, color=BONE, h="left", v="middle", wrap=False):
        # Sizes below are written at their original values and scaled here, in one place, so
        # the whole interface can be made more readable by moving one number rather than
        # thirty-odd. Playtested at a short window, everything on screen was hard to read.
        return (
            "ui_text",
            [
                ("color", list(color)),
                ("font", font),
                ("h_align", h),
                ("pixel_size", round(size * TYPE, 2)),
                ("text", value),
                ("v_align", v),
                ("wrap", wrap),
            ],
        )

    @staticmethod
    def button(label, font=DISPLAY, size=15.0, bg=ROW, bg_focus=ROW_HOT,
               fg=BONE_DIM, fg_focus=ICHOR, radius=4.0, h="center"):
        return (
            "ui_button",
            [
                ("bg_color", list(bg)),
                ("bg_color_focused", list(bg_focus)),
                ("corner_radius", radius),
                ("font", font),
                ("h_align", h),
                ("label", label),
                ("pixel_size", round(size * TYPE, 2)),
                ("text_color", list(fg)),
                ("text_color_focused", list(fg_focus)),
                ("v_align", "middle"),
            ],
        )

    @staticmethod
    def selectable(group="", interactable=True):
        return ("ui_selectable", [("group", group), ("interactable", interactable)])

    @staticmethod
    def effect(shader, color0, color1, background=False, sort_order=0):
        return (
            "ui_effect",
            [
                ("background", background),
                ("color0", list(color0)),
                ("color1", list(color1)),
                ("shader", shader),
                ("sort_order", sort_order),
            ],
        )

    @staticmethod
    def material(shader, color0, color1):
        return ("ui_material", [("color0", list(color0)), ("color1", list(color1)), ("shader", shader)])

    @staticmethod
    def progress(track, fill, radius=6.0):
        return (
            "ui_progress_bar",
            [("corner_radius", radius), ("fill_color", list(fill)), ("track_color", list(track)), ("value", 0.0)],
        )

    @staticmethod
    def mask():
        return ("ui_mask", [("enabled", True), ("padding", 0.0)])

    @staticmethod
    def text_box(placeholder, max_length, content_type, size=18.0):
        return (
            "ui_text_box",
            [
                ("bg_color", list(PANEL_DEEP)),
                ("bg_color_focused", list(ROW)),
                ("caret_color", list(ICHOR)),
                ("content_type", content_type),
                ("corner_radius", 4.0),
                ("font", BODY),
                ("max_length", max_length),
                ("padding", 10.0),
                ("pixel_size", round(size * TYPE, 2)),
                ("placeholder", placeholder),
                ("placeholder_color", list(BONE_FAINT)),
                ("selection_color", list(ICHOR_DIM)),
                ("text", ""),
                ("text_color", list(BONE)),
            ],
        )

    @staticmethod
    def toggle(on=True):
        return (
            "ui_toggle",
            [
                ("corner_radius", 13.0),
                ("knob_color", list(BONE)),
                ("knob_radius", 10.0),
                ("on", on),
                ("on_color", list(ICHOR_DIM)),
                ("track_color", list(PANEL_DEEP)),
            ],
        )

    @staticmethod
    def slider(lo, hi, step, value):
        return (
            "ui_slider",
            [
                ("corner_radius", 4.0),
                ("fill_color", list(DREAD)),
                ("handle_color", list(BONE)),
                ("handle_radius", 8.0),
                ("max", hi),
                ("min", lo),
                ("step", step),
                ("track_color", list(PANEL_DEEP)),
                ("value", value),
            ],
        )

    # ── emit ────────────────────────────────────────────────────────────────────────
    def render(self, header_comment):
        out = [header_comment, "[scene]", f"features = {self.features}", "kind = '2d'", "name = ''", "version = 16", ""]
        for entity in self.entities:
            out.append("[[entities]]")
            if entity["transform"]:
                out.append("euler = [ 0.0, 0.0, 0.0 ]")
            out.append(f"name = '{entity['name']}'")
            out.append(f"node = {node_id(entity['name'])}")
            parent = entity["parent"]
            out.append(f"parent = {-1 if parent is None else parent}")
            if parent is not None:
                out.append(f"parent_node = {node_id(self.entities[parent]['name'])}")
            if entity["transform"]:
                out.append("position = [ 0.0, 0.0, 0.0 ]")
                out.append("scale = [ 1.0, 1.0, 1.0 ]")
            out.append("")
            for table, fields in entity["components"]:
                out.append(f"    [entities.{table}]")
                for key, value in fields:
                    out.append(f"    {key} = {fmt(value)}")
                out.append("")
        return "\n".join(out).rstrip() + "\n"


def fmt(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return "'" + value.replace("'", "") + "'"
    if isinstance(value, list):
        return "[ " + ", ".join(f"{v:.4g}" for v in value) + " ]"
    if isinstance(value, int):
        return str(value)
    return f"{value:.6g}"


CAMERA = """[[entities]]
euler = [ 0.0, 0.0, 0.0 ]
name = 'Main Camera'
node = {node}
parent = -1
position = [ 0.0, 0.0, 10.0 ]
scale = [ 1.0, 1.0, 1.0 ]

    [entities.camera]
    background = 'solid_colour'
    clear_color = [ {clear} ]
    far = 1000.0
    fov = 60.0
    gradient_angle = 0.0
    main = true
    near = 0.1
    orthographic_height = 10.0
    projection = 'orthographic'

        [[entities.camera.gradient_stops]]
        colour = [ 0.02, 0.024, 0.032 ]
        position = 0.0

        [[entities.camera.gradient_stops]]
        colour = [ 0.008, 0.008, 0.012 ]
        position = 1.0
"""

CANVAS = """[[entities]]
name = '{name}'
node = {node}
parent = -1

    [entities.ui_canvas]
    reference = [ 1920.0, 1080.0 ]
    scale_mode = 'constant_pixel'
    sort_bias = 0

    [entities.ui_rect]
    anchor_max = [ 1.0, 1.0 ]
    anchor_min = [ 0.0, 0.0 ]
    offset_max = [ 0.0, 0.0 ]
    offset_min = [ 0.0, 0.0 ]
    pivot = [ 0.5, 0.5 ]
"""

CENTRE = (0.5, 0.5)
BOTTOM = (0.5, 1.0)
MID = (0.5, 0.5)


# ══════════════════════════════════════════════════════════════════════════════════════
# The settings rows, authored identically wherever they appear.
#
# There are two settings pages - the threshold's and the one over the vigil - because a setting
# you can only reach from the title screen is a setting you cannot judge, and type size most of
# all: the complaint it answers is about the parish's text, which the title screen has none of.
# Two pages would be two chances to add a row to one and forget the other, so both are built
# from here and the prefix is the only difference.
def settings_rows(centred, prefix, top, group):
    H = 26.0
    centred(prefix + "WhisperLabel", [Scene.text("SHOW WHISPERS", DISPLAY, 15.0, BONE_FAINT)], -100, top, 200, H)
    centred(prefix + "WhisperToggle", [Scene.toggle(True), Scene.selectable(group)], 172, top, 56, H)
    centred(prefix + "ShakeLabel", [Scene.text("", DISPLAY, 15.0, BONE_FAINT)], -100, top + SETTINGS_STEP, 200, H)
    centred(prefix + "ShakeSlider", [Scene.slider(0.0, 1.5, 0.1, 1.0), Scene.selectable(group)], 140, top + SETTINGS_STEP, 120, H)
    # Type size. The complaint it answers - "the whole interface is too small" - used to need
    # a rebuild AND the same number moved by hand in two languages.
    centred(prefix + "TypeLabel", [Scene.text("", DISPLAY, 15.0, BONE_FAINT)], -100, top + SETTINGS_STEP * 2, 200, H)
    centred(prefix + "TypeSlider", [Scene.slider(SMALLEST_TYPE, LARGEST_TYPE, 0.05, TYPE), Scene.selectable(group)], 140, top + SETTINGS_STEP * 2, 120, H)


# Threshold - entirely static, so all of it is authored.
# ══════════════════════════════════════════════════════════════════════════════════════
def threshold():
    s = Scene()
    # Placeholders so parent indices line up with the hand-written prologue below.
    s.add("Main Camera", transform=True)
    s.add("Threshold", transform=True)
    canvas = s.add("ThresholdUI", transform=False)

    def centred(name, comps, x, y, w, h):
        return s.add(name, canvas, [Scene.box(x, y, w, h, MID, CENTRE)] + comps)

    s.add("ThGloom", canvas, [
        Scene.stretch((0, 0), (1, 1), (0, 0), (0, 0)),
        Scene.effect("ui_gloom", VOID, DREAD_DEEP, background=True, sort_order=-10),
    ])

    # ── Chrome: on every page, because it is what the screen IS rather than what it is asking.
    #
    # The gaps below are not chosen by eye. Layout is authored at one scale and the glyphs inside
    # it grow with the keeper's setting, so every vertical gap has to be at least the written
    # size of the label above it times the largest scale the slider offers - or dragging the
    # slider up runs one line into the next. Measured, the title-to-subtitle gap allowed x1.31
    # against a slider that went to x1.75, so the top third of the range broke the title screen.
    def room(written, padding=8.0):
        return written * LARGEST_TYPE + padding

    y = -300.0
    centred("ThTitle", [Scene.text("HOLLOWTIDE", DISPLAY, 72.0, BONE, "center")], 0, y, 900, 90)
    y += room(72.0)
    centred("ThSubtitle", [Scene.text("keep the parish, and count what it costs you", WHISPER, 18.0, BONE_FAINT, "center")], 0, y, 900, 30)
    y += room(18.0)
    # What this keeper already is, for a keeper who is already something. The threshold used
    # to greet a player with fifty-eight sigils and five visitors named exactly as it greeted
    # someone who had never opened the game - so a title screen that is the front door to a
    # long save said nothing at all about the save.
    centred("ThStanding", [Scene.text("", WHISPER, 16.0, BONE_DIM, "center")], 0, y, 900, 26)
    y += room(16.0, padding=18.0)
    COLUMN_TOP = y
    centred("ThStatus", [Scene.text("", WHISPER, 16.0, BONE_DIM, "center")], 0, 356, 900, 26)

    # One 400px column for everything, on every page, so nothing moves sideways when a page
    # changes. A menu whose controls jump around between pages reads as three screens rather
    # than as one screen showing different things.
    FIELD_W = 400.0
    ROW_H = 48.0

    # ── Root: who you are, and the four things you can do. ──────────────────────────────
    at = COLUMN_TOP
    centred("ThNameLabel", [Scene.text("YOUR NAME", DISPLAY, 15.0, BONE_FAINT)], 0, at, FIELD_W, 22)
    at += room(15.0)
    centred("ThNameBox", [Scene.text_box("Keeper", 20, "alphanumeric")], 0, at, FIELD_W, 44)
    at += room(18.0, padding=30.0)

    # KEEP VIGIL is the reason the game is open, so it is the widest, the brightest, and the
    # first thing focus lands on. The three below it are the same size as each other and
    # quieter than it - a menu where every entry shouts equally has no first entry.
    centred("ThPlay", [Scene.button("KEEP VIGIL", DISPLAY, 17.0, ROW, mix(ROW_HOT, ICHOR, 0.34), BONE, ICHOR), Scene.selectable("threshold")], 0, at, FIELD_W, 54)
    at += room(17.0, padding=26.0)
    centred("ThCongregation", [Scene.button("CONGREGATION", DISPLAY, 15.0, ROW, mix(ROW_HOT, SIGIL, 0.30), BONE_DIM, SIGIL), Scene.selectable("threshold")], 0, at, FIELD_W, ROW_H)
    at += room(15.0, padding=22.0)
    centred("ThSettings", [Scene.button("SETTINGS", DISPLAY, 15.0, ROW, ROW_HOT, BONE_DIM, BONE), Scene.selectable("threshold")], 0, at, FIELD_W, ROW_H)
    at += room(15.0, padding=22.0)
    centred("ThLeave", [Scene.button("LEAVE", DISPLAY, 15.0, ROW, mix(ROW_HOT, DREAD, 0.35), BONE_FAINT, DREAD), Scene.selectable("threshold")], 0, at, FIELD_W, ROW_H)

    # ── Congregation: only ever seen by somebody who went looking for it. ───────────────
    at = COLUMN_TOP
    centred("ThAddressLabel", [Scene.text("HOST ADDRESS", DISPLAY, 15.0, BONE_FAINT)], 0, at, FIELD_W, 22)
    at += room(15.0)
    centred("ThAddressBox", [Scene.text_box("127.0.0.1:7777", 48, "host")], 0, at, FIELD_W, 44)
    at += room(18.0, padding=30.0)
    centred("ThHost", [Scene.button("HOST A CONGREGATION", DISPLAY, 15.0, ROW, mix(ROW_HOT, SIGIL, 0.30), BONE, SIGIL), Scene.selectable("threshold")], 0, at, FIELD_W, ROW_H)
    at += room(15.0, padding=22.0)
    centred("ThJoin", [Scene.button("JOIN ONE", DISPLAY, 15.0, ROW, mix(ROW_HOT, SIGIL, 0.30), BONE, SIGIL), Scene.selectable("threshold")], 0, at, FIELD_W, ROW_H)

    # ── Settings: one row each, control flush to the column's right edge. ──────────────
    settings_rows(centred, "Th", COLUMN_TOP, "threshold")

    # Beginning again, at the bottom of the page a player has to go looking for rather than on
    # the front door. It asks twice before it does anything - there is no undo behind this
    # button and no dialog system to put in front of it, so the button is its own confirmation.
    centred("ThWipe", [Scene.button("BEGIN A NEW VIGIL", DISPLAY, 14.0, ROW, mix(ROW_HOT, DREAD, 0.45), BONE_FAINT, DREAD), Scene.selectable("threshold")], 0, COLUMN_TOP + SETTINGS_STEP * 3 + 26.0, 260, 36)

    # One back, shared by every page that is not the root - which is the page it goes to.
    centred("ThBack", [Scene.button("BACK", DISPLAY, 15.0, ROW, ROW_HOT, BONE_DIM, BONE), Scene.selectable("threshold")], 0, 300, 180, 40)

    body = s.render("# Hollowtide - the threshold. Authored chrome; ThresholdScreen only binds and drives it.\n")
    # Splice the hand-written camera / script-root / canvas prologue over the placeholders.
    prologue = (
        CAMERA.format(node=node_id("Main Camera"), clear="0.012, 0.014, 0.018")
        + "\n"
        + """[[entities]]
euler = [ 0.0, 0.0, 0.0 ]
name = 'Threshold'
node = %d
parent = -1
position = [ 0.0, 0.0, 0.0 ]
scale = [ 1.0, 1.0, 1.0 ]

    [[entities.scripts]]
    type = 'ThresholdScreen'
""" % node_id("Threshold")
        + "\n"
        + CANVAS.format(name="ThresholdUI", node=node_id("ThresholdUI"))
    )
    return splice(body, prologue, 3)


# ══════════════════════════════════════════════════════════════════════════════════════
# Vigil - the fixed chrome. Ledger ROWS stay script-built under LedgerViewport.
# ══════════════════════════════════════════════════════════════════════════════════════
def vigil():
    s = Scene()
    s.add("Main Camera", transform=True)
    s.add("Parish", transform=True)
    canvas = s.add("HollowUI", transform=False)
    s.add("Session", transform=True)
    for i in range(4):
        s.add(f"Spawn{i}", transform=True)

    # ── backdrop ────────────────────────────────────────────────────────────────────
    s.add("VigilGloom", canvas, [
        Scene.stretch((0, 0), (1, 1), (0, 0), (0, 0)),
        Scene.effect("ui_gloom", VOID, DREAD_DEEP, background=True, sort_order=-10),
    ])

    # The parish, drawn analytically. Behind everything but the gloom, and covering the whole
    # canvas: it IS the world now, so there is nothing in the scene left for it to hide.
    s.add("ParishBackdrop", canvas, [
        Scene.stretch((0, 0), (1, 1), (0, 0), (0, 0)),
        Scene.effect("ui_parish", (0.027, 0.031, 0.043, 1.0), (0.698, 0.271, 0.227, 1.0),
                     background=True, sort_order=-9),
    ])

    # ── the bar ─────────────────────────────────────────────────────────────────────
    bar = s.add("HudBar", canvas, [
        Scene.stretch((0, 0), (1, 0), (0, 0), (-LEDGER_W, BAR_H)),
        Scene.image(PANEL_DEEP),
    ])
    def at(parent, name, comps, x, y, w, h, pivot=(0.0, 0.0), anchor=(0.0, 0.0)):
        return s.add(name, parent, [Scene.box(x, y, w, h, pivot, anchor)] + comps)

    # Four columns on a 28px gutter, each holding one idea. The previous layout had the
    # ichor number sitting on top of the keeper line and the rate sharing a rect with the
    # multiplier - readable only because the strings happened to be short.
    #   identity 28..328 | ichor 356..636 | multiplier 664..904 | dread 932..1332 | sigils right
    # TWO ROWS, and every element starts on one of them. Row A carries the headline of each
    # column, row B its detail; nothing sits at an in-between y, which is what turns a bar
    # into a grid rather than nine independent guesses.
    ROW_A, ROW_B = 14.0, 64.0
    at(bar, "HudTitle", [Scene.text("HOLLOWTIDE", DISPLAY, 22.0, BONE_DIM)], 28, ROW_A, 300, 30)
    at(bar, "HudIchor", [Scene.text("0", BODY, 40.0, ICHOR)], 356, ROW_A, 280, 48)
    # Widened from 160 to run up to the value column, because this caption stopped being the
    # static word "DREAD" and now carries what the bargain PAYS - the one number the game's
    # central decision turns on, and the one it never showed. A player could read how close the
    # dark was and how long they had, but not what standing there was buying them, which left
    # "is riding the meter worth it?" as a question the interface refused to answer.
    at(bar, "HudDreadCaption", [Scene.text("DREAD", DISPLAY, 15.0, BONE_FAINT)], 932, ROW_A, 260, 22)
    at(bar, "HudDreadValue", [Scene.text("0.0%", BODY, 16.0, DREAD, "right")], 1192, ROW_A, 140, 22)

    at(bar, "HudKeeper", [Scene.text("", BODY, 16.0, BONE_DIM)], 28, ROW_B, 300, 26)
    at(bar, "HudRate", [Scene.text("0/s", BODY, 18.0, ICHOR_DIM)], 356, ROW_B, 280, 24)
    at(bar, "HudMultiplier", [Scene.text("", BODY, 18.0, SIGIL)], 664, ROW_B, 240, 24)
    at(bar, "HudDreadBar", [Scene.image(CLEAR, 6.0), Scene.progress((0.086, 0.075, 0.078, 1.0), DREAD)], 932, ROW_B, 400, 20)
    at(bar, "HudSigils", [Scene.text("", BODY, 18.0, SIGIL, "right")], -28, ROW_B, 300, 28, (1.0, 0.0), (1.0, 0.0))

    # ── the nave ────────────────────────────────────────────────────────────────────
    nave = s.add("HudNave", canvas, [
        Scene.stretch((0, 0), (1, 1), (0, BAR_H), (-LEDGER_W, 0)),
        Scene.image(CLEAR),
    ])
    at(nave, "NaveSigil", [Scene.image((1, 1, 1, 1)), Scene.material("ui_sigil", ICHOR, DREAD), Scene.selectable("nave")], 0, -60, 268, 268, MID, CENTRE)
    at(nave, "NaveHint", [Scene.text("GATHER", DISPLAY, 17.0, BONE_FAINT, "center")], 0, 108, 400, 26, MID, CENTRE)
    at(nave, "NaveHandValue", [Scene.text("", BODY, 17.0, ICHOR_DIM, "center")], 0, 136, 400, 26, MID, CENTRE)
    # FERVOUR. It sits directly under the hand figure and directly above the buttons, because
    # it is the one meter you fill with the sigil rather than with the parish - the bar belongs
    # in the gap between what your hand is worth and what you can spend it on. It was worth up
    # to 1.6x and drained in twelve seconds while being drawn nowhere at all, which made the
    # second-largest multiplier in the game invisible, and made a boon that slows its drain a
    # purchase against something the player could not see.
    at(nave, "NaveFervourBar", [Scene.image(CLEAR, 4.0), Scene.progress((0.086, 0.075, 0.078, 1.0), ICHOR)], 0, 157, 260, 8, MID, CENTRE)
    # Stoke and ward are a pair - the two directions you can push the meter - so they share a
    # row. The bell is a different kind of thing (and only exists in a congregation), so it
    # sits on its own beneath them rather than making a row of three that is sometimes two.
    at(nave, "NaveStoke", [Scene.button("STOKE THE DARK", DISPLAY, 16.0, ROW, mix(ROW_HOT, DREAD, 0.55), BONE, BONE), Scene.selectable("nave")], -132, 186, 250, 44, MID, CENTRE)
    at(nave, "NaveWard", [Scene.button("RAISE WARD", DISPLAY, 16.0, ROW, mix(ROW_HOT, ICHOR, 0.35), BONE, BONE), Scene.selectable("nave")], 132, 186, 250, 44, MID, CENTRE)
    at(nave, "NaveBell", [Scene.button("RING THE BELL", DISPLAY, 16.0, ROW, mix(ROW_HOT, SIGIL, 0.40), BONE, BONE), Scene.selectable("nave")], 0, 240, 250, 44, MID, CENTRE)

    # ── the visitation ──────────────────────────────────────────────────────────────
    # Hidden unless something is walking. It sits ABOVE the sigil rather than beside the
    # buttons because for nine seconds it is the only thing in the game that matters, and a
    # banner tucked into a corner would read as another readout rather than as an arrival.
    vis = at(nave, "VisitationPanel", [Scene.image(PANEL_DEEP, 8.0)], 0, -262, 720, 132, MID, CENTRE)
    at(vis, "VisitationName", [Scene.text("", DISPLAY, 24.0, DREAD, "center")], 20, 14, 680, 32)
    at(vis, "VisitationLine", [Scene.text("", BODY, 16.0, BONE_DIM, "center", wrap=True)], 40, 52, 640, 60)
    at(vis, "VisitationClock", [Scene.text("", BODY, 15.0, BONE_FAINT, "center")], 20, 102, 680, 22)

    # Four answers on one row, always in the same order and always all four shown - including
    # the ones this keeper cannot currently pay for. A menu that hides the answers you cannot
    # afford teaches nothing; a keeper has to be able to SEE that a ward was the thing to have
    # bought, which is how the next encounter goes better than this one.
    ANSWERS = [
        ("AnswerWard", "WARD IT"),
        ("AnswerOffer", "OFFER"),
        ("AnswerBell", "THE BELL"),
        ("AnswerStill", "STAND STILL"),
    ]
    for i, (nm, label) in enumerate(ANSWERS):
        at(nave, nm, [
            Scene.button(label, DISPLAY, 15.0, ROW, mix(ROW_HOT, DREAD, 0.40), BONE, BONE),
            Scene.selectable("nave"),
        ], -264 + i * 176, 300, 168, 44, MID, CENTRE)

    # ── something turned up ─────────────────────────────────────────────────────────
    # Along the BOTTOM of the nave, not over the sigil. Above the sigil it sat in the middle of
    # where a keeper is looking while they click, so a find interrupted the thing that produced
    # it; at the foot of the view it is read without being in the way.
    #
    # It still yields to a visitation. They no longer share a slot, but at window heights below
    # about 1030 the answer row and the foot of the nave meet, and a find is never worth
    # covering an answer the keeper has nine seconds to give.
    found = at(nave, "FoundPanel", [Scene.image(PANEL_DEEP, 8.0)], 0, -24, 660, 126, BOTTOM, BOTTOM)
    # VOID, not white: ui_relic computes its own colour and takes only the alpha from the
    # element, so on any frame before the material resolves the plain image draws instead - and
    # a white one flashes as a solid block. The same trap the rites and the ledger icons hit.
    at(found, "FoundArt", [Scene.image(VOID), Scene.material("ui_relic", ICHOR, DREAD)], 16, 16, 84, 84)
    at(found, "FoundGrade", [Scene.text("", DISPLAY, 15.0, ICHOR)], 116, 12, 480, 26)
    at(found, "FoundName", [Scene.text("", DISPLAY, 21.0, BONE)], 116, 40, 480, 34)
    at(found, "FoundPowers", [Scene.text("", BODY, 15.0, ICHOR_DIM)], 116, 78, 480, 28)

    # ── inspecting a relic ──────────────────────────────────────────────────────────
    # Sits to the LEFT of the ledger and outside it, for two reasons: the ledger's viewport is
    # masked, so anything parented into it would be clipped by the scroll area, and a panel that
    # covered the list would hide the row the keeper is pointing at. Hidden unless something is
    # under the pointer.
    INSPECT_W = 430.0
    inspect = s.add("RelicInspect", canvas, [
        Scene.box(-(LEDGER_W + INSPECT_W + 14.0), 150, INSPECT_W, 430, (0, 0), (1, 0)),
        Scene.image(PANEL_DEEP, 8.0),
    ])
    at(inspect, "InspectArt", [Scene.image(VOID), Scene.material("ui_relic", ICHOR, DREAD)], 18, 18, 104, 104)
    at(inspect, "InspectName", [Scene.text("", DISPLAY, 20.0, BONE, wrap=True)], 136, 16, 278, 60)
    at(inspect, "InspectGrade", [Scene.text("", DISPLAY, 15.0, ICHOR)], 136, 80, 278, 26)
    at(inspect, "InspectRule", [Scene.image(ROW)], 18, 136, INSPECT_W - 36, 2)
    at(inspect, "InspectWhat", [Scene.text("WHAT IT DOES", DISPLAY, 13.0, BONE_FAINT)], 18, 148, 260, 22)
    for i in range(5):
        at(inspect, f"InspectPower{i}", [Scene.text("", BODY, 16.0, ICHOR_DIM)], 18, 178 + i * 30, INSPECT_W - 36, 28)
    at(inspect, "InspectFlavour", [Scene.text("", BODY, 14.0, BONE_FAINT, wrap=True)], 18, 336, INSPECT_W - 36, 52)
    at(inspect, "InspectFoot", [Scene.text("", BODY, 15.0, BONE_DIM)], 18, 394, INSPECT_W - 36, 24)

    # ── the ledger ──────────────────────────────────────────────────────────────────
    ledger = s.add("LedgerPanel", canvas, [
        Scene.stretch((1, 0), (1, 1), (-LEDGER_W, 0), (0, 0)),
        Scene.image(PANEL),
    ])
    at(ledger, "LedgerHeading", [Scene.text("THE LEDGER", DISPLAY, 21.0, BONE_DIM)], 20, 18, 300, 30)
    # Six tabs. RELICS is where a keeper looks at what the parish has given up, and VOICES is
    # where they read what it has said - the whisper feed shows a line for nine seconds and then
    # loses it forever, which is a poor way to treat the only part of the game that has a voice.
    # Both belong in the same column as everything else the keeper reads or spends.
    LEDGER_TABS = ["RITES", "OFFERINGS", "COMMUNION", "MARKS", "RELICS", "VOICES"]
    tab_w = (LEDGER_W - 40.0 - 6.0 * (len(LEDGER_TABS) - 1)) / len(LEDGER_TABS)
    for i, label in enumerate(LEDGER_TABS):
        at(ledger, f"LedgerTab{i}", [Scene.button(label, DISPLAY, 13.0), Scene.selectable("ledger")], 20 + i * (tab_w + 6.0), 56, tab_w, 36)
    # Shares the strip the buy-amount buttons use, which is empty on every tab but the rites.
    at(ledger, "LedgerWearBest", [Scene.button("WEAR THE BEST THREE", DISPLAY, 15.0), Scene.selectable("ledger")], 20, 102, 300, 32)
    for i, label in enumerate(["x1", "x10", "x100", "MAX"]):
        at(ledger, f"LedgerAmount{i}", [Scene.button(label, DISPLAY, 16.0), Scene.selectable("ledger")], 20 + i * 78.0, 102, 72, 32)
    s.add("LedgerViewport", ledger, [
        Scene.stretch((0, 0), (1, 1), (20, 150), (-20, -20)),
        Scene.image(CLEAR),
        Scene.mask(),
    ])

    # ── the congregation ────────────────────────────────────────────────────────────
    cong = s.add("CongregationPanel", canvas, [
        Scene.stretch((0, 0), (0, 1), (20, BAR_H + 16), (20 + 344.0, -220), ),
        Scene.image(PANEL, 4.0),
    ])
    at(cong, "CongHeading", [Scene.text("THE CONGREGATION", DISPLAY, 16.0, BONE_DIM)], 16, 12, 300, 26)
    for i in range(4):
        row = at(cong, f"CongRow{i}", [Scene.image(ROW, 4.0)], 12, 44 + i * 104.0, 320, 96)
        at(row, f"CongRow{i}Name", [Scene.text("", BODY, 16.0, BONE)], 12, 8, 180, 22)
        at(row, f"CongRow{i}Ping", [Scene.text("", BODY, 15.0, BONE_FAINT, "right")], 200, 8, 108, 22)
        at(row, f"CongRow{i}Rate", [Scene.text("", BODY, 16.0, ICHOR_DIM)], 12, 30, 240, 22)
        track = at(row, f"CongRow{i}Track", [Scene.image(PANEL_DEEP, 3.0)], 12, 52, 296, 6)
        at(track, f"CongRow{i}Fill", [Scene.image(DREAD, 3.0)], 0, 0, 1, 6)
        # Three verbs now, on one line: give them ichor, give them your dread, or give them
        # something you dug up. Narrowed to fit rather than stacked onto a second row, because
        # a row that is sometimes two buttons tall and sometimes three is a panel that jumps
        # about as keepers join.
        at(row, f"CongRow{i}Tithe", [Scene.button("TITHE", DISPLAY, 13.0, PANEL_DEEP, mix(ROW_HOT, ICHOR, 0.35), BONE_DIM, ICHOR), Scene.selectable("congregation")], 12, 64, 93, 26)
        at(row, f"CongRow{i}Shunt", [Scene.button("SHUNT", DISPLAY, 13.0, PANEL_DEEP, mix(ROW_HOT, DREAD, 0.45), BONE_DIM, DREAD), Scene.selectable("congregation")], 113, 64, 93, 26)
        at(row, f"CongRow{i}Give", [Scene.button("RELIC", DISPLAY, 13.0, PANEL_DEEP, mix(ROW_HOT, ICHOR, 0.20), BONE_DIM, ICHOR), Scene.selectable("congregation")], 214, 64, 94, 26)

    # ── the offline report ──────────────────────────────────────────────────────────
    offline = s.add("OfflinePanel", canvas, [
        Scene.box(0, 0, 640, 340, MID, CENTRE),
        Scene.image(PANEL_DEEP, 6.0),
    ])
    at(offline, "OfflineHeading", [Scene.text("WHILE YOU WERE AWAY", DISPLAY, 20.0, BONE_DIM)], 32, 30, 560, 30)
    at(offline, "OfflineBody", [Scene.text("", BODY, 17.0, BONE, "left", "top", True)], 32, 78, 560, 150)
    at(offline, "OfflineDismiss", [Scene.button("TAKE UP THE VIGIL", DISPLAY, 15.0, ROW, mix(ROW_HOT, ICHOR, 0.30), BONE, ICHOR), Scene.selectable("offline")], 190, 262, 260, 44)

    # The whisper feed: a fixed pool of six lines along the bottom, newest last. Fixed count,
    # so it is chrome - the script only ever rewrites the string and the colour.
    for i in range(6):
        y = -34.0 - (5 - i) * 26.0
        s.add(f"WhisperLine{i}", canvas, [
            Scene.box(34, y, 920, 26, (0.0, 1.0), (0.0, 1.0)),
            Scene.text("", WHISPER, 18.0, BONE_DIM),
        ])

    # ── The vigil's own menu ────────────────────────────────────────────────────────
    # Escape used to leave the parish outright: one press, no confirmation, straight back to
    # the title screen. It saved first, so nothing was lost - but it is still the whole game
    # closing on the key people press to mean "not this".
    #
    # It does NOT pause. The parish keeps working underneath and a visitation that arrives
    # while this is open resolves as it would have. A menu that stopped the meter would be a
    # button that suspends the game's one bargain while you think about it.
    def vg(name, comps, x, y, w, h):
        return s.add(name, canvas, [Scene.box(x, y, w, h, MID, CENTRE)] + comps)

    # The veil dims the parish rather than hiding it: what the menu is over stays legible,
    # which is the whole reason the settings are reachable from here.
    s.add("VgMenuVeil", canvas, [
        Scene.stretch((0, 0), (1, 1), (0, 0), (0, 0)),
        # Authored dark; the script sets the exact weight per page, because the settings page
        # has to be judged against what is behind it and the root page does not.
        Scene.image((VOID[0], VOID[1], VOID[2], 0.82), 0.0),
    ])
    vg("VgMenuTitle", [Scene.text("THE VIGIL STANDS", DISPLAY, 34.0, BONE, "center")], 0, -180, 900, 46)

    MENU_W = 360.0
    vg("VgResume", [Scene.button("RETURN TO IT", DISPLAY, 16.0, ROW, mix(ROW_HOT, ICHOR, 0.34), BONE, ICHOR), Scene.selectable("vigilmenu")], 0, -80, MENU_W, 50)
    vg("VgSettings", [Scene.button("SETTINGS", DISPLAY, 15.0, ROW, ROW_HOT, BONE_DIM, BONE), Scene.selectable("vigilmenu")], 0, -24, MENU_W, 46)
    vg("VgLeave", [Scene.button("LEAVE THE PARISH", DISPLAY, 15.0, ROW, mix(ROW_HOT, DREAD, 0.35), BONE_FAINT, DREAD), Scene.selectable("vigilmenu")], 0, 28, MENU_W, 46)

    settings_rows(vg, "Vg", -80, "vigilmenu")
    vg("VgBack", [Scene.button("BACK", DISPLAY, 15.0, ROW, ROW_HOT, BONE_DIM, BONE), Scene.selectable("vigilmenu")], 0, 60, 180, 40)

    # Flash last: an overlay drawn above everything, gated by its own sort order.
    s.add("VigilFlash", canvas, [
        Scene.stretch((0, 0), (1, 1), (0, 0), (0, 0)),
        Scene.effect("ui_gloom", DREAD, VOID, background=False, sort_order=1000),
    ])

    body = s.render("# Hollowtide - the vigil. Authored chrome; only the ledger ROWS are script-built,\n"
                    "# because they come from the content tables and are re-filled per tab.\n")
    spawns = "".join(
        """[[entities]]
euler = [ 0.0, 0.0, 0.0 ]
name = 'Spawn%d'
node = %d
parent = -1
position = [ %.1f, -0.1, 0.0 ]
scale = [ 1.0, 1.0, 1.0 ]

""" % (i, node_id(f"Spawn{i}"), x)
        for i, x in enumerate([2.6, 3.9, 5.2, 6.5])
    )
    prologue = (
        CAMERA.format(node=node_id("Main Camera"), clear="0.016, 0.019, 0.024")
        + "\n"
        + """[[entities]]
euler = [ 0.0, 0.0, 0.0 ]
name = 'Parish'
node = %d
parent = -1
position = [ 0.0, 0.0, 0.0 ]
scale = [ 1.0, 1.0, 1.0 ]

    [entities.light_2d_settings]
    ambientIntensity = 0.42
    ambient_color = [ 0.1, 0.12, 0.16 ]
    shadowSoftness = 2.4
    shadowStrength = 0.85

    [[entities.scripts]]
    type = 'HollowtideGame'
""" % node_id("Parish")
        + "\n"
        + CANVAS.format(name="HollowUI", node=node_id("HollowUI"))
        + "\n"
        + """[[entities]]
euler = [ 0.0, 0.0, 0.0 ]
name = 'Session'
node = %d
parent = -1
position = [ 0.0, 0.0, 0.0 ]
scale = [ 1.0, 1.0, 1.0 ]

    [[entities.scripts]]
    type = 'HollowtideSession'

""" % node_id("Session")
        + spawns
    )
    return splice(body, prologue, 8)


def splice(body, prologue, placeholder_count):
    """Replace the first N generated placeholder entities with the hand-written prologue."""
    blocks = body.split("[[entities]]")
    head = blocks[0]
    rest = blocks[1 + placeholder_count:]
    return head + prologue + "\n" + "[[entities]]".join([""] + rest).lstrip("\n")


# ── entry point ─────────────────────────────────────────────────────────────────────────

SCENES = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scenes"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true",
                        help="overwrite scenes that already exist, discarding any editor edits")
    parser.add_argument("--out", default=SCENES, help="directory to write into (default: the project's scenes/)")
    args = parser.parse_args()

    written = []
    for name, build in (("Threshold", threshold), ("Vigil", vigil)):
        path = os.path.join(args.out, name + ".scene.toml")
        if os.path.exists(path) and not args.force:
            print(f"refusing to overwrite {path}", file=sys.stderr)
            print("  It may hold editor edits this script knows nothing about.", file=sys.stderr)
            print(f"  Port them into the layout in {os.path.basename(__file__)} first,"
                  " then re-run with --force.", file=sys.stderr)
            return 1
        io.open(path, "w", encoding="utf-8", newline="\n").write(build())
        written.append(path)

    for path in written:
        print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
