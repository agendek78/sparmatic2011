/// \file motor.c
/// 
/// 
/// 

/*
 * motor.c
 *
 *  Created on: 19.11.2011
 *      Author: matthias
 */

#include <avr/io.h>
#include <util/delay.h>

#include "lcd.h"
#include "keys.h"
#include "motor.h"
#include "timer.h"
#include "adc.h"


#define MOTOR_PORT PORTE
#define MOTOR_DDR DDRE
#define MOTOR_PIN_L PE7
#define MOTOR_PIN_R PE6

#define MOTOR_SENSE_DDR DDRE
#define MOTOR_SENSE_PORT PORTE
#define MOTOR_SENSE_PORT_IN PINE
#define MOTOR_SENSE_LED_PIN PE2


#define ADC_CH_MOTOR (2)
#define ADC_CH_MOTOR_SENSE (0)
#define MOTOR_DIR_OUT (MOTOR_DDR |= (1 << MOTOR_PIN_L) | (1 << MOTOR_PIN_R))
#define MOTOR_DIR_IN (MOTOR_DDR &= ~(1 << MOTOR_PIN_L) | (1 << MOTOR_PIN_R))
#define MOTOR_STOP (MOTOR_PORT &= ~((1 << MOTOR_PIN_L) | (1 << MOTOR_PIN_R)))
#define MOTOR_OPEN (MOTOR_PORT |= (1 << MOTOR_PIN_L))
#define MOTOR_CLOSE (MOTOR_PORT |= (1 << MOTOR_PIN_R))

#define MOTOR_SENSE_ON (MOTOR_SENSE_PORT |= (1 << MOTOR_SENSE_LED_PIN))
#define MOTOR_SENSE_OFF (MOTOR_SENSE_PORT &= ~(1 << MOTOR_SENSE_LED_PIN))

#define MOTOR_TIMER_START (	LCDCRA |= (1 << LCDIE))

#define DIR_OPEN 1
#define DIR_CLOSE -1
#define DIR_STOP 0

/* hardcoded blocking current limit. Currents are in ADC digits.
 * For real current the formular is as follows:
 * I = (1024 - ADCval) * Ubat / 2.2 */
#define MOTOR_CURRENT_BLOCK 930
/* harder limit for detecting open end position */
#define MOTOR_CURRENT_BLOCK_OPEN 950
/* minimum change in current to detect valve */
#define MOTOR_CURRENT_VALVE_DETECT 5
/* all speeds are given in ms per tick.
 * A tick is a period on motor sense pin: LH for Forward, HL for Backwards.
 */
/* block: Stop if no pulse is received in that interval */
#define MOTOR_SPEED_BLOCK 110
/* faster turn off at open end position */
#define MOTOR_SPEED_BLOCK_OPEN 60

#define MOTOR_POSITION_MAX 380
/* ventOpen - ventClosed has to be min. X motor steps */
#define MOTOR_VENT_RANGE_MIN 70

/* stop earlier than target */
#define MOTOR_TARGET_STOP_EARLY 6
#define MOTOR_TARGET_HYSTERESIS 10

typedef enum {STOP_NULL = 0, STOP_TIMEOUT = 1, STOP_CURRENT = 2, STOP_POSITION = 4, STOP_TARGET = 8} MOTOR_STOP_SOURCE;

volatile static uint8_t MotorStopSource;

volatile static int8_t Direction = 0;
volatile static uint8_t PWM;
volatile int16_t MotorPosition = 0;
int16_t PositionValveOpen;
int16_t PositionValveClosed;

/* used as stop condition, initialized to very slow speed stop/block stop */
static uint8_t MotorTimeout = 95;
static uint16_t CurrentLimit = 940;
static int16_t TargetPosition = -1;


/* Motorgeschwindigkeit Leerlauf ~ 70-90 Schritte pro Sekunde
 * Motorgeschwindigkeit Last ~40-80 Schritte pro Sekunde
 * Motorstrom Leerlauf ~30 mA
 * Motorstrom Kurzschluss ~180 mA
 * Motorstrom starke Belastung
 */


/// \brief .
/// 
/// 
void motorInit(void)
{
	  MOTOR_PORT &= ~((1 << MOTOR_PIN_L) | (1 << MOTOR_PIN_R));

	  MOTOR_SENSE_DDR |= (1 << MOTOR_SENSE_LED_PIN);
	  PCMSK0 |= (1 << PCINT1);
}


/// \brief .
/// 
/// 
static uint16_t getCurrent(void)
{
	return getAdc(ADC_CH_MOTOR);
}


/// \brief .
/// 
/// 
void motorStopMove(void)
{
	disableTimeout();
	MOTOR_STOP;
	MOTOR_DIR_IN;
	_delay_ms(800);		/* todo think about different implementation */
	MOTOR_SENSE_OFF;
	Direction = DIR_STOP;
}


/// \brief .
/// 
/// 
void motorStopTimeout(void)
{
	MotorStopSource |= STOP_TIMEOUT;
	motorStopMove();
}


