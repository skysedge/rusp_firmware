//Make the ePaper eink do various things

int epd_displayContacts(int n){
	// Construct ePaper object if not already created (using placement new on static buffer)
	extern uint8_t eink_buffer[];
	extern bool eink_constructed;
	
	if (!eink_constructed) {
		Serial.println("ePaper: Creating display object...");
		eink = new (eink_buffer) GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)>(
			GxEPD2_290_flex(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
		);
		eink_constructed = true;
		Serial.println("ePaper: Object created");
	}
	
	// Initialize ePaper right before use
	Serial.println("ePaper: Initializing for contacts...");
	
	// CRITICAL: End any existing SPI transactions (OLED leaves one open)
	SPI.endTransaction();
	delay(10);
	
	Serial.println("ePaper: Calling init()...");
	Serial.flush();
	
	eink->init(9600);
	
	// Reinitialize Serial in case eink->init() broke it
	Serial.end();
	delay(10);
	Serial.begin(115200);
	delay(100);
	
	Serial.println("ePaper: Init complete, Serial restarted");
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
	
	Serial.print("Page ");
	Serial.print(page_num);
	Serial.print(": Showing contacts ");
	Serial.print(start_contact);
	Serial.print(" to ");
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
			for (int j = 0; j < (kc-2); j++){	//Count up to the highest stored numbre in CNumber
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
	
	Serial.println("ePaper: Contacts complete, hibernated");
	return page_num;  // Return the page number for speed dial reference
}

void epd_splash(){
	// Construct ePaper object if not already created (using placement new on static buffer)
	extern uint8_t eink_buffer[];
	extern bool eink_constructed;
	
	if (!eink_constructed) {
		Serial.flush();  // Ensure TX buffer is empty
		while (Serial.available()) Serial.read();  // Clear RX buffer
		
		Serial.print("ePaper: Creating");
		Serial.flush();
		Serial.print(" display");
		Serial.flush();
		Serial.print(" object...");
		Serial.flush();
		Serial.println();
		
		eink = new (eink_buffer) GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)>(
			GxEPD2_290_flex(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
		);
		eink_constructed = true;
		
		Serial.println("ePaper: Object created");
		Serial.flush();
	}
	
	// Initialize ePaper right before use
	Serial.print("ePaper: I");
	Serial.flush();
	Serial.print("nitializing");
	Serial.flush();
	Serial.print(" for splash...");
	Serial.flush();
	Serial.println();
	
	// CRITICAL: End any existing SPI transactions (OLED leaves one open)
	SPI.endTransaction();
	delay(10);
	
	Serial.println("ePaper: Calling init()...");
	Serial.flush();
	
	eink->init(9600);
	
	// Reinitialize Serial in case eink->init() broke it
	Serial.end();
	delay(10);
	Serial.begin(115200);
	delay(100);
	
	Serial.println("ePaper: Init complete, Serial restarted");
	Serial.flush();
	
	eink->setRotation(0);
	eink->setTextColor(GxEPD_BLACK);
	
	eink->firstPage();	//this function is called before every time ePaper is updated. Has nothing to do with what I call page numbers in this section of the program.
  	eink->setFullWindow();
	do {
		eink->fillScreen(GxEPD_WHITE); // set the background to white (fill the buffer with value for white)
		eink->setFont(&FreeSans9pt7b);
		eink->setCursor(12, 30);
		eink->print("R");
		eink->setCursor(31, 35);
		eink->print("o");
		eink->setCursor(50, 40);
		eink->print("t");
		eink->setCursor(69, 45);
		eink->print("a");
		eink->setCursor(88, 50);
		eink->print("r");
		eink->setCursor(107, 55);
		eink->print("y");
		eink->setFont(&FreeSerifItalic9pt7b);
		eink->setCursor(15, 70);
		eink->print("UnSmArT");
		eink->setFont(&FreeSerif9pt7b);
		eink->setCursor(30, 90);
		eink->print("PHONE");
		eink->setCursor(8, 110);
		eink->setFont();
		eink->print("(for making calls)");
		eink->setCursor(42, 150);
		eink->setFont(&FreeMono9pt7b);
		eink->print("AKA");
		eink->setFont(&FreeSerifItalic9pt7b);
		eink->setCursor(10, 180);
		eink->print("an electronic,");
		eink->setFont(&FreeSans9pt7b);
		eink->setCursor(35, 200);
		eink->print("portable,");
		eink->setFont(&FreeMono9pt7b);
		eink->setCursor(15, 220);
		eink->print("digital,");
		eink->setFont(&FreeSerifItalic9pt7b);
		eink->setCursor(30, 240);
		eink->print("wireless,");
		eink->setFont(&FreeMonoBold9pt7b);
		eink->setCursor(10, 260);
		eink->print("TELEPHONE");
		delay(50);
	} while (eink->nextPage());
	eink->hibernate();	//If this isn't here, wonky behavior ensues.
	
	Serial.println("ePaper: Splash complete, hibernated");
}

void epd_splashOld(){
	// Construct ePaper object if not already created (using placement new on static buffer)
	extern uint8_t eink_buffer[];
	extern bool eink_constructed;
	
	if (!eink_constructed) {
		Serial.println("ePaper: Creating display object...");
		eink = new (eink_buffer) GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)>(
			GxEPD2_290_flex(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
		);
		eink_constructed = true;
		Serial.println("ePaper: Object created");
	}
	
	eink->firstPage();	//this function is called before every time ePaper is updated. Has nothing to do with what I call page numbers in this section of the program.
  	eink->setFullWindow();
	do {
		eink->fillScreen(GxEPD_WHITE); // set the background to white (fill the buffer with value for white)
		eink->setFont();	//default font
		eink->setCursor(2, 185);
		eink->print("Missed call:");
		eink->setFont(&FreeMonoBold9pt7b);
		eink->setCursor(8, 208);
		eink->print("???-????");
		eink->setFont(&FreeSans9pt7b);
		eink->setCursor(5, 60);
		eink->print("Wireless");
		eink->setFont(&FreeSerifItalic9pt7b);
		eink->setCursor(30, 80);
		eink->print("Electronic");
		eink->setFont(&FreeMono9pt7b);
		eink->setCursor(10, 100);
		eink->print("Digital");
		eink->setFont(&FreeSerif9pt7b);
		eink->setCursor(33, 120);
		eink->print("Portable");
		eink->setFont(&FreeSans9pt7b);
		eink->setCursor(0, 145);
		eink->print("TeLePhOnE");
		delay(50);
	} while (eink->nextPage());
	eink->hibernate();	//If this isn't here, wonky behavior ensues.
}
