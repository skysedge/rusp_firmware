#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>
#include <SPI.h>

// Adafruit GFX Library
#include "gfxfont.h"

// These bitmaps must be within the project directory.
// The files in the Adafruit-GFX library will not work.
#include "./Fonts/FreeSans12pt7b.h"

// Define the font PROGMEM glyphs array.
#define GLYPHS FreeSans12pt7bGlyphs
// Define the font PROGMEM bitmap array.
#define BITMAPS FreeSans12pt7bBitmaps

// Define non-data SPI pins.
#define CLR_CS     (PORTA &= ~bit(0))  // Set digital 22 (PA0) LOW
#define SET_CS     (PORTA |=  bit(0))  // Set digital 22 (PA0) HIGH
#define CLR_RESET  (PORTC &= ~bit(0))  // Set digital 37 (PC0) LOW
#define SET_RESET  (PORTC |=  bit(0))  // Set digital 37 (PC0) HIGH
#define CLR_DC     (PORTC &= ~bit(1))  // Set digital 36 (PC1) LOW
#define SET_DC     (PORTC |=  bit(1))  // Set digital 36 (PC1) HIGH

// Define the 12 V enable pin.
#define EN_12V 33

// Define relevant OLED display properties.
#define H_RES 128  // Horizontal resolution
#define V_RES 64  // Vertical resolution
#define MAX_BRIGHT 255  // Maximum pixel brightness
#define MIN_BRIGHT 0  // Minimum pixel brightness

// Let these be public function declarations.
void oled_enable();
void oled_disable();
void oled_init();
void oled_clear();
void oled_draw_str(char* str, uint16_t curs_x, uint16_t curs_y);
void oled_draw_char(char c, uint16_t curs_x, uint16_t curs_y);
void oled_erase_str(char* str, uint16_t curs_x, uint16_t curs_y);
void oled_erase_char(char c, uint16_t curs_x, uint16_t curs_y);
void oled_print(char* str, uint16_t curs_x, uint16_t curs_y);
void oled_scroll(char* str, uint16_t curs_x, uint16_t curs_y, uint16_t init_delay, uint16_t scroll_delay);

#endif