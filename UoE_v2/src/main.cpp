/**
 * UART-over-Ethernet Transparent Bridge
 * Hardware: Arduino Mega 2560 + W5100 Ethernet Shield + MAX3232 on Serial1
 *
 * Single firmware for both nodes (SERVER / CLIENT).
 * All configurable params stored in EEPROM, edited via Serial0 commands.
 */

#include <SPI.h>
#include <Ethernet.h>
#include <EEPROM.h>
#include <avr/wdt.h>

// ─── Firmware Version ────────────────────────────────────────────────────────
#define FW_VERSION "2.0.0"

// ─── Pin Definitions ─────────────────────────────────────────────────────────
#define PIN_LED_ACTIVITY  LED_BUILTIN   // Pin 13 - flash on UART/TCP activity
#define PIN_LED_CONNECT   17            // Solid = TCP up, blink = connecting
#define PIN_LED_ERROR     16            // Solid = errors present

// ─── Constants ───────────────────────────────────────────────────────────────
#define SERIAL0_BAUD         115200
// Ref build flag `-DSERIAL_RX_BUFFER_SIZE=256` in platformio.ini
#define UART_BUF_SIZE        256     // Max bytes to read from UART per loop
#define CLI_BUF_SIZE         80      // Max bytes to read from CLI per loop
#define TCP_RX_CHUNK         64      // Max bytes to read from TCP per loop
#define UART_IDLE_MS         20      // Flush UART buffer after this idle gap
#define ACTIVITY_FLASH_MS    30      // LED_BUILTIN on-time per event
#define CONNECT_BLINK_MS     500     // Connection LED blink half-period
#define ERROR_LED_PERSIST_MS 10000   // Error LED on for 10s after last error
#define WDT_TIMEOUT          WDTO_4S

// ─── Framing Protocol ────────────────────────────────────────────────────────
// TCP frames: [TYPE (1 byte)][LEN (1 byte)][PAYLOAD (0-255 bytes)]
// This allows for NUL bytes to pass through the data stream.
#define FRAME_TYPE_DATA   0x44      // 'D' data frame
#define FRAME_TYPE_HB     0x48      // 'H' heartbeat frame (LEN=0, no payload)

// ─── Client reconnect back-off ───────────────────────────────────────────────
#define BACKOFF_INIT_MS   1000
#define BACKOFF_MAX_MS    10000  // Cap at 10s

// ─── Heartbeat defaults ──────────────────────────────────────────────────────
#define HB_DEFAULT_SEC    5
#define HB_MISS_FACTOR    3         // Dead after 3x interval with no HB

// ─── EEPROM Layout ───────────────────────────────────────────────────────────
#define EEPROM_MAGIC      0xA5 // Identifies EEPROM containing valid config data
#define EE_MAGIC          0   // 1 byte
#define EE_ROLE           1   // 1 byte  (0=SERVER, 1=CLIENT)
#define EE_MAC            2   // 6 bytes
#define EE_IP             8   // 4 bytes
#define EE_REMOTE_IP      12  // 4 bytes
#define EE_PORT           16  // 2 bytes (uint16)
#define EE_BAUD           18  // 4 bytes (uint32)
#define EE_HB_SEC         22  // 1 byte
// Total: 23 bytes

// ─── Runtime Config ──────────────────────────────────────────────────────────
struct Config {
  uint8_t  role;  // 0 = SERVER, 1 = CLIENT
  uint8_t  mac[6];
  uint8_t  ip[4];
  uint8_t  remoteIp[4];
  uint16_t port;
  uint32_t baud;
  uint8_t  hbIntervalSec;
};

static Config cfg;

// ─── Network Objects ─────────────────────────────────────────────────────────
static EthernetServer *tcpServer = nullptr;
static EthernetClient  tcpClient;

// ─── Buffers ─────────────────────────────────────────────────────────────────
static uint8_t uartBuf[UART_BUF_SIZE];
static uint16_t uartBufLen = 0;

