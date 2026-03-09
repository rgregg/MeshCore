#pragma once

#ifdef ETH_ENABLED

#include <Arduino.h>

// Must be defined before including PubSubClient
#define MQTT_MAX_PACKET_SIZE 512

#include <PubSubClient.h>
#include <RAK13800_W5100S.h>

#define MQTT_CONFIG_GUARD  0x4D515454  // "MQTT"
#define MQTT_PREFS_FILE    "/mqtt_prefs"

#define MQTT_RING_BUF_SIZE 16

struct MqttConfig {
  bool enabled;
  uint32_t broker_ip;
  uint16_t port;
  uint16_t interval_secs;
  char username[32];
  char password[32];
  char prefix[32];
  uint32_t guard;
};

struct PacketEvent {
  uint8_t type;
  uint8_t route_direct;
  uint8_t len;
  uint8_t payload_len;
  int16_t snr_x4;  // SNR * 4 (to match existing convention)
  int16_t rssi;
  bool is_rx;       // true=RX, false=TX
};

class MqttTelemetryCallbacks {
public:
  virtual void formatStatsReply(char *reply) = 0;
  virtual void formatRadioStatsReply(char *reply) = 0;
  virtual void formatPacketStatsReply(char *reply) = 0;
  virtual void formatNeighborsReply(char *reply) = 0;
  virtual const char* getNodeName() = 0;
};

class MqttTelemetry {
  EthernetClient _ethClient;
  PubSubClient _mqttClient;
  MqttTelemetryCallbacks* _callbacks;
  FILESYSTEM* _fs;

  MqttConfig _config;
  char _nodeName[32];

  // Reconnect state
  unsigned long _lastReconnectAttempt;
  unsigned long _reconnectInterval;

  // Periodic publish state
  unsigned long _lastPublish;

  // Ring buffer for packet events
  PacketEvent _ring[MQTT_RING_BUF_SIZE];
  volatile uint8_t _ringHead;
  volatile uint8_t _ringTail;

  // Topic buffer
  char _topicBuf[96];

  void buildTopic(const char* suffix);
  void publishStatus(bool online);
  void publishStats();
  void drainRingBuffer();
  bool tryConnect();

  void loadConfig();
  void saveConfig();

public:
  MqttTelemetry();

  void begin(FILESYSTEM* fs, MqttTelemetryCallbacks* callbacks, const char* nodeName);
  void loop(unsigned long now_ms);
  bool handleCommand(const char* command, char* reply);
  bool isConnected();

  void publishPacketRx(uint8_t type, bool route_direct, uint8_t len, uint8_t payload_len, float snr, float rssi);
  void publishPacketTx(uint8_t type, bool route_direct, uint8_t len, uint8_t payload_len);
};

#endif // ETH_ENABLED
