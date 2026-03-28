// Status module implementation: uptime tracking and status reports.
// Reads shared runtime counters/state and prints diagnostic summaries.

#include <Arduino.h>

#include "fw_version.h"
#include "config.h"
#include "status.h"
#include "shared_state.h"
#include "utils.h"

void updateUptime() {
  uint32_t now = millis();

  // Unsigned subtraction is overflow-safe, so this works when millis() wraps around after ~49 days.
  uint32_t elapsed = (now - lastUptimeTickMs);

  if (elapsed >= 1000UL) {
    uint32_t secs = (elapsed / 1000UL);  // Whole seconds elapsed since last update.
    uptimeTotalSec = (uptimeTotalSec + secs);
    lastUptimeTickMs = (lastUptimeTickMs + (secs * 1000UL));
  }
}

void printUptime() {
  uint32_t t = uptimeTotalSec;

  // Calc days/hours/minutes/seconds
  uint16_t d = (t / 86400UL);
  t = (t % 86400UL);
  uint8_t h = (t / 3600UL);
  t = (t % 3600UL);
  uint8_t m = (t / 60UL);
  uint8_t s = (t % 60UL);

  if (d > 0) {
    Serial.print(d);
    Serial.print(F("d "));
  }

  Serial.print(h);
  Serial.print(F("h "));
  Serial.print(m);
  Serial.print(F("m "));
  Serial.print(s);
  Serial.print(F("s"));
}

void printStatus() {
  Serial.println(F("=== UART-over-Ethernet Bridge ==="));
  Serial.print(F("  Firmware : v")); Serial.println(F(FW_VERSION));
  Serial.print(F("  Role     : ")); Serial.println(cfg.role == 0 ? F("SERVER") : F("CLIENT"));
  Serial.print(F("  MAC      : ")); printMAC(cfg.mac); Serial.println();
  Serial.print(F("  IP       : ")); printIP(cfg.ip); Serial.println();
  Serial.print(F("  Subnet   : ")); printIP(cfg.subnet); Serial.println();
  Serial.print(F("  Gateway  : ")); printIP(cfg.gateway); Serial.println();
  Serial.print(F("  Remote   : ")); printIP(cfg.remoteIp); Serial.println();
  Serial.print(F("  TCP Port : ")); Serial.println(cfg.port);
  Serial.print(F("  UART Baud: "));
  Serial.print(cfg.baud);
  if (isHighRiskUartBaud(cfg.baud)) {
    Serial.print(F(" (WARN: >38400 may lose data during blocking ops)"));
  }
  Serial.println();
  Serial.print(F("  HB Int.  : ")); Serial.print(cfg.hbIntervalSec); Serial.println(F(" s"));
  Serial.print(F("  Debug    : ")); Serial.println(cfg.debug ? F("ON") : F("OFF"));
  Serial.println(F("--- Network ---"));
  Serial.print(F("  Eth Link : ")); Serial.println(ethLinkUp ? F("UP") : F("DOWN"));
  Serial.print(F("  TCP      : ")); Serial.println(tcpConnected ? F("CONNECTED") : F("DISCONNECTED"));
  Serial.print(F("  Reconnects: ")); Serial.println(reconnectCount);
  Serial.print(F("  Errors    : ")); Serial.println(errorCount);
  Serial.println(F("--- Data ---"));
  Serial.print(F("  UART RX  : ")); Serial.print(bytesRxUart); Serial.println(F(" B"));
  Serial.print(F("  UART TX  : ")); Serial.print(bytesTxUart); Serial.println(F(" B"));
  Serial.print(F("  TCP  RX  : ")); Serial.print(bytesRxTcp); Serial.println(F(" B"));
  Serial.print(F("  TCP  TX  : ")); Serial.print(bytesTxTcp); Serial.println(F(" B"));
  if (cfg.debug) {
    Serial.println(F("--- TCP Timing ---"));
    Serial.print(F("  Write Peak: ")); Serial.print(peakTcpWriteMs); Serial.println(F(" ms"));
    Serial.print(F("  Read  Peak: ")); Serial.print(peakTcpReadMs); Serial.println(F(" ms"));
    Serial.print(F("  Conn  Peak: ")); Serial.print(peakTcpConnectMs); Serial.println(F(" ms"));
  }
  Serial.println(F("--- UART Health ---"));
  Serial.print(F("  RX Buf Overflows: ")); Serial.println(uartBufferOverflowCount);
  Serial.print(F("  RX Buf Peak Used: ")); Serial.print(uartRxBufPeakUsed);
  Serial.print(F(" / ")); Serial.println(SERIAL_RX_BUFFER_SIZE);
  Serial.println(F("-------------------"));
  Serial.print(F("  Uptime   : ")); printUptime(); Serial.println();
  Serial.println(F("================================="));
}
