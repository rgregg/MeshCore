#pragma once

#ifdef ETH_ENABLED

#include <Arduino.h>
#include <Packet.h>

// MQTT_MAX_PACKET_SIZE must be set via build_flags (-D MQTT_MAX_PACKET_SIZE=768)
// so it applies to both our code and the PubSubClient library compilation unit.
#ifndef MQTT_MAX_PACKET_SIZE
  #warning "MQTT_MAX_PACKET_SIZE not set via build flags, defaulting to 768"
  #define MQTT_MAX_PACKET_SIZE 768
#endif

#include <PubSubClient.h>
#include <RAK13800_W5100S.h>

struct NodePrefs;

#define MQTT_CONFIG_GUARD  0x4D515454  // "MQTT"
#define MQTT_PREFS_FILE    "/mqtt_prefs"

#define MQTT_RING_BUF_SIZE 8

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
  uint32_t timestamp_epoch;
  uint8_t type;
  uint8_t header;
  uint8_t route_type;
  uint8_t raw_len;
  uint8_t payload_len;
  uint8_t dest_hash;
  uint8_t src_hash;
  int16_t snr_x4;
  int16_t rssi;
  int16_t score_x1000;
  bool is_rx;
  uint8_t hash[MAX_HASH_SIZE];
  uint8_t raw[MAX_TRANS_UNIT];
};

class MqttTelemetryCallbacks {
public:
  virtual void formatStatsReply(char *reply) = 0;
  virtual void formatRadioStatsReply(char *reply) = 0;
  virtual void formatPacketStatsReply(char *reply) = 0;
  virtual int formatNeighborsJson(char *reply, int max_len) = 0;
  virtual const char* getNodeName() = 0;
  virtual uint32_t getEpochTime() = 0;
  virtual const char* getPublicKeyHex() = 0;
  virtual const char* getFirmwareVer() = 0;
  virtual const char* getBuildDate() = 0;
  virtual const char* getRole() = 0;
  virtual NodePrefs* getNodePrefs() = 0;
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

  // Topic buffer (prefix + 64-char pubkey + suffix)
  char _topicBuf[128];

  void buildTopic(const char* suffix);
  void publishStatus(bool online);
  void publishConfig();
  void publishStats();
  void drainRingBuffer();
  bool tryConnect();

  void loadConfig();
  void saveConfig();

  static void formatHex(char* dest, const uint8_t* src, int len);
  static void formatTimestamp(char* dest, uint32_t epoch);

public:
  MqttTelemetry();

  void begin(FILESYSTEM* fs, MqttTelemetryCallbacks* callbacks, const char* nodeName);
  void loop(unsigned long now_ms);
  bool handleCommand(const char* command, char* reply);
  bool isConnected();

  void publishPacketRx(mesh::Packet* pkt, int len, float score, float snr, float rssi);
  void publishPacketTx(mesh::Packet* pkt, int len);
  void publishAdvert(const mesh::Identity& id, uint32_t timestamp,
                     const uint8_t* app_data, size_t app_data_len,
                     float snr, uint8_t hops);
};

#endif // ETH_ENABLED
