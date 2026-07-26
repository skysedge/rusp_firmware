"""E-ink splash fractal: Mandelbrot membership used by the bitmap generator.

`gen_epd_fractal_splash.py` packs this into `epd_fractal_splash.h` so the
device only blits PROGMEM — it does not compute the set at boot.
"""

from __future__ import annotations

FP_SHIFT = 12
FP_ONE = 1 << FP_SHIFT
MAX_ITER = 20


def mandelbrot_ink(px: int, py: int, width: int, height: int) -> bool:
	"""
	Map (px, py) into the classic Mandelbrot window and ink the set
	(and odd escape bands for texture on the e-ink panel).
	"""
	if width <= 0 or height <= 0:
		return False
	# cx in [-2.0, 0.6], cy in [-1.2, 1.2]
	cx = (-2 * FP_ONE) + (px * (26 * FP_ONE // 10)) // width
	cy = (-12 * FP_ONE // 10) + (py * (24 * FP_ONE // 10)) // height
	x = 0
	y = 0
	for _n in range(MAX_ITER):
		x2 = (x * x) >> FP_SHIFT
		y2 = (y * y) >> FP_SHIFT
		if x2 + y2 > (4 << FP_SHIFT):
			return False  # escaped → paper (white)
		xy = (x * y) >> (FP_SHIFT - 1)  # 2*x*y
		x = x2 - y2 + cx
		y = xy + cy
	return True  # bounded → ink (black)
