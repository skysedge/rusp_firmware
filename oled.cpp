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


/* CFAL25664C0-021Mx Demonstration Code
 * https://github.com/crystalfontz/CFAL25664C0-021Mx/tree/main
 *
 * Solomon Systech SSD1362: 256 x 64, 16 Gray Scale Dot Matrix High Power OLED/PLED Segment/Common Driver with Controller
 * https://www.crystalfontz.com/controllers/Solomon%20Systech/SSD1362/490
 *
 * Samuel Woronick
 */


#include "oled.h"

#include <stdio.h>
#include <string.h>


// ---- DEFINE PUBLIC FUNCTIONS ----


// Enable the OLED 12V rail (does not by itself turn pixels on).
void oled_enable() {
	digitalWrite(EN_12V, HIGH);
}


// Intentionally a no-op for EN_12V: that rail feeds other hardware.
// Use oled_display_off() for idle blanking instead.
void oled_disable() {
}


// ---- DEFINE PRIVATE FUNCTIONS ----

static void write_cmd(uint8_t cmd);


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
	CLR_CS;	// Select the LCD controller.
	CLR_DC;	// Select the LCD command register.
	SPI.transfer(cmd);	// Send the command via the SPI.
	SET_CS;	// Deselect the LCD controller.
}


void oled_display_off(void)
{
	write_cmd(0xAE); /* Display OFF (sleep mode) */
}


