/*
    This file is part of AutoQuad.

    AutoQuad is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    AutoQuad is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with AutoQuad.  If not, see <http://www.gnu.org/licenses/>.

    Copyright � 2011, 2012, 2013  Bill Nesbitt
*/

#include "gps.h"
#include "telemetry.h"
#include "serial.h"
#include "digital.h"
#include "nav.h"
#include "comm.h"
#include "aq_timer.h"
#include "config.h"
#include "util.h"
#include "imu.h"
#include "aq_mavlink.h"
#include "filer.h"
#include <CoOS.h>
#include <string.h>

gpsStruct_t gpsData __attribute__((section(".ccm")));

OS_STK *gpsTaskStack;

#ifdef GPS_LOG_BUF
char gpsLog[GPS_LOG_BUF];
#endif

void gpsSendSetup(void) {
    ubloxSendSetup();
}

void gpsCheckBaud(serialPort_t *s) {
    if ((IMU_LASTUPD - gpsData.lastMessage) > 10000000) {
	if (!gpsData.baudCycle[++gpsData.baudSlot])
	    gpsData.baudSlot = 0;
	AQ_NOTICE("GPS: trying new baud rate\n");
	serialChangeBaud(s, gpsData.baudCycle[gpsData.baudSlot]);
	ubloxInitGps();
	serialChangeBaud(s, GPS_BAUD_RATE);
	gpsSendSetup();
	gpsData.lastMessage = IMU_LASTUPD;
    }
}

void gpsTaskCode(void *p) {
    serialPort_t *s = gpsData.gpsPort;
    char c;
    char ledOn;
#ifdef GPS_LOG_BUF
    int logPointer = 0;
#endif
    unsigned int ret = 0;

    AQ_NOTICE("GPS task task started\n");

    ubloxInit();

    while (1) {
	yield(10);
	gpsCheckBaud(s);

	ledOn = digitalGet(gpsData.gpsLed);
	if (!ledOn)
	    digitalHi(gpsData.gpsLed);

	while (serialAvailable(s)) {
	    c = serialRead(s);
	    ret = ubloxCharIn(c);

	    // position update
	    if (ret == 1) {
		// notify world of new data
		CoSetFlag(gpsData.gpsPosFlag);
	    }
	    // velocity update
	    else if (ret == 2) {
		// notify world of new data
		CoSetFlag(gpsData.gpsVelFlag);
	    }
	    // lost sync
	    else if (ret == 3) {
		gpsCheckBaud(s);
	    }

#ifdef GPS_LOG_BUF
	    gpsLog[logPointer] = c;
	    logPointer = (logPointer + 1) % GPS_LOG_BUF;
#endif
	}

#ifdef GPS_LOG_BUF
	filerSetHead(gpsData.logHandle, logPointer);
#endif

	if (!ledOn)
	    digitalLo(gpsData.gpsLed);
    }
}

void gpsPassThrough(commRcvrStruct_t *r) {
    while (commAvailable(r))
	serialWrite(gpsData.gpsPort, commReadChar(r));
}

void gpsInit(void) {
#ifdef GPS_TP_PORT
    GPIO_InitTypeDef GPIO_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;
    EXTI_InitTypeDef EXTI_InitStructure;
#endif

    AQ_NOTICE("GPS init\n");

    memset((void *)&gpsData, 0, sizeof(gpsData));

    gpsData.baudCycle[0] = GPS_BAUD_RATE;
    gpsData.baudCycle[1] = 9600;
    gpsData.baudCycle[2] = 0;

    gpsData.baudSlot = 0;

    gpsData.gpsLed = digitalInit(GPS_LED_PORT, GPS_LED_PIN);
    digitalLo(gpsData.gpsLed);

    gpsData.gpsPort = serialOpen(GPS_USART, GPS_BAUD_RATE, USART_HardwareFlowControl_None, 512, 512);

    // manual reset flags
    gpsData.gpsVelFlag = CoCreateFlag(0, 0);
    gpsData.gpsPosFlag = CoCreateFlag(0, 0);
    gpsTaskStack = aqStackInit(GPS_STACK_SIZE, "GPS");

    gpsData.gpsTask = CoCreateTask(gpsTaskCode, (void *)0, GPS_PRIORITY, &gpsTaskStack[GPS_STACK_SIZE-1], GPS_STACK_SIZE);

#ifdef GPS_TP_PORT
    // External Interrupt line PE1 for timepulse
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPS_TP_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPS_TP_PORT, &GPIO_InitStructure);

    SYSCFG_EXTILineConfig(GPS_TP_PORT_SOURCE, GPS_TP_PIN_SOURCE);

    EXTI_InitStructure.EXTI_Line = GPS_TP_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    // Timepulse interrupt from GPS
    NVIC_InitStructure.NVIC_IRQChannel = GPS_TP_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
#endif

    gpsData.microsPerSecond = AQ_US_PER_SEC<<11;

    gpsData.hAcc = gpsData.vAcc = gpsData.sAcc = 100.0f;

#ifdef GPS_LOG_BUF
    gpsData.logHandle = filerGetHandle(GPS_FNAME);
    filerStream(gpsData.logHandle, gpsLog, GPS_LOG_BUF);
#endif

    commRegisterRcvrFunc(COMM_TYPE_GPS, gpsPassThrough);
}

void gpsSendPacket(unsigned char len, char *buf) {
    unsigned int i;

    for (i = 0; i < len; i++)
	serialWrite(gpsData.gpsPort, buf[i]);
}

#ifdef GPS_TP_HANDLER
void GPS_TP_HANDLER() {
    unsigned long tp = timerMicros();
    unsigned long diff = (tp - gpsData.lastTimepulse);

    if (diff > 950000 && diff < 1050000)
	gpsData.microsPerSecond -= (gpsData.microsPerSecond - (signed long)((tp - gpsData.lastTimepulse)<<11))>>5;
    gpsData.lastTimepulse = tp;
    gpsData.TPtowMS = gpsData.lastReceivedTPtowMS;

    EXTI_ClearITPendingBit(GPS_TP_EXTI_LINE);
}
#endif
