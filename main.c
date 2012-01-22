/*  GC to N64 : Gamecube controller to N64 adapter firmware
    Copyright (C) 2011  Raphael Assenat <raph@raphnet.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <string.h>
#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>

#include "gamecube.h"
#include "boarddef.h"
#include "sync.h"

#define DEBUG_LOW()		PORTB &= ~(1<<5);
#define DEBUG_HIGH()	PORTB |= (1<<5);


Gamepad *gcpad;
unsigned char gc_report[GCN64_REPORT_SIZE];

static volatile unsigned char g_nes_polled = 0;

static volatile unsigned char nesbyte = 0xff;

#define NES_DATA_PORT 	PORTC
#define NES_DATA_BIT	0
#define NES_CLOCK_BIT	1
#define NES_CLOCK_PIN	PINC
#define NES_LATCH_PIN	PIND
#define NES_LATCH_BIT	2

#define NES_BIT_A		0
#define NES_BIT_B		1
#define NES_BIT_SELECT	2
#define NES_BIT_START	3
#define NES_BIT_UP		4
#define NES_BIT_DOWN	5
#define NES_BIT_LEFT	6
#define NES_BIT_RIGHT	7

ISR(INT0_vect)
{
	unsigned char bit, dat = nesbyte;

	/**           __
	 * Latch ____|  |________________________________________
	 *       _________   _   _   _   _   _   _   _   ________
	 * Clk            |_| |_| |_| |_| |_| |_| |_| |_|
	 *
	 * Data      |       |   |   |   |   |   |   |
	 *           A       B   Sel St  U   D   L   R      
	 */
	
	if (dat & 0x80) {
		NES_DATA_PORT |= (1<<NES_DATA_BIT);
	} else {
		NES_DATA_PORT &= ~(1<<NES_DATA_BIT);
	}

	/* Wait until the latch pulse is over before continuing.
	 * This makes it possible to detect repeated latches
	 * by monitoring the pin later.
	 */
	while (NES_LATCH_PIN & (1<<NES_LATCH_BIT));

	bit = 0x40;
	while(1) 
	{
		// wait clock falling edge
		while (NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)) 
		{  	
			// If latch rises again, exit the interrupt. We
			// will re-enter this handler again very shortly because
			// this rising edge will set the INTF0 flag.
			if (NES_LATCH_PIN & (1<<NES_LATCH_BIT)) {
				return;
			}
		}

		if (dat & bit) {
			NES_DATA_PORT |= (1<<NES_DATA_BIT);
		} else {
			NES_DATA_PORT &= ~(1<<NES_DATA_BIT);
		}

		bit >>= 1;
		if (!bit)
			break;

		if (NES_LATCH_PIN & (1<<NES_LATCH_BIT)) {
			// If latch rises again, exit the interrupt. We
			// will re-enter this handler again very shortly because
			// this rising edge will set the INTF0 flag.
			return;
		}
	}	
	
	/* One last clock cycle to go before we set the
	 * 'idle' level of the data line */
	while ((NES_CLOCK_PIN & (1<<NES_CLOCK_BIT))) 
	{  	
		if (NES_LATCH_PIN & (1<<NES_LATCH_BIT)) {
			return;
		}

	}
	
	NES_DATA_PORT &= ~(1<<NES_DATA_BIT);

	/* Let the main loop know about this interrupt occuring. */
	g_nes_polled = 1;
}


void byteTo8Bytes(unsigned char val, unsigned char volatile *dst)
{
	unsigned char c = 0x80;

	do {
		*dst = val & c;
		dst++;
		c >>= 1;
	} while(c);
}

unsigned char scaleValue(unsigned char raw)
{
	return ((char)raw) * 24000L / 32767L;
}

void toNes(int pressed, int nes_btn_id)
{
	if (pressed)
		nesbyte &= ~(0x80 >> nes_btn_id);
	else
		nesbyte |= (0x80 >> nes_btn_id);

}

void axisToNes(unsigned char val, int nes_btn_low, int nes_btn_high, unsigned char thres)
{
	if (val < (0x80 - thres)) {
		toNes(1, nes_btn_low);
	} 

	if (val > (0x80 + thres)) {
		toNes(1, nes_btn_high);
	}
}

void axisToNes_mario(unsigned char val, int nes_btn_low, int nes_btn_high, int nes_run_button, unsigned char walk_thres, unsigned char run_thres)
{
	if (val < (0x80 - walk_thres)) {
		toNes(1, nes_btn_low);
		if (val < (0x80 - run_thres)) {
			toNes(1, nes_run_button);
		}
	} 

	if (val > (0x80 + walk_thres)) {
		toNes(1, nes_btn_high);
		if (val > (0x80 + run_thres)) {
			toNes(1, nes_run_button);
		}
	}
}

int main(void)
{
	
	gcpad = gamecubeGetGamepad();

	/* PORTD
	 * 2: NES Latch interrupt
	 */
	DDRD = 0;
	PORTD = 0xff;

	DDRB = 0;
	PORTB = 0xff;
	DDRB = 1<<5;

	/* PORTC
	 * 0: Data (output) 
	 * 1: Clock (input)
	 */
	DDRC=1;
	PORTC=0xff;

	// configure external interrupt 0 to trigger on rising edge
	MCUCR |= (1<<ISC01) | (1<<ISC00);
	GICR |= (1<<INT0);
	GICR &= ~(1<<INT1);

	sei();
	

	gcpad->init();

	DEBUG_HIGH();
	_delay_ms(500);

	/* Read from Gamecube controller */
	gcpad->update();
	gcpad->buildReport(gc_report);

	DEBUG_LOW();

	sync_init();

	while(1)
	{
		if (g_nes_polled) {
			g_nes_polled = 0;
			sync_master_polled_us();
		}

		if (sync_may_poll()) {	

//			DEBUG_HIGH();
			gcpad->update();
//			DEBUG_LOW();


			if (gcpad->changed()) {
	//			DEBUG_HIGH();
				// Read the gamepad	
				gcpad->buildReport(gc_report);				

				toNes(GC_GET_A(gc_report), 			NES_BIT_A);	
				toNes(GC_GET_B(gc_report), 			NES_BIT_B);	
				toNes(GC_GET_Z(gc_report), 			NES_BIT_SELECT);
				toNes(GC_GET_START(gc_report), 		NES_BIT_START);
				toNes(GC_GET_DPAD_UP(gc_report), 	NES_BIT_UP);
				toNes(GC_GET_DPAD_DOWN(gc_report), 	NES_BIT_DOWN);
				toNes(GC_GET_DPAD_LEFT(gc_report), 	NES_BIT_LEFT);
				toNes(GC_GET_DPAD_RIGHT(gc_report), NES_BIT_RIGHT);

			//	axisToNes(gc_report[0], NES_BIT_LEFT, NES_BIT_RIGHT, 32);
				
				axisToNes_mario(gc_report[0], NES_BIT_LEFT, NES_BIT_RIGHT, NES_BIT_B, 32, 64);
				axisToNes_mario(gc_report[1], NES_BIT_UP, NES_BIT_DOWN, NES_BIT_B, 32, 64);
//				axisToNes(gc_report[1], NES_BIT_UP, NES_BIT_DOWN, 32);
				
	//			DEBUG_LOW();
			}
		}
	}
}


