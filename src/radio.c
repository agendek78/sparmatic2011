/// \file funk.c
/// 
/// 
/// 

/*
 * high layer rf protocol.
 */

#include <stdint.h>
#include "nRF24L01.h"
#include "radio.h"
#include "ntc.h"
#include "timer.h"
#include "lcd.h"
#include "motor.h"
#include "adc.h"
#include "control.h"
#include "programming.h"

/* from main */
extern uint16_t BatteryMV;



/// \brief .
/// 
/// 
void radioRxDataAvailable(void)
{
	uint8_t rxData[32];
	uint8_t readLen;
	readLen = nRF24L01_get_data(rxData);
}



/// \brief .
/// 
/// 
/**
 * initializes lower level rf protocol, sets high level address.
 * @param ownAddress high level address: 2-255
 */
void radioInit(void)
{
	nRF24L01_init();
	nRF24L01_set_RADDR_01(0, ThermostatAdr);
	nRF24L01_set_TADDR(ThermostatAdr);
	nRF24L01_set_rx_callback(&radioRxDataAvailable);
}


/// \brief .
/// 
/// 
/*
void txPacket(uint8_t adr, MESSAGE_TYPE type, uint8_t *data)
{
	nRF24L01_wakeUp(0);
	nRF24L01_send(data, 32, 1);
}
*/


/// \brief .
/// 
/// 
void radioSend(void)
{
	MSG_FROM_THRM msg;
	TIME time = getTime();
	msg.info.temperatureActual = getNtcTemperature();
	msg.info.valve = getMotorPosition();
	msg.info.battery = getBatteryVoltage();
	msg.info.temperatureNominal = targetTemperature;
	msg.time.day = time.weekday;
	msg.time.hour = time.hour;
	msg.time.minute = time.minute;

	nRF24L01_send((uint8_t *)&msg, sizeof(msg), 0);
}
