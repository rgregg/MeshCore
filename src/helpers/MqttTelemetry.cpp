#ifdef ETH_ENABLED

#include <helpers/IdentityStore.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/CommonCLI.h>
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
  _fs->remove(MQTT_PREFS_FILE);
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

void MqttTelemetry::formatHex(char* dest, const uint8_t* src, int len) {
  for (int i = 0; i < len; i++) {
    sprintf(dest + i * 2, "%02X", src[i]);
  }
}

void MqttTelemetry::formatTimestamp(char* dest, uint32_t epoch) {
  // Format as ISO 8601: YYYY-MM-DDTHH:MM:SS
  // Simple epoch-to-date conversion (no timezone, UTC assumed)
  uint32_t t = epoch;
  uint32_t secs = t % 60; t /= 60;
  uint32_t mins = t % 60; t /= 60;
  uint32_t hours = t % 24; t /= 24;

  // Days since epoch to Y/M/D
  uint32_t days = t;
  uint32_t year = 1970;
  while (true) {
    uint32_t diy = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
    if (days < diy) break;
    days -= diy;
    year++;
  }
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  uint8_t mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (leap) mdays[1] = 29;
  uint32_t month = 0;
  while (month < 12 && days >= mdays[month]) {
    days -= mdays[month];
    month++;
  }

  sprintf(dest, "%04lu-%02lu-%02luT%02lu:%02lu:%02lu",
    (unsigned long)year, (unsigned long)(month + 1), (unsigned long)(days + 1),
    (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
}

void MqttTelemetry::buildTopic(const char* suffix) {
  const char* id = _callbacks ? _callbacks->getPublicKeyHex() : _nodeName;
  snprintf(_topicBuf, sizeof(_topicBuf), "%s/%s/%s", _config.prefix, id, suffix);
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
    // Publish node config (retained)
    publishConfig();
    _reconnectInterval = MQTT_RECONNECT_MIN_MS;
    Serial.println("MQTT: connected");
  }
  return connected;
}

void MqttTelemetry::publishStatus(bool online) {
  buildTopic("status");
  _mqttClient.publish(_topicBuf, online ? "online" : "offline", true);
}

void MqttTelemetry::publishConfig() {
  if (!_callbacks) return;

  NodePrefs* prefs = _callbacks->getNodePrefs();
  static char buf[384];

  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"firmware\":\"%s\",\"build_date\":\"%s\","
    "\"role\":\"%s\","
    "\"freq\":%.1f,\"bw\":%.0f,\"sf\":%u,\"cr\":%u,"
    "\"tx_power\":%d,"
    "\"lat\":%.6f,\"lon\":%.6f}",
    prefs->node_name,
    _callbacks->getFirmwareVer(),
    _callbacks->getBuildDate(),
    _callbacks->getRole(),
    prefs->freq, prefs->bw, prefs->sf, prefs->cr,
    prefs->tx_power_dbm,
    prefs->node_lat, prefs->node_lon);

  buildTopic("config");
  _mqttClient.publish(_topicBuf, buf, true);  // retained
}

void MqttTelemetry::publishStats() {
  if (!_callbacks) return;

  // Refresh retained topics
  publishStatus(true);
  publishConfig();

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

  // Neighbors (JSON array, may need more space)
  static char nbuf[512];
  _callbacks->formatNeighborsJson(nbuf, sizeof(nbuf));
  buildTopic("neighbors");
  _mqttClient.publish(_topicBuf, nbuf);
}

static bool pktHasAddressHashes(uint8_t type) {
  return type == PAYLOAD_TYPE_PATH || type == PAYLOAD_TYPE_REQ ||
         type == PAYLOAD_TYPE_RESPONSE || type == PAYLOAD_TYPE_TXT_MSG;
}

