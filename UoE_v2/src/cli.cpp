// CLI module implementation for Serial command polling and command handling.

#include <Arduino.h>

#include "cli.h"
#include "config.h"
#include "status.h"
#include "shared_state.h"
#include "utils.h"

void loadDefaults();
void saveConfig();
void rebootNow();
bool sendRemoteStatusRequest();

#define REMOTE_STATUS_TIMEOUT_FLOOR_MS 2000UL

static char cliBuf[CLI_BUF_SIZE];
static uint8_t cliLen = 0;

static uint32_t getRemoteStatusTimeoutMs() {
  uint32_t hbBased = (uint32_t)cfg.hbIntervalSec * 2000UL;  // 2x heartbeat interval
  return (hbBased > REMOTE_STATUS_TIMEOUT_FLOOR_MS) ? hbBased : REMOTE_STATUS_TIMEOUT_FLOOR_MS;
}

static void checkRemoteStatusCompletion() {
  if (remoteStatusResponseReady) {
    remoteStatusResponseReady = false;
    remoteStatusRequestPending = false;
    Serial.println(F("[SYS] Remote status response received."));
    printRemoteStatus(remoteStatusLast);
  }

  if (remoteStatusRequestPending) {
    uint32_t now = millis();
    uint32_t elapsed = now - remoteStatusRequestStartMs;
    if (elapsed > getRemoteStatusTimeoutMs()) {
      remoteStatusRequestPending = false;
      Serial.println(F("[ERR] Remote status request timed out (remote may be disconnected or running older firmware)."));
    }
  }
}

static void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  status               Show current config & stats"));
  Serial.println(F("  get remote status    Request and display remote status"));
  Serial.println(F("  set role <server|client>"));
  Serial.println(F("  set ip <x.x.x.x>     Own IP address"));
  Serial.println(F("  set subnet <x.x.x.x> Subnet mask"));
  Serial.println(F("  set gateway <x.x.x.x> Default gateway"));
  Serial.println(F("  set remote <x.x.x.x> Peer IP address"));
  Serial.println(F("  set mac <XX:XX:..>   Own MAC address"));
  Serial.println(F("  set port <N>         TCP port"));
  Serial.println(F("  set baud <N>         1200/2400/4800/9600/14400/19200/38400/57600/115200"));
  Serial.println(F("    - WARN: baud rates >38400 increase risk of data loss during blocking ops. Use with caution."));
  Serial.println(F("  set hbinterval <N>   Heartbeat interval (seconds)"));
  Serial.println(F("  save                 Write config to EEPROM & reboot"));
  Serial.println(F("  reboot               Reboot now"));
  Serial.println(F("  defaults             Restore factory defaults & reboot"));
  Serial.println(F("  clearerrors          Reset error/reconnect counters"));
  Serial.println(F("  set debug <on|off>   Toggle verbose debug logging"));
  Serial.println(F("  help                 Show this help"));
}

