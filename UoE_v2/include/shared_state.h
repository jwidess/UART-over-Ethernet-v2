// Shared state declarations used across modules.
// Definitions remain in main.cpp, this header just avoids duplicate extern blocks.

#pragma once

#include <Arduino.h>

extern bool tcpConnected;
extern bool ethLinkUp;

extern uint32_t uptimeTotalSec;
extern uint32_t lastUptimeTickMs;
extern uint32_t lastErrorMs;

extern uint32_t reconnectCount;
extern uint32_t errorCount;
extern uint32_t bytesRxUart;
extern uint32_t bytesTxUart;
extern uint32_t bytesRxTcp;
extern uint32_t bytesTxTcp;
extern uint32_t peakTcpWriteMs;
extern uint32_t peakTcpConnectMs;
extern uint32_t peakTcpReadMs;
extern uint32_t uartBufferOverflowCount;
extern uint16_t uartRxBufPeakUsed;