void MqttTelemetry::publishPacketRx(mesh::Packet* pkt, int len, float score, float snr, float rssi) {
  uint8_t next = (_ringHead + 1) % MQTT_RING_BUF_SIZE;
  if (next == _ringTail) return;  // buffer full, drop

  PacketEvent& evt = _ring[_ringHead];
  evt.timestamp_epoch = _callbacks ? _callbacks->getEpochTime() : 0;
  evt.type = pkt->getPayloadType();
  evt.header = pkt->header;
  evt.route_type = pkt->getRouteType();
  evt.payload_len = pkt->payload_len;
  evt.dest_hash = pktHasAddressHashes(evt.type) ? pkt->payload[0] : 0;
  evt.src_hash = pktHasAddressHashes(evt.type) ? pkt->payload[1] : 0;
  evt.snr_x4 = (int16_t)(snr * 4);
  evt.rssi = (int16_t)rssi;
  evt.score_x1000 = (int16_t)(score * 1000);
  evt.is_rx = true;

  // Capture raw bytes and hash
  evt.raw_len = pkt->writeTo(evt.raw);
  pkt->calculatePacketHash(evt.hash);

  _ringHead = next;
}

void MqttTelemetry::publishPacketTx(mesh::Packet* pkt, int len) {
  uint8_t next = (_ringHead + 1) % MQTT_RING_BUF_SIZE;
  if (next == _ringTail) return;  // buffer full, drop

  PacketEvent& evt = _ring[_ringHead];
  evt.timestamp_epoch = _callbacks ? _callbacks->getEpochTime() : 0;
  evt.type = pkt->getPayloadType();
  evt.header = pkt->header;
  evt.route_type = pkt->getRouteType();
  evt.payload_len = pkt->payload_len;
  evt.dest_hash = pktHasAddressHashes(evt.type) ? pkt->payload[0] : 0;
  evt.src_hash = pktHasAddressHashes(evt.type) ? pkt->payload[1] : 0;
  evt.snr_x4 = 0;
  evt.rssi = 0;
  evt.score_x1000 = 0;
  evt.is_rx = false;

  // Capture raw bytes and hash
  evt.raw_len = pkt->writeTo(evt.raw);
  pkt->calculatePacketHash(evt.hash);

  _ringHead = next;
}

void MqttTelemetry::drainRingBuffer() {
  // Use static buffers to avoid ~1.3KB stack usage
  static char buf[740];
  static char hex_tmp[MAX_TRANS_UNIT * 2 + 1];
  static char hash_hex[MAX_HASH_SIZE * 2 + 1];
  char ts[24];

  while (_ringTail != _ringHead) {
    PacketEvent& evt = _ring[_ringTail];

    formatTimestamp(ts, evt.timestamp_epoch);
    formatHex(hash_hex, evt.hash, MAX_HASH_SIZE);
    formatHex(hex_tmp, evt.raw, evt.raw_len);

    const char* route = (evt.route_type == ROUTE_TYPE_FLOOD || evt.route_type == ROUTE_TYPE_TRANSPORT_FLOOD) ? "F" : "D";

    int n = snprintf(buf, sizeof(buf),
      "{\"timestamp\":\"%s\",\"type\":\"PACKET\","
      "\"direction\":\"%s\","
      "\"len\":%u,\"packet_type\":\"%u\",\"route\":\"%s\","
      "\"payload_len\":%u,"
      "\"raw\":\"%s\","
      "\"SNR\":%d,\"RSSI\":%d,\"score\":%d,"
      "\"hash\":\"%s\"",
      ts, evt.is_rx ? "rx" : "tx",
      evt.raw_len, evt.type, route,
      evt.payload_len,
      hex_tmp,
      (int)(evt.snr_x4 / 4), (int)evt.rssi, (int)evt.score_x1000,
      hash_hex);

    // Add path field for packets with src/dest hashes
    if (evt.dest_hash || evt.src_hash) {
      n += snprintf(buf + n, sizeof(buf) - n,
        ",\"path\":\"%02X -> %02X\"", evt.src_hash, evt.dest_hash);
    }

    snprintf(buf + n, sizeof(buf) - n, "}");

    buildTopic("packets");
    _mqttClient.publish(_topicBuf, buf);
    _ringTail = (_ringTail + 1) % MQTT_RING_BUF_SIZE;
  }
}

