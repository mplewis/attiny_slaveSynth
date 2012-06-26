/* ATtiny85 as an I2C Slave
 * 
 * ???
 *
 * ATtiny Pin 1 = (RESET) N/U                      ATtiny Pin 2 = (D3) N/U
 * ATtiny Pin 3 = (D4) to LED1                     ATtiny Pin 4 = GND
 * ATtiny Pin 5 = I2C SDA on DS1621  & GPIO        ATtiny Pin 6 = (D1) to LED2
 * ATtiny Pin 7 = I2C SCK on DS1621  & GPIO        ATtiny Pin 8 = VCC (2.7-5.5V)
 *
 *  RESET  ---- [VCC]
 *     D3  |AT|  SCK
 *     D4  |85|  D1
 *   [GND] ----  SDA
 */

#include "TinyWireS.h"
#include "tia_gen.h"

#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit)) // clear bit
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))  // set bit


const int I2C_addr = 02;

const int pinStatus = 3;
const int pinSpkr = 4;

// time to turn the led on or off for each blink
const int ledTimeOn = 100;
const int ledTimeOff = 50;

int numBlinks = 0;
bool ledState = false;
long nextBlinkMillis = 0;
int delayUs;

bool chipEnable = false;			// boot with sound disabled
bool ledEnable = true;				// boot with led enabled

/* only one of these generators is active at a time
 * if both are enabled, noise gen takes precedence
 */
bool noiseGenEnable = false;		// enables noise generation
bool atariGenEnable = false;		// enables Atari sound generator

// frequency sweeping only works with Atari and square waves
bool freqSweepEnable = false;		// enables freq sweeping
float targetFrequency = 440;		// target freq is when the sweep stops
long sweepDelay = 0;				// higher sweep delay = slower sweep
long nextSweepMicros = 0;			// indicates next system time (in microseconds) to adjust frequency
int sweepStep = 1;					// number of Hz to step per sweep cycle
int sweepDirection = 1;				// +1 if sweeping up, -1 if sweeping down

uint8_t selectedWaveform = 1;		// startup Atari waveform = 1
float selectedFrequency = 440;		// default freq on chip enable = 440 Hz (A4)

int ledByte, atariByte, noiseByte;	// must be defined outside switch-case statement

// use a union to convert four bytes to one float
union byteFloat_t { 
	byte asBytes[4];
	float asFloat;
} convBytesFloat;

// and use a union to convert two bytes to one int
union byteInt_t { 
	byte asBytes[2];
	int asInt;
} convBytesInt;

void setup(){
	pinMode(pinStatus, OUTPUT);
	pinMode(pinSpkr, OUTPUT);
	TinyWireS.begin(I2C_addr);
	recalcFreq();
	numBlinks = 3;
}

