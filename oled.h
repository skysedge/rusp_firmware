#ifndef RUSP_OLED_H
#define RUSP_OLED_H

#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>
#include <SPI.h>

// Adafruit GFX Library
#include <gfxfont.h>

#include "pins.h"

// These bitmaps must be within the project directory.
// The files in the Adafruit-GFX library will not work.
//#include "./Fonts/FreeMono12pt7b.h"

// Define the font PROGMEM glyphs array.
//#define GLYPHS FreeMono12pt7bGlyphs
// Define the font PROGMEM bitmap array.
//#define BITMAPS FreeMono12pt7bBitmaps
//#include "Fonts/B612Mono_Regular8pt7b.h"
//#define BITMAPS B612Mono_Regular8pt7bBitmaps
//#define GLYPHS B612Mono_Regular8pt7bGlyphs
//#include "Fonts/FreeMono9pt7b.h"
//#define BITMAPS FreeMono9pt7bBitmaps
//#define GLYPHS FreeMono9pt7bGlyphs
//#include "Fonts/B612Mono_Regular8pt7b.h"
//#define BITMAPS B612Mono_Regular8pt7bBitmaps
//#define GLYPHS B612Mono_Regular8pt7bGlyphs
#include "Fonts/IosevkaFixed_Regular12pt7b.h"
#include "Fonts/IosevkaFixed_Regular8pt7b.h"
#define BITMAPS IosevkaFixed_Regular12pt7bBitmaps
#define GLYPHS IosevkaFixed_Regular12pt7bGlyphs
/* Call-phase status between icons (smaller than body UI). */
#define OLED_STATUS_GLYPHS IosevkaFixed_Regular8pt7bGlyphs
#define OLED_STATUS_BITMAPS IosevkaFixed_Regular8pt7bBitmaps
/* Phone numbers: narrow Iosevka 12pt so (234) 567-8901 fits one line. */
#define OLED_NUMBER_GLYPHS IosevkaFixed_Regular12pt7bGlyphs
#define OLED_NUMBER_BITMAPS IosevkaFixed_Regular12pt7bBitmaps

// Define non-data SPI pins.
#define CLR_CS     (PORTA &= ~bit(0))  // Set digital 22 (PA0) LOW
#define SET_CS     (PORTA |=  bit(0))  // Set digital 22 (PA0) HIGH
#define CLR_RESET  (PORTC &= ~bit(0))  // Set digital 37 (PC0) LOW
#define SET_RESET  (PORTC |=  bit(0))  // Set digital 37 (PC0) HIGH
#define CLR_DC     (PORTC &= ~bit(1))  // Set digital 36 (PC1) LOW
#define SET_DC     (PORTC |=  bit(1))  // Set digital 36 (PC1) HIGH

// Define relevant OLED display properties.
#define H_RES 256  // Horizontal resolution in pixels
// SSD1362 GDDRAM: one column address holds two 4-bit pixels, so drawing
// coordinates (curs_x / string width) use this column count, not H_RES.
#define H_COLS (H_RES / 2)
#define V_RES 64  // Vertical resolution
#define MAX_BRIGHT 255  // Maximum pixel brightness
#define MIN_BRIGHT 0  // Minimum pixel brightness

// Let these be public function declarations.
void oled_enable();
void oled_disable();
/* SSD1362 display OFF (0xAE) / ON (0xAF). Keeps EN_12V powered. */
void oled_display_off(void);
void oled_display_on(void);
void oled_init();
/*
 * After EPD/SD use the shared SPI bus: restore 8 MHz Mode0 transaction and
 * wake the panel. Without this, post-splash UI draws fail (blank OLED).
 */
void oled_reclaim_spi(void);
void oled_clear();
void oled_draw_str(char* str, uint16_t curs_x, uint16_t curs_y);
void oled_draw_char(char c, uint16_t curs_x, uint16_t curs_y);
void oled_erase_str(char* str, uint16_t curs_x, uint16_t curs_y);
void oled_erase_char(char c, uint16_t curs_x, uint16_t curs_y);
void oled_print(char* str, uint16_t curs_x, uint16_t curs_y);
/* Center-justified dialed-number display: one centered row if it fits, else
 * wrap onto a second centered row. */
void oled_print_number(char* str);
/* Center-justified status text (startup / progress). line2 may be NULL. */
void oled_print_status(const char* line1, const char* line2);
/*
 * Post-boot home / call UI:
 *   top — signal bars (left), battery icon (right); optional status centered
 *   bottom — phone number, or "Ready to dial" when idle
 * signal_bars is 0..4. number may be NULL/empty.
 */
/*
 * Repaint one band of the post-boot UI. Callers that know only the meters or
 * only the number changed should use these instead of oled_show_ui, which
 * repaints the whole panel and blocks the CPU long enough to overrun the
 * modem UART receive buffer.
 */
void oled_ui_draw_top(
	const char* status, int batt_pct, bool charging, int signal_bars
);
void oled_ui_draw_bottom(const char* status, const char* number);

void oled_show_ui(
	const char* status, const char* number,
	int batt_pct, bool charging, int signal_bars
);
void oled_scroll(char* str, uint16_t curs_x, uint16_t curs_y, uint16_t init_delay, uint16_t scroll_delay);

#endif

