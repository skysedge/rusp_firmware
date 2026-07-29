"""Map AT+CSQ RSSI to 0..4 signal bars for the OLED icon."""


def signal_bars_from_rssi(rssi: int) -> int:
	"""rssi is 0..31, or 99 if unknown (3GPP +CSQ)."""
	if rssi == 99 or rssi <= 0:
		return 0
	if rssi >= 22:
		return 4
	if rssi >= 15:
		return 3
	if rssi >= 8:
		return 2
	return 1
