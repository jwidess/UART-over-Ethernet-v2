// LED module implementation for activity flash, connect blink, and error state.

#include <Arduino.h>

#include "leds.h"
#include "shared_state.h"

static uint32_t lastConnBlinkMs = 0;
static uint32_t activityOffMs = 0;
static bool connLedState = false;

void flashActivity() {
  digitalWrite(PIN_LED_ACTIVITY, HIGH);
  activityOffMs = millis() + ACTIVITY_FLASH_MS;
}

void updateActivityLed() {
  if (activityOffMs && (millis() >= activityOffMs)) {
    digitalWrite(PIN_LED_ACTIVITY, LOW);
    activityOffMs = 0;
  }
}

void updateConnectLed() {
  if (tcpConnected) {
    digitalWrite(PIN_LED_CONNECT, HIGH);
  } else if (ethLinkUp) {
    // Blink while trying to connect
    uint32_t now = millis();
    if ((now - lastConnBlinkMs) >= CONNECT_BLINK_MS) {
      connLedState = !connLedState;
      digitalWrite(PIN_LED_CONNECT, connLedState ? HIGH : LOW);
      lastConnBlinkMs = now;
    }
  } else {
    digitalWrite(PIN_LED_CONNECT, LOW);
  }
}

void updateErrorLed() {
  // Light for ERROR_LED_PERSIST_MS after most recent error, then clear
  bool recentError = (lastErrorMs > 0) && ((millis() - lastErrorMs) < ERROR_LED_PERSIST_MS);
  digitalWrite(PIN_LED_ERROR, recentError ? HIGH : LOW);
}
