"""Tests for the e-ink Mandelbrot splash pixel test.

Why: splash must be a real fractal (set membership), not random noise.
Failure modes when regressions occur:
- Origin of the window not in the set → empty/broken cardioid.
- All pixels inked → solid black panel.
- Escape banding never true → featureless exterior.
"""

import unittest

from epd_fractal import mandelbrot_ink


class MandelbrotSplashTests(unittest.TestCase):
	def test_center_of_cardioid_is_inked(self):
		# Why: c=0 is in the Mandelbrot set; maps near mid of our window.
		# Failure: set detection broken → splash mostly white.
		w, h = 128, 296
		# cx≈0 → px ≈ 2.0/2.6 * w ≈ 98; cy≈0 → py = h/2
		self.assertTrue(mandelbrot_ink(98, h // 2, w, h))

	def test_far_corner_escapes(self):
		# Why: c≈(-2,-1.2) is outside the set → white paper.
		# Failure: exterior filled black with the cardioid.
		self.assertFalse(mandelbrot_ink(0, 0, 128, 296))

	def test_produces_both_ink_and_paper(self):
		# Why: a usable splash needs contrast across the panel.
		# Failure: uniform field (all True or all False).
		ink = paper = False
		for py in range(0, 296, 8):
			for px in range(0, 128, 4):
				if mandelbrot_ink(px, py, 128, 296):
					ink = True
				else:
					paper = True
				if ink and paper:
					break
			if ink and paper:
				break
		self.assertTrue(ink and paper)


if __name__ == "__main__":
	unittest.main()
