// Status module: uptime and human-readable status output.

#pragma once

#include <Arduino.h>

struct RemoteStatusPayload {
	uint8_t version;
	uint8_t flags;
	uint16_t uartRxBufPeakUsed;
	uint32_t uptimeTotalSec;
	uint32_t reconnectCount;
	uint32_t errorCount;
	uint32_t uartBufferOverflowCount;
	uint32_t bytesRxUart;
	uint32_t bytesTxUart;
	uint32_t bytesRxTcp;
	uint32_t bytesTxTcp;
	uint32_t peakTcpWriteMs;
	uint32_t peakTcpReadMs;
	uint32_t peakTcpConnectMs;
} __attribute__((packed));

void buildRemoteStatusPayload(RemoteStatusPayload &payload);
bool parseRemoteStatusPayload(const uint8_t *buf, uint8_t len, RemoteStatusPayload &outPayload);
void printRemoteStatus(const RemoteStatusPayload &payload);

void updateUptime();
void printUptime();
void printStatus();
