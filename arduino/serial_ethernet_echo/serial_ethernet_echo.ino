#include <Arduino.h>

#define ETH_DEBUG_LOGGING 1

#include "SerialEthernetInterface.h"

SerialEthernetInterface serial_eth;
static uint8_t rx_buf[MAX_FRAME_SIZE];

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && (millis() - start) < 5000) {
    delay(10);
  }

  Serial.println("SerialEthernet echo starting...");

  if (!serial_eth.begin()) {
    Serial.println("Ethernet begin failed; halting.");
    while (true) {
      delay(1000);
    }
  }

  serial_eth.enable();
  Serial.println("Ethernet ready. Connect to TCP port 8080.");
  Serial.println("Frames are MeshCore-style: '>' + len(LSB/MSB) + payload.");
}

void loop() {
  size_t len = serial_eth.checkRecvFrame(rx_buf);
  if (len > 0) {
    serial_eth.writeFrame(rx_buf, len);
  }

  delay(1);
}