static char cliBuf[CLI_BUF_SIZE];
static uint8_t cliLen = 0;

// ─── Timing ──────────────────────────────────────────────────────────────────
static unsigned long lastUartByteMs   = 0;
static unsigned long lastHbSentMs     = 0;
static unsigned long lastHbRecvMs     = 0;
static unsigned long lastConnBlinkMs  = 0;
static unsigned long activityOffMs    = 0;
static unsigned long bootTimeMs       = 0;
static unsigned long backoffMs        = BACKOFF_INIT_MS;
static unsigned long lastConnectAttemptMs = 0;
static unsigned long lastErrorMs          = 0;

// ─── Stats ───────────────────────────────────────────────────────────────────
static uint32_t reconnectCount = 0;
static uint32_t errorCount     = 0;
static uint32_t bytesRxUart    = 0;
static uint32_t bytesTxUart    = 0;
static uint32_t bytesRxTcp     = 0;
static uint32_t bytesTxTcp     = 0;

// ─── State ───────────────────────────────────────────────────────────────────
static bool tcpConnected      = false;
static bool prevTcpConnected  = false;
static bool connLedState      = false;
static bool ethLinkUp         = false;
static bool debugMode         = false;

// ─── TCP RX framing state machine ────────────────────────────────────────────
enum RxState : uint8_t { RX_WAIT_TYPE, RX_WAIT_LEN, RX_READ_PAYLOAD };
static RxState   rxState      = RX_WAIT_TYPE;
static uint8_t   rxFrameType  = 0;
static uint8_t   rxFrameLen   = 0;
static uint8_t   rxPayloadIdx = 0;
static uint8_t   rxPayload[UART_BUF_SIZE];

// ─── Error helper ────────────────────────────────────────────────────────────
static inline void noteError() {
  errorCount++;
  lastErrorMs = millis();
}

static void rebootNow() {
  Serial.flush();
  wdt_enable(WDTO_15MS);
  while (1) {} // Wait for watchdog reset
}

// =============================================================================
//  EEPROM helpers
// =============================================================================

static void loadDefaults() {
  cfg.role = 0; // SERVER
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
}

static void loadConfig() {
  if (EEPROM.read(EE_MAGIC) != EEPROM_MAGIC) {
    Serial.println(F("[EEPROM] No valid config found: loading defaults."));
    loadDefaults();
    return;
  }
  cfg.role = EEPROM.read(EE_ROLE);
  for (uint8_t i = 0; i < 6; i++) cfg.mac[i] = EEPROM.read(EE_MAC + i);
  for (uint8_t i = 0; i < 4; i++) cfg.ip[i]  = EEPROM.read(EE_IP + i);
  for (uint8_t i = 0; i < 4; i++) cfg.remoteIp[i] = EEPROM.read(EE_REMOTE_IP + i);
  cfg.port  = (uint16_t)EEPROM.read(EE_PORT) | ((uint16_t)EEPROM.read(EE_PORT + 1) << 8);
  cfg.baud  = (uint32_t)EEPROM.read(EE_BAUD)
            | ((uint32_t)EEPROM.read(EE_BAUD + 1) << 8)
            | ((uint32_t)EEPROM.read(EE_BAUD + 2) << 16)
            | ((uint32_t)EEPROM.read(EE_BAUD + 3) << 24);
  cfg.hbIntervalSec = EEPROM.read(EE_HB_SEC);
  if (cfg.hbIntervalSec == 0 || cfg.hbIntervalSec > 60) cfg.hbIntervalSec = HB_DEFAULT_SEC;
  Serial.println(F("[EEPROM] Valid config loaded!"));
}

