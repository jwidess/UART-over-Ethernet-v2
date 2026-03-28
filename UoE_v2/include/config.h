// Config module: runtime settings structure plus EEPROM load/save APIs.

#pragma once

#include <Arduino.h>

// Heartbeat defaults used by config sanitization.
#define HB_DEFAULT_SEC 5
#define HB_MISS_FACTOR 3

struct Config {
  uint8_t role;  // 0 = SERVER, 1 = CLIENT
  uint8_t mac[6];
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];
  uint8_t remoteIp[4];
  uint16_t port;
  uint32_t baud;
  uint8_t hbIntervalSec;
  uint8_t debug;  // 0 = off, 1 = on
};

extern Config cfg;

void loadDefaults();
void loadConfig();
void saveConfig();