/// \brief .
/// 
/// 
static void motorMove(int8_t dir)
{
	MOTOR_SENSE_ON;
	MOTOR_STOP;
	MOTOR_DIR_OUT;
	MotorStopSource = STOP_NULL;
	enableTimeout(motorStopTimeout, 255);	/* first timeout long to allow startup */
	switch(dir)
	{
		case DIR_OPEN:
			MOTOR_OPEN;
			break;
		case DIR_CLOSE:
			MOTOR_CLOSE;
			break;
		default:
			MOTOR_SENSE_OFF;
			break;
	}
	Direction = dir;
	PWM = 0;
	MOTOR_TIMER_START;
}


/// \brief .
/// 
/// 
/**
 * drives valve position to given value.
 * @param valve valve opening from 0..255
 */
void motorMoveTo(uint8_t valve)
{
	int16_t newPosition = PositionValveClosed + ((PositionValveOpen - PositionValveClosed) * valve) / 255;
	TargetPosition = newPosition;
	if(newPosition > MotorPosition + MOTOR_TARGET_HYSTERESIS)
	{
		TargetPosition -= MOTOR_TARGET_STOP_EARLY;
		motorMove(DIR_OPEN);
	}
	else if(newPosition < MotorPosition - MOTOR_TARGET_HYSTERESIS)
	{
		TargetPosition += MOTOR_TARGET_STOP_EARLY;
		motorMove(DIR_CLOSE);
	}

}


/// \brief .
/// 
/// 
/*
 * resets motor calibration and fully opens until block.
 * @return not zero on hardware failure (no other errors possible)
 */
uint8_t motorFullOpen(void)
{
	MotorTimeout = MOTOR_SPEED_BLOCK_OPEN;
	CurrentLimit = MOTOR_CURRENT_BLOCK_OPEN;
	MotorPosition = 0;
	TargetPosition = -1;
	motorMove(DIR_OPEN);
	while(motorIsRunning())
		;
	MotorPosition = MOTOR_POSITION_MAX;
	if(MotorStopSource & ~(STOP_CURRENT | STOP_TIMEOUT))
		return 1;	/* current detection and stop detection are okay */
	
	return 0;
}


/// \brief .
/// 
/// 
/*
 * Closes until detection of touching the vent. close further until fully closed.
 * @return not zero on error
 */
uint8_t motorAdapt(void)
{
	uint16_t currentNormal;
	
	MotorTimeout = MOTOR_SPEED_BLOCK;
	CurrentLimit = MOTOR_CURRENT_BLOCK;
	
	motorMove(DIR_CLOSE);
	/* let vent start moving */
	while(motorIsRunning() && MotorPosition > MOTOR_POSITION_MAX - 11)
		;
	currentNormal = getCurrent();
	
	if(!motorIsRunning())
		return 1;
	
	
	/* wait for a small increase in motor power consumption -> vent touched */
	while(motorIsRunning() && getCurrent() > (currentNormal - MOTOR_CURRENT_VALVE_DETECT))
		;
	PositionValveOpen = MotorPosition;
	
	if(!motorIsRunning())	
		return 1;
	
	/* wait for motor turning off -> vent closed*/
	while(motorIsRunning())
		;
	
	PositionValveClosed = MotorPosition;
	
	/* check for min. vent range, we may have an error in detection */
	if(PositionValveOpen - PositionValveClosed < MOTOR_VENT_RANGE_MIN)
		return 1;
	
	if (MotorStopSource & ~(STOP_CURRENT | STOP_TIMEOUT))
		return 1;
	
	return 0;
}


/// \brief Check and update motor position.
/// 
/// called by opto sensor interrupt
/// 
uint8_t motorStep(void)
{
	// mask sensor pin because pin change interrupt is triggered by all pins on port
	uint8_t state = MOTOR_SENSE_PORT_IN & (1 << MOTOR_SENSE_PIN);
	
	if(((Direction == DIR_OPEN) && state) || ((Direction == DIR_CLOSE) && !state)) 
	{
		setTimeout(MotorTimeout);
		MotorPosition += Direction;
		
		// limits
		if(MotorPosition < 0 || MotorPosition > MOTOR_POSITION_MAX) 
		{
			MotorStopSource |= STOP_POSITION;
			motorStopMove();
		}
		
		// target
		if(	TargetPosition >= 0
				&& (   (Direction == DIR_OPEN && MotorPosition >= TargetPosition)
						|| (Direction == DIR_CLOSE && MotorPosition <= TargetPosition)
						)
			)
		{
			MotorStopSource |= STOP_TARGET;
			TargetPosition = -1;
			motorStopMove();
		}
		return 1;
	}
	return 0;
}


/// \brief .
/// 
/// 
uint8_t motorIsRunning(void)
{
	return (Direction != DIR_STOP);
}


/// \brief .
/// 
/// 
/**
 * periodic callback as long as the motor is running.
 * used for current limit or stop condition tests.
 */
void motorTimer(void)
{
	if(motorIsRunning()) 
	{
		uint16_t current = getCurrent();
		if(current < CurrentLimit) 
		{
			MotorStopSource |= STOP_CURRENT;
			motorStopMove();
		} 
		else 
		{
			if(PWM == 0) 
			{
				switch(Direction)
					{
					case DIR_OPEN:
						MOTOR_OPEN;
						break;
					case DIR_CLOSE:
						MOTOR_CLOSE;
						break;
					default:
						break;
					}
				PWM = 0;
			} 
			else 
			{
				MOTOR_STOP;
				++PWM;
			}
		}
	}
}
