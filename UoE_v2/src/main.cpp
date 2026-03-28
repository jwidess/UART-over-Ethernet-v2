/**
 * UART-over-Ethernet Transparent Bridge
 * Hardware: Arduino Mega 2560 + W5100 Ethernet Shield + MAX3232 on Serial1
 *
 * Single firmware for both nodes (SERVER / CLIENT).
 * All configurable params stored in EEPROM, edited via Serial0 commands.
 */

#include <SPI.h>
#include <Ethernet.h>
#include <avr/wdt.h>

#include "fw_version.h"
#include "config.h"
#include "status.h"
#include "cli.h"
#include "utils.h"
#include "leds.h"

// ─── Constants ───────────────────────────────────────────────────────────────
#define SERIAL0_BAUD         115200
// Ref build flag `-DSERIAL_RX_BUFFER_SIZE=1024` in platformio.ini
#define UART_BUF_SIZE        256     // Max bytes to read from UART per loop
#define TCP_RX_CHUNK         64      // Max bytes to read from TCP per loop
#define UART_IDLE_MS         20      // Flush UART buffer after this idle gap
#define WDT_TIMEOUT          WDTO_4S

// ─── Framing Protocol ────────────────────────────────────────────────────────
// TCP frames: [TYPE (1 byte)][LEN (1 byte)][PAYLOAD (0-255 bytes)]
// This allows for NUL bytes to pass through the data stream.
#define FRAME_TYPE_DATA   0x44      // 'D' data frame
#define FRAME_TYPE_HB     0x48      // 'H' heartbeat frame (LEN=0, no payload)

// ─── Client reconnect back-off ───────────────────────────────────────────────
#define BACKOFF_INIT_MS   1000
#define BACKOFF_MAX_MS    10000  // Cap at 10s

// ─── Network Objects ─────────────────────────────────────────────────────────
static EthernetServer *tcpServer = nullptr;
static EthernetClient  tcpClient;

// ─── Buffers ─────────────────────────────────────────────────────────────────
static uint8_t uartBuf[UART_BUF_SIZE];
static uint16_t uartBufLen = 0;

// ─── Timing ──────────────────────────────────────────────────────────────────
static uint32_t lastUartByteMs       = 0;
static uint32_t lastHbSentMs         = 0;
static uint32_t lastHbRecvMs         = 0;
uint32_t uptimeTotalSec              = 0;
uint32_t lastUptimeTickMs            = 0;
static uint32_t backoffMs            = BACKOFF_INIT_MS;
static uint32_t lastConnectAttemptMs = 0;
uint32_t lastErrorMs                 = 0;

// ─── Stats ───────────────────────────────────────────────────────────────────
uint32_t reconnectCount = 0;
uint32_t errorCount     = 0;
uint32_t bytesRxUart    = 0;
uint32_t bytesTxUart    = 0;
uint32_t bytesRxTcp     = 0;
uint32_t bytesTxTcp     = 0;
uint32_t peakTcpWriteMs   = 0;
uint32_t peakTcpConnectMs = 0;
uint32_t peakTcpReadMs    = 0;

// UART RX ring buffer health:
// NOTE: We cannot check the hardware overrun flag (DOR1 in UCSR1A) as the
// Arduino core ISR (_rx_complete_irq) reads UDR1 on every received byte,
// which clears DOR1 before our main loop can see it. Instead we detect overflow
// at the ring buffer. When Serial1.available() reaches SERIAL_RX_BUFFER_SIZE-1
// (max), the ISR silently discards incoming bytes.
uint32_t uartBufferOverflowCount = 0;
uint16_t uartRxBufPeakUsed       = 0;  // High-water mark

// ─── State ───────────────────────────────────────────────────────────────────
bool tcpConnected             = false;
static bool prevTcpConnected  = false;
bool ethLinkUp                = false;

#define debugMode (cfg.debug)

static void updatePeakTiming(uint32_t elapsedMs, uint32_t &peakMs) {
  if (elapsedMs > peakMs) {
    peakMs = elapsedMs;
  }
}

static void logTcpTiming(const __FlashStringHelper *opLabel,
                         uint32_t elapsedMs,
                         uint32_t thresholdMs,
                         int payloadBytes = -1) {
  if (!debugMode) {
    return;
  }
  if (elapsedMs <= thresholdMs) {
    return;
  }

  Serial.print(F("[TCP-DBG] "));
  Serial.print(opLabel);
  Serial.print(F(" took "));
  Serial.print(elapsedMs);
  Serial.print(F(" ms"));
  if (payloadBytes >= 0) {
    Serial.print(F(" ("));
    Serial.print(payloadBytes);
    Serial.print(F(" B payload)"));
  }
  Serial.println();
}

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