void loop() {

	// make status LED blink if ledEnable is true and numBlinks indicates blinking is required
	if (ledEnable) {
		if ((numBlinks > 0) and (millis() >= nextBlinkMillis)) {
			if (ledState == false) { // led is off: flip led state, set led high, set next blink timer
				ledState = true;
				sbi(PORTB, PORTB3); // I think this is faster
				nextBlinkMillis = millis() + ledTimeOn;
			} else { // led is on: flip led state, set led low, decrement blink counter, set next blink timer
				ledState = false;
				numBlinks--;
				cbi(PORTB, PORTB3);
				nextBlinkMillis = millis() + ledTimeOff;
			}
		}
	}

	// get data from I2C
	if (TinyWireS.available()) {
	
		// first byte is a command byte
		byte byteCmd = TinyWireS.receive();
	
		// select what to do based on command byte
		switch (byteCmd) {
			
			/* 0 (48d, 30h): no following bytes: disable chip
			 * Stops all sound generation until enable command is received
			 */
			case '0':
				numBlinks = 1;
				chipEnable = false;
				break;
			
			/* 1 (49d, 31h): no following bytes: enable chip
			 * Restarts sound generation if chip is disabled
			 */
			case '1':
				numBlinks = 1;
				chipEnable = true;
				break;

			/* L (76d, 4Ch): one byte (uint8_t): status LED enable
			 * odd turns on status LED, even turns off
			 */
			case 'L':
				numBlinks = 1;
				// wait for data; this is blocking
				while (!TinyWireS.available()) { /* nop */ }
				ledByte = TinyWireS.receive();
				if (ledByte % 2 == 0) {
					ledEnable = false;
					ledState = false;
					cbi(PORTB, PORTB3);
				} else {
					ledEnable = true;
				}
				break;

			/* N (78d, 4Eh): one byte (uint8_t): noise gen enable
			 * odd turns on noise gen, even turns off
			 */
			case 'N':
				numBlinks = 1;
				// wait for data; this is blocking
				while (!TinyWireS.available()) { /* nop */ }
				noiseByte = TinyWireS.receive();
				if (noiseByte % 2 == 0) {
					noiseGenEnable = false;
				} else {
					noiseGenEnable = true;
				}
				break;

			/* A (65d, 41h): one byte (uint8_t): Atari gen enable
			 * odd turns on Atari 2600 audio gen, even turns off
			 * if both atariGenEnable and noiseGenEnable are true, noiseGen is used
			 */
			case 'A':
				numBlinks = 1;
				// wait for data; this is blocking
				while (!TinyWireS.available()) { /* nop */ }
				atariByte = TinyWireS.receive();
				if (atariByte % 2 == 0) {
					atariGenEnable = false;
				} else {
					atariGenEnable = true;
				}
				break;

			/* W (87d, 57h): one byte (uint8_t): waveform select
			 * currently, selects waveform type for Atari gen
			 * if Atari is disabled, generates a square wave regardless of selectedWaveform
			 */
			case 'W':
				numBlinks = 1;
				// wait for data; this is blocking
				while (!TinyWireS.available()) { /* nop */ }
				selectedWaveform = TinyWireS.receive();
				break;

			/* F (70d, 46h): four bytes (float): select frequency
			 * freq will be calculated for square wave gen and Atari gen,
			 * though actual Atari freq will depend on which gen is active
			 */
			case 'F':
				numBlinks = 1;
				/* Arduino C++ uses little endian by default
				 * [                          float                          ]
				 * [ least sig byte 0 ][ byte 1 ][ byte 2 ][ most sig byte 3 ]
				 * Receive bytes in the order they are sent
				 */
				for (int i = 0; i <= 3; i++) {
					// wait for data; this is blocking
					while (!TinyWireS.available()) { /* nop */ }
					convBytesFloat.asBytes[i] = TinyWireS.receive();
				}
				// output the freq float to the usual variable
				selectedFrequency = convBytesFloat.asFloat;
				// recalculate timing vars
				recalcFreq();
				// disable freq sweeping
				freqSweepEnable = false;
				break;

			/* S (83d, 53h): ten bytes (4b float, 4b float, 2b int, 2b int): frequency sweep
			 * first float selects start freq
			 * second float selects end freq
			 * first int selects size of sweep cycle step
			 * second int selects delay between sweeps in microseconds
			 */
			case 'S':
				
				numBlinks = 1;

				for (int i = 0; i <= 3; i++) {
					// wait for data; this is blocking
					while (!TinyWireS.available()) { /* nop */ }
					convBytesFloat.asBytes[i] = TinyWireS.receive();
				}
				// output the starting freq to the current frequency variable
				selectedFrequency = convBytesFloat.asFloat;
				
				for (int i = 0; i <= 3; i++) {
					// wait for data; this is blocking
					while (!TinyWireS.available()) { /* nop */ }
					convBytesFloat.asBytes[i] = TinyWireS.receive();
				}
				// output the ending freq to the target frequency variable
				targetFrequency = convBytesFloat.asFloat;

				for (int i = 0; i <= 1; i++) {
					// wait for data; this is blocking
					while (!TinyWireS.available()) { /* nop */ }
					convBytesInt.asBytes[i] = TinyWireS.receive();
				}
				// output the sweep delay to the sweep delay variable
				sweepStep = convBytesInt.asInt;

				for (int i = 0; i <= 1; i++) {
					// wait for data; this is blocking
					while (!TinyWireS.available()) { /* nop */ }
					convBytesInt.asBytes[i] = TinyWireS.receive();
				}
				// output the sweep delay to the sweep delay variable
				sweepDelay = convBytesInt.asInt;

				// set the next inc/dec time
				nextSweepMicros = micros() + sweepDelay;

				if (selectedFrequency < targetFrequency) {
					// sweep is going up
					sweepDirection = 1;
				} else {
					// sweep is going down
					sweepDirection = -1;
				}

				// recalculate timing vars
				recalcFreq();
				
				// enable sweeping
				freqSweepEnable = true;
				
				break;

			default: // error: command not recognized: blink status LED 5 times
				numBlinks = 5;
				break;
		}
	}

	// sound is only generated if chipEnable is true
	if (chipEnable) {
		if (freqSweepEnable && micros() >= nextSweepMicros) {
			nextSweepMicros = micros() + sweepDelay;
			if (sweepDirection >= 0) {
				selectedFrequency += sweepStep;
				selectedFrequency -= sweepStep; // FIXME for debug purposes
				if (selectedFrequency >= targetFrequency) {
					selectedFrequency = targetFrequency;
					freqSweepEnable = false;
				}
			} else {
				selectedFrequency -= sweepStep;
				selectedFrequency += sweepStep; // FIXME for debug purposes
				if (selectedFrequency <= targetFrequency) {
					selectedFrequency = targetFrequency;
					freqSweepEnable = false;
				}
			}
			recalcFreq();
		}
		if (noiseGenEnable) {
			digitalWrite(pinSpkr, random(2));
		} else if (atariGenEnable) {
			uint8_t currSample = tia_out(selectedWaveform); // 0 or 1
			digitalWrite(pinSpkr, currSample);
			delayMicroseconds(delayUs);
		} else {
			digitalWrite(pinSpkr, HIGH);
			delayMicroseconds(delayUs);
			digitalWrite(pinSpkr, LOW);
			delayMicroseconds(delayUs);
		}
	}
}

// calculates a microsecond delay for 1/2 square wave periods from selectedFrequency
void recalcFreq() {
	delayUs = 500000 / selectedFrequency;
}