static void saveConfig() {
  EEPROM.update(EE_MAGIC, EEPROM_MAGIC);
  EEPROM.update(EE_ROLE, cfg.role);
  for (uint8_t i = 0; i < 6; i++) EEPROM.update(EE_MAC + i, cfg.mac[i]);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EE_IP + i, cfg.ip[i]);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EE_REMOTE_IP + i, cfg.remoteIp[i]);
  EEPROM.update(EE_PORT,     (uint8_t)(cfg.port & 0xFF));
  EEPROM.update(EE_PORT + 1, (uint8_t)(cfg.port >> 8));
  EEPROM.update(EE_BAUD,     (uint8_t)(cfg.baud & 0xFF));
  EEPROM.update(EE_BAUD + 1, (uint8_t)((cfg.baud >> 8) & 0xFF));
  EEPROM.update(EE_BAUD + 2, (uint8_t)((cfg.baud >> 16) & 0xFF));
  EEPROM.update(EE_BAUD + 3, (uint8_t)((cfg.baud >> 24) & 0xFF));
  EEPROM.update(EE_HB_SEC, cfg.hbIntervalSec);
  Serial.println(F("[EEPROM] Config saved."));
}

// =============================================================================
//  Print helper utils
// =============================================================================

static void printIP(const uint8_t *ip) {
  for (uint8_t i = 0; i < 4; i++) {
    Serial.print(ip[i]);
    if (i < 3) Serial.print('.');
  }
}

static void printMAC(const uint8_t *mac) {
  for (uint8_t i = 0; i < 6; i++) {
    if (mac[i] < 0x10) Serial.print('0');
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
}

static bool parseIP(const char *str, uint8_t *out) {
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

static bool parseMAC(const char *str, uint8_t *out) {
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

// =============================================================================
//  Activity LED
// =============================================================================

static void flashActivity() {
  digitalWrite(PIN_LED_ACTIVITY, HIGH);
  activityOffMs = millis() + ACTIVITY_FLASH_MS;
}

static void updateActivityLed() {
  if (activityOffMs && (millis() >= activityOffMs)) {
    digitalWrite(PIN_LED_ACTIVITY, LOW);
    activityOffMs = 0;
  }
}

// =============================================================================
//  Connection & Error LEDs
// =============================================================================

static void updateConnectLed() {
  if (tcpConnected) {
    digitalWrite(PIN_LED_CONNECT, HIGH);
  } else if (ethLinkUp) {
    // Blink while trying to connect
    unsigned long now = millis();
    if ((now - lastConnBlinkMs) >= CONNECT_BLINK_MS) {
      connLedState = !connLedState;
      digitalWrite(PIN_LED_CONNECT, connLedState ? HIGH : LOW);
      lastConnBlinkMs = now;
    }
  } else {
    digitalWrite(PIN_LED_CONNECT, LOW);
  }
}

static void updateErrorLed() {
  // Light for ERROR_LED_PERSIST_MS after most recent error, then clear
  bool recentError = (lastErrorMs > 0) && ((millis() - lastErrorMs) < ERROR_LED_PERSIST_MS);
  digitalWrite(PIN_LED_ERROR, recentError ? HIGH : LOW);
}

// =============================================================================
//  Status display
// =============================================================================

static void printStatus() {
  unsigned long upSec = (millis() - bootTimeMs) / 1000UL; // Uptime in seconds

  Serial.println(F("=== UART-over-Ethernet Bridge ==="));
  Serial.print(F("  Firmware : v")); Serial.println(F(FW_VERSION));
  Serial.print(F("  Role     : ")); Serial.println(cfg.role == 0 ? F("SERVER") : F("CLIENT"));
  Serial.print(F("  MAC      : ")); printMAC(cfg.mac); Serial.println();
  Serial.print(F("  IP       : ")); printIP(cfg.ip);   Serial.println();
  Serial.print(F("  Remote   : ")); printIP(cfg.remoteIp); Serial.println();
  Serial.print(F("  TCP Port : ")); Serial.println(cfg.port);
  Serial.print(F("  UART Baud: ")); Serial.println(cfg.baud);
  Serial.print(F("  HB Int.  : ")); Serial.print(cfg.hbIntervalSec); Serial.println(F(" s"));
  Serial.println(F("--- Network ---"));
  Serial.print(F("  Eth Link : ")); Serial.println(ethLinkUp ? F("UP") : F("DOWN"));
  Serial.print(F("  TCP      : ")); Serial.println(tcpConnected ? F("CONNECTED") : F("DISCONNECTED"));
  Serial.print(F("  Reconnects: ")); Serial.println(reconnectCount);
  Serial.print(F("  Errors    : ")); Serial.println(errorCount);
  Serial.println(F("--- Data ---"));
  Serial.print(F("  UART RX  : ")); Serial.print(bytesRxUart); Serial.println(F(" B"));
  Serial.print(F("  UART TX  : ")); Serial.print(bytesTxUart); Serial.println(F(" B"));
  Serial.print(F("  TCP  RX  : ")); Serial.print(bytesRxTcp);  Serial.println(F(" B"));
  Serial.print(F("  TCP  TX  : ")); Serial.print(bytesTxTcp);  Serial.println(F(" B"));
  Serial.print(F("  Uptime   : ")); Serial.print(upSec); Serial.println(F(" s"));
  Serial.println(F("================================="));
}

// =============================================================================
//  CLI processor
// =============================================================================

static void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  status               Show current config & stats"));
  Serial.println(F("  set role <server|client>"));
  Serial.println(F("  set ip <x.x.x.x>     Own IP address"));
  Serial.println(F("  set remote <x.x.x.x> Peer IP address"));
  Serial.println(F("  set mac <XX:XX:..>   Own MAC address"));
  Serial.println(F("  set port <N>         TCP port"));
  Serial.println(F("  set baud <N>         1200/2400/4800/9600/14400/19200"));
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
      // Only standard baud rates up to 19200
      if (v == 1200 || v == 2400 || v == 4800 || v == 9600 || v == 14400 || v == 19200) {
        cfg.baud = (uint32_t)v;
        Serial.print(F("[CFG] Baud = ")); Serial.println(cfg.baud);
      } else Serial.println(F("[ERR] Baud must be: 1200, 2400, 4800, 9600, 14400, 19200"));
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
      if (strcasecmp(val, "on") == 0) { debugMode = true; Serial.println(F("[CFG] Debug = ON")); }
      else if (strcasecmp(val, "off") == 0) { debugMode = false; Serial.println(F("[CFG] Debug = OFF")); }
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

static void pollCli() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (cliLen > 0) {
        cliBuf[cliLen] = '\0';
        processCli(cliBuf);
        cliLen = 0;
      }
    } else if (cliLen < CLI_BUF_SIZE - 1) {
      cliBuf[cliLen++] = c;
    }
  }
}

