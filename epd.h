#ifndef RUSP_EPD_H
#define RUSP_EPD_H

#include <GxEPD2_BW.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSerifItalic9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>

// Maximum height macro from old firmware
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= 800 / (EPD::WIDTH / 8) ? EPD::HEIGHT : 800 / (EPD::WIDTH / 8))

// ePaper display object (pointer)
extern GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)> *eink;

// Global variables for contact display
extern char CName[30];      // Contact name buffer
extern int CNumber[30];     // Contact number buffer  
extern int kc;              // Contact number length
extern int pg;              // ePaper page number

// Function declarations
int epd_displayContacts(int n);
void epd_splash();
void epd_splashOld();

// SD contact reading function (needs to be implemented)
void SDgetContact(int line);

#endif // RUSP_EPD_H
