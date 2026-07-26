#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#include "pins.h"
#include "sd.h"

static struct sd_state sd;
static bool sd_ready = false;


void sd_init(HardwareSerial *cons)
{
	sd.cons = cons;
	sd_ready = false;

	if (sd.card.init(SPI_HALF_SPEED, CHIPSELECT)) {
		sd.cons->println("SD card found");
	} else {
		sd.cons->println("no SD card");
		return;
	}

	// is this eating a lot of power?
	if (!SD.begin(CHIPSELECT)) {
		sd.cons->println("SD.begin failed");
		return;
	}
	sd_ready = true;
	sd_read_all();
}


bool sd_is_ready()
{
	return sd_ready;
}


void sd_recover_spi(void)
{
	/*
	 * Deselect every SPI CS after EPD. Do not call SD.begin() here — that
	 * reconfigures the bus to card speed and leaves the OLED unable to draw.
	 */
	SPI.endTransaction();
	pinMode(CHIPSELECT, OUTPUT);
	digitalWrite(CHIPSELECT, HIGH);
	pinMode(EPD_CS, OUTPUT);
	digitalWrite(EPD_CS, HIGH);
	if (sd.cons)
		sd.cons->println(F("SD CS idle (bus free for OLED)"));
}


bool sd_log_append(const char *line)
{
	if (!sd_ready || line == nullptr)
		return false;
	File f = SD.open("DIAL.LOG", FILE_WRITE);
	if (!f)
		return false;
	f.println(line);
	f.close();
	return true;
}


bool sd_pins_log_append(const char *line)
{
	if (!sd_ready || line == nullptr)
		return false;
	File f = SD.open("PINS.LOG", FILE_WRITE);
	if (!f)
		return false;
	f.println(line);
	f.close();
	return true;
}


static void sd_cmd_help(HardwareSerial *cons)
{
	cons->println(F("SD commands (115200 8N1):"));
	cons->println(F("  sd help"));
	cons->println(F("  sd ls"));
	cons->println(F("  sd cat <FILE>     e.g. sd cat DIAL.LOG / PINS.LOG"));
}


static void sd_cmd_ls(HardwareSerial *cons)
{
	if (!sd_ready) {
		cons->println(F("SD not ready"));
		return;
	}
	File root = SD.open("/");
	if (!root) {
		cons->println(F("SD: cannot open /"));
		return;
	}
	cons->println(F("--- SD root ---"));
	while (true) {
		File entry = root.openNextFile();
		if (!entry)
			break;
		cons->print(entry.name());
		cons->print(F("\t"));
		if (entry.isDirectory()) {
			cons->println(F("<DIR>"));
		} else {
			cons->println(entry.size());
		}
		entry.close();
	}
	root.close();
	cons->println(F("--- end ---"));
}


static void sd_cmd_cat(HardwareSerial *cons, const char *path)
{
	if (!sd_ready) {
		cons->println(F("SD not ready"));
		return;
	}
	if (path == nullptr || path[0] == '\0') {
		cons->println(F("usage: sd cat <FILE>"));
		return;
	}
	File f = SD.open(path, FILE_READ);
	if (!f) {
		cons->print(F("SD: cannot open "));
		cons->println(path);
		return;
	}
	cons->print(F("--- "));
	cons->print(path);
	cons->println(F(" ---"));
	while (f.available()) {
		cons->write((uint8_t)f.read());
	}
	f.close();
	cons->println();
	cons->println(F("--- end ---"));
}


bool sd_handle_serial_command(HardwareSerial *cons, const char *line)
{
	if (cons == nullptr || line == nullptr)
		return false;
	/* Accept "sd", "sd ...", case-sensitive prefix. */
	if (!(line[0] == 's' && line[1] == 'd'
	      && (line[2] == '\0' || line[2] == ' '))) {
		return false;
	}

	const char *args = line + 2;
	while (*args == ' ')
		args++;

	if (*args == '\0' || strcmp(args, "help") == 0) {
		sd_cmd_help(cons);
		return true;
	}
	if (strcmp(args, "ls") == 0) {
		sd_cmd_ls(cons);
		return true;
	}
	if (strncmp(args, "cat ", 4) == 0) {
		const char *path = args + 4;
		while (*path == ' ')
			path++;
		sd_cmd_cat(cons, path);
		return true;
	}
	if (strcmp(args, "cat") == 0) {
		cons->println(F("usage: sd cat <FILE>"));
		return true;
	}

	cons->print(F("SD: unknown command: "));
	cons->println(args);
	sd_cmd_help(cons);
	return true;
}


void sd_read_all(){
	// open all files, ignoring file not found errors
	#define X(name) File file_##name = SD.open(STR(name), FILE_READ);
	CONFIG_FILES
	#undef X

	// load all file contents into sd.filename
	unsigned i;
	#define X(name) i = 0; \
	while (file_##name.available()) { \
		char c = file_##name.read(); \
		if (c == '\n') c = 0; \
		if (c == '\r') c = 0; \
		sd.name[i] = c; \
		i += 1; \
	}
	CONFIG_FILES
	#undef X
}


#define X(name) \
char *sd_##name() { \
	return sd.name; \
}
CONFIG_FILES
#undef X
