#pragma once

#include <Arduino.h>

void printIP(const uint8_t *ip);
void printMAC(const uint8_t *mac);
bool parseIP(const char *str, uint8_t *out);
bool parseMAC(const char *str, uint8_t *out);