void rebootNow() {
  Serial.flush();
  wdt_enable(WDTO_15MS);
  while (1) {} // Wait for watchdog reset
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

  uint32_t now = millis();
  if (now - lastConnectAttemptMs < backoffMs) return;
  lastConnectAttemptMs = now;

  IPAddress remote(cfg.remoteIp[0], cfg.remoteIp[1], cfg.remoteIp[2], cfg.remoteIp[3]);
  Serial.print(F("[TCP] Connecting to "));
  Serial.print(remote);
  Serial.print(':');
  Serial.print(cfg.port);
  Serial.println(F("..."));

  // connect() can block while the stack performs ARP/TCP handshake.
  uint32_t connectStartMs = millis();
  int result = tcpClient.connect(remote, cfg.port);
  uint32_t connectElapsedMs = millis() - connectStartMs;
  updatePeakTiming(connectElapsedMs, peakTcpConnectMs);
  logTcpTiming(F("connect()"), connectElapsedMs, 0UL);

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
    backoffMs = min(backoffMs * 2, (uint32_t)BACKOFF_MAX_MS);
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
  uint32_t now = millis();
  uint32_t hbTimeoutMs = (uint32_t)cfg.hbIntervalSec * HB_MISS_FACTOR * 1000UL;
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
  uint32_t now = millis();
  if (now - lastHbSentMs >= (uint32_t)cfg.hbIntervalSec * 1000UL) {
    uint8_t hbFrame[2] = { FRAME_TYPE_HB, 0x00 };
    tcpClient.write(hbFrame, 2);
    lastHbSentMs = now;
    if (debugMode) {
      Serial.println(F("[HB-DBG] Sent heartbeat"));
    }
  }
}

// =============================================================================
//  UART RX buffer health
// =============================================================================

static void checkUartBufferOverflow() {
  uint16_t pendingRx = Serial1.available();

  // Track high-water mark
  if (pendingRx > uartRxBufPeakUsed) {
    uartRxBufPeakUsed = pendingRx;
  }

  // SERIAL_RX_BUFFER_SIZE - 1 is the max capacity of the Arduino ring buffer
  // If we hit this, the ISR is silently discarding incoming bytes.
  if (pendingRx >= SERIAL_RX_BUFFER_SIZE - 1) {
    uartBufferOverflowCount++;
    noteError();
    Serial.print(F("[UART-ERR] RX ring buffer full! Overflow #"));
    Serial.println(uartBufferOverflowCount);
  }

  // Warn once when buffer exceeds 75% capacity
  static bool highWaterWarned = false;
  if (pendingRx > ((SERIAL_RX_BUFFER_SIZE * 3) / 4)) {
    if (!highWaterWarned) {
      Serial.print(F("[UART-WARN] RX buf high: "));
      Serial.print(pendingRx);
      Serial.print('/');
      Serial.println(SERIAL_RX_BUFFER_SIZE);
      highWaterWarned = true;
    }
  } else {
    highWaterWarned = false;
  }
}

// =============================================================================
//  Data bridge
// =============================================================================

static void bridgeUartToTcp() {
  // Read available UART bytes into buffer
  while (Serial1.available() && (uartBufLen < UART_BUF_SIZE)) {
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

      // Measure full framed write (header + payload).
      uint32_t writeStartMs = millis();
      tcpClient.write(header, 2);
      size_t sent = tcpClient.write(uartBuf + offset, chunkLen);
      uint32_t writeElapsedMs = millis() - writeStartMs;

      updatePeakTiming(writeElapsedMs, peakTcpWriteMs);
      logTcpTiming(F("write()"), writeElapsedMs, 5UL, chunkLen);

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

  // read() may block briefly depending on socket state and buffering.
  uint32_t readStartMs = millis();
  int n = tcpClient.read(chunk, toRead);
  uint32_t readElapsedMs = millis() - readStartMs;

  updatePeakTiming(readElapsedMs, peakTcpReadMs);
  logTcpTiming(F("read()"), readElapsedMs, 5UL);

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
            if (debugMode) {
              Serial.println(F("[HB-DBG] Received heartbeat"));
            }
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
  delay(1000);  // Give USB serial time to init
  if (!Serial) {
    delay(2000);  // Extra time to open Serial
  }

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

  // Ethernet init with explicit subnet/gateway from config.
  IPAddress myIP(cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
  IPAddress subnet(cfg.subnet[0], cfg.subnet[1], cfg.subnet[2], cfg.subnet[3]);
  IPAddress gateway(cfg.gateway[0], cfg.gateway[1], cfg.gateway[2], cfg.gateway[3]);
  IPAddress dns(0, 0, 0, 0);
  Ethernet.begin(cfg.mac, myIP, dns, gateway, subnet);

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

  lastUptimeTickMs = millis();

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
  updateUptime();

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

  // ── UART health check ─────────────────────────────────────────────────────
  checkUartBufferOverflow();

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