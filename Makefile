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
# Raised deliberately from 1028 / 3150 to take the 256-byte modem receive
# buffer (+384 in .bss across the two live serials) and the call-waiting
# detection (+110 .data, +88 .bss). Buying back lost URCs with RAM is the
# whole point of having freed it; both figures are still far below the 2982 /
# 4027 this started at.
#
# .bss raised from 3622 to 3714 (+92) for the asynchronous AT engine in
# lara.cpp: a 64-byte line assembly buffer that has to survive between
# service calls, plus its state and the +CLCC row accumulator. The blocking
# reader held the equivalent on the stack for the duration of one call, so
# this converts transient stack into permanent .bss rather than adding a new
# peak. It comes out of the free pool, which is 3346 bytes after the change.
#
# .data ratcheted 1138 -> 1132 with the blocking +CSQ and +CLCC pollers, which
# the async engine replaced and left with no callers.
RAM_DATA_MAX ?= 1132
RAM_BSS_MAX ?= 3714

# Explicit build path so memcheck can find the ELF without guessing at the
# arduino-cli sketch cache hash.
# Modem UART receive buffer.
#
# The core default is 64 bytes, about 5.5 ms of data at 115200 baud, while the
# main loop still blocks for roughly 155 ms on ui_refresh() issuing AT+CSQ.
# Any URC arriving in that window is lost once the buffer fills, which was
# observed as URCs truncated to a bare "+U" or "1,0" — and a lost
# +UCALLSTAT: 1,6 leaves a finished call marked active forever, because
# unsolicited output is never retransmitted.
#
# 256 bytes covers the measured worst-case loop latency with margin. This is
# what the RAM freed by the PROGMEM work bought.
SERIAL_RX_BUFFER_SIZE ?= 256
EXTRA_CPP_FLAGS ?= -DSERIAL_RX_BUFFER_SIZE=${SERIAL_RX_BUFFER_SIZE}

BUILD_DIR ?= build
ARDUINO_DATA_DIRS := ${HOME}/.arduino15 ${HOME}/Library/Arduino15
AVR_SIZE ?= $(firstword $(wildcard $(foreach d,${ARDUINO_DATA_DIRS},\
	$(d)/packages/arduino/tools/avr-gcc/*/bin/avr-size)))


default: compile usb

# --build-path is not optional here. Without it arduino-cli builds into its
# own cache while memcheck builds into BUILD_DIR, so the two disagree and
# `usb` flashes whichever binary the cache happens to hold. That silently put
# a hours-old image on the board while every local check passed.
compile:
	"${ARDUINO_CLI}" compile -b ${BOARD} --board-options ${BOARD_OPTS} \
		--build-property compiler.cpp.extra_flags="${EXTRA_CPP_FLAGS}" \
		--build-path ${BUILD_DIR}

# Depends on compile, and flashes that exact output. Uploading without
# --input-dir takes whatever is in arduino-cli's cache, which is not
# necessarily what was just built.
usb: compile
	"${ARDUINO_CLI}" upload -b ${BOARD} -p $(PORT) -vt \
		--board-options ${BOARD_OPTS} --input-dir ${BUILD_DIR}

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
		--build-property compiler.cpp.extra_flags="${EXTRA_CPP_FLAGS}" \
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

# Raw console capture for hardware debugging, as opposed to pulse-monitor,
# which parses dial pulses. Runs until interrupted; pass SECONDS=n to bound it.
#
# Redirect to a file and leave it in the foreground. Backgrounding this with
# nohup from a short-lived shell gets the process torn down when that shell
# exits, which produces a log that stops seconds after boot and looks
# indistinguishable from an idle device.
# CAPTURE_PORT is deliberately not PORT: PORT defaults to a Linux device node,
# and pinning capture to a device that does not exist on this host makes it
# wait for a board that will never appear. Left empty, console_capture finds
# whichever port is present on either platform.
CAPTURE_PORT ?=
CAPTURE_ARGS ?= $(if $(CAPTURE_PORT),--port $(CAPTURE_PORT),) \
		$(if $(SECONDS),--seconds $(SECONDS),)

capture:
	cd ${PULSE_MONITOR_DIR} && \
		if [ -x .venv/bin/python ]; then \
			.venv/bin/python console_capture.py ${CAPTURE_ARGS}; \
		else \
			python3 console_capture.py ${CAPTURE_ARGS}; \
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

