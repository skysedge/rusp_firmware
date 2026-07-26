"""
Why: USB plug/unplug cannot be observed on Serial after reconnect (MCU reset).
Failure without this: hunt concludes 'no pins toggled' because edges were
never stored across the reset that ends each plug cycle.
"""

import unittest

from chg_hist import (
	pack_record,
	ring_append,
	should_record_edge,
	unpack_record,
)


class TestChgHist(unittest.TestCase):
	def test_no_record_without_previous(self):
		self.assertFalse(should_record_edge(None, 1))

	def test_record_only_on_change(self):
		self.assertFalse(should_record_edge(1, 1))
		self.assertTrue(should_record_edge(1, 0))
		self.assertTrue(should_record_edge(0, 1))

	def test_pack_unpack_roundtrip(self):
		blob = pack_record(123456, 1)
		self.assertEqual(len(blob), 5)
		self.assertEqual(unpack_record(blob), (123456, 1))

	def test_ring_drops_oldest(self):
		recs = []
		for i in range(14):
			recs = ring_append(recs, i * 100, i & 1, max_n=12)
		self.assertEqual(len(recs), 12)
		self.assertEqual(recs[0], (200, 0))  # dropped 0 and 100
		self.assertEqual(recs[-1], (1300, 1))


if __name__ == "__main__":
	unittest.main()
