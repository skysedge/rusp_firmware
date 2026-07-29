"""
CHG edge history policy (mirrors firmware EEPROM ring).

USB unplug kills Serial; USB replug resets the MCU. Plug edges are therefore
invisible on Serial (sampling starts with the cable already in). Edges while
running on battery must be stored in non-volatile memory and printed at boot.
"""

CHG_HIST_MAGIC = 0xC7
CHG_HIST_MAX = 12


def should_record_edge(prev_raw, new_raw) -> bool:
	"""Record only when the raw CHG_STAT level changes."""
	if prev_raw is None:
		return False
	return int(prev_raw) != int(new_raw)


def pack_record(ms: int, raw: int) -> bytes:
	"""5-byte record: millis uint32 LE + raw uint8."""
	ms_i = int(ms) & 0xFFFFFFFF
	raw_i = int(raw) & 0xFF
	return bytes(
		(
			ms_i & 0xFF,
			(ms_i >> 8) & 0xFF,
			(ms_i >> 16) & 0xFF,
			(ms_i >> 24) & 0xFF,
			raw_i,
		)
	)


def unpack_record(data: bytes):
	if len(data) < 5:
		raise ValueError("short record")
	ms = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24)
	return ms, data[4]


def ring_append(records, ms: int, raw: int, max_n: int = CHG_HIST_MAX):
	"""
	Append one edge; drop oldest when full.
	records: list of (ms, raw)
	"""
	out = list(records)
	out.append((int(ms), int(raw) & 0xFF))
	if len(out) > max_n:
		out = out[-max_n:]
	return out
