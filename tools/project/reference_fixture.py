"""Generate/check the identical authored data for the two desktop reference games."""
from __future__ import annotations

import argparse
import io
import json
import math
from pathlib import Path
import struct
import wave
import zlib

ROOT = Path(__file__).resolve().parents[2]
PROJECTS = [ROOT / "projects" / ("AgentPipeline" + name) for name in ("Rust", "CSharp")]
HEADER = "[scene]\nversion = 16\nkind = '2d'\nfeatures = ['sprites', 'physics_2d']\n"


def entity(name, x=0, y=0, z=0):
    return f"\n[[entities]]\nname = '{name}'\nposition = [{x}.0, {y}.0, {z}.0]\nscale = [1.0, 1.0, 1.0]\n"


SPRITE = """
[entities.sprite]
texture = 'project://assets/textures/reference.png'
pixel_size = [16.0, 16.0]
pixels_per_unit = 16.0
tint = [0.2, 0.8, 1.0, 1.0]
visible = true
"""
CAMERA = entity("Camera", z=10) + """
[entities.camera]
main = true
projection = 'orthographic'
orthographic_height = 6.0
background = 'solid_colour'
clear_color = [0.02, 0.03, 0.06]
near = 0.1
far = 100.0
"""
COUNTER = entity("Counter") + """
[entities.ui_canvas]
reference = [640.0, 360.0]
scale_mode = 'constant_pixel'
[entities.ui_rect]
anchor_min = [0.0, 0.0]
anchor_max = [0.0, 0.0]
offset_min = [16.0, 16.0]
offset_max = [320.0, 56.0]
[entities.ui_text]
text = 'Score 0'
font = 'Roboto'
size = 24.0
color = [1.0, 1.0, 1.0, 1.0]
"""


def body(kind, size, trigger=False):
    return f"""
[entities.rigid_body_2d]
body_type = '{kind}'
gravity_scale = 0.0
fixed_rotation = true
[entities.collider_2d]
shape = 'box'
size = [{size[0]}, {size[1]}]
is_trigger = {str(trigger).lower()}
density = 1.0
friction = 0.0
"""


def assertion(entity_name, prop, value, script="$engine", **extra):
    return dict(entity=entity_name, type=script, property=prop, equals=value, **extra)


def config():
    return dict(
        profile="native-desktop-2d-v1", readSaves=True,
        input=[dict(tick=0, key="space", down=True), dict(tick=20, key="space", down=False)],
        checkpoints=[
            dict(tick=10, assertions=[
                assertion("ReferencePickup", "exists", True),
                assertion("Counter", "uiText", "Score 10"),
                assertion("Player", "position", [0.3, 0, 0], tolerance=0.08),
                assertion("$runtime", "audioStarted", 1),
            ]),
            dict(tick=20, assertions=[
                assertion("ReferencePickup", "exists", False),
                assertion("Counter", "uiText", "Score 20"),
            ]),
            dict(tick=59, assertions=[
                assertion("Player", "collision", True, "ReferencePlayer"),
                assertion("Player", "trigger", True, "ReferencePlayer"),
            ]),
        ],
        assertions=[
            assertion("Result", "ticks", 30, "ReferenceResult"),
            assertion("Result", "score", 20, "ReferenceResult"),
            assertion("Result", "collision", True, "ReferenceResult"),
            assertion("Result", "trigger", True, "ReferenceResult"),
            assertion("Result", "label", "caf\u00e9\0agent", "ReferenceResult"),
            assertion("Counter", "uiText", "Saved score 20"),
            assertion("Player", "exists", False),
            assertion("ReferencePickup", "exists", False),
            assertion("$runtime", "scene", "ReferenceResult"),
            assertion("$runtime", "audioStarted", 1),
        ],
    )


def assets():
    sound = io.BytesIO()
    with wave.open(sound, "wb") as wav:
        wav.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
        wav.writeframes(b"".join(struct.pack("<h", int(4000 * math.sin(i * 2 * math.pi * 440 / 22050))) for i in range(2205)))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    pixels = b"".join(b"\0" + bytes([255, 255, 255, 255]) * 16 for _ in range(16))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")
    start = HEADER + entity("Player") + SPRITE + body("dynamic", [0.5, 0.5]) + "\n[[entities.scripts]]\ntype = 'ReferencePlayer'\n"
    start += CAMERA + COUNTER
    start += entity("Trigger", x=1) + body("static", [0.5, 4.0], True)
    start += entity("Wall", x=2) + body("static", [0.5, 4.0])
    result = HEADER + entity("Result") + "\n[[entities.scripts]]\ntype = 'ReferenceResult'\n" + CAMERA + COUNTER
    prefab = HEADER + entity("ReferencePickup", y=2) + SPRITE
    return {
        "scenes/ReferenceStart.scene.toml": start.encode(),
        "scenes/ReferenceResult.scene.toml": result.encode(),
        "assets/prefabs/ReferencePickup.prefab.toml": prefab.encode(),
        "assets/audio/reference.wav": sound.getvalue(),
        "assets/textures/reference.png": png,
        "reference-test.json": (json.dumps(config(), indent=2, ensure_ascii=False) + "\n").encode(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="Regenerate authored fixture data in both reference projects")
    args = parser.parse_args()
    errors = []
    for project in PROJECTS:
        for relative, expected in assets().items():
            path = project / relative
            if args.write:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(expected)
            elif not path.exists() or path.read_bytes().replace(b"\r\n", b"\n") != expected.replace(b"\r\n", b"\n"):
                errors.append(str(path.relative_to(ROOT)))
    if errors:
        raise SystemExit("Reference fixture differs: " + ", ".join(errors))
    print("Both reference fixtures match the shared authored data and seeded inputs.")


if __name__ == "__main__":
    main()