void oled_display_on(void)
{
	oled_enable();
	write_cmd(0xAF); /* Display ON */
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
	SET_DC;	// Select the LCD data register.
	CLR_CS;	// Select the LCD controller.
	SPI.transfer(data);	// Send the data via the SPI.
	SET_CS;	// Deselect the LCD controller.
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
	write_cmd(0x15);	// 9.1.1 Set Column Address (15h)
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
	write_cmd(0x75);	// 9.1.2 Set Row Address (75h)
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
void oled_reclaim_spi(void)
{
	/*
	 * EPD calls SPI.endTransaction(); SD.begin() then owns the bus at a
	 * low clock. Re-select OLED timings and ensure the panel is out of
	 * sleep so ui_refresh() can draw again.
	 */
	pinMode(EN_12V, OUTPUT);
	digitalWrite(EN_12V, HIGH);
	DDRA |= bit(0);
	DDRC |= bit(0);
	DDRC |= bit(1);
	SET_CS;
	pinMode(CHIPSELECT, OUTPUT);
	digitalWrite(CHIPSELECT, HIGH);
	pinMode(EPD_CS, OUTPUT);
	digitalWrite(EPD_CS, HIGH);
	SPI.endTransaction();
	SPI.begin();
	SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
	oled_display_on();
}


void oled_init() {
	pinMode(EN_12V, OUTPUT);	// Configure 12 V enable pin
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

	write_cmd(0Xfd); //	Set command lock
	write_cmd(0X12); //	(12H=Unlock,16H=Lock)
	write_cmd(0XAE); //	Display OFF (sleep mode)

	set_col_addr(0x00, H_RES - 1);	// [0, 127]
	set_row_addr(0x00, V_RES - 1);	// [0, 31]

	write_cmd(0X81);	// Set contrast
	write_cmd(0x2f);

	write_cmd(0Xa0);	// Set remap
	write_cmd(0Xc3);

	write_cmd(0Xa1);	// Set display start line
	write_cmd(0X00);

	write_cmd(0Xa2);	// Set display offset
	write_cmd(0X00);

	write_cmd(0Xa4);	//Normal Display

	write_cmd(0Xa8);	//Set Multiplex Ratio
	write_cmd(0X3f);

	write_cmd(0Xab);	//Set VDD regulator
	write_cmd(0X01);	//Regulator Enable

	write_cmd(0Xad);	//External /Internal IREF Selection
	write_cmd(0X8E);

	write_cmd(0Xb1);	//Set Phase Length
	write_cmd(0X22);

	write_cmd(0Xb3);	//Display clock Divider
	write_cmd(0Xa0);

	write_cmd(0Xb6);	//Set Second precharge Period
	write_cmd(0X04);

	write_cmd(0Xb9);	//Set Linear LUT

	write_cmd(0Xbc);	//Set pre-charge voltage level
	write_cmd(0X10);	//0.5*Vcc

	write_cmd(0Xbd);	// Pre-Charge voltage capacitor Selection
	write_cmd(0X01);

	write_cmd(0Xbe);	// Set COM Deselect Voltage Level
	write_cmd(0X07);	// 0.82 * Vcc

	oled_clear();		 // Clear Screen
	write_cmd(0Xaf); // Display ON
}


/* Draw the character on the OLED display at the given offset.
 *
 * @param c The character to draw
 * @param curs_x The horizontal offset from the top-left corner at which to start drawing
 * @param curs_y The vertical offset from the top-left corner at which to start drawing
 */
static void oled_draw_char_gfx(
	char c, uint16_t curs_x, uint16_t curs_y,
	const GFXglyph *glyphs, const uint8_t *bitmaps
)
{
	uint16_t start_idx = pgm_read_word(&glyphs[c - 32].bitmapOffset);
	uint8_t g_width = pgm_read_byte(&glyphs[c - 32].width);
	uint8_t g_height = pgm_read_byte(&glyphs[c - 32].height);

	set_col_addr(curs_x, curs_x + (g_width >> 1) + (g_width & 0x01) - 1);
	set_row_addr(curs_y, curs_y + g_height - 1);

	uint16_t g_pixel_idx = 0;
	uint16_t g_pixels = (uint16_t)(g_width * g_height);
	uint16_t next_skip_idx = g_pixel_idx + g_width;
	bool is_odd_width = (g_width & 0x01) == 1;
	uint8_t buf = 0x00;
	uint8_t buf_size = 0;
	uint8_t buf_capacity = 2;

	while (true) {
		uint8_t byte = pgm_read_byte(&bitmaps[start_idx++]);

		for (uint8_t bit_idx = 0; bit_idx < 8; bit_idx++) {
			buf = (uint8_t)(buf << 4);
			buf_size += 1;

			if (is_odd_width && g_pixel_idx == next_skip_idx) {
				bit_idx--;
				next_skip_idx = (uint16_t)(g_pixel_idx + g_width);
			} else {
				if (((byte << bit_idx) & 0x80) == 0x80)
					buf |= 0x0F;
				g_pixel_idx++;
			}

			if (buf_size == buf_capacity) {
				write_data(buf);
				buf = 0;
				buf_size = 0;
			}

			if (g_pixel_idx >= g_pixels) {
				if (is_odd_width && g_pixel_idx == next_skip_idx) {
					buf = (uint8_t)(buf << 4);
					buf_size += 1;
				}
				if (buf_size)
					write_data(buf);
				return;
			}
		}
	}
}

void oled_draw_char(char c, uint16_t curs_x, uint16_t curs_y) {
	oled_draw_char_gfx(c, curs_x, curs_y, GLYPHS, BITMAPS);
}


/* Cursor advance used by oled_draw_str (nibble-aligned GDDRAM addressing). */
static uint16_t oled_glyph_advance_gfx(char c, const GFXglyph *glyphs)
{
	uint8_t x_advance = pgm_read_byte(&glyphs[c - 32].xAdvance);
	return (uint16_t)((x_advance >> 1) + (x_advance & 0x01));
}

static uint16_t oled_glyph_advance(char c)
{
	return oled_glyph_advance_gfx(c, GLYPHS);
}


static uint16_t oled_str_pixel_width_gfx(const char* str, const GFXglyph *glyphs)
{
	uint16_t width = 0;
	for (size_t i = 0; str[i] != '\0'; i++) {
		width = (uint16_t)(width + oled_glyph_advance_gfx(str[i], glyphs));
	}
	return width;
}

static uint16_t oled_str_pixel_width(const char* str)
{
	return oled_str_pixel_width_gfx(str, GLYPHS);
}


static void oled_draw_str_gfx(
	const char* str, uint16_t curs_x, uint16_t curs_y,
	const GFXglyph *glyphs, const uint8_t *bitmaps
)
{
	for (int char_idx = 0; str[char_idx] != '\0'; char_idx++) {
		uint16_t advance = oled_glyph_advance_gfx(str[char_idx], glyphs);
		if (curs_x + advance > H_COLS)
			break;
		oled_draw_char_gfx(str[char_idx], curs_x, curs_y, glyphs, bitmaps);
		curs_x = (uint16_t)(curs_x + advance);
	}
}

/* Draw the string on the OLED display at the given offset.
 *
 * The function will truncate the string if it exceeds the horizontal resolution.
 *
 * @param str The string to draw
 * @param curs_x The horizontal offset from the upper-left corner at which to start drawing
 * @param curs_y The vertical offset from the upper-left corner at which to start drawing
 */
void oled_draw_str(char* str, uint16_t curs_x, uint16_t curs_y) {
	oled_draw_str_gfx(str, curs_x, curs_y, GLYPHS, BITMAPS);
}


/* Vertical positions for dialed-number layout (12pt font on 64px display). */
#define OLED_NUMBER_ROW1_Y 12
#define OLED_NUMBER_ROW2_Y 38
/* Post-boot chrome: icons along the top, message/number along the bottom. */
#define OLED_ICON_Y 4
/* 12pt number / status baseline under the icon row. */
#define OLED_BOTTOM_Y 40
#define OLED_BOTTOM_Y_SMALL 42

static size_t oled_fit_prefix_len(const char* str, uint16_t avail_px)
{
	size_t n = 0;
	uint16_t width = 0;
	while (str[n] != '\0') {
		uint16_t advance = oled_glyph_advance(str[n]);
		if ((uint16_t)(width + advance) > avail_px)
			break;
		width = (uint16_t)(width + advance);
		n++;
	}
	return n;
}


/**
 * Draw a string centered horizontally on one row.
 * Width and X position are in GDDRAM columns (H_COLS), matching oled_draw_str.
 */
static void oled_draw_str_centered(const char* str, uint16_t curs_y)
{
	if (str == nullptr || str[0] == '\0')
		return;
	uint16_t width = oled_str_pixel_width(str);
	uint16_t curs_x = 0;
	if (width < H_COLS)
		curs_x = (uint16_t)((H_COLS - width) / 2);
	oled_draw_str((char*)str, curs_x, curs_y);
}


/**
 * Draw a dialed number center-justified.
 *
 * Fits on row 1 when possible. If wider than the display, puts as much as
 * fits on row 1 and continues the remainder on row 2 (also centered).
 * If the remainder still overflows row 2, keeps the trailing part of it.
 */
static void oled_draw_number_centered_wrap(char* str)
{
	if (str == nullptr || str[0] == '\0')
		return;

	if (oled_str_pixel_width(str) <= H_COLS) {
		oled_draw_str_centered(str, OLED_NUMBER_ROW1_Y);
		return;
	}

	size_t split = oled_fit_prefix_len(str, H_COLS);
	if (split == 0)
		split = 1;

	char line1[64];
	if (split >= sizeof(line1))
		split = sizeof(line1) - 1;
	memcpy(line1, str, split);
	line1[split] = '\0';

	const char* rest = str + split;
	char line2[64];
	size_t rest_len = strlen(rest);
	size_t line2_start = 0;
	if (oled_str_pixel_width(rest) > H_COLS) {
		while (line2_start < rest_len
		       && oled_str_pixel_width(rest + line2_start) > H_COLS) {
			line2_start++;
		}
	}
	size_t line2_len = rest_len - line2_start;
	if (line2_len >= sizeof(line2))
		line2_len = sizeof(line2) - 1;
	memcpy(line2, rest + line2_start, line2_len);
	line2[line2_len] = '\0';

	oled_draw_str_centered(line1, OLED_NUMBER_ROW1_Y);
	oled_draw_str_centered(line2, OLED_NUMBER_ROW2_Y);
}


void oled_print_status(const char* line1, const char* line2)
{
	oled_enable();
	oled_clear();
	if (line2 == nullptr || line2[0] == '\0') {
		oled_draw_str_centered(line1, OLED_NUMBER_ROW1_Y);
	} else {
		oled_draw_str_centered(line1, OLED_NUMBER_ROW1_Y);
		oled_draw_str_centered(line2, OLED_NUMBER_ROW2_Y);
	}
}


/*
 * Fill a GDDRAM column/row window. Each column address is two horizontal
 * pixels; bright_nibble is 0x0..0xF written to both nibbles of each byte.
 */
static void oled_fill_cols(
	uint8_t col0, uint8_t col1, uint8_t row0, uint8_t row1, uint8_t bright_nibble
)
{
	if (col1 < col0 || row1 < row0)
		return;
	set_col_addr(col0, col1);
	set_row_addr(row0, row1);
	uint8_t byte = (uint8_t)((bright_nibble << 4) | bright_nibble);
	uint16_t n = (uint16_t)(col1 - col0 + 1) * (uint16_t)(row1 - row0 + 1);
	while (n--)
		write_data(byte);
}

/**
 * Draw a compact graphical battery at the top-right.
 * Fill level is the meter; tip is solid when charging.
 * When charging, overlay one solid flash (battery_ui.battery_bolt_*) with
 * a single polarity inverted against overall fill level.
 */
static void oled_draw_battery_icon(int batt_pct, bool charging)
{
	if (batt_pct < 0)
		batt_pct = 0;
	if (batt_pct > 100)
		batt_pct = 100;

	const uint8_t body_w = 12;
	const uint8_t tip_w = 2;
	const uint8_t h = 10;
	const uint8_t total_w = (uint8_t)(body_w + tip_w);
	const uint8_t x = (uint8_t)(H_COLS - total_w - 1);
	const uint8_t y = OLED_ICON_Y;
	const uint8_t on = 0x0F;
	const uint8_t off = 0x00;

	oled_fill_cols(x, (uint8_t)(x + body_w - 1), y, y, on);
	oled_fill_cols(
		x, (uint8_t)(x + body_w - 1), (uint8_t)(y + h - 1),
		(uint8_t)(y + h - 1), on
	);
	oled_fill_cols(x, x, y, (uint8_t)(y + h - 1), on);
	oled_fill_cols(
		(uint8_t)(x + body_w - 1), (uint8_t)(x + body_w - 1),
		y, (uint8_t)(y + h - 1), on
	);

	oled_fill_cols(
		(uint8_t)(x + 1), (uint8_t)(x + body_w - 2),
		(uint8_t)(y + 1), (uint8_t)(y + h - 2), off
	);

	const int inner_w = body_w - 2;
	int filled = (batt_pct * inner_w + 50) / 100;
	if (filled > inner_w)
		filled = inner_w;
	if (filled > 0) {
		oled_fill_cols(
			(uint8_t)(x + 1), (uint8_t)(x + filled),
			(uint8_t)(y + 1), (uint8_t)(y + h - 2), on
		);
	}

	const uint8_t tip_x = (uint8_t)(x + body_w);
	const uint8_t tip_y0 = (uint8_t)(y + 3);
	const uint8_t tip_y1 = (uint8_t)(y + h - 4);
	if (charging) {
		oled_fill_cols(
			tip_x, (uint8_t)(tip_x + tip_w - 1), tip_y0, tip_y1, on
		);
		/*
		 * One-polarity flash: lit when under half full, dark when ≥ half.
		 * Matches battery_ui.battery_bolt_lit — keeps the glyph intact.
		 */
		const bool bolt_lit = (filled * 2 < inner_w);
		const uint8_t bolt_bright = bolt_lit ? on : off;
		static const int8_t bolt[][2] = {
			{5, 1}, {6, 1}, {7, 1}, {8, 1},
			{4, 2}, {5, 2}, {6, 2}, {7, 2},
			{2, 3}, {3, 3}, {4, 3}, {5, 3}, {6, 3}, {7, 3},
			{5, 4}, {6, 4}, {7, 4},
			{3, 5}, {4, 5}, {5, 5}, {6, 5},
			{2, 6}, {3, 6}, {4, 6},
			{3, 7}, {4, 7},
		};
		for (uint8_t i = 0; i < sizeof(bolt) / sizeof(bolt[0]); i++) {
			oled_fill_cols(
				(uint8_t)(x + bolt[i][0]),
				(uint8_t)(x + bolt[i][0]),
				(uint8_t)(y + bolt[i][1]),
				(uint8_t)(y + bolt[i][1]),
				bolt_bright
			);
		}
	} else {
		oled_fill_cols(
			tip_x, (uint8_t)(tip_x + tip_w - 1), tip_y0, tip_y0, on
		);
		oled_fill_cols(
			tip_x, (uint8_t)(tip_x + tip_w - 1), tip_y1, tip_y1, on
		);
		oled_fill_cols(
			(uint8_t)(tip_x + tip_w - 1), (uint8_t)(tip_x + tip_w - 1),
			tip_y0, tip_y1, on
		);
	}
}

/** Draw 0..4 ascending signal bars at the top-left. */
static void oled_draw_signal_icon(int bars)
{
	if (bars < 0)
		bars = 0;
	if (bars > 4)
		bars = 4;

	const uint8_t x0 = 1;
	const uint8_t base = (uint8_t)(OLED_ICON_Y + 10);
	const uint8_t heights[4] = {3, 5, 7, 9};
	const uint8_t on = 0x0F;

	for (uint8_t i = 0; i < 4; i++) {
		uint8_t x = (uint8_t)(x0 + i * 3);
		uint8_t h = heights[i];
		uint8_t y0 = (uint8_t)(base - h);
		if (i < (uint8_t)bars) {
			oled_fill_cols(x, (uint8_t)(x + 1), y0, (uint8_t)(base - 1), on);
		}
	}
}

static void oled_draw_bottom_text_gfx(
	const char* text, uint16_t curs_y,
	const GFXglyph *glyphs, const uint8_t *bitmaps
)
{
	if (text == nullptr || text[0] == '\0')
		return;
	uint16_t width = oled_str_pixel_width_gfx(text, glyphs);
	const char *draw = text;
	if (width > H_COLS) {
		size_t len = strlen(text);
		size_t start = 0;
		while (start < len
		       && oled_str_pixel_width_gfx(text + start, glyphs) > H_COLS) {
			start++;
		}
		draw = text + start;
		width = oled_str_pixel_width_gfx(draw, glyphs);
	}
	uint16_t curs_x = 0;
	if (width < H_COLS)
		curs_x = (uint16_t)((H_COLS - width) / 2);
	oled_draw_str_gfx(draw, curs_x, curs_y, glyphs, bitmaps);
}

static void oled_draw_bottom_text(const char* text)
{
	oled_draw_bottom_text_gfx(text, OLED_BOTTOM_Y_SMALL, GLYPHS, BITMAPS);
}

static void oled_draw_phone_number(const char* text)
{
	/* Narrow Iosevka 12pt — formatted NANP fits in H_COLS. */
	oled_draw_bottom_text_gfx(
		text, OLED_BOTTOM_Y,
		OLED_NUMBER_GLYPHS, OLED_NUMBER_BITMAPS
	);
}

void oled_show_ui(
	const char* status, const char* number,
	int batt_pct, bool charging, int signal_bars
)
{
	oled_enable();
	oled_clear();

	oled_draw_signal_icon(signal_bars);
	oled_draw_battery_icon(batt_pct, charging);

	const bool has_number = (number != nullptr && number[0] != '\0');
	const bool idle =
		(status == nullptr || status[0] == '\0'
		 || strcmp_P(status, PSTR("Ready")) == 0);

	/*
	 * Non-idle call phase (Dialing / Ringing / In call / …) in a smaller
	 * 8pt font, centered between the icons.
	 */
	if (!idle && status != nullptr) {
		const uint16_t side = 18;
		const uint16_t budget =
			(H_COLS > (uint16_t)(side * 2))
				? (uint16_t)(H_COLS - side * 2) : 0;
		char mid[20];
		size_t n = 0;
		uint16_t w = 0;
		while (status[n] != '\0' && n + 1 < sizeof(mid)) {
			uint16_t adv = oled_glyph_advance_gfx(
				status[n], OLED_STATUS_GLYPHS
			);
			if ((uint16_t)(w + adv) > budget)
				break;
			mid[n] = status[n];
			w = (uint16_t)(w + adv);
			n++;
		}
		mid[n] = '\0';
		if (mid[0] != '\0') {
			uint16_t x = side;
			if (w < budget)
				x = (uint16_t)(side + (budget - w) / 2);
			/* Nudge down slightly so 8pt sits visually under icon tops. */
			oled_draw_str_gfx(
				mid, x, (uint16_t)(OLED_ICON_Y + 2),
				OLED_STATUS_GLYPHS, OLED_STATUS_BITMAPS
			);
		}
	}

	if (has_number)
		oled_draw_phone_number(number);
	else if (idle)
		oled_draw_bottom_text("Ready to dial");
	else
		oled_draw_bottom_text(status);
}


/* Erase the character on the OLED display at the given offset.
 *
 * @param c The character to erase
 * @param curs_x The horizontal offset from the top-left corner at which to start erasing
 * @param curs_y The vertical offset from the top-left corner at which to start erasing
 */
void oled_erase_char(char c, uint16_t curs_x, uint16_t curs_y) {
	// Get the glyph's width and height from flash memory.
	uint16_t g_width = pgm_read_word(&GLYPHS[c - 32].width);
	uint16_t g_height = pgm_read_word(&GLYPHS[c - 32].height);

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
		uint8_t g_width = pgm_read_byte(&GLYPHS[str[char_idx] - 32].width);

		// Check whether the glyph will exceed the horizontal resolution of the display.
		if(curs_x + g_width > H_RES)
			break;

		// Erase the glyph from the display.
		oled_erase_char(str[char_idx], curs_x, curs_y);

		// Advance the X cursor according to the width of the space defined in the font.
		// This value considers both the width of the given glyph and the space between glyphs.
		curs_x = curs_x + pgm_read_byte(&GLYPHS[str[char_idx] - 32].xAdvance);
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
void oled_print(char* str, uint16_t curs_x, uint16_t curs_y) {
	oled_enable();
	oled_clear();

	// Draw the string on the display.
	oled_draw_str(str, curs_x, curs_y);
}


void oled_print_number(char* str)
{
	oled_enable();
	oled_clear();
	oled_draw_number_centered_wrap(str);
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