static void processCli(const char *line) {
  // Trim leading spaces
  while (*line == ' ') line++;
  if (*line == '\0') return;

  if (strcasecmp(line, "status") == 0) {
    printStatus();
  }
  else if (strcasecmp(line, "get remote status") == 0) {
    if (!tcpConnected) {
      Serial.println(F("[ERR] TCP is not connected."));
      return;
    }
    if (remoteStatusRequestPending) {
      Serial.println(F("[ERR] Remote status request already pending."));
      return;
    }
    remoteStatusResponseReady = false;
    if (sendRemoteStatusRequest()) {
      remoteStatusRequestPending = true;
      remoteStatusRequestStartMs = millis();
      Serial.println(F("[SYS] Requested remote status. Waiting for response..."));
    } else {
      Serial.println(F("[ERR] Failed to send remote status request."));
    }
  }
  else if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
    printHelp();
  }
  else if (strcasecmp(line, "reboot") == 0) {
    Serial.println(F("[SYS] Rebooting..."));
    rebootNow();
  }
  else if (strcasecmp(line, "defaults") == 0) {
    loadDefaults();
    saveConfig();
    Serial.println(F("[SYS] Defaults restored. Rebooting..."));
    rebootNow();
  }
  else if (strcasecmp(line, "save") == 0) {
    saveConfig();
    Serial.println(F("[SYS] Saved. Rebooting..."));
    rebootNow();
  }
  else if (strcasecmp(line, "clearerrors") == 0) {
    errorCount = 0;
    reconnectCount = 0;
    lastErrorMs = 0;  // Also clear LED timer
    uartBufferOverflowCount = 0;
    uartRxBufPeakUsed = 0;
    peakTcpWriteMs = 0;
    peakTcpReadMs = 0;
    peakTcpConnectMs = 0;
    Serial.println(F("[SYS] Counters cleared."));
  }
  else if (strncasecmp(line, "set ", 4) == 0) {
    const char *arg = line + 4;

    if (strncasecmp(arg, "role ", 5) == 0) {
      const char *val = arg + 5;
      if (strcasecmp(val, "server") == 0) { cfg.role = 0; Serial.println(F("[CFG] Role = SERVER")); }
      else if (strcasecmp(val, "client") == 0) { cfg.role = 1; Serial.println(F("[CFG] Role = CLIENT")); }
      else Serial.println(F("[ERR] Use: set role <server|client>"));
    }
    else if (strncasecmp(arg, "ip ", 3) == 0) {
      if (parseIP(arg + 3, cfg.ip)) {
        Serial.print(F("[CFG] IP = ")); printIP(cfg.ip); Serial.println();
      } else Serial.println(F("[ERR] Invalid IP format."));
    }
    else if (strncasecmp(arg, "remote ", 7) == 0) {
      if (parseIP(arg + 7, cfg.remoteIp)) {
        Serial.print(F("[CFG] Remote = ")); printIP(cfg.remoteIp); Serial.println();
      } else Serial.println(F("[ERR] Invalid IP format."));
    }
    else if (strncasecmp(arg, "subnet ", 7) == 0) {
      if (parseIP(arg + 7, cfg.subnet)) {
        Serial.print(F("[CFG] Subnet = ")); printIP(cfg.subnet); Serial.println();
      } else Serial.println(F("[ERR] Invalid subnet format."));
    }
    else if (strncasecmp(arg, "gateway ", 8) == 0) {
      if (parseIP(arg + 8, cfg.gateway)) {
        Serial.print(F("[CFG] Gateway = ")); printIP(cfg.gateway); Serial.println();
      } else Serial.println(F("[ERR] Invalid gateway format."));
    }
    else if (strncasecmp(arg, "mac ", 4) == 0) {
      if (parseMAC(arg + 4, cfg.mac)) {
        Serial.print(F("[CFG] MAC = ")); printMAC(cfg.mac); Serial.println();
      } else Serial.println(F("[ERR] Invalid MAC format. Use XX:XX:XX:XX:XX:XX"));
    }
    else if (strncasecmp(arg, "port ", 5) == 0) {
      long v = atol(arg + 5);
      if (v > 0 && v <= 65535) {
        cfg.port = (uint16_t)v;
        Serial.print(F("[CFG] Port = ")); Serial.println(cfg.port);
      } else Serial.println(F("[ERR] Port must be 1-65535."));
    }
    else if (strncasecmp(arg, "baud ", 5) == 0) {
      long v = atol(arg + 5);
      if (isSupportedUartBaud((uint32_t)v)) {
        cfg.baud = (uint32_t)v;
        Serial.print(F("[CFG] Baud = ")); Serial.println(cfg.baud);
        if (isHighRiskUartBaud(cfg.baud)) {
          Serial.println(F("[WARN] Baud >38400 may lose data during blocking operations. Use with caution."));
        }
      } else Serial.println(F("[ERR] Baud must be: 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200"));
    }
    else if (strncasecmp(arg, "hbinterval ", 11) == 0) {
      long v = atol(arg + 11);
      if (v >= 1 && v <= 60) {
        cfg.hbIntervalSec = (uint8_t)v;
        Serial.print(F("[CFG] HB interval = ")); Serial.print(cfg.hbIntervalSec); Serial.println(F(" s"));
      } else Serial.println(F("[ERR] HB interval must be 1-60."));
    }
    else if (strncasecmp(arg, "debug ", 6) == 0) {
      const char *val = arg + 6;
      if (strcasecmp(val, "on") == 0) { cfg.debug = 1; Serial.println(F("[CFG] Debug = ON")); }
      else if (strcasecmp(val, "off") == 0) { cfg.debug = 0; Serial.println(F("[CFG] Debug = OFF")); }
      else Serial.println(F("[ERR] Use: set debug <on|off>"));
    }
    else {
      Serial.println(F("[ERR] Unknown parameter. Type 'help'."));
    }
  }
  else {
    Serial.println(F("[ERR] Unknown command. Type 'help'."));
  }
}

void pollCli() {
  checkRemoteStatusCompletion();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (cliLen > 0) {
        cliBuf[cliLen] = '\0';
        processCli(cliBuf);
        cliLen = 0;
      }
    } else if (cliLen < sizeof(cliBuf) - 1) {
      cliBuf[cliLen++] = c;
    }
  }
}