// =============================================================================
//  TCP connection management
// =============================================================================

static void disconnectTcp() {
  if (tcpClient) {
    tcpClient.stop();
  }
  tcpConnected = false;
  backoffMs = BACKOFF_INIT_MS;
  // Reset framing state machine for next connection
  rxState = RX_WAIT_TYPE;
  rxPayloadIdx = 0;
}

static void handleServerAccept() {
  // NOTE: Must use accept(), NOT available().
  // In Ethernet library 2.0+, available() does not transfer socket ownership
  // to the caller, the server keeps managing/recycling the socket, causing
  // disconnects after a few seconds. accept() properly hands off the socket.
  EthernetClient incoming = tcpServer->accept();
  if (incoming) {
    if (tcpConnected) {
      // Already have client, reject new one
      Serial.println(F("[TCP-DBG] Rejected additional client (already connected)."));
      incoming.stop();
      return;
    }
    tcpClient = incoming;
    tcpConnected = true;
    lastHbRecvMs = millis();
    lastHbSentMs = millis();
    Serial.print(F("[TCP] Client connected from "));
    IPAddress remote = tcpClient.remoteIP();
    Serial.print(remote);
    Serial.println();
  }
}

static void handleClientConnect() {
  if (tcpConnected) return;

  unsigned long now = millis();
  if (now - lastConnectAttemptMs < backoffMs) return;
  lastConnectAttemptMs = now;

  IPAddress remote(cfg.remoteIp[0], cfg.remoteIp[1], cfg.remoteIp[2], cfg.remoteIp[3]);
  Serial.print(F("[TCP] Connecting to "));
  Serial.print(remote);
  Serial.print(':');
  Serial.print(cfg.port);
  Serial.println(F("..."));

  int result = tcpClient.connect(remote, cfg.port);
  if (result == 1) {
    tcpConnected = true;
    lastHbRecvMs = millis();
    lastHbSentMs = millis();
    backoffMs = BACKOFF_INIT_MS;
    Serial.println(F("[TCP] Connected!"));
  } else {
    noteError();
    Serial.print(F("[TCP] Connect failed (code "));
    Serial.print(result);
    Serial.println(F("). Backing off..."));
    // Exponential back off capped at BACKOFF_MAX_MS
    backoffMs = min(backoffMs * 2, (unsigned long)BACKOFF_MAX_MS);
  }
}

