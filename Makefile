#BOARD ?= MegaCore:avr:1280
#BAUD ?= 115200
#BOARD_OPTS ?= clock=16MHz_external,baudrate=${BAUD}
BOARD ?= MegaCore:avr:2560
# Urboot on UART0 is autobaud; use the highest MegaCore menu rate.
# 1000000 fails urclock sync on this board; 115200 is the reliable max.
BAUD ?= 115200
BOARD_OPTS ?= clock=7_3728MHz_external,baudrate=${BAUD}
ARDUINO_PACKAGES ?= ${HOME}/.arduino15/packages
U2_FW_DIR ?= ${ARDUINO_PACKAGES}/arduino/hardware/avr/1.8.6/firmwares/atmegaxxu2
PORT ?= /dev/ttyACM0
# Prefer PATH, then Arduino IDE.app bundled binary (macOS).
ARDUINO_CLI ?= $(shell \
	command -v arduino-cli 2>/dev/null || \
	ls "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" 2>/dev/null || \
	echo arduino-cli)

# Pulse debounce debug sketch (tools/pulse_debounce_debug). Overrides production
# firmware on the MCU until you re-flash with `make usb` / IDE upload.
PULSE_DEBUG_DIR := tools/pulse_debounce_debug
# Connected RUSP boards are ATmega2560 (same as `make usb`). Override with
# PULSE_DEBUG_BOARD=MegaCore:avr:1280 if flashing a 1280.
PULSE_DEBUG_BOARD ?= MegaCore:avr:2560
PULSE_DEBUG_BOARD_OPTS ?= clock=7_3728MHz_external,baudrate=${BAUD}
PULSE_MONITOR_DIR := tools/pulse_monitor
MEMCHECK_DIR := tools/memcheck

# RAM budget (see tools/memcheck/memcheck.py). The ATmega2560 has 8192 bytes
# of SRAM shared by globals, heap and stack; these ceilings exist so growth
# has to be a decision rather than a discovery.
#
# Ratchet these down as savings land. Raising one is how 85% happened.
#   .data — copied from flash to RAM at boot. Dominated by string literals
#           that are not in PROGMEM, which is what makes it worth attacking.
#   .bss  — zeroed globals. Dominated by eink_buffer, the SD library and
#           dial_dbg_q.
RAM_DATA_MAX ?= 1028
RAM_BSS_MAX ?= 3150

# Explicit build path so memcheck can find the ELF without guessing at the
# arduino-cli sketch cache hash.
BUILD_DIR ?= build
ARDUINO_DATA_DIRS := ${HOME}/.arduino15 ${HOME}/Library/Arduino15
AVR_SIZE ?= $(firstword $(wildcard $(foreach d,${ARDUINO_DATA_DIRS},\
	$(d)/packages/arduino/tools/avr-gcc/*/bin/avr-size)))


default: compile usb

compile:
	"${ARDUINO_CLI}" compile -b ${BOARD} --board-options ${BOARD_OPTS}

usb:
	"${ARDUINO_CLI}" upload -b ${BOARD} -p $(PORT) -vt \
		--board-options ${BOARD_OPTS}

pulse-debug-compile:
	"${ARDUINO_CLI}" compile -b ${PULSE_DEBUG_BOARD} \
		--board-options ${PULSE_DEBUG_BOARD_OPTS} \
		${PULSE_DEBUG_DIR}

# Example (macOS): make pulse-debug PORT=/dev/cu.usbmodem1101
pulse-debug: pulse-debug-compile
	"${ARDUINO_CLI}" upload -b ${PULSE_DEBUG_BOARD} -p $(PORT) -vt \
		--board-options ${PULSE_DEBUG_BOARD_OPTS} \
		${PULSE_DEBUG_DIR}

pulse-monitor-test:
	cd ${PULSE_MONITOR_DIR} && python3 -m unittest discover -p 'test_*.py' -v

memcheck-test:
	cd ${MEMCHECK_DIR} && python3 -m unittest discover -p 'test_*.py' -v

# All host-side tests.
test: pulse-monitor-test memcheck-test

# Compile to a known path and fail if the firmware exceeds the RAM budget.
memcheck:
	"${ARDUINO_CLI}" compile -b ${BOARD} --board-options ${BOARD_OPTS} \
		--build-path ${BUILD_DIR}
	python3 ${MEMCHECK_DIR}/memcheck.py ${BUILD_DIR}/rusp_firmware.ino.elf \
		--avr-size "${AVR_SIZE}" \
		--data-max ${RAM_DATA_MAX} --bss-max ${RAM_BSS_MAX}

# Requires: python3 -m venv tools/pulse_monitor/.venv && \
#   tools/pulse_monitor/.venv/bin/pip install -r tools/pulse_monitor/requirements.txt
# App Serial for the debug sketch matches production (115200). Makefile BAUD
# is the urboot autobaud upload rate (default 1000000).
PULSE_MONITOR_BAUD ?= 115200

pulse-monitor:
	cd ${PULSE_MONITOR_DIR} && \
		if [ -x .venv/bin/python ]; then \
			.venv/bin/python pulse_monitor.py --port $(PORT) --baud ${PULSE_MONITOR_BAUD}; \
		else \
			python3 pulse_monitor.py --port $(PORT) --baud ${PULSE_MONITOR_BAUD}; \
		fi

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

