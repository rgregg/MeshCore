#ifdef ETH_ENABLED

#include <helpers/IdentityStore.h>
#include "MqttTelemetry.h"

#define MQTT_RECONNECT_MIN_MS    5000
#define MQTT_RECONNECT_MAX_MS  120000

MqttTelemetry::MqttTelemetry()
    : _mqttClient(_ethClient),
      _callbacks(nullptr),
      _fs(nullptr),
      _lastReconnectAttempt(0),
      _reconnectInterval(MQTT_RECONNECT_MIN_MS),
      _lastPublish(0),
      _ringHead(0),
      _ringTail(0)
{
  memset(&_config, 0, sizeof(_config));
  _config.port = 1883;
  _config.interval_secs = 60;
  strncpy(_config.prefix, "meshcore", sizeof(_config.prefix));
  _nodeName[0] = 0;
}

void MqttTelemetry::begin(FILESYSTEM* fs, MqttTelemetryCallbacks* callbacks, const char* nodeName) {
  _fs = fs;
  _callbacks = callbacks;
  strncpy(_nodeName, nodeName, sizeof(_nodeName) - 1);
  _nodeName[sizeof(_nodeName) - 1] = 0;
  loadConfig();
}

void MqttTelemetry::loadConfig() {
  if (!_fs) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(MQTT_PREFS_FILE, "r");
#else
  File f = _fs->open(MQTT_PREFS_FILE);
#endif
  if (f) {
    MqttConfig tmp;
    if (f.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.guard == MQTT_CONFIG_GUARD) {
      _config = tmp;
    }
    f.close();
  }
}

