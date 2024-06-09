/*
 * Manage control of Crystalfontz OLED P/N CFAL25664C0-021M-[W or Y]
 * Uses the Adafruit GFX Font Library directly. An explanation of the font file syntax is here: 
 * https://learn.adafruit.com/creating-custom-symbol-font-for-adafruit-gfx-library/understanding-the-font-specification
 * First/main section of font file contains a large binary bitmap, where the bytes are expressed in hex
 * The latter section of the file is essentially a ciper which describes the location of each glph in the bitmap
 * 	1: index (starting point) in bitmap array
 * 	2: width of glyph
 * 	3: height of glyph 
 * 	4: when drawing glyph, pixels to next character. A monospaced font would have this be always the same.
 * 	5: dx, horizontal centering w.r.t. baseline
 * 	6: dY, vertical centering w.r.t. baseline
 *
 * Justine Haupt
 */


/* Migrate to C functions, refactor, prune, and add documentation.
 *
 * CFAL25664C0-021Mx Demonstration Code
 * https://github.com/crystalfontz/CFAL25664C0-021Mx/tree/main
 *
 * Solomon Systech SSD1362: 256 x 64, 16 Gray Scale Dot Matrix High Power OLED/PLED Segment/Common Driver with Controller
 * https://www.crystalfontz.com/controllers/Solomon%20Systech/SSD1362/490
 *
 * Samuel Woronick
 */


#include "oled.h"


// ---- DEFINE PUBLIC FUNCTIONS ----


// Enable the OLED display. 
void oled_enable() {
  digitalWrite(EN_12V, HIGH);
}


// Disable the OLED display. 
void oled_disable() {
  digitalWrite(EN_12V, LOW);
}


// ---- DEFINE PRIVATE FUNCTIONS ----


/* Extract the bit at the given index.
 *
 * @param byte The byte from which to extract the bit
 * @param bit_idx The index of the bit in the byte
 */
static bool get_bit(uint8_t byte, uint8_t bit_idx) {
  return (byte >> bit_idx) & 1;
}


/* 7.1.3 MCU Serial Interface (4-wire SPI)
 * 
 * "The [4-wire] serial interface consists of serial clock SCLK, serial data SDIN, D/C#, CS#. In SPI mode, D0 acts as
 * SCLK, D1 acts as SDIN. For the unused data pins from D2 to D7, E and R/W# can be connected to an external ground.
 * 
 * SDIN is shifted into an 8-bit shift register on every rising edge of SCLK in the order of D7, D6, ... D0. D/C#
 * is sampled on every eighth clock and the data byte in the shift register is written to the Graphic Display Data
 * RAM (GDDRAM) or command register in the same clock.
 *
 * Under serial mode, only write operations are allowed."
 *
 * 8.1 Data Read / Write
 *
 * "The serial interface mode is always in write mode. The (Graphics DDR SDRAM) GDDRAM column address pointer 
 * will be increased automatically by one after each data write."
 *
 * @param data Data to write to the LCD command register
 */
static void write_cmd(uint8_t cmd) {
    CLR_CS;  // Select the LCD controller.
    CLR_DC;  // Select the LCD command register.
    SPI.transfer(cmd);  // Send the command via the SPI.
    SET_CS;  // Deselect the LCD controller.
}


/* 7.1.3 MCU Serial Interface (4-wire SPI)
 * 
 * "The [4-wire] serial interface consists of serial clock SCLK, serial data SDIN, D/C#, CS#. In SPI mode, D0 acts as
 * SCLK, D1 acts as SDIN. For the unused data pins from D2 to D7, E and R/W# can be connected to an external ground.
 * 
 * SDIN is shifted into an 8-bit shift register on every rising edge of SCLK in the order of D7, D6, ... D0. D/C#
 * is sampled on every eighth clock and the data byte in the shift register is written to the Graphic Display Data
 * RAM (GDDRAM) or command register in the same clock.
 *
 * Under serial mode, only write operations are allowed."
 *
 * 8.1 Data Read / Write
 *
 * "The serial interface mode is always in write mode. The GDDRAM column address pointer will be increased automatically 
 * by one after each data write."
 *
 * @param data Data to write to the LCD data register
 */