static void checkTcpConnection() {
  if (!tcpConnected) return;

  // Check if socket is still connected
  if (!tcpClient.connected()) {
    Serial.print(F("[TCP] Connection lost (socket closed). status="));
    Serial.println(tcpClient.status(), HEX);
    disconnectTcp();
    reconnectCount++;
    noteError();
    return;
  }

  // Check heartbeat timeout
  unsigned long now = millis();
  unsigned long hbTimeoutMs = (unsigned long)cfg.hbIntervalSec * HB_MISS_FACTOR * 1000UL;
  if (now - lastHbRecvMs > hbTimeoutMs) {
    Serial.print(F("[TCP] Heartbeat timeout (last HB "));
    Serial.print((now - lastHbRecvMs) / 1000);
    Serial.println(F("s ago) - disconnecting."));
    disconnectTcp();
    reconnectCount++;
    noteError();
  }
}

// =============================================================================
//  Heartbeat
// =============================================================================

static void sendHeartbeat() {
  if (!tcpConnected) return;
  unsigned long now = millis();
  if (now - lastHbSentMs >= (unsigned long)cfg.hbIntervalSec * 1000UL) {
    uint8_t hbFrame[2] = { FRAME_TYPE_HB, 0x00 };
    tcpClient.write(hbFrame, 2);
    lastHbSentMs = now;
    Serial.println(F("[HB-DBG] Sent heartbeat"));
  }
}

// =============================================================================
//  Data bridge
// =============================================================================

static void bridgeUartToTcp() {
  // Read available UART bytes into buffer
  while (Serial1.available() && uartBufLen < UART_BUF_SIZE) {
    uartBuf[uartBufLen++] = Serial1.read();
    bytesRxUart++;
    lastUartByteMs = millis();
    flashActivity();
  }

  if (uartBufLen == 0) return;

  // Decide whether to flush
  bool shouldFlush = false;

  // Check for CR or LF in buffer
  for (uint16_t i = 0; i < uartBufLen; i++) {
    if (uartBuf[i] == '\r' || uartBuf[i] == '\n') {
      shouldFlush = true;
      break;
    }
  }

  // Check idle timeout
  if (!shouldFlush && (millis() - lastUartByteMs >= UART_IDLE_MS)) {
    shouldFlush = true;
  }

  // Buffer full, force flush
  if (uartBufLen >= UART_BUF_SIZE) {
    shouldFlush = true;
  }

  if (shouldFlush && tcpConnected) {
    // DEBUG: show what we're reading from Serial1
    if (debugMode) {
      Serial.print(F("[UART-DBG] TX "));
      Serial.print(uartBufLen);
      Serial.print(F("B: "));
      for (uint16_t i = 0; i < min(uartBufLen, (uint16_t)16); i++) {
        if (uartBuf[i] < 0x10) Serial.print('0');
        Serial.print(uartBuf[i], HEX);
        Serial.print(' ');
      }
      if (uartBufLen > 16) Serial.print(F("..."));
      Serial.println();
    }

    // Send as framed data: [TYPE=0x44][LEN][PAYLOAD]
    // If buffer > 255 bytes we'd need multiple frames, but UART_BUF_SIZE=256
    // and a uint8_t len can hold 0-255, so we cap at 255 per frame.
    uint16_t offset = 0;
    while (offset < uartBufLen) {
      uint8_t chunkLen = (uartBufLen - offset > 255) ? 255 : (uint8_t)(uartBufLen - offset);
      uint8_t header[2] = { FRAME_TYPE_DATA, chunkLen };
      tcpClient.write(header, 2);
      size_t sent = tcpClient.write(uartBuf + offset, chunkLen);
      bytesTxTcp += sent;
      if (sent < chunkLen) {
        noteError();
        Serial.print(F("[WARN] Partial TCP write: "));
        Serial.print(sent);
        Serial.print('/');
        Serial.println(chunkLen);
      }
      offset += chunkLen;
    }
    flashActivity();
    uartBufLen = 0;
  } else if (shouldFlush && !tcpConnected) {
    // TCP not up, discard buffered data
    uartBufLen = 0;
  }
}

