#include <Arduino.h>

#include "utils.h"

void printIP(const uint8_t *ip) {
  for (uint8_t i = 0; i < 4; i++) {
    Serial.print(ip[i]);
    if (i < 3) Serial.print('.');
  }
}

void printMAC(const uint8_t *mac) {
  for (uint8_t i = 0; i < 6; i++) {
    if (mac[i] < 0x10) Serial.print('0');
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
}

bool parseIP(const char *str, uint8_t *out) {
  uint16_t octets[4];
  uint8_t count = 0;
  const char *p = str;
  while (count < 4) {
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p || v < 0 || v > 255) return false;
    octets[count++] = (uint16_t)v;
    if (count < 4) {
      if (*end != '.') return false;
      p = end + 1;
    }
  }
  for (uint8_t i = 0; i < 4; i++) out[i] = (uint8_t)octets[i];
  return true;
}

bool parseMAC(const char *str, uint8_t *out) {
  // Accept XX:XX:XX:XX:XX:XX
  if (strlen(str) != 17) return false;
  for (uint8_t i = 0; i < 6; i++) {
    char hi = str[i * 3];
    char lo = str[i * 3 + 1];
    char sep = (i < 5) ? str[i * 3 + 2] : ':';
    if (i < 5 && sep != ':') return false;
    auto hexVal = [](char c) -> int8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    int8_t h = hexVal(hi);
    int8_t l = hexVal(lo);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}
