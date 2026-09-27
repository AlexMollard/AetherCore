"""Generate the Twinsanity effect atlas (look upgrade, logs/look/FxLook/README.md).

Procedural, not ISO-derived: soft modern sprites the particle shader filters bilinearly
(the 128 px disc pages stay point-sampled). 512 x 512, sRGB PNG, straight alpha, RGB kept
valid under zero alpha so bilinear filtering never pulls in a dark fringe.

Cells (pixel rects, used by CrateFx as uv = rect / 512):
  smoke    (  0,   0, 256, 256)  billowy puff; RGB = cavity detail (lit by the shader)
  fire     (256,   0, 512, 256)  turbulent fireball; RGB = heat (white core, grey rim)
  flash    (  0, 256, 256, 512)  flash core: hot centre, soft glow, eight soft rays
  spark    (256, 256, 384, 384)  hot dot with a halo
  ring     (384, 256, 512, 384)  shockwave ring
  star     (256, 384, 384, 512)  four-point twinkle
  ember    (384, 384, 512, 512)  small ragged puff (dust, debris, feathers' smoke)

Usage: python tools/fx-atlas/fx_atlas.py [out.png]
(default projects/Twinsanity/assets/particles/fx_atlas.png)
"""
import sys

import numpy as np
from PIL import Image

N = 512
rng = np.random.default_rng(0x7A5)


def grid(size):
	c = (np.arange(size) + 0.5) / size * 2.0 - 1.0
	x, y = np.meshgrid(c, c)
	return x, y


def value_noise(size, cells, seed):
	"""Tileable-enough smooth value noise, 0..1, `cells` lattice cells across."""
	r = np.random.default_rng(seed).random((cells + 1, cells + 1))
	t = np.linspace(0.0, cells, size, endpoint=False)
	i = t.astype(int)
	f = t - i
	f = f * f * (3.0 - 2.0 * f)
	a = r[np.ix_(i, i)]
	b = r[np.ix_(i, i + 1)]
	c = r[np.ix_(i + 1, i)]
	d = r[np.ix_(i + 1, i + 1)]
	fx = f[None, :]
	fy = f[:, None]
	return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(size, seed, octaves=5, base=4):
	out = np.zeros((size, size))
	amp, tot = 1.0, 0.0
	for o in range(octaves):
		out += amp * value_noise(size, base << o, seed + o)
		tot += amp
		amp *= 0.5
	return out / tot


def smoothstep(e0, e1, x):
	t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)


def blobs(size, count, spread, rmin, rmax, seed):
	"""Union of soft balls: the billow silhouette. Returns density 0..1."""
	r = np.random.default_rng(seed)
	x, y = grid(size)
	d = np.zeros((size, size))
	for _ in range(count):
		ang = r.random() * 2 * np.pi
		rad = r.random() ** 0.7 * spread
		cx, cy = np.cos(ang) * rad, np.sin(ang) * rad
		br = rmin + (rmax - rmin) * r.random()
		d = np.maximum(d, 1.0 - ((x - cx) ** 2 + (y - cy) ** 2) / (br * br))
	return np.clip(d, 0.0, 1.0)


def billows(size, count, spread, rmin, rmax, seed):
	"""Overlapping balls as a cumulus: coverage 0..1 and each pixel's front-most ball's facing
	(1 at a ball's centre, 0 at its rim), so the creases between billows shade dark."""
	r = np.random.default_rng(seed)
	x, y = grid(size)
	cover = np.zeros((size, size))
	wsum = np.full((size, size), 1e-6)
	facing = np.zeros((size, size))
	for _ in range(count):
		ang = r.random() * 2 * np.pi
		rad = r.random() ** 0.7 * spread
		cx, cy = np.cos(ang) * rad, np.sin(ang) * rad
		br = rmin + (rmax - rmin) * r.random()
		q = 1.0 - ((x - cx) ** 2 + (y - cy) ** 2) / (br * br)
		h = np.sqrt(np.clip(q, 0.0, 1.0))
		# Soft z-buffer: the front-most ball wins smoothly, so billows meet in soft creases.
		w = np.where(q > 0.0, np.exp(np.clip(18.0 * (h * br + (0.5 - r.random()) * 0.1), -50, 50)) * q, 0.0)
		wsum += w
		facing += w * h
		cover = np.maximum(cover, q)
	facing /= wsum
	return np.clip(cover, 0.0, 1.0), facing


