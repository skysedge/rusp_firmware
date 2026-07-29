#ifndef RUSP_SD_H
#define RUSP_SD_H_

#include <HardwareSerial.h>
#include <SD.h>

#define STR_EXPAND(tok) #tok
#define STR(tok) STR_EXPAND(tok)

// https://en.wikipedia.org/wiki/X_macro
#define CONFIG_FILES \
	X(PREPEND) \
	X(CONTACT1) \
	X(CONTACT2) \
	X(CONTACT3) \
	X(CONTACT4) \
	X(CONTACT5) \
	X(CONTACT6) \
	X(CONTACT7) \
	X(CONTACT8) \
	X(CONTACT9) \
	X(CONTACT0)

#define MAX_CONF_STR_LEN 64

// state of the lara system
struct sd_state {
	// serial port the console is on
	HardwareSerial *cons;
	// SD library stuff
	Sd2Card card;
	SdVolume volume;
	SdFile root;
	// config strings
	#define X(name) char name[MAX_CONF_STR_LEN];
	CONFIG_FILES
	#undef X
};

void sd_init(HardwareSerial *cons);
void sd_info();
void sd_read_all();
/* True if the card initialized successfully at boot. */
bool sd_is_ready();
/*
 * After EPD (or other SPI masters) finish: deselect CS lines and re-begin
 * the SD library so sd cat / append work again.
 */
void sd_recover_spi(void);
/* Append one line to DIAL.LOG (creates the file if needed). */
bool sd_log_append(const char *line);
/* Append one line to PINS.LOG (USB/charge pin-scan capture). */
bool sd_pins_log_append(const char *line);
/*
 * Handle a console line that starts with "sd".
 * Commands: sd help | sd ls | sd cat <FILE>
 * Returns true if the line was an SD command (caller should not forward to modem).
 */
bool sd_handle_serial_command(HardwareSerial *cons, const char *line);

#define X(name) char *sd_##name();
CONFIG_FILES
#undef X

#endif	// include guard

