
#include "helpers/BaseSerialInterface.h"
#include <SPI.h>
#include <RAK13800_W5100S.h>

// expects ETH_ENABLED = 1
#define TCP_PORT 8080

class SerialEthernetInterface : public BaseSerialInterface {
  bool deviceConnected;
  bool _isEnabled;
  unsigned long _last_write;
  unsigned long adv_restart_time;

  EthernetServer server;
  EthernetClient client;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE 4
  int recv_queue_len;
  Frame recv_queue[FRAME_QUEUE_SIZE];
  int send_queue_len;
  Frame send_queue[FRAME_QUEUE_SIZE];

  void clearBuffers() { recv_queue_len = 0; send_queue_len = 0; }

  protected:

  public:
    SerialEthernetInterface() : server(EthernetServer(TCP_PORT)) { 
        deviceConnected = false;
        _isEnabled = false;
        _last_write = 0;
        send_queue_len = recv_queue_len = 0;
    }
    bool begin();

    // BaseSerialInterface methods
    void enable() override;
    void disable() override;
    bool isEnabled() const override { return _isEnabled; }

    bool isConnected() const override;
    bool isWriteBusy() const override;

    size_t writeFrame(const uint8_t src[], size_t len) override;
    size_t checkRecvFrame(uint8_t dest[]) override;

private:
  void generateDeviceMac(uint8_t mac[6]);
};


#if ETH_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define ETH_DEBUG_PRINT(F, ...) Serial.printf("ETH: " F, ##__VA_ARGS__)
  #define ETH_DEBUG_PRINTLN(F, ...) Serial.printf("ETH: " F "\n", ##__VA_ARGS__)
#else
  #define ETH_DEBUG_PRINT(...) {}
  #define ETH_DEBUG_PRINTLN(...) {}
#endif
