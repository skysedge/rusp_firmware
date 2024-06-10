#include <Arduino.h>

#include "pins.h"
#include "sd.h"

static struct sd_state sd;


void sd_init(HardwareSerial *cons)
{
	sd.cons = cons;

	if (sd.card.init(SPI_HALF_SPEED, CHIPSELECT)) {
		sd.cons->println("SD card found");
	} else {
		sd.cons->println("no SD card");
	}

	// is this eating a lot of power?
	SD.begin(CHIPSELECT);
	sd_read_all();
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

