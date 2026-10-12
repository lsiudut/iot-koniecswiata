#pragma once

#include <atomic>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"

#include <freertos/FreeRTOS.h>
#include <soc/soc_caps.h>

#if defined(SOC_PCNT_SUPPORTED) && __has_include(<driver/pulse_cnt.h>)
#include <driver/pulse_cnt.h>
#define HCPBRIDGE_DEBUG_HAS_PCNT
#endif

class Stream;

namespace esphome::hcpbridge_debug {

// Passive diagnostics for the upstream HCPBridge Modbus slave.
//
// The upstream engine logs only after it receives a CRC-valid request for
// slave 2, so a dead bus, swapped A/B, wrong baud rate, and a motor that never
// scans all look identical. This component wraps the ModbusRTU stream to
// record every byte read from and written to the RS485 UART, counts raw RX
// edges with PCNT, and counts UART break/frame/parity errors. Frames are
// queued from the high-priority Modbus task and logged from the main loop so
// response timing on the bus is not affected by logging.
class HCPBridgeDebug : public Component {
 public:
  static constexpr uint8_t MAX_FRAME_BYTES = 64;
  static constexpr uint8_t QUEUE_LEN = 32;

  enum Direction : uint8_t { DIR_RX = 0, DIR_TX = 1 };

  struct Frame {
    uint32_t ms;
    uint16_t len;  // total length; data is truncated to MAX_FRAME_BYTES
    Direction dir;
    uint8_t data[MAX_FRAME_BYTES];
  };

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_rx_pin(int pin) { this->rx_pin_ = pin; }
  void set_log_frames(bool log_frames) { this->log_frames_ = log_frames; }
  void set_report_interval(uint32_t interval) { this->report_interval_ = interval; }

  // Called from the Modbus task through the stream wrapper.
  void record_byte(Direction dir, uint8_t byte);

 protected:
  bool install_tap_();
  void setup_pcnt_();
  void close_frame_locked_();
  void process_frame_(const Frame &frame);
  void report_();

  int rx_pin_{-1};
  bool log_frames_{true};
  uint32_t report_interval_{10000};
  uint32_t last_report_{0};
  bool tap_installed_{false};
  Stream *tap_{nullptr};

  // Shared with the Modbus task, protected by lock_.
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  Frame current_{};
  bool current_open_{false};
  uint32_t last_byte_us_{0};
  Frame queue_[QUEUE_LEN];
  uint8_t queue_head_{0};
  uint8_t queue_count_{0};
  uint32_t dropped_frames_{0};
  uint32_t rx_bytes_{0};
  uint32_t tx_bytes_{0};

  // UART error counters, written from the Arduino UART event task.
  std::atomic<uint32_t> err_break_{0};
  std::atomic<uint32_t> err_frame_{0};
  std::atomic<uint32_t> err_parity_{0};
  std::atomic<uint32_t> err_overflow_{0};

#ifdef HCPBRIDGE_DEBUG_HAS_PCNT
  pcnt_unit_handle_t pcnt_unit_{nullptr};
  pcnt_channel_handle_t pcnt_chan_{nullptr};
#endif
  uint32_t rx_edges_{0};

  // Main-loop statistics; "period" values reset after each report.
  struct Stats {
    uint32_t rx_frames{0};
    uint32_t crc_ok{0};
    uint32_t crc_bad{0};
    uint32_t short_frames{0};
    uint32_t for_us{0};
    uint32_t broadcast{0};
    uint32_t other_slave{0};
    uint32_t echoes{0};
    uint32_t tx_frames{0};
    uint32_t bus_scans{0};
    uint32_t rx_edges{0};
    uint32_t rx_bytes{0};
    uint32_t tx_bytes{0};
    uint32_t err_break{0};
    uint32_t err_frame{0};
    uint32_t err_parity{0};
    uint32_t err_overflow{0};
  };
  Stats total_{};
  Stats period_{};
  uint32_t seen_addresses_[8]{};  // bitmap of CRC-valid slave addresses
  Frame last_tx_{};
  bool have_last_tx_{false};
  uint32_t last_rx_frame_ms_{0};
};

}  // namespace esphome::hcpbridge_debug
