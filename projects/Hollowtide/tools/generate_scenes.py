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
import sys

# ── Palette (mirrors scripts/Palette.cs) ────────────────────────────────────────────
VOID = (0.020, 0.024, 0.030, 1.000)
PANEL = (0.043, 0.050, 0.060, 0.941)
PANEL_DEEP = (0.027, 0.032, 0.040, 0.960)
ROW = (0.075, 0.086, 0.101, 0.900)
ROW_HOT = (0.125, 0.145, 0.160, 0.960)
BONE = (0.855, 0.851, 0.816, 1.0)
BONE_DIM = (0.502, 0.510, 0.522, 1.0)
BONE_FAINT = (0.290, 0.302, 0.322, 1.0)
ICHOR = (0.454, 0.855, 0.678, 1.0)
ICHOR_DIM = (0.220, 0.450, 0.360, 1.0)
DREAD = (0.706, 0.267, 0.220, 1.0)
DREAD_DEEP = (0.380, 0.110, 0.110, 1.0)
SIGIL = (0.741, 0.639, 0.925, 1.0)
CLEAR = (0.0, 0.0, 0.0, 0.0)

BODY = "Roboto-Regular"
WHISPER = "IBMPlexMono-Italic"
DISPLAY = "PixelStorm"

LEDGER_W = 620.0
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
        return (
            "ui_text",
            [
                ("color", list(color)),
                ("font", font),
                ("h_align", h),
                ("pixel_size", size),
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
                ("pixel_size", size),
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
                ("pixel_size", size),
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
MID = (0.5, 0.5)


# ══════════════════════════════════════════════════════════════════════════════════════
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
    centred("ThTitle", [Scene.text("HOLLOWTIDE", DISPLAY, 72.0, BONE, "center")], 0, -250, 900, 90)
    centred("ThSubtitle", [Scene.text("keep the parish, and count what it costs you", WHISPER, 18.0, BONE_FAINT, "center")], 0, -186, 900, 30)

    # A stack, not a scatter. Each field is a caption sitting ABOVE its control, both on the
    # same 400px column, so every left edge on the screen is the same left edge. The captions
    # used to be a half-width box laid across the control next to them: legible only because
    # the words were short.
    FIELD_W = 400.0
    centred("ThNameLabel", [Scene.text("YOUR NAME", DISPLAY, 15.0, BONE_FAINT)], 0, -129, FIELD_W, 22)
    centred("ThNameBox", [Scene.text_box("Keeper", 20, "alphanumeric")], 0, -90, FIELD_W, 44)
    centred("ThAddressLabel", [Scene.text("HOST ADDRESS", DISPLAY, 15.0, BONE_FAINT)], 0, -33, FIELD_W, 22)
    centred("ThAddressBox", [Scene.text_box("127.0.0.1:7777", 48, "host")], 0, 6, FIELD_W, 44)

    centred("ThAlone", [Scene.button("KEEP VIGIL ALONE", DISPLAY, 15.0, ROW, mix(ROW_HOT, ICHOR, 0.30), BONE, ICHOR), Scene.selectable("threshold")], 0, 84, FIELD_W, 48)
    centred("ThHost", [Scene.button("HOST A CONGREGATION", DISPLAY, 15.0, ROW, mix(ROW_HOT, SIGIL, 0.30), BONE_DIM, SIGIL), Scene.selectable("threshold")], -104, 142, 192, 44)
    centred("ThJoin", [Scene.button("JOIN ONE", DISPLAY, 15.0, ROW, mix(ROW_HOT, SIGIL, 0.30), BONE_DIM, SIGIL), Scene.selectable("threshold")], 104, 142, 192, 44)

    # Settings live in the SAME 400px column as the fields above, one setting per row with
    # its control flush to the column's right edge. They used to straddle a wider band than
    # anything else on screen, which read as a second, misaligned form.
    SETTINGS_H = 26.0
    centred("ThWhisperLabel", [Scene.text("SHOW WHISPERS", DISPLAY, 15.0, BONE_FAINT)], -100, 217, 200, SETTINGS_H)
    centred("ThWhisperToggle", [Scene.toggle(True), Scene.selectable("threshold")], 172, 217, 56, SETTINGS_H)
    centred("ThShakeLabel", [Scene.text("", DISPLAY, 15.0, BONE_FAINT)], -100, 255, 200, SETTINGS_H)
    centred("ThShakeSlider", [Scene.slider(0.0, 1.5, 0.1, 1.0), Scene.selectable("threshold")], 140, 255, 120, SETTINGS_H)

    centred("ThStatus", [Scene.text("", WHISPER, 16.0, BONE_DIM, "center")], 0, 312, 900, 26)

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
    at(bar, "HudDreadCaption", [Scene.text("DREAD", DISPLAY, 15.0, BONE_FAINT)], 932, ROW_A, 160, 22)
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
    # Stoke and ward are a pair - the two directions you can push the meter - so they share a
    # row. The bell is a different kind of thing (and only exists in a congregation), so it
    # sits on its own beneath them rather than making a row of three that is sometimes two.
    at(nave, "NaveStoke", [Scene.button("STOKE THE DARK", DISPLAY, 16.0, ROW, mix(ROW_HOT, DREAD, 0.55), BONE, BONE), Scene.selectable("nave")], -132, 186, 250, 44, MID, CENTRE)
    at(nave, "NaveWard", [Scene.button("RAISE WARD", DISPLAY, 16.0, ROW, mix(ROW_HOT, ICHOR, 0.35), BONE, BONE), Scene.selectable("nave")], 132, 186, 250, 44, MID, CENTRE)
    at(nave, "NaveBell", [Scene.button("RING THE BELL", DISPLAY, 16.0, ROW, mix(ROW_HOT, SIGIL, 0.40), BONE, BONE), Scene.selectable("nave")], 0, 240, 250, 44, MID, CENTRE)

    # ── the ledger ──────────────────────────────────────────────────────────────────
    ledger = s.add("LedgerPanel", canvas, [
        Scene.stretch((1, 0), (1, 1), (-LEDGER_W, 0), (0, 0)),
        Scene.image(PANEL),
    ])
    at(ledger, "LedgerHeading", [Scene.text("THE LEDGER", DISPLAY, 21.0, BONE_DIM)], 20, 18, 300, 30)
    tab_w = (LEDGER_W - 40.0 - 18.0) / 4.0
    for i, label in enumerate(["RITES", "OFFERINGS", "COMMUNION", "MARKS"]):
        at(ledger, f"LedgerTab{i}", [Scene.button(label, DISPLAY, 15.0), Scene.selectable("ledger")], 20 + i * (tab_w + 6.0), 56, tab_w, 36)
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
        at(row, f"CongRow{i}Tithe", [Scene.button("TITHE", DISPLAY, 15.0, PANEL_DEEP, mix(ROW_HOT, ICHOR, 0.35), BONE_DIM, ICHOR), Scene.selectable("congregation")], 12, 64, 140, 26)
        at(row, f"CongRow{i}Shunt", [Scene.button("SHUNT", DISPLAY, 15.0, PANEL_DEEP, mix(ROW_HOT, DREAD, 0.45), BONE_DIM, DREAD), Scene.selectable("congregation")], 168, 64, 140, 26)

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


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(4))


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
