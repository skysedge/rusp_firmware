"""Tests for packing the Mandelbrot splash into Adafruit 1bpp format.

Why: firmware draws a precomputed bitmap; a wrong bit order or size shows
garbage or a blank panel.
Failure: MSB/LSB swap flips the set; length mismatch truncates the image.
"""

import unittest

from epd_fractal import mandelbrot_ink
from gen_epd_fractal_splash import HEIGHT, WIDTH, pack_bitmap


class PackBitmapTests(unittest.TestCase):
	def test_size_and_sample_pixels(self):
		# Why: header must be full-panel; sample bits must match mandelbrot_ink.
		# Failure: wrong dimensions or inverted/shifted ink.
		data = pack_bitmap()
		row_bytes = (WIDTH + 7) // 8
		self.assertEqual(len(data), row_bytes * HEIGHT)

		samples = [(98, HEIGHT // 2), (0, 0), (64, 100)]
		for px, py in samples:
			byte = data[py * row_bytes + (px // 8)]
			bit = (byte & (0x80 >> (px & 7))) != 0
			self.assertEqual(
				bit,
				mandelbrot_ink(px, py, WIDTH, HEIGHT),
				msg=f"mismatch at ({px},{py})",
			)


if __name__ == "__main__":
	unittest.main()
