// SD Contact Reading Function for ePaper Integration
// This reads contacts from the old-style contacts.txt format
// Format: "ContactName: 1234567890"

#include <Arduino.h>
#include <SD.h>
#include "pins.h"
#include "epd.h"

// Global variable definitions
char CName[30] = {0};       // Contact name buffer
int CNumber[30] = {0};      // Contact number buffer  
int kc = 0;                 // Contact number length
int pg = 1;                 // ePaper page number

void SDgetContact(int line) {
    int m = 0;
    char cholder;
    bool preColon = false;
    bool exitNow = false;
    
    File myFile = SD.open("contacts.txt", FILE_READ);
    if (!myFile) {
        Serial.println("Failed to open contacts.txt");
        // Clear the buffers
        for (int i = 0; i < 30; i++) {
            CName[i] = 0;
            CNumber[i] = 0;
        }
        kc = 0;
        return;
    }
    
    myFile.seek(0);
    kc = 0;  // Reset the contact phone number index
    
    // Navigate to the requested line
    for (int i = 0; i < (line - 1);) {
        cholder = myFile.read();
        if (cholder == '\n') {  // Inc i when we find a carriage return
            i++;
        } else if (cholder == '#') {
            cholder = 0;
            exitNow = true;
            break;
        }
    }
    
    // Now we are at the right line. Loop through each byte until we hit CR
    while (true) {
        if (exitNow == true) {
            break;
        }
        cholder = myFile.read();
        if (preColon == false) {
            if (cholder == ':') {  // Detect colon
                preColon = true;
            } else {
                CName[m] = cholder;
                m++;
            }
        } else {
            if (cholder == ' ') {  // Detect space (skip it)
                // Do nothing
            } else {
                CNumber[kc] = cholder - '0';
                kc++;
            }
        }
        if (cholder == '\n') {
            break;
        }
    }
    
    // Clear the remainder of CName
    while (m <= 30) {
        CName[m] = 0;
        m++;
    }
    
    // Truncate name to 11 chars to fit on ePD
    m = 11;
    while (m <= 30) {
        CName[m] = 0;
        m++;
    }
    
    myFile.close();
}