def cell_smoke(size=256):
	x, y = grid(size)
	cover, facing = billows(size, 16, 0.5, 0.2, 0.36, 11)
	n = fbm(size, 21)
	fine = fbm(size, 29, octaves=4, base=8)
	# Crisp billow silhouette with a soft, noisy rim, dense inside: reads as a puff, not a haze.
	dens = smoothstep(0.0, 0.35, cover * (0.6 + 0.8 * n)) * (0.92 + 0.08 * fine)
	dens *= smoothstep(1.0, 0.86, np.sqrt(x * x + y * y))  # never touches the cell edge
	# Billow crowns light, the creases between them dark: detail the sun lighting multiplies.
	detail = 0.45 + 0.55 * facing ** 0.7 * (0.8 + 0.4 * fine)
	rgb = np.repeat(np.clip(detail, 0, 1)[..., None], 3, axis=2)
	return rgb, dens


def cell_fire(size=256):
	x, y = grid(size)
	r = np.sqrt(x * x + y * y)
	n = fbm(size, 41, base=5)
	n2 = fbm(size, 57, base=3)
	shape = smoothstep(0.95, 0.25, r + (n - 0.5) * 0.7)
	heat = np.clip(shape * (0.55 + 0.9 * n2) - r * 0.25, 0.0, 1.0)
	alpha = smoothstep(0.0, 0.7, shape * (0.5 + 0.8 * n2)) * smoothstep(1.0, 0.85, r)
	# White-hot core -> pale -> grey rim; the colour keys give it its hue.
	v = 0.35 + 0.65 * smoothstep(0.1, 0.8, heat)
	rgb = np.repeat(v[..., None], 3, axis=2)
	return rgb, alpha


def cell_flash(size=256):
	x, y = grid(size)
	r = np.sqrt(x * x + y * y)
	a = np.arctan2(y, x)
	core = np.exp(-(r / 0.12) ** 2)
	glow = np.exp(-(r / 0.42) ** 2) * 0.55
	rays = (0.5 + 0.5 * np.cos(a * 8.0)) ** 6 * np.exp(-(r / 0.55) ** 2) * 0.5
	rays += (0.5 + 0.5 * np.cos(a * 8.0 + np.pi / 8)) ** 10 * np.exp(-(r / 0.55) ** 2) * 0.3
	v = np.clip(core + glow + rays, 0.0, 1.0) * smoothstep(1.0, 0.9, r)
	rgb = np.ones((size, size, 3))
	return rgb, v


def cell_spark(size=128):
	x, y = grid(size)
	r = np.sqrt(x * x + y * y)
	v = np.clip(np.exp(-(r / 0.16) ** 2) + np.exp(-(r / 0.45) ** 2) * 0.35, 0, 1) * smoothstep(1.0, 0.85, r)
	return np.ones((size, size, 3)), v


def cell_ring(size=128):
	x, y = grid(size)
	r = np.sqrt(x * x + y * y)
	v = np.exp(-((r - 0.78) / 0.07) ** 2) + np.exp(-((r - 0.7) / 0.22) ** 2) * 0.25
	v = np.clip(v, 0, 1) * smoothstep(1.0, 0.93, r)
	return np.ones((size, size, 3)), v


def cell_star(size=128):
	x, y = grid(size)
	r = np.sqrt(x * x + y * y)
	arms = np.exp(-(np.abs(x) / 0.035)) * np.exp(-(np.abs(y) / 0.55)) + np.exp(-(np.abs(y) / 0.035)) * np.exp(-(np.abs(x) / 0.55))
	v = np.clip(arms + np.exp(-(r / 0.12) ** 2), 0, 1) * smoothstep(1.0, 0.85, r)
	return np.ones((size, size, 3)), v


def cell_ember(size=128):
	x, y = grid(size)
	body = blobs(size, 6, 0.3, 0.25, 0.45, 77)
	n = fbm(size, 83, base=4)
	dens = smoothstep(0.1, 0.8, body * (0.6 + 0.8 * n)) * smoothstep(1.0, 0.8, np.sqrt(x * x + y * y))
	detail = 0.75 + 0.25 * n
	return np.repeat(detail[..., None], 3, axis=2), dens


def main():
	out = sys.argv[1] if len(sys.argv) > 1 else "projects/Twinsanity/assets/particles/fx_atlas.png"
	img = np.zeros((N, N, 4))
	img[..., :3] = 1.0
	for (px, py), fn in (((0, 0), cell_smoke), ((256, 0), cell_fire), ((0, 256), cell_flash),
		((256, 256), cell_spark), ((384, 256), cell_ring), ((256, 384), cell_star), ((384, 384), cell_ember)):
		rgb, a = fn()
		s = a.shape[0]
		img[py:py + s, px:px + s, :3] = rgb
		img[py:py + s, px:px + s, 3] = a
	Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(out)
	print("wrote", out)


if __name__ == "__main__":
	main()