static void write_data(uint8_t data) {
    SET_DC;  // Select the LCD data register.
    CLR_CS;  // Select the LCD controller.
    SPI.transfer(data);  // Send the data via the SPI.
    SET_CS;  // Deselect the LCD controller.
}


/*
 * 9.1.1 Set Column Address (15h)
 *
 * "This triple byte command specifies column start address and end address of the display data RAM. This
 * command also sets the column address pointer to column start address. This pointer is used to define the
 * current read/write column address in graphic display data RAM. If horizontal address increment mode is
 * enabled by command A0h, after finishing read/write one column data, it is incremented automatically to the
 * next column address. Whenever the column address pointer finishes accessing the end column address, it is
 * reset back to start column address and the row address is incremented to the next row."
 * 
 * The interval is closed, so the end address is included.
 *
 * @param start_addr Column start address
 * @param end_addr Column end address, inclusive
 */
static void set_col_addr(uint8_t start_addr, uint8_t end_addr) {
    write_cmd(0x15);  // 9.1.1 Set Column Address (15h)
    write_cmd(start_addr);
    write_cmd(end_addr);
}


/* 9.1.2 Set Row Address (75h) 
 * 
 * "This triple byte command specifies column start address and end address of the display data RAM. This
 * command also sets the column address pointer to column start address. This pointer is used to define the
 * current read/write column address in graphic display data RAM. If horizontal address increment mode is
 * enabled by command A0h, after finishing read/write one column data, it is incremented automatically to the
 * next column address. Whenever the column address pointer finishes accessing the end column address, it is
 * reset back to start column address and the row address is incremented to the next row."
 * 
 * The interval is closed, so the end address is included.
 *
 * @param start_addr Row start address
 * @param end_addr Row end address, inclusive
 */
static void set_row_addr(uint8_t start_addr, uint8_t end_addr) {
    write_cmd(0x75);  // 9.1.2 Set Row Address (75h) 
    write_cmd(start_addr);
    write_cmd(end_addr);
}


// ---- DEFINE PUBLIC FUNCTIONS ----


// Clear the OLED display.
void oled_clear() {
  // Set the address range to the entire display.
  set_col_addr(0, H_RES - 1);
  set_row_addr(0, V_RES - 1);
    
  // Clear the display by writing the null byte to all pixels.
  for (uint32_t pixel_idx = 0; pixel_idx < H_RES * V_RES; pixel_idx++) {
    write_data(MIN_BRIGHT);
  }
}

/* TO DO
 *
 */
void oled_init() {
    pinMode(EN_12V, OUTPUT);  // Configure 12 V enable pin
    DDRA |= bit(0);	 // Set port A0 as output
    DDRC |= bit(0);	 // Set port C0 as output
    DDRC |= bit(1);	 // Set port C1 as output
    
    delay(100);
    SET_CS;
    SPI.begin();
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));

    CLR_RESET;
    delay(100);
    SET_RESET;

    oled_enable();

    write_cmd(0Xfd); //  Set command lock
    write_cmd(0X12); //  (12H=Unlock,16H=Lock)
    write_cmd(0XAE); //  Display OFF (sleep mode)

    set_col_addr(0x00, 0x7f);  // [0, 127]
    set_row_addr(0x00, 0x3f);  // [0, 31]

    write_cmd(0X81);  // Set contrast
    write_cmd(0x2f);

    write_cmd(0Xa0);  // Set remap
    write_cmd(0Xc3);

    write_cmd(0Xa1);  // Set display start line
    write_cmd(0X00);

    write_cmd(0Xa2);  // Set display offset
    write_cmd(0X00);

    write_cmd(0Xa4);  //Normal Display

    write_cmd(0Xa8);  //Set Multiplex Ratio
    write_cmd(0X3f);

    write_cmd(0Xab);  //Set VDD regulator
    write_cmd(0X01);  //Regulator Enable

    write_cmd(0Xad);  //External /Internal IREF Selection
    write_cmd(0X8E);

    write_cmd(0Xb1);  //Set Phase Length
    write_cmd(0X22);

    write_cmd(0Xb3);  //Display clock Divider
    write_cmd(0Xa0);

    write_cmd(0Xb6);  //Set Second precharge Period
    write_cmd(0X04);

    write_cmd(0Xb9);  //Set Linear LUT

    write_cmd(0Xbc);  //Set pre-charge voltage level
    write_cmd(0X10);  //0.5*Vcc

    write_cmd(0Xbd);  // Pre-Charge voltage capacitor Selection
    write_cmd(0X01);

    write_cmd(0Xbe);  // Set COM Deselect Voltage Level
    write_cmd(0X07);  // 0.82 * Vcc

    oled_clear();		 // Clear Screen
    write_cmd(0Xaf); // Display ON
}

