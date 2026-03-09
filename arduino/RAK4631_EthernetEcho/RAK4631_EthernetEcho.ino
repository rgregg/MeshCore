#include <SPI.h>
#include <RAK13800_W5100S.h>

// RAK4631 SPI1 pins used by RAK13800 (W5100S)
#define PIN_SPI1_MISO 29
#define PIN_SPI1_MOSI 30
#define PIN_SPI1_SCK 3

SPIClass ETH_SPI_PORT(NRF_SPIM1, PIN_SPI1_MISO, PIN_SPI1_SCK, PIN_SPI1_MOSI);

#define PIN_ETH_POWER_EN WB_IO2
#define PIN_ETHERNET_RESET 21
#define PIN_ETHERNET_SS 26

static const uint16_t kTcpPort = 5000;
EthernetServer server(kTcpPort);
EthernetClient client;
bool ethReady = false;
unsigned long lastInitAttempt = 0;

static void generateDeviceMac(uint8_t mac[6]) {
  uint32_t device_id = NRF_FICR->DEVICEID[0];

  mac[0] = 0x02;
  mac[1] = 0x92;
  mac[2] = 0x1F;
  mac[3] = (device_id >> 16) & 0xFF;
  mac[4] = (device_id >> 8) & 0xFF;
  mac[5] = device_id & 0xFF;
}

static bool initEthernet() {
  pinMode(PIN_ETH_POWER_EN, OUTPUT);
  digitalWrite(PIN_ETH_POWER_EN, HIGH);
  delay(100);

  pinMode(PIN_ETHERNET_RESET, OUTPUT);
  digitalWrite(PIN_ETHERNET_RESET, LOW);
  delay(100);
  digitalWrite(PIN_ETHERNET_RESET, HIGH);

  uint8_t mac[6];
  generateDeviceMac(mac);

  Ethernet.init(ETH_SPI_PORT, PIN_ETHERNET_SS);
  if (Ethernet.begin(mac) == 0) {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      Serial.println("Ethernet hardware not found.");
    } else if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println("Ethernet cable not connected.");
    } else {
      Serial.println("Ethernet: DHCP failed.");
    }
    return false;
  }

  Serial.print("IP: ");
  Serial.println(Ethernet.localIP());
  Serial.print("Gateway: ");
  Serial.println(Ethernet.gatewayIP());
  Serial.print("Subnet: ");
  Serial.println(Ethernet.subnetMask());

  server.begin();
  Serial.print("TCP echo server on port ");
  Serial.println(kTcpPort);
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  ethReady = initEthernet();
  if (!ethReady) {
    Serial.println("Ethernet init failed; retrying in loop.");
  }
}

void loop() {
  if (!ethReady) {
    if (millis() - lastInitAttempt > 2000) {
      lastInitAttempt = millis();
      ethReady = initEthernet();
    }
    delay(10);
    return;
  }

  if (Ethernet.linkStatus() == LinkOFF) {
    delay(100);
    return;
  }

  Ethernet.maintain();

  if (!client || !client.connected()) {
    client.stop();
    client = server.available();
    return;
  }

  while (client.available() > 0) {
    int c = client.read();
    if (c >= 0) {
      client.write(static_cast<uint8_t>(c));
    }
  }
}
