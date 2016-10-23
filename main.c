/*  GC to NES : Gamecube controller to NES adapter
    Copyright (C) 2012-2016  Raphael Assenat <raph@raphnet.net>

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

#include "gcn64_protocol.h"
#include "gamecube.h"
#include "boarddef.h"
#include "sync.h"
#include "atmega168compat.h"

#define DEBUG_LOW()		PORTB &= ~(1<<5);
#define DEBUG_HIGH()	PORTB |= (1<<5);

#ifdef AT168_COMPATIBLE
	#define COMPAT_GIFR	EIFR
#else
	#define COMPAT_GIFR	GIFR
#endif


Gamepad *gcpad;
unsigned char gc_report[GCN64_REPORT_SIZE];

static volatile unsigned char g_nes_polled = 0;
static volatile unsigned char g_turbo_on = 0;
static volatile unsigned char int_counter = 0;

static volatile unsigned char nesbyte = 0xff;
static volatile unsigned char reuse;

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
	unsigned char bit, dat;

	//DEBUG_HIGH();

	if (g_turbo_on) {
		int_counter++;
	}
#if 1
	// This is to detect 'continuously in handler' conditions.
	// eg: Paperboy pause screen is continuously latching and reading the controller. 
	// Many times per frame without delay. How can we read the gamecube controller to
	// detect the 'start' button being pressed to exit the pause screen??	
	//
	// Unfortunately, in paperboy, unconnecting the controller exits the pause screen...
	// I think no pause is better than no-exit pause? Ah if I had a shift register
	// on board it would be easier.
	//
	reuse++;
	if (reuse==0xff) {
		// reuse is cleared each time we perform a read from the gamecube controller.
#ifdef AT168_COMPATIBLE
		EIMSK &= ~(1<<INT0);
#else
		GICR &= ~(1<<INT0);
#endif
		
		// let the data line be high, so it looks as no buttons are pressed.
		// This also looks like no controller to the game.
		NES_DATA_PORT |= (1<<NES_DATA_BIT);

		return;
	}
#endif

relatch:
	COMPAT_GIFR |= (1<<INTF0);
	dat = nesbyte;

	if (g_turbo_on) {
		if (int_counter&0x4) {
			dat |= 0xc0;
		}
	}
	
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



	dat <<= 1;
	for (bit=0x80; bit; bit>>=1) 
	{

		/* The big unrolled polling loop here is necessary. Otherwise,
		 * there is too much jitter/delay in detecting the clock's falling
		 * edge. 
		 *
		 * The solution is simple : Don't check for timeouts inside the loop. 
		 * Unrolling like this means the end *is* the timeout :)
		 *
		 * This also frees us time to check for repeated and buried 
		 * latches. I.e one that would occur suddenly right in the middle
		 * of an incomplete clocking.
		 * 
		 * The timeout is necessary for games which latch the controller but
		 * don't read all the bits. For instance, metroid does a first latch,
		 * reads the 8 bits, then latch again, and does nothing. We need
		 * a way to exit this interrupt handler to let the main run and poll
		 * the Gamecube controller! The timeout approach works well. If the 
		 * game is not clocking us after a certain amount of time, we assume
		 * this is it. Let's just hope there are no games where the programmer
		 * decided to poll the controller in two steps with a long delay in
		 * the middle of the clocking period...
		 *
		 * The timeout must be carefully chosed. If a game polls too slowly,
		 * we don't want to timeout! Here are a few measurements:
		 *
		 *   Game            Clock period (uS)
		 * - Super mario 3 : 13 uS
		 * - Super mario 2 : 24 uS
		 * - Super mario   : 15.80 uS
		 * - Metroid       : 15.80 uS
		 * - Life force    : 24 uS
		 * - Karnov        : 19.40 uS
		 * - TNMT          : 25.20 uS
		 * - Link          : 15.20 uS
		 *
		 * But in the end, it turns out the clock cycle is not what we
		 * should be basing our timeout on. Some games such as Legendary Wings
		 * will latch the controller, waste a lot of time, and then read
		 * the 8 bits. We must not timeout there!
		 */



		// wait clock falling edge
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;
		if (!(NES_CLOCK_PIN & (1<<NES_CLOCK_BIT)))
			goto dobit1;
		if (COMPAT_GIFR & (1<<INTF0))
			goto relatch;


		goto int0_done;

dobit1:
		if (dat & bit) {
			NES_DATA_PORT |= (1<<NES_DATA_BIT);
		} else {
			NES_DATA_PORT &= ~(1<<NES_DATA_BIT);
		}
	}	
	

int0_done:

	/* Let the main loop know about this interrupt occuring. */
	g_nes_polled = 1;
	//DEBUG_LOW();
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

#define MAPPING_DEFAULT			0
#define MAPPING_LOWER_THRESHOLD	1
#define MAPPING_AUTORUN			2


#define AXIS_ON_OFF_THRESHOLD	56

#define AXIS_ON_OFF_THRESHOLD2	32

static int cur_mapping = MAPPING_DEFAULT;

