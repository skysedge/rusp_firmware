default: compile
	arduino-cli upload -b MegaCore:avr:1280 -p /dev/ttyACM0 -v

compile:
	arduino-cli compile -b MegaCore:avr:1280

program:
	arduino-cli upload -b MegaCore:avr:1280 -P avrispmkii -v

