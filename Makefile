default: compile usb

compile:
	arduino-cli compile -b MegaCore:avr:1280

usb:
	arduino-cli upload -b MegaCore:avr:1280 -p /dev/ttyACM0 -v

program:
	arduino-cli upload -b MegaCore:avr:1280 -P avrispmkii -v

bootloader:
	arduino-cli burn-bootloader -b MegaCore:avr:1280 -P avrispmkii -v

u2:
	cd ${HOME}/.arduino15/packages/arduino/hardware/avr/1.8.6/firmwares/; \
	cd atmegaxxu2; \
	avrdude -p at90usb82 -F -P usb -c avrispmkii \
		-U flash:w:MEGA-dfu_and_usbserial_combined.hex \
		-U lfuse:w:0xFF:m -U hfuse:w:0xD9:m \
		-U efuse:w:0xF4:m -U lock:w:0x0F:m