void MqttTelemetry::saveConfig() {
  if (!_fs) return;
  _config.guard = MQTT_CONFIG_GUARD;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File f = _fs->open(MQTT_PREFS_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File f = _fs->open(MQTT_PREFS_FILE, "w");
#else
  File f = _fs->open(MQTT_PREFS_FILE, "w", true);
#endif
  if (f) {
    f.write((const uint8_t*)&_config, sizeof(_config));
    f.close();
  }
}

void MqttTelemetry::buildTopic(const char* suffix) {
  snprintf(_topicBuf, sizeof(_topicBuf), "%s/%s/%s", _config.prefix, _nodeName, suffix);
}

bool MqttTelemetry::tryConnect() {
  IPAddress broker;
  broker = _config.broker_ip;

  _mqttClient.setServer(broker, _config.port);

  // Build LWT topic
  buildTopic("status");

  bool connected;
  if (_config.username[0]) {
    connected = _mqttClient.connect(_nodeName, _config.username, _config.password,
                                     _topicBuf, 1, true, "offline");
  } else {
    connected = _mqttClient.connect(_nodeName, _topicBuf, 1, true, "offline");
  }

  if (connected) {
    // Publish "online" status (retained)
    _mqttClient.publish(_topicBuf, "online", true);
    _reconnectInterval = MQTT_RECONNECT_MIN_MS;
    Serial.println("MQTT: connected");
  }
  return connected;
}

void MqttTelemetry::publishStatus(bool online) {
  buildTopic("status");
  _mqttClient.publish(_topicBuf, online ? "online" : "offline", true);
}

void MqttTelemetry::publishStats() {
  if (!_callbacks) return;

  char buf[256];

  // Core stats
  _callbacks->formatStatsReply(buf);
  buildTopic("stats/core");
  _mqttClient.publish(_topicBuf, buf);

  // Radio stats
  _callbacks->formatRadioStatsReply(buf);
  buildTopic("stats/radio");
  _mqttClient.publish(_topicBuf, buf);

  // Packet stats
  _callbacks->formatPacketStatsReply(buf);
  buildTopic("stats/packets");
  _mqttClient.publish(_topicBuf, buf);

  // Neighbors
  _callbacks->formatNeighborsReply(buf);
  buildTopic("neighbors");
  _mqttClient.publish(_topicBuf, buf);
}

void MqttTelemetry::publishPacketRx(uint8_t type, bool route_direct, uint8_t len, uint8_t payload_len, float snr, float rssi) {
  uint8_t next = (_ringHead + 1) % MQTT_RING_BUF_SIZE;
  if (next == _ringTail) return;  // buffer full, drop

  _ring[_ringHead].type = type;
  _ring[_ringHead].route_direct = route_direct ? 1 : 0;
  _ring[_ringHead].len = len;
  _ring[_ringHead].payload_len = payload_len;
  _ring[_ringHead].snr_x4 = (int16_t)(snr * 4);
  _ring[_ringHead].rssi = (int16_t)rssi;
  _ring[_ringHead].is_rx = true;
  _ringHead = next;
}

void MqttTelemetry::publishPacketTx(uint8_t type, bool route_direct, uint8_t len, uint8_t payload_len) {
  uint8_t next = (_ringHead + 1) % MQTT_RING_BUF_SIZE;
  if (next == _ringTail) return;  // buffer full, drop

  _ring[_ringHead].type = type;
  _ring[_ringHead].route_direct = route_direct ? 1 : 0;
  _ring[_ringHead].len = len;
  _ring[_ringHead].payload_len = payload_len;
  _ring[_ringHead].snr_x4 = 0;
  _ring[_ringHead].rssi = 0;
  _ring[_ringHead].is_rx = false;
  _ringHead = next;
}

void MqttTelemetry::drainRingBuffer() {
  char buf[128];
  while (_ringTail != _ringHead) {
    PacketEvent& evt = _ring[_ringTail];
    if (evt.is_rx) {
      snprintf(buf, sizeof(buf),
        "{\"type\":%u,\"route\":\"%s\",\"len\":%u,\"payload_len\":%u,\"snr\":%.2f,\"rssi\":%d}",
        evt.type, evt.route_direct ? "D" : "F", evt.len, evt.payload_len,
        evt.snr_x4 / 4.0f, evt.rssi);
      buildTopic("packet/rx");
    } else {
      snprintf(buf, sizeof(buf),
        "{\"type\":%u,\"route\":\"%s\",\"len\":%u,\"payload_len\":%u}",
        evt.type, evt.route_direct ? "D" : "F", evt.len, evt.payload_len);
      buildTopic("packet/tx");
    }
    _mqttClient.publish(_topicBuf, buf);
    _ringTail = (_ringTail + 1) % MQTT_RING_BUF_SIZE;
  }
}

bool MqttTelemetry::isConnected() {
  return _mqttClient.connected();
}

void MqttTelemetry::loop(unsigned long now_ms) {
  if (!_config.enabled || _config.broker_ip == 0) return;

  if (_mqttClient.connected()) {
    _mqttClient.loop();

    // Drain packet event ring buffer
    drainRingBuffer();

    // Periodic stats publish
    if (now_ms - _lastPublish >= (unsigned long)_config.interval_secs * 1000) {
      publishStats();
      _lastPublish = now_ms;
    }
  } else {
    // Non-blocking reconnect with exponential backoff
    if (now_ms - _lastReconnectAttempt >= _reconnectInterval) {
      _lastReconnectAttempt = now_ms;
      if (tryConnect()) {
        _lastPublish = now_ms;  // reset publish timer on connect
      } else {
        // Exponential backoff
        _reconnectInterval = min(_reconnectInterval * 2, (unsigned long)MQTT_RECONNECT_MAX_MS);
      }
    }
  }
}

bool MqttTelemetry::handleCommand(const char* command, char* reply) {
  if (strncmp(command, "mqtt", 4) != 0) return false;
  if (command[4] != 0 && command[4] != ' ') return false;  // must be "mqtt" or "mqtt ..."

  const char* arg = command + 4;
  while (*arg == ' ') arg++;

  if (*arg == 0) {
    // Show status
    if (!_config.enabled) {
      strcpy(reply, "MQTT: disabled");
    } else {
      IPAddress broker(_config.broker_ip);
      sprintf(reply, "MQTT: %s broker=%u.%u.%u.%u:%u interval=%us prefix=%s",
              _mqttClient.connected() ? "connected" : "disconnected",
              broker[0], broker[1], broker[2], broker[3],
              _config.port, _config.interval_secs, _config.prefix);
    }
    return true;
  }

  if (strcmp(arg, "on") == 0) {
    if (_config.broker_ip == 0) {
      strcpy(reply, "Err - set broker first");
    } else {
      _config.enabled = true;
      _reconnectInterval = MQTT_RECONNECT_MIN_MS;
      _lastReconnectAttempt = 0;
      saveConfig();
      strcpy(reply, "MQTT: enabled");
    }
    return true;
  }

  if (strcmp(arg, "off") == 0) {
    if (_mqttClient.connected()) {
      publishStatus(false);
      _mqttClient.disconnect();
    }
    _config.enabled = false;
    saveConfig();
    strcpy(reply, "MQTT: disabled");
    return true;
  }

  if (strncmp(arg, "broker ", 7) == 0) {
    IPAddress ip;
    if (ip.fromString(arg + 7)) {
      _config.broker_ip = (uint32_t)ip;
      // Disconnect if connected so it reconnects with new broker
      if (_mqttClient.connected()) {
        publishStatus(false);
        _mqttClient.disconnect();
      }
      _reconnectInterval = MQTT_RECONNECT_MIN_MS;
      _lastReconnectAttempt = 0;
      saveConfig();
      sprintf(reply, "MQTT: broker=%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else {
      strcpy(reply, "Err - invalid IP");
    }
    return true;
  }

  if (strncmp(arg, "port ", 5) == 0) {
    int port = atoi(arg + 5);
    if (port > 0 && port <= 65535) {
      _config.port = (uint16_t)port;
      saveConfig();
      sprintf(reply, "MQTT: port=%u", _config.port);
    } else {
      strcpy(reply, "Err - invalid port");
    }
    return true;
  }

  if (strncmp(arg, "user ", 5) == 0) {
    strncpy(_config.username, arg + 5, sizeof(_config.username) - 1);
    _config.username[sizeof(_config.username) - 1] = 0;
    saveConfig();
    sprintf(reply, "MQTT: user=%s", _config.username);
    return true;
  }

  if (strncmp(arg, "pass ", 5) == 0) {
    strncpy(_config.password, arg + 5, sizeof(_config.password) - 1);
    _config.password[sizeof(_config.password) - 1] = 0;
    saveConfig();
    strcpy(reply, "MQTT: password set");
    return true;
  }

  if (strncmp(arg, "prefix ", 7) == 0) {
    strncpy(_config.prefix, arg + 7, sizeof(_config.prefix) - 1);
    _config.prefix[sizeof(_config.prefix) - 1] = 0;
    saveConfig();
    sprintf(reply, "MQTT: prefix=%s", _config.prefix);
    return true;
  }

  if (strncmp(arg, "interval ", 9) == 0) {
    int secs = atoi(arg + 9);
    if (secs >= 10 && secs <= 3600) {
      _config.interval_secs = (uint16_t)secs;
      saveConfig();
      sprintf(reply, "MQTT: interval=%us", _config.interval_secs);
    } else {
      strcpy(reply, "Err - range 10-3600");
    }
    return true;
  }

  strcpy(reply, "Err - unknown mqtt command");
  return true;
}

#endif // ETH_ENABLED
