#include "EthernetBridge.h"

#ifdef WITH_ETHERNET_BRIDGE

#ifndef ETHERNET_BRIDGE_DEST_IP
  #define ETHERNET_BRIDGE_DEST_IP IPAddress(255, 255, 255, 255)
#endif

EthernetBridge::EthernetBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _dest_ip(ETHERNET_BRIDGE_DEST_IP) {}

void EthernetBridge::begin() {
  BRIDGE_DEBUG_PRINTLN("EthernetBridge: starting on UDP port %d\n", ETHERNET_BRIDGE_PORT);
  if (_udp.begin(ETHERNET_BRIDGE_PORT)) {
    _initialized = true;
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: ready\n");
  } else {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: failed to bind UDP port %d\n", ETHERNET_BRIDGE_PORT);
  }
}

void EthernetBridge::end() {
  BRIDGE_DEBUG_PRINTLN("EthernetBridge: stopping\n");
  _udp.stop();
  _initialized = false;
}

void EthernetBridge::loop() {
  if (!_initialized) return;

  int pkt_size = _udp.parsePacket();
  if (pkt_size <= 0) return;

  int len = _udp.read(_rx_buffer, sizeof(_rx_buffer));
  if (len < (int)UDP_OVERHEAD) {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX datagram too short (%d bytes)\n", len);
    return;
  }

  // Validate magic header
  uint16_t magic = ((uint16_t)_rx_buffer[0] << 8) | _rx_buffer[1];
  if (magic != BRIDGE_PACKET_MAGIC) {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX bad magic 0x%04x\n", magic);
    return;
  }

  uint16_t payload_len = ((uint16_t)_rx_buffer[2] << 8) | _rx_buffer[3];
  if (payload_len > (MAX_TRANS_UNIT + 1) || (int)(payload_len + UDP_OVERHEAD) > len) {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX invalid length %d\n", payload_len);
    return;
  }

  uint16_t received_checksum =
      ((uint16_t)_rx_buffer[4 + payload_len] << 8) | _rx_buffer[5 + payload_len];
  if (!validateChecksum(_rx_buffer + 4, payload_len, received_checksum)) {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX checksum mismatch, rcv=0x%04x\n", received_checksum);
    return;
  }

  BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX len=%d crc=0x%04x\n", payload_len, received_checksum);

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX failed to allocate packet\n");
    return;
  }

  if (pkt->readFrom(_rx_buffer + 4, payload_len)) {
    onPacketReceived(pkt);
  } else {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: RX failed to parse packet\n");
    _mgr->free(pkt);
  }
}

void EthernetBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !packet) return;

  if (_seen_packets.hasSeen(packet)) return;

  uint8_t buffer[MAX_UDP_PACKET_SIZE];
  uint16_t len = packet->writeTo(buffer + 4);

  if (len > (MAX_TRANS_UNIT + 1)) {
    BRIDGE_DEBUG_PRINTLN("EthernetBridge: TX packet too large (payload=%d, max=%d)\n",
                         len, MAX_TRANS_UNIT + 1);
    return;
  }

  buffer[0] = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF;
  buffer[1] =  BRIDGE_PACKET_MAGIC       & 0xFF;
  buffer[2] = (len >> 8) & 0xFF;
  buffer[3] =  len       & 0xFF;

  uint16_t checksum = fletcher16(buffer + 4, len);
  buffer[4 + len] = (checksum >> 8) & 0xFF;
  buffer[5 + len] =  checksum       & 0xFF;

  _udp.beginPacket(_dest_ip, ETHERNET_BRIDGE_PORT);
  _udp.write(buffer, len + UDP_OVERHEAD);
  _udp.endPacket();

  BRIDGE_DEBUG_PRINTLN("EthernetBridge: TX len=%d crc=0x%04x\n", len, checksum);
}

void EthernetBridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}

#endif  // WITH_ETHERNET_BRIDGE