/* Draw the character on the OLED display at the given offset.
 *
 * @param c The character to draw
 * @param curs_x The horizontal offset from the top-left corner at which to start drawing
 * @param curs_y The vertical offset from the top-left corner at which to start drawing
 */
void oled_draw_char(char c, uint16_t curs_x, uint16_t curs_y) {
  // Get glyph's width, height, and start index (in the bitmap) from flash memory.
  uint32_t start_idx = pgm_read_word(&FreeSans12pt7bGlyphs[c - 32].bitmapOffset); 
  uint16_t g_width = pgm_read_word(&FreeSans12pt7bGlyphs[c - 32].width);
  uint16_t g_height = pgm_read_word(&FreeSans12pt7bGlyphs[c - 32].height);

  // Set the cell area for the glyph.
  set_col_addr(curs_x, curs_x + g_width - 1);
  set_row_addr(curs_y, curs_y + g_height - 1);

  // Calculate the number of bytes in the bitmap.
  uint32_t num_bytes = (g_width * g_height) / 8;

  // Let the byte index be the the index of the current byte in the glyph bitmap.
  for (uint32_t byte_idx = start_idx; byte_idx < start_idx + num_bytes; byte_idx++) {
    // Retrieve the next byte from the bitmap.
    uint8_t byte = pgm_read_byte(&FreeSans12pt7bBitmaps[byte_idx]);

    // Iterate backwards through the bit indexes.
    for (int8_t bit_idx = 7; bit_idx >= 0; bit_idx--) {
      // Extract the bit at the given index.
      bool is_pixel_set = get_bit(byte, bit_idx);
      // Set the brightness of the pixel according to whether it is set in the bitmap.
      write_data(is_pixel_set ? MAX_BRIGHT : MIN_BRIGHT);
    }
  }
}

/* Draw the string on the OLED display at the given offset.
 *
 * The function will truncate the string if it exceeds the horizontal resolution.
 *
 * @param str The string to draw
 * @param curs_x The horizontal offset from the top-left corner at which to start drawing
 * @param curs_y The vertical offset from the top-left corner at which to start drawing
 */
void oled_draw_str(char* str, uint16_t curs_x, uint16_t curs_y) {
  // Iterate the characters in the string to draw.
  for (int char_idx = 0; char_idx < strlen(str); char_idx++) {
    // Get glyph's width from flash memory.
    int g_width = pgm_read_word(&FreeSans12pt7bGlyphs[str[char_idx] - 32].width);
    
    // Check whether the glyph will exceed the horizontal resolution of the display.
    if(curs_x + g_width > H_RES)
      break;

    // Draw the glyph on the display.
    oled_draw_char(str[char_idx], curs_x, curs_y);
    
    // Advance the X cursor according to the width of the space defined in the font.
    // This value considers both the width of the given glyph and the space between glyphs.
    curs_x = curs_x + pgm_read_byte(&FreeSans12pt7bGlyphs[str[char_idx] - 32].xAdvance);
  }
}


