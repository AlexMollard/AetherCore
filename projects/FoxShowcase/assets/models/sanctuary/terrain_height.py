"""Height in world units for the FoxShowcase landscape."""
import math


def terrain_height(x, z):
    if x <= -64.0:
        return -0.65
    if x < -56.0:
        return -0.65 + (x + 64.0) * (0.55 / 8.0)
    if x < -45.0:
        return -0.1 + (x + 56.0) * (0.4 / 11.0)
    if abs(x) <= 20.0 and abs(z) <= 20.0:
        return 0.0
    fade = min(1.0, max(0.0, (max(abs(x), abs(z)) - 20.0) / 10.0))
    rolling = fade * (0.30 * math.sin(x * 0.12) * math.cos(z * 0.10))
    rise = min(1.0, max(0.0, (-z - 35.0) / 25.0))
    rise = rise * rise * (3.0 - 2.0 * rise)
    peaks = sum(
        amplitude * math.exp(-((x - px) / width_x) ** 2 - ((z - pz) / width_z) ** 2)
        for px, pz, width_x, width_z, amplitude in (
            (-34.0, -75.0, 18.0, 22.0, 28.0),
            (0.0, -76.0, 21.0, 24.0, 31.0),
            (38.0, -73.0, 19.0, 22.0, 29.0),
        )
    )
    ridges = 1.5 + 1.4 * abs(math.sin(x * 0.14 + z * 0.09))
    mountain = rise * (peaks + ridges)
    inland = rolling + mountain
    if x < -40.0:
        blend = (x + 45.0) / 5.0
        return 0.3 * (1.0 - blend) + inland * blend
    pond_radius_squared = ((x + 30.0) ** 2 + (z - 42.0) ** 2) / 30.25
    return inland - 0.7 * max(0.0, 1.0 - pond_radius_squared)
