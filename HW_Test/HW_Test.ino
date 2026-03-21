/*
 * Arduino Mega 2560 Hardware Verification Sketch
 * - Verifies Serial (USB)
 * - Verifies Serial1 (RS232 Adapter on Pins 18/19)
 * - Verifies W5100 Ethernet Shield (DHCP and Google HTTP connection)
 */

#include <SPI.h>
#include <Ethernet.h>

// MAC address of Ethernet shield (REPLACE WITH YOUR SHIELDS)
byte mac[] = {0x12, 0x34, 0x56, 0x78, 0xAB, 0xCD}; // 12:34:56:78:AB:CD
EthernetClient client;
char server[] = "www.google.com"; 

void setup() {
  Serial.begin(115200);
  Serial1.begin(19200);
  delay(3000); 

  // Send initial data to both serial ports
  Serial.println("\n--- Arduino Mega 2560 Verification Test ---");
  Serial.println("Testing Serial 0 (USB).");
  Serial1.println("\n--- Arduino Mega 2560 Verification Test ---");
  Serial1.println("Testing Serial 1 (RS232).");

  // Ask for input on Serial 0
  Serial.println("\n[STEP 1] Please type something in Serial 0 (USB) and press Enter...");
  while (Serial.available() == 0) {}
  String str0 = Serial.readStringUntil('\n');
  str0.trim();
  Serial.print("Success! Received on Serial 0: ");
  Serial.println(str0);

  // Ask for input on Serial 1
  Serial.println("\n[STEP 2] Please type something in the RS232 terminal and press Enter...");
  Serial1.println("\n[STEP 2] Please type something here (Serial 1) and press Enter...");
  while (Serial1.available() == 0) {}
  String str1 = Serial1.readStringUntil('\n');
  str1.trim();
  Serial.print("Success! Received on Serial 1: ");
  Serial.println(str1);
  Serial1.print("Success! Received on Serial 1: ");
  Serial1.println(str1);

  // Test Ethernet Shield
  Serial.println("\n[STEP 3] Testing Ethernet W5100...");
  
  //Init with a dummy static IP to start the W5100
  IPAddress dummyIP(198, 168, 254, 1);
  Ethernet.begin(mac, dummyIP);

  // Verify shield is communicating over SPI
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println("Error: Ethernet shield was not found. Check pins and seating.");
  } 
  else {
    // Check if ethernet cable is plugged in
    auto link = Ethernet.linkStatus();
    
    if (link == LinkOFF) {
      Serial.println("Error: Ethernet cable is disconnected. Skipping DHCP request.");
    } 
    else {
      if (link == Unknown) {
        Serial.println("Note: W5100 hardware doesn't support software link detection.");
        Serial.println("Attempting DHCP with a 10 second timeout...");
      } else {
        Serial.println("Ethernet cable detected! Requesting IP address via DHCP...");
      }

      // Re-init using DHCP. 
      // Ethernet.begin(mac, timeout_ms, responseTimeout_ms)
      if (Ethernet.begin(mac, 10000, 4000) == 0) {
        Serial.println("!!! FAILED to configure Ethernet using DHCP. Check router connection.");
      } else {
        Serial.print("Ethernet connected successfully! IP address: ");
        Serial.println(Ethernet.localIP());

        // Connect to Google on port 80
        Serial.println("Attempting to connect to www.google.com...");
        if (client.connect(server, 80)) {
          Serial.println("Successfully connected to Google! (Network verified)");

          // Send HTTP HEAD request and read status line.
          client.println("HEAD / HTTP/1.1");
          client.println("Host: www.google.com");
          client.println("Connection: close");
          client.println();

          unsigned long deadline = millis() + 5000;
          while (client.connected() && !client.available() && millis() < deadline) {
            // wait for response up to 5s
          }

          if (client.available()) {
            String statusLine = client.readStringUntil('\n');
            statusLine.trim();
            Serial.print("Google response: ");
            Serial.println(statusLine);
          } else {
            Serial.println("No response received from Google.");
          }

          client.stop();
        } else {
          Serial.println("Connection to Google failed.");
        }
      }
    }
  }
  
  Serial.println("\n--- Verification Complete ---");
  Serial.println("Entering passthrough mode. Data typed in Serial will echo to Serial1, and vice versa.");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    Serial1.print(c);
  }
  if (Serial1.available()) {
    char c = Serial1.read();
    Serial.print(c);
  }
}
