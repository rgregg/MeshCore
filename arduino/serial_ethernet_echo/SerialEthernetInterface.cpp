#include "SerialEthernetInterface.h"
#include <SPI.h>

// For RAK 4631
#define PIN_SPI1_MISO (29) // (0 + 29)
#define PIN_SPI1_MOSI (30) // (0 + 30)
#define PIN_SPI1_SCK (3)   // (0 + 3)

SPIClass ETH_SPI_PORT(NRF_SPIM1, PIN_SPI1_MISO, PIN_SPI1_SCK, PIN_SPI1_MOSI);

#define PIN_ETHERNET_RESET 21
#define PIN_ETHERNET_SS 26

bool SerialEthernetInterface::begin() {

  ETH_DEBUG_PRINTLN("Ethernet initalizing");

  #ifdef PIN_ETH_POWER_EN
        ETH_DEBUG_PRINTLN("Ethernet power enable");
        pinMode(PIN_ETH_POWER_EN, OUTPUT);
        digitalWrite(PIN_ETH_POWER_EN, HIGH); // Power up.
        delay(100);
        ETH_DEBUG_PRINTLN("Ethernet power enabled");
  #endif

  #ifdef PIN_ETHERNET_RESET
        pinMode(PIN_ETHERNET_RESET, OUTPUT);
        digitalWrite(PIN_ETHERNET_RESET, LOW); // Reset Time.
        delay(100);
        digitalWrite(PIN_ETHERNET_RESET, HIGH); // Reset Time.
        ETH_DEBUG_PRINTLN("Ethernet reset pulse");
  #endif

  uint8_t mac[6];
  generateDeviceMac(mac);
  ETH_DEBUG_PRINTLN("Init");
  Ethernet.init(ETH_SPI_PORT, PIN_ETHERNET_SS);
  ETH_DEBUG_PRINTLN("Begin");
  if (Ethernet.begin(mac) == 0) {
    ETH_DEBUG_PRINTLN("Begin failed.");

    // DHCP failed -- let's figure out why
    if (Ethernet.hardwareStatus() == EthernetNoHardware)  // Check for Ethernet hardware present.
    {
      ETH_DEBUG_PRINTLN("Ethernet hardware not found.");
      return false;
    }
    if (Ethernet.linkStatus() == LinkOFF)     // No physical connection
    {
      ETH_DEBUG_PRINTLN("Ethernet cable not connected.");
      return false;
    }
    ETH_DEBUG_PRINTLN("Ethernet: DHCP failed for unknown reason.");
    return false;
  }
  else
  {
    IPAddress ip = Ethernet.localIP();
    ETH_DEBUG_PRINTLN("Ethernet IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  }

  server.begin();   // start listening for clients
  ETH_DEBUG_PRINTLN("Ethernet: listening on TCP port: %d", TCP_PORT);
  return true;
}

void SerialEthernetInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();
}

void SerialEthernetInterface::disable() {
  _isEnabled = false;
}

size_t SerialEthernetInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    ETH_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d\n", len);
    return 0;
  }

  if (deviceConnected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      ETH_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

bool SerialEthernetInterface::isWriteBusy() const {
  return false;
}

size_t SerialEthernetInterface::checkRecvFrame(uint8_t dest[]) {
  // check if new client connected
  auto newClient = server.available();
  if (newClient) {
    // disconnect existing client
    deviceConnected = false;
    client.stop();

    // switch active connection to new client
    client = newClient;
  }

  if (client.connected()) {
    if (!deviceConnected) {
      ETH_DEBUG_PRINTLN("Got connection");
      deviceConnected = true;
    }
  } else {
    if (deviceConnected) {
      deviceConnected = false;
      ETH_DEBUG_PRINTLN("Disconnected");
    }
  }

  if (deviceConnected) {
    if (send_queue_len > 0) {   // first, check send queue

      _last_write = millis();
      int len = send_queue[0].len;

      uint8_t pkt[3+len]; // use same header as serial interface so client can delimit frames
      pkt[0] = '>';
      pkt[1] = (len & 0xFF);  // LSB
      pkt[2] = (len >> 8);    // MSB
      memcpy(&pkt[3], send_queue[0].buf, send_queue[0].len);
      client.write(pkt, 3 + len);
      send_queue_len--;
      for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
        send_queue[i] = send_queue[i + 1];
      }
    } else {
      int len = client.available();
      if (len > 0) {
        uint8_t buf[MAX_FRAME_SIZE + 4];
        client.readBytes(buf, len);
        memcpy(dest, buf+3, len-3); // remove header (don't even check ... problems are on the other dir)
        return len-3;
      }
    }
  }

  return 0;
}

bool SerialEthernetInterface::isConnected() const {
  return deviceConnected;  //pServer != NULL && pServer->getConnectedCount() > 0;
}

void SerialEthernetInterface::generateDeviceMac(uint8_t mac[6]) {
  uint64_t device_id = ((uint64_t)NRF_FICR->DEVICEID[1] << 32) | (uint64_t)NRF_FICR->DEVICEID[0];

  mac[0] = 0x02; // Locally administered, unicast
  mac[1] = (device_id >> 40) & 0xFF;
  mac[2] = (device_id >> 32) & 0xFF;
  mac[3] = (device_id >> 24) & 0xFF;
  mac[4] = (device_id >> 16) & 0xFF;
  mac[5] = (device_id >> 8) & 0xFF;
}
