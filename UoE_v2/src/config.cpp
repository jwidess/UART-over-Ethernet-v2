// Config module implementation: EEPROM defaults/load/save behavior.

#include <Arduino.h>
#include <EEPROM.h>

#include "config.h"

#define EEPROM_MAGIC      0xA5
#define EE_MAGIC          0
#define EE_ROLE           1
#define EE_MAC            2
#define EE_IP             8
#define EE_REMOTE_IP      12
#define EE_PORT           16
#define EE_BAUD           18
#define EE_HB_SEC         22
#define EE_DEBUG          23

Config cfg;

void loadDefaults() {
  cfg.role = 0;  // SERVER
  // Default MAC: 12:34:56:78:AB:CD
  cfg.mac[0] = 0x12; cfg.mac[1] = 0x34; cfg.mac[2] = 0x56;
  cfg.mac[3] = 0x78; cfg.mac[4] = 0xAB; cfg.mac[5] = 0xCD;
  // Default IPs: 192.168.254.100, remote=192.168.254.101
  cfg.ip[0] = 192; cfg.ip[1] = 168;
  cfg.ip[2] = 254; cfg.ip[3] = 100;
  cfg.remoteIp[0] = 192; cfg.remoteIp[1] = 168;
  cfg.remoteIp[2] = 254; cfg.remoteIp[3] = 101;
  cfg.port = 3000;
  cfg.baud = 19200;
  cfg.hbIntervalSec = HB_DEFAULT_SEC;
  cfg.debug = 0;
}

void loadConfig() {
  if (EEPROM.read(EE_MAGIC) != EEPROM_MAGIC) {
    Serial.println(F("[EEPROM] No valid config found: loading defaults."));
    loadDefaults();
    return;
  }
  cfg.role = EEPROM.read(EE_ROLE);
  for (uint8_t i = 0; i < 6; i++) cfg.mac[i] = EEPROM.read(EE_MAC + i);
  for (uint8_t i = 0; i < 4; i++) cfg.ip[i] = EEPROM.read(EE_IP + i);
  for (uint8_t i = 0; i < 4; i++) cfg.remoteIp[i] = EEPROM.read(EE_REMOTE_IP + i);
  cfg.port = (uint16_t)EEPROM.read(EE_PORT) | ((uint16_t)EEPROM.read(EE_PORT + 1) << 8);
  cfg.baud = (uint32_t)EEPROM.read(EE_BAUD)
           | ((uint32_t)EEPROM.read(EE_BAUD + 1) << 8)
           | ((uint32_t)EEPROM.read(EE_BAUD + 2) << 16)
           | ((uint32_t)EEPROM.read(EE_BAUD + 3) << 24);
  cfg.hbIntervalSec = EEPROM.read(EE_HB_SEC);
  if (cfg.hbIntervalSec == 0 || cfg.hbIntervalSec > 60) cfg.hbIntervalSec = HB_DEFAULT_SEC;
  cfg.debug = (EEPROM.read(EE_DEBUG) == 1) ? 1 : 0;
  Serial.println(F("[EEPROM] Valid config loaded!"));
}

void saveConfig() {
  EEPROM.update(EE_MAGIC, EEPROM_MAGIC);
  EEPROM.update(EE_ROLE, cfg.role);
  for (uint8_t i = 0; i < 6; i++) EEPROM.update(EE_MAC + i, cfg.mac[i]);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EE_IP + i, cfg.ip[i]);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EE_REMOTE_IP + i, cfg.remoteIp[i]);
  EEPROM.update(EE_PORT, (uint8_t)(cfg.port & 0xFF));
  EEPROM.update(EE_PORT + 1, (uint8_t)(cfg.port >> 8));
  EEPROM.update(EE_BAUD, (uint8_t)(cfg.baud & 0xFF));
  EEPROM.update(EE_BAUD + 1, (uint8_t)((cfg.baud >> 8) & 0xFF));
  EEPROM.update(EE_BAUD + 2, (uint8_t)((cfg.baud >> 16) & 0xFF));
  EEPROM.update(EE_BAUD + 3, (uint8_t)((cfg.baud >> 24) & 0xFF));
  EEPROM.update(EE_HB_SEC, cfg.hbIntervalSec);
  EEPROM.update(EE_DEBUG, cfg.debug);
  Serial.println(F("[EEPROM] Config saved."));
}
