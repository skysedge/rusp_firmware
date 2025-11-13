#BOARD ?= MegaCore:avr:1280
#BAUD ?= 115200
#BOARD_OPTS ?= clock=16MHz_external,baudrate=${BAUD}
BOARD ?= MegaCore:avr:2560
BAUD ?= 9600
BOARD_OPTS ?= clock=7_3728MHz_external,baudrate=${BAUD}
ARDUINO_PACKAGES ?= ${HOME}/.arduino15/packages
U2_FW_DIR ?= ${ARDUINO_PACKAGES}/arduino/hardware/avr/1.8.6/firmwares/atmegaxxu2

default: compile usb

compile:
	arduino-cli compile -b ${BOARD} --board-options ${BOARD_OPTS}

usb:
	arduino-cli upload -b ${BOARD} -p /dev/ttyACM0 -vt \
		--board-options ${BOARD_OPTS}

program:
	arduino-cli upload -b ${BOARD} -P avrispmkii -vt \
		--board-options ${BOARD_OPTS}
	echo "Note: after programming with the programmer, you won't be able" \
		"to flash over USB, unless you burn the bootloader again. I'm" \
		"not sure why this is. ---imyxh"

bootloader:
	arduino-cli burn-bootloader -b ${BOARD} -P avrispmkii -vt \
		--board-options ${BOARD_OPTS}

u2:
	# efuse and hfuse settings are defaults in the datasheet;
	# lfuse is default except we unprogram CKDIV8 and CKSEL0 for 16 MHz;
	# lock bits are default (and quite restrictive, but they get cleared
	# during chip erase)
	avrdude -p m16u2 -P usb -c avrispmkii \
		-U flash:w:${U2_FW_DIR}/MEGA-dfu_and_usbserial_combined.hex \
		-U efuse:w:0xF4:m -U hfuse:w:0xD9:m -U lfuse:w:0xDF:m \
		-U lock:w:0xEC:m -v

