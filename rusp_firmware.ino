#include <avr/io.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

#define OFFSIGNAL 27
#define RELAY_OFF 35
#define LL_OE 38	//output enable for logic level converters
#define EN_3V3 34
#define CELL_ON A0

#define NET_STAT A5
#define CELL_CTS A3
#define CELL_RTS A4
#define CELL_RESET A1
#define CELL_PWR_DET A2


void setup()
{
	pinMode(OFFSIGNAL, INPUT_PULLUP);
	pinMode(RELAY_OFF, OUTPUT);
	pinMode(LL_OE, OUTPUT);
	pinMode(EN_3V3, OUTPUT);
	pinMode(CELL_ON, OUTPUT);
	pinMode(NET_STAT, INPUT);
	pinMode(CELL_CTS, INPUT);
	pinMode(CELL_RTS, OUTPUT);
	pinMode(CELL_RESET, OUTPUT);
	pinMode(CELL_PWR_DET, INPUT);

	Serial.begin(115200);
	digitalWrite(LL_OE, HIGH);
	digitalWrite(EN_3V3, HIGH);

	delay(2000);
	Serial.println("hello! turning LARA on");

	// turn LARA on
	digitalWrite(CELL_ON, HIGH);
	delay(1000);
	digitalWrite(CELL_ON, LOW);
	unsigned long stopwatch = millis();
	while (digitalRead(CELL_PWR_DET) == LOW) {
		if (millis() - stopwatch > 5000) {
			Serial.println("timeout waiting for CELL_PWR_DET");
			break;	// timeout
		}
	}

	delay(1000);

	// initialize LARA
	Serial1.begin(115200);
	delay(500);
	Serial1.write("AT\r");
	delay(1000);
	Serial1.write("AT+CMEE=2\r");
	Serial1.write("AT+CREG=1\r");
	Serial1.write("AT+UCALLSTAT=1\r");
	delay(1000);
	Serial.println("LARA is ready; passing through");
}


void loop()
{
	// LARA<->terminal serial passthrough
	if (Serial.available())
		Serial1.write(Serial.read());
	if (Serial1.available())
		Serial.write(Serial1.read());
	if (digitalRead(OFFSIGNAL) == LOW)
		shutdown();
}


void shutdown()
{
	Serial.println("shutdown called; waiting for cell powerdown");
	Serial1.write("AT+CPWROFF\r");
	unsigned long stopwatch = millis();
	while (digitalRead(CELL_PWR_DET) == HIGH) {
		if (millis() - stopwatch > 5000) {
			Serial.println("timeout waiting for CELL_PWR_DET==LOW");
			break;	// timeout
		}
	}
	digitalWrite(RELAY_OFF, HIGH);
	// POWER KILLED
}

