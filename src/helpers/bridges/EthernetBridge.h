#pragma once

#include "helpers/bridges/BridgeBase.h"

#ifdef WITH_ETHERNET_BRIDGE

#include <EthernetUdp.h>

#ifndef ETHERNET_BRIDGE_PORT
  #define ETHERNET_BRIDGE_PORT 5005
#endif

/**
 * @brief Bridge implementation using UDP over Ethernet for packet transport
 *
 * Enables mesh packet transport over Ethernet via UDP datagrams.
 * Uses the same wire format as RS232Bridge for cross-bridge compatibility:
 *
 * Packet structure:
 * [2 bytes] Magic Header (0xC03E)
 * [2 bytes] Payload Length
 * [n bytes] Mesh Packet Payload
 * [2 bytes] Fletcher-16 Checksum
 *
 * Modes:
 * - Broadcast (default): sends to 255.255.255.255 — useful for passive analysis/tap
 * - Unicast: define ETHERNET_BRIDGE_DEST_IP to target a specific peer node for bridging
 *
 * All modes listen on ETHERNET_BRIDGE_PORT for inbound packet injection.
 *
 * Configuration:
 * - Define WITH_ETHERNET_BRIDGE to enable this bridge
 * - Define ETHERNET_BRIDGE_PORT to override the default UDP port (5005)
 * - Define ETHERNET_BRIDGE_DEST_IP as an IPAddress literal for unicast/bridge mode
 *   e.g. -D ETHERNET_BRIDGE_DEST_IP="IPAddress(192,168,1,100)"
 * - Ethernet must already be initialized before calling begin()
 */
class EthernetBridge : public BridgeBase {
public:
  /**
   * @brief Constructs an EthernetBridge instance
   *
   * @param prefs Node preferences for configuration settings
   * @param mgr PacketManager for allocating and queuing packets
   * @param rtc RTCClock for timestamping debug messages
   */
  EthernetBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  /**
   * Opens the UDP socket and begins listening.
   * Assumes Ethernet.begin() has already been called.
   */
  void begin() override;

  /**
   * Closes the UDP socket.
   */
  void end() override;

  /**
   * Checks for incoming UDP datagrams and injects valid packets into the mesh.
   * Call from the main loop.
   */
  void loop() override;

  /**
   * Serializes and transmits a mesh packet as a UDP datagram.
   * Uses duplicate detection to avoid retransmitting packets received from the bridge.
   *
   * @param packet The mesh packet to transmit
   */
  void sendPacket(mesh::Packet *packet) override;

  /**
   * Forwards a received and validated packet into the mesh inbound queue.
   *
   * @param packet The received mesh packet
   */
  void onPacketReceived(mesh::Packet *packet) override;

private:
  static constexpr uint16_t UDP_OVERHEAD =
      BRIDGE_MAGIC_SIZE + BRIDGE_LENGTH_SIZE + BRIDGE_CHECKSUM_SIZE;
  static constexpr uint16_t MAX_UDP_PACKET_SIZE = (MAX_TRANS_UNIT + 1) + UDP_OVERHEAD;

  EthernetUDP _udp;
  IPAddress _dest_ip;
  uint8_t _rx_buffer[MAX_UDP_PACKET_SIZE];
};

#endif  // WITH_ETHERNET_BRIDGE