void doMapping()
{
	switch(cur_mapping) {
		case MAPPING_DEFAULT:
			toNes(GC_GET_A(gc_report), 			NES_BIT_A);	
			toNes(GC_GET_B(gc_report), 			NES_BIT_B);	
			toNes(GC_GET_Z(gc_report), 			NES_BIT_SELECT);
			toNes(GC_GET_START(gc_report), 		NES_BIT_START);
			toNes(GC_GET_DPAD_UP(gc_report), 	NES_BIT_UP);
			toNes(GC_GET_DPAD_DOWN(gc_report), 	NES_BIT_DOWN);
			toNes(GC_GET_DPAD_LEFT(gc_report), 	NES_BIT_LEFT);
			toNes(GC_GET_DPAD_RIGHT(gc_report), NES_BIT_RIGHT);

			axisToNes(gc_report[0], NES_BIT_LEFT, NES_BIT_RIGHT, AXIS_ON_OFF_THRESHOLD);
			axisToNes(gc_report[1], NES_BIT_UP, NES_BIT_DOWN, AXIS_ON_OFF_THRESHOLD);
			break;

		case MAPPING_LOWER_THRESHOLD:
			toNes(GC_GET_A(gc_report), 			NES_BIT_A);	
			toNes(GC_GET_B(gc_report), 			NES_BIT_B);	
			toNes(GC_GET_Z(gc_report), 			NES_BIT_SELECT);
			toNes(GC_GET_START(gc_report), 		NES_BIT_START);
			toNes(GC_GET_DPAD_UP(gc_report), 	NES_BIT_UP);
			toNes(GC_GET_DPAD_DOWN(gc_report), 	NES_BIT_DOWN);
			toNes(GC_GET_DPAD_LEFT(gc_report), 	NES_BIT_LEFT);
			toNes(GC_GET_DPAD_RIGHT(gc_report), NES_BIT_RIGHT);

			axisToNes(gc_report[0], NES_BIT_LEFT, NES_BIT_RIGHT, AXIS_ON_OFF_THRESHOLD2);
			axisToNes(gc_report[1], NES_BIT_UP, NES_BIT_DOWN, AXIS_ON_OFF_THRESHOLD2);
			break;

		case MAPPING_AUTORUN:
			toNes(GC_GET_A(gc_report), 			NES_BIT_A);	
			toNes(GC_GET_B(gc_report), 			NES_BIT_B);	
			toNes(GC_GET_Z(gc_report), 			NES_BIT_SELECT);
			toNes(GC_GET_START(gc_report), 		NES_BIT_START);
			toNes(GC_GET_DPAD_UP(gc_report), 	NES_BIT_UP);
			toNes(GC_GET_DPAD_DOWN(gc_report), 	NES_BIT_DOWN);
			toNes(GC_GET_DPAD_LEFT(gc_report), 	NES_BIT_LEFT);
			toNes(GC_GET_DPAD_RIGHT(gc_report), NES_BIT_RIGHT);

			axisToNes_mario(gc_report[0], NES_BIT_LEFT, NES_BIT_RIGHT, NES_BIT_B, 32, 64);

			// This is not useful in mario, but as it does not appear to cause
			// any problems, I do it anyway since it might be good for other games.
			// (e.g. 2D view from above, with B button to run)
			axisToNes_mario(gc_report[1], NES_BIT_UP, NES_BIT_DOWN, NES_BIT_B, 32, 64);

			break;
	}

	if (GC_GET_L(gc_report)) {
		g_turbo_on = 1;
	} else {
		g_turbo_on = 0;
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
	DEBUG_LOW();

	/* PORTC
	 * 0: Data (output) 
	 * 1: Clock (input)
	 */
	DDRC=1;
	PORTC=0xff;

	// configure external interrupt 0 to trigger on rising edge
#ifdef AT168_COMPATIBLE
	EIMSK |= (1<<INT0);
	EIMSK &= ~(1<<INT1);
	EICRA = (1<<ISC01) | (1<<ISC00);
#else
	MCUCR |= (1<<ISC01) | (1<<ISC00);
	GICR |= (1<<INT0);
	GICR &= ~(1<<INT1);
#endif

	gcn64protocol_hwinit();
	gcpad->init();

	_delay_ms(500);

	/* Read from Gamecube controller */
	gcpad->update();
	gcpad->buildReport(gc_report, 0);


	if (GC_GET_A(gc_report)) {
		cur_mapping = MAPPING_AUTORUN;
	}
	if (GC_GET_B(gc_report)) {
		cur_mapping = MAPPING_LOWER_THRESHOLD;
	}


	sync_init();

	sei();

	while(1)
	{
		if (g_nes_polled) {
			//DEBUG_HIGH();
			g_nes_polled = 0;
			sync_master_polled_us();
//			DEBUG_LOW();
		}

		if (sync_may_poll() || (reuse == 0xff)) {	

//			DEBUG_HIGH();
			gcpad->update();
//			DEBUG_LOW();


			if (gcpad->changed(0)) {
				// Read the gamepad
				gcpad->buildReport(gc_report, 0);
				// prepare the controller data byte
				doMapping();
			}

			// It does not matter if the data changed or not. What matters
			// is that it is a fresh read.
			if (reuse == 0xff) {
				// reenable int
#ifdef AT168_COMPATIBLE
				EIMSK |= (1<<INT0);
#else
				GICR |= (1<<INT0);
#endif
			}
			reuse = 0;
		}
	}
}