static void bridgeTcpToUart() {
  if (!tcpConnected) return;

  int avail = tcpClient.available();
  if (avail <= 0) return;

  uint8_t chunk[TCP_RX_CHUNK];
  int toRead = min(avail, (int)TCP_RX_CHUNK);
  int n = tcpClient.read(chunk, toRead);
  if (n <= 0) return;

  bytesRxTcp += n;

  // Process bytes through framing state machine
  for (int i = 0; i < n; i++) {
    uint8_t b = chunk[i];
    switch (rxState) {
      case RX_WAIT_TYPE:
        if (b == FRAME_TYPE_DATA || b == FRAME_TYPE_HB) {
          rxFrameType = b;
          rxState = RX_WAIT_LEN;
        } else {
          // Unknown frame type, skip / log
          Serial.print(F("[FRAME-DBG] Unknown type: 0x"));
          Serial.println(b, HEX);
          noteError();
        }
        break;

      case RX_WAIT_LEN:
        rxFrameLen = b;
        rxPayloadIdx = 0;
        if (rxFrameLen == 0) {
          // Zero length frame, heartbeat or empty data
          if (rxFrameType == FRAME_TYPE_HB) {
            lastHbRecvMs = millis();
            Serial.println(F("[HB-DBG] Received heartbeat"));
          }
          rxState = RX_WAIT_TYPE;
        } else {
          rxState = RX_READ_PAYLOAD;
        }
        break;

      case RX_READ_PAYLOAD:
        rxPayload[rxPayloadIdx++] = b;
        if (rxPayloadIdx >= rxFrameLen) {
          // Full frame rxd
          if (rxFrameType == FRAME_TYPE_DATA) {
            // DEBUG: show what we're forwarding to Serial1
            if (debugMode) {
              Serial.print(F("[TCP-DBG] RX data "));
              Serial.print(rxFrameLen);
              Serial.print(F("B: "));
              for (uint8_t j = 0; j < min(rxFrameLen, (uint8_t)16); j++) {
                if (rxPayload[j] < 0x10) Serial.print('0');
                Serial.print(rxPayload[j], HEX);
                Serial.print(' ');
              }
              if (rxFrameLen > 16) Serial.print(F("..."));
              Serial.println();
            }
            Serial1.write(rxPayload, rxFrameLen);
            bytesTxUart += rxFrameLen;
            flashActivity();
          } else if (rxFrameType == FRAME_TYPE_HB) {
            lastHbRecvMs = millis();
          }
          rxState = RX_WAIT_TYPE;
        }
        break;
    }
  }
}

// =============================================================================
//  Ethernet link check
// =============================================================================

