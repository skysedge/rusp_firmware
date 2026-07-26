//Make the ePaper eink do various things
#include "epd.h"

/*
 * The ePaper driver object is built on demand and torn down after use rather
 * than living in a permanent static arena.
 *
 * sizeof(GxEPD2_BW<...>) is ~880 bytes — 11% of this part's SRAM — and the
 * panel is only touched for the boot splash and the contacts screen, both of
 * which finish and hibernate. Holding that buffer for the life of the program
 * spent the headroom that the stack and heap need at run time.
 *
 * Callers must pair epd_acquire() with epd_release(). Returns false if the
 * allocation fails, in which case `eink` stays null and the caller must skip
 * the draw instead of dereferencing it.
 */
bool epd_acquire()
{
	if (eink != nullptr)
		return true;
	eink = new GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)>(
		GxEPD2_290_flex(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
	);
	return eink != nullptr;
}

void epd_release()
{
	delete eink;
	eink = nullptr;
}

int epd_displayContacts(int n){
	if (!epd_acquire()) {
		Serial.println(F("ePaper: alloc failed"));
		return n;
	}
	
	// Initialize ePaper right before use
	Serial.println(F("ePaper: Initializing for contacts..."));
	
	// CRITICAL: End any existing SPI transactions (OLED leaves one open)
	SPI.endTransaction();
	delay(10);
	
	Serial.println(F("ePaper: Calling init()..."));
	Serial.flush();
	
	eink->init(9600);
	
	// Reinitialize Serial in case eink->init() broke it
	Serial.end();
	delay(10);
	Serial.begin(115200);
	delay(100);
	
	Serial.println(F("ePaper: Init complete, Serial restarted"));
	Serial.flush();
	
	eink->setRotation(0);
	eink->setTextColor(GxEPD_BLACK);
	
	int col = 0;
	int row = 12;
	int ctcID = 1;
	
	// Calculate which contacts to show and which page this is
	// Dial 1 → page 1, show contacts 1-9
	// Dial 2 → page 2, show contacts 10-18
	// Dial 0 → page 2, show contacts 10-18 (same as dial 2)
	int page_num = (n == 0) ? 2 : n;
	int start_contact = ((n == 0) ? 10 : ((n - 1) * 9 + 1));
	int end_contact = start_contact + 8;  // 9 contacts per page
	
	Serial.print(F("Page "));
	Serial.print(page_num);
	Serial.print(F(": Showing contacts "));
	Serial.print(start_contact);
	Serial.print(F(" to "));
	Serial.println(end_contact);
	
	eink->fillScreen(GxEPD_WHITE);
	eink->setFont();
	eink->firstPage();
	do {
		row = 12;  // Reset row position
		ctcID = 1; // Display ID always 1-9 on each page
		
		for(int line = start_contact; line <= end_contact; line++){
			SDgetContact(line);	//Fills CName[] and CNumber[] for the current line
			eink->setFont(&FreeSans9pt7b);	//Font for contact name
			eink->setCursor(col, row);
			eink->print(ctcID);
			eink->setCursor(col+10, row);
			eink->print(".");
			eink->setCursor(col+16, row);
			eink->print(CName);
			eink->setCursor(col+25, row+12);	//space to phone number below contact name
			eink->setFont();		//Font for contact phone number
			for (int j = 0; j < kc; j++){	//Display all digits in CNumber
				eink->print(CNumber[j]);
				if (j == 2 || j == 5){
					eink->print("-");	//Format the phone number with dashes
				}
			}
			row = row + 33;	//Space to next contact name
			ctcID++;
		}
	} while (eink->nextPage());
	eink->hibernate();
	epd_release();

	Serial.println(F("ePaper: Contacts complete, hibernated"));
	return page_num;  // Return the page number for speed dial reference
}

void epd_splash(){
	Serial.flush();  // Ensure TX buffer is empty
	while (Serial.available()) Serial.read();  // Clear RX buffer

	Serial.println(F("ePaper: Creating display object..."));
	Serial.flush();

	if (!epd_acquire()) {
		Serial.println(F("ePaper: alloc failed"));
		return;
	}

	Serial.println(F("ePaper: Object created"));
	Serial.flush();
	
	// Initialize ePaper right before use
	Serial.print(F("ePaper: I"));
	Serial.flush();
	Serial.print(F("nitializing"));
	Serial.flush();
	Serial.print(F(" for splash..."));
	Serial.flush();
	Serial.println();
	
	// CRITICAL: End any existing SPI transactions (OLED leaves one open)
	SPI.endTransaction();
	delay(10);
	
	Serial.println(F("ePaper: Calling init()..."));
	Serial.flush();
	
	eink->init(9600);
	
	// Reinitialize Serial in case eink->init() broke it
	Serial.end();
	delay(10);
	Serial.begin(115200);
	delay(100);
	
	Serial.println(F("ePaper: Init complete, Serial restarted"));
	Serial.flush();
	
	eink->setRotation(0);
	
	/*
	 * Precomputed Mandelbrot splash (epd_fractal_splash.h).
	 * Regenerate with: tools/pulse_monitor/gen_epd_fractal_splash.py
	 */
	eink->firstPage();
	eink->setFullWindow();
	do {
		eink->fillScreen(GxEPD_WHITE);
		eink->drawBitmap(
			0, 0,
			EPD_FRACTAL_SPLASH,
			EPD_FRACTAL_SPLASH_WIDTH,
			EPD_FRACTAL_SPLASH_HEIGHT,
			GxEPD_BLACK
		);
	} while (eink->nextPage());
	eink->hibernate();	//If this isn't here, wonky behavior ensues.
	epd_release();

	Serial.println(F("ePaper: Fractal splash complete, hibernated"));
}