/* Erase the character on the OLED display at the given offset.
 *
 * @param c The character to erase
 * @param curs_x The horizontal offset from the top-left corner at which to start erasing
 * @param curs_y The vertical offset from the top-left corner at which to start erasing
 */
void oled_erase_char(char c, uint16_t curs_x, uint16_t curs_y) {
  // Get the glyph's width and height from flash memory.
  uint16_t g_width = pgm_read_word(&FreeSans12pt7bGlyphs[c - 32].width);
  uint16_t g_height = pgm_read_word(&FreeSans12pt7bGlyphs[c - 32].height);

  // Set the cell area for the glyph.
  set_col_addr(curs_x, curs_x + g_width - 1);
  set_row_addr(curs_y, curs_y + g_height - 1);

  // Calculate the number of pixels in the cell area.
  uint32_t num_pixels = g_width * g_height;

  // Let the byte index be the the index of the current byte in the glyph bitmap.
  for (uint32_t pixel_idx = 0; pixel_idx < num_pixels; pixel_idx++) {
    // Turn off the pixel.
    write_data(MIN_BRIGHT);
  }
}


/* Erase the string on the OLED display at the given offset.
 *
 * @param str The string to erase
 * @param curs_x The horizontal offset from the top-left corner at which to start erasing
 * @param curs_y The vertical offset from the top-left corner at which to start erasing
 */
void oled_erase_str(char* str, uint16_t curs_x, uint16_t curs_y) {
  // Iterate the characters in the string to erase.
  for (int char_idx = 0; char_idx < strlen(str); char_idx++) {
    // Get glyph's width from flash memory.
    int g_width = pgm_read_word(&FreeSans12pt7bGlyphs[str[char_idx] - 32].width);
    
    // Check whether the glyph will exceed the horizontal resolution of the display.
    if(curs_x + g_width > H_RES)
      break;

    // Erase the glyph from the display.
    oled_erase_char(str[char_idx], curs_x, curs_y);
    
    // Advance the X cursor according to the width of the space defined in the font.
    // This value considers both the width of the given glyph and the space between glyphs.
    curs_x = curs_x + pgm_read_byte(&FreeSans12pt7bGlyphs[str[char_idx] - 32].xAdvance);
  }
}


/* Print the string to the OLED display. 
 *
 * The function enables and clears the display before printing the string.
 * The function will truncate the string if it exceeds the horizontal resolution.
 *
 * @param str The string to print
 * @param curs_x The horizontal offset from the top-left corner at which to print
 * @param curs_y The vertical offset from the top-left corner at which to print 
 */
void oled_print(char* str, int curs_x, int curs_y) {
  oled_enable();
  oled_clear();

  // Draw the string on the display.
  oled_draw_str(str, curs_x, curs_y);
}


/* Print the string at the given offset, and then scroll to the left.
 *
 * The string may exceed the horizontal resolution because it will scroll into view. 
 *
 * @param str The string to draw
 * @param curs_x The horizontal offset from the top-left corner at which to start drawing
 * @param curs_y The vertical offset from the top-left corner at which to start drawing
 * @param init_delay The initial delay to wait before scrolling starts
 * @param scroll_delay The delay to wait between scrolling updates
 */
void oled_scroll(char* str, uint16_t curs_x, uint16_t curs_y, uint16_t init_delay, uint16_t scroll_delay) {
  oled_enable();
  oled_clear();

  // Draw the string, wait for the initial delay, and then erase the string. 
  oled_draw_str(str, curs_x, curs_y);
  delay(init_delay);
  oled_erase_str(str, curs_x, curs_y);

  // Iterate the characters in the string to scroll across the screen.
  for (uint16_t char_idx = 1; char_idx < strlen(str); char_idx++) {
    // Draw the string, wait for the scroll delay, and then erase the string. 
    oled_draw_str(str + char_idx, curs_x, curs_y);
    delay(scroll_delay);
    oled_erase_str(str + char_idx, curs_x, curs_y);
  }
}
