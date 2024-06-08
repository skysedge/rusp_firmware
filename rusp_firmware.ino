#include "lara.h"

#define OFFSIGNAL 27
#define RELAY_OFF 35
#define LL_OE 38	//output enable for logic level converters
#define EN_3V3 34


void setup()
{
	pinMode(OFFSIGNAL, INPUT_PULLUP);
	pinMode(RELAY_OFF, OUTPUT);
	pinMode(LL_OE, OUTPUT);
	pinMode(EN_3V3, OUTPUT);

	Serial.begin(115200);
	digitalWrite(LL_OE, HIGH);
	digitalWrite(EN_3V3, HIGH);

	delay(2000);

	Serial.println("hello! turning LARA on");
	lara_on(&Serial1, &Serial, 5000);
}


void loop()
{
	lara_passthrough();
	if (digitalRead(OFFSIGNAL) == LOW)
		shutdown();
}


void shutdown()
{
	Serial.println("shutdown called; waiting for cell powerdown");
	lara_off(5000);
	Serial.end();
	digitalWrite(RELAY_OFF, HIGH);
	// POWER KILLED
}