static void checkEthLink() {
  auto link = Ethernet.linkStatus();
  bool up = (link == LinkON) || (link == Unknown);
  // W5100 doesn't properly report link status so Unknown = assume up
  if (up != ethLinkUp) {
    ethLinkUp = up;
    if (ethLinkUp) {
      Serial.println(F("[ETH] Link UP"));
    } else {
      Serial.println(F("[ETH] Link DOWN"));
      if (tcpConnected) {
        disconnectTcp();
        reconnectCount++;
        noteError();
      }
    }
  }
}

// =============================================================================
//  setup()
// =============================================================================

void setup() {
  // Disable WDT in case the bootloader left on (Optiboot could fix this maybe?)
  MCUSR = 0;
  wdt_disable();

  // LED pins
  pinMode(PIN_LED_ACTIVITY, OUTPUT);
  pinMode(PIN_LED_CONNECT, OUTPUT);
  pinMode(PIN_LED_ERROR, OUTPUT);
  digitalWrite(PIN_LED_ACTIVITY, LOW);
  digitalWrite(PIN_LED_CONNECT, LOW);
  digitalWrite(PIN_LED_ERROR, LOW);

  // Serial0 - debug/CLI
  Serial.begin(SERIAL0_BAUD);
  delay(2000);  // Give USB serial time to init

  Serial.println();
  Serial.println(F("==================================="));
  Serial.println(F(" UART-over-Ethernet Bridge v" FW_VERSION));
  Serial.println(F("==================================="));

  // Load config from EEPROM
  loadConfig();

  // Print loaded config
  printStatus();

  // Serial1 - RS-232 via MAX3232
  Serial1.begin(cfg.baud);

  // Ethernet init (no gateway or subnet needed for direct IP)
  IPAddress myIP(cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
  Ethernet.begin(cfg.mac, myIP);

  // Check hardware
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println(F("[ETH] ERROR: W5100 not found!!!"));
    digitalWrite(PIN_LED_ERROR, HIGH);
    noteError();
    // Continue anyway, allows for CLI config
  } else {
    Serial.println(F("[ETH] W5100 detected."));
  }

  ethLinkUp = true;  // Assume up init (W5100 reports Unknown)

  // Start server or prepare for client mode
  if (cfg.role == 0) {
    tcpServer = new EthernetServer(cfg.port);
    tcpServer->begin();
    Serial.print(F("[TCP] Server listening on port "));
    Serial.println(cfg.port);
  } else {
    Serial.println(F("[TCP] Client mode - will connect on first loop."));
    lastConnectAttemptMs = millis() - BACKOFF_INIT_MS; // Connect immediately
  }

  bootTimeMs = millis();

  // Enable watchdog LAST (after all init)
  wdt_enable(WDT_TIMEOUT);
  Serial.println(F("[SYS] Watchdog enabled (4s). Ready."));
  Serial.println(F("Type 'help' for commands."));
  Serial.println();
}

// =============================================================================
//  loop()
// =============================================================================

void loop() {
  wdt_reset();

  // ── Ethernet maintenance ───────────────────────────────────────────────────
  Ethernet.maintain();
  checkEthLink();

  // ── TCP connection management ──────────────────────────────────────────────
  if (cfg.role == 0) {
    handleServerAccept();
  } else {
    if (!tcpConnected) {
      handleClientConnect();
    }
  }

  checkTcpConnection();

  // ── Heartbeat ──────────────────────────────────────────────────────────────
  sendHeartbeat();

  // ── Data bridge ────────────────────────────────────────────────────────────
  bridgeUartToTcp();
  bridgeTcpToUart();

  // ── CLI ────────────────────────────────────────────────────────────────────
  pollCli();

  // ── LED updates ────────────────────────────────────────────────────────────
  updateActivityLed();
  updateConnectLed();
  updateErrorLed();

  // ── Log state transitions ──────────────────────────────────────────────────
  if (tcpConnected != prevTcpConnected) {
    if (tcpConnected) {
      Serial.println(F("[TCP] ● Link established."));
    } else {
      Serial.println(F("[TCP] ○ Link lost."));
    }
    prevTcpConnected = tcpConnected;
  }
}