void MqttTelemetry::publishAdvert(const mesh::Identity& id, uint32_t timestamp,
                                   const uint8_t* app_data, size_t app_data_len,
                                   float snr, uint8_t hops) {
  if (!_mqttClient.connected()) return;

  // Parse advert data
  AdvertDataParser parser(app_data, app_data_len);
  if (!parser.isValid()) return;

  static char buf[384];
  char pub_hex[PUB_KEY_SIZE * 2 + 1];
  char ts[24];
  formatHex(pub_hex, id.pub_key, PUB_KEY_SIZE);
  formatTimestamp(ts, _callbacks ? _callbacks->getEpochTime() : 0);

  const char* type_str;
  switch (parser.getType()) {
    case ADV_TYPE_CHAT: type_str = "chat"; break;
    case ADV_TYPE_REPEATER: type_str = "repeater"; break;
    case ADV_TYPE_ROOM: type_str = "room"; break;
    case ADV_TYPE_SENSOR: type_str = "sensor"; break;
    default: type_str = "unknown"; break;
  }

  int n = snprintf(buf, sizeof(buf),
    "{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\","
    "\"timestamp\":\"%s\",\"advert_ts\":%lu,"
    "\"snr\":%.2f,\"hops\":%u",
    pub_hex,
    parser.hasName() ? parser.getName() : "",
    type_str,
    ts, (unsigned long)timestamp,
    snr, hops);

  if (parser.hasLatLon()) {
    n += snprintf(buf + n, sizeof(buf) - n,
      ",\"lat\":%.6f,\"lon\":%.6f",
      parser.getLat(), parser.getLon());
  }

  snprintf(buf + n, sizeof(buf) - n, "}");

  buildTopic("adverts");
  _mqttClient.publish(_topicBuf, buf);
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
    // Show full status and settings
    IPAddress broker(_config.broker_ip);
    int n = sprintf(reply, "MQTT: %s",
            !_config.enabled ? "disabled" :
            _mqttClient.connected() ? "connected" : "disconnected");
    if (_config.broker_ip != 0) {
      n += sprintf(reply + n, " mqtt://");
      if (_config.username[0]) {
        n += sprintf(reply + n, "%s@", _config.username);
      }
      n += sprintf(reply + n, "%u.%u.%u.%u:%u",
              broker[0], broker[1], broker[2], broker[3], _config.port);
    }
    sprintf(reply + n, " prefix=%s interval=%us", _config.prefix, _config.interval_secs);
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

  // mqtt mqtt://user:pass@broker:port  (or just mqtt broker_ip)
  if (strncmp(arg, "mqtt://", 7) == 0 || strchr(arg, '.') != nullptr) {
    const char* p = arg;
    if (strncmp(p, "mqtt://", 7) == 0) p += 7;

    // Parse optional user:pass@
    const char* at = strchr(p, '@');
    if (at) {
      // Everything before @ is user[:pass]
      char userpass[64];
      int uplen = min((int)(at - p), (int)sizeof(userpass) - 1);
      strncpy(userpass, p, uplen);
      userpass[uplen] = 0;

      char* colon = strchr(userpass, ':');
      if (colon) {
        *colon = 0;
        strncpy(_config.username, userpass, sizeof(_config.username) - 1);
        _config.username[sizeof(_config.username) - 1] = 0;
        strncpy(_config.password, colon + 1, sizeof(_config.password) - 1);
        _config.password[sizeof(_config.password) - 1] = 0;
      } else {
        strncpy(_config.username, userpass, sizeof(_config.username) - 1);
        _config.username[sizeof(_config.username) - 1] = 0;
        _config.password[0] = 0;
      }
      p = at + 1;
    }

    // Parse broker_ip[:port]
    char host[32];
    const char* colon = strchr(p, ':');
    if (colon) {
      int hlen = min((int)(colon - p), (int)sizeof(host) - 1);
      strncpy(host, p, hlen);
      host[hlen] = 0;
      int port = atoi(colon + 1);
      if (port > 0 && port <= 65535) {
        _config.port = (uint16_t)port;
      }
    } else {
      strncpy(host, p, sizeof(host) - 1);
      host[sizeof(host) - 1] = 0;
    }

    IPAddress ip;
    if (ip.fromString(host)) {
      _config.broker_ip = (uint32_t)ip;
      if (_mqttClient.connected()) {
        publishStatus(false);
        _mqttClient.disconnect();
      }
      _reconnectInterval = MQTT_RECONNECT_MIN_MS;
      _lastReconnectAttempt = 0;
      saveConfig();
      IPAddress broker(_config.broker_ip);
      sprintf(reply, "MQTT: broker=%u.%u.%u.%u:%u", broker[0], broker[1], broker[2], broker[3], _config.port);
    } else {
      strcpy(reply, "Err - invalid IP");
    }
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
