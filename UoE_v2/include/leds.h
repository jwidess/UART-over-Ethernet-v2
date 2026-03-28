#pragma once

#include <Arduino.h>

#define PIN_LED_ACTIVITY  LED_BUILTIN   // Pin 13 - flash on UART/TCP activity
#define PIN_LED_CONNECT   17            // Solid = TCP up, blink = connecting
#define PIN_LED_ERROR     16            // Solid = errors present

#define ACTIVITY_FLASH_MS    30      // LED_BUILTIN on-time per event
#define CONNECT_BLINK_MS     500     // Connection LED blink half-period
#define ERROR_LED_PERSIST_MS 10000   // Error LED on for 10s after last error

void flashActivity();
void updateActivityLed();
void updateConnectLed();
void updateErrorLed();
