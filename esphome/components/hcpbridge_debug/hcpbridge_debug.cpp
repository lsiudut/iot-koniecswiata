#include "hcpbridge_debug.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <string>

#include "esphome/components/hcpbridge/hoermann.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>

// Explicit template instantiation may name private members. This is used to
// reach the ModbusRTU instance owned by the upstream singleton without
// patching or vendoring HCPBridge.
namespace {

template<typename Tag, typename Tag::type M> struct PrivateAccess {
  friend typename Tag::type get_member(Tag) { return M; }
};

struct EngineModbusTag {
  using type = ModbusRTU HoermannGarageEngine::*;
  friend type get_member(EngineModbusTag);
};
template struct PrivateAccess<EngineModbusTag, &HoermannGarageEngine::mb>;

struct RtuPortTag {
  using type = Stream *ModbusRTUTemplate::*;
  friend type get_member(RtuPortTag);
};
template struct PrivateAccess<RtuPortTag, &ModbusRTUTemplate::_port>;

}  // namespace

namespace esphome::hcpbridge_debug {

static const char *const TAG = "hcpbridge_debug";

// Upstream HCPBridge constants (hoermann.h / hoermann.cpp).
static constexpr uint8_t HCP_SLAVE_ID = SLAVE_ID;
static constexpr uint32_t FRAME_GAP_US = 1000;
static constexpr uint32_t ECHO_WINDOW_MS = 50;

// Transparent Stream wrapper that mirrors every byte into HCPBridgeDebug.
class TapStream : public Stream {
 public:
  TapStream(Stream *inner, HCPBridgeDebug *parent) : inner_(inner), parent_(parent) {}

  int available() override { return this->inner_->available(); }
  int peek() override { return this->inner_->peek(); }
  int read() override {
    int c = this->inner_->read();
    if (c >= 0)
      this->parent_->record_byte(HCPBridgeDebug::DIR_RX, static_cast<uint8_t>(c));
    return c;
  }
  size_t write(uint8_t c) override {
    this->parent_->record_byte(HCPBridgeDebug::DIR_TX, c);
    return this->inner_->write(c);
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    for (size_t i = 0; i < size; i++)
      this->parent_->record_byte(HCPBridgeDebug::DIR_TX, buffer[i]);
    return this->inner_->write(buffer, size);
  }
  int availableForWrite() override { return this->inner_->availableForWrite(); }
  void flush() override { this->inner_->flush(); }

 protected:
  Stream *inner_;
  HCPBridgeDebug *parent_;
};

static uint16_t modbus_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }

void HCPBridgeDebug::setup() {
  this->setup_pcnt_();
  this->tap_installed_ = this->install_tap_();
  this->last_report_ = millis();
}

bool HCPBridgeDebug::install_tap_() {
  ModbusRTU &mb = HoermannGarageEngine::getInstance().*get_member(EngineModbusTag{});
  Stream *&port = mb.*get_member(RtuPortTag{});
  if (port == nullptr)
    return false;

  // The Modbus task may be inside task() while the pointer changes. Both the
  // old and new pointers lead to the same UART, so either value is safe.
  this->tap_ = new TapStream(port, this);  // NOLINT
  port = this->tap_;

  RS485.onReceiveError([this](hardwareSerial_error_t err) {
    switch (err) {
      case UART_BREAK_ERROR:
        this->err_break_++;
        break;
      case UART_FRAME_ERROR:
        this->err_frame_++;
        break;
      case UART_PARITY_ERROR:
        this->err_parity_++;
        break;
      case UART_FIFO_OVF_ERROR:
      case UART_BUFFER_FULL_ERROR:
        this->err_overflow_++;
        break;
      default:
        break;
    }
  });
  return true;
}

void HCPBridgeDebug::setup_pcnt_() {
#ifdef HCPBRIDGE_DEBUG_HAS_PCNT
  if (this->rx_pin_ < 0)
    return;
  pcnt_unit_config_t unit_config{};
  unit_config.low_limit = -1;
  unit_config.high_limit = 32767;
  if (pcnt_new_unit(&unit_config, &this->pcnt_unit_) != ESP_OK) {
    ESP_LOGW(TAG, "PCNT unit allocation failed; RX edge counting disabled");
    this->pcnt_unit_ = nullptr;
    return;
  }
  // The PCNT channel only taps the pad through the GPIO matrix; the UART RX
  // routing is left untouched. One bit at 57600 baud is about 17 us.
  pcnt_glitch_filter_config_t filter{};
  filter.max_glitch_ns = 1000;
  pcnt_unit_set_glitch_filter(this->pcnt_unit_, &filter);
  pcnt_chan_config_t chan_config{};
  chan_config.edge_gpio_num = this->rx_pin_;
  chan_config.level_gpio_num = -1;
  if (pcnt_new_channel(this->pcnt_unit_, &chan_config, &this->pcnt_chan_) != ESP_OK) {
    ESP_LOGW(TAG, "PCNT channel allocation failed; RX edge counting disabled");
    pcnt_del_unit(this->pcnt_unit_);
    this->pcnt_unit_ = nullptr;
    return;
  }
  // Count falling edges: each UART start bit produces at least one.
  pcnt_channel_set_edge_action(this->pcnt_chan_, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
  pcnt_unit_enable(this->pcnt_unit_);
  pcnt_unit_clear_count(this->pcnt_unit_);
  pcnt_unit_start(this->pcnt_unit_);
#endif
}

void HCPBridgeDebug::record_byte(Direction dir, uint8_t byte) {
  uint32_t now_us = micros();
  taskENTER_CRITICAL(&this->lock_);
  if (this->current_open_ && (this->current_.dir != dir || now_us - this->last_byte_us_ > FRAME_GAP_US))
    this->close_frame_locked_();
  if (!this->current_open_) {
    this->current_.ms = millis();
    this->current_.dir = dir;
    this->current_.len = 0;
    this->current_open_ = true;
  }
  if (this->current_.len < MAX_FRAME_BYTES)
    this->current_.data[this->current_.len] = byte;
  if (this->current_.len < UINT16_MAX)
    this->current_.len++;
  this->last_byte_us_ = now_us;
  if (dir == DIR_RX) {
    this->rx_bytes_++;
  } else {
    this->tx_bytes_++;
  }
  taskEXIT_CRITICAL(&this->lock_);
}

void HCPBridgeDebug::close_frame_locked_() {
  this->current_open_ = false;
  if (this->queue_count_ == QUEUE_LEN) {
    this->dropped_frames_++;
    return;
  }
  uint8_t idx = (this->queue_head_ + this->queue_count_) % QUEUE_LEN;
  this->queue_[idx] = this->current_;
  this->queue_count_++;
}

void HCPBridgeDebug::loop() {
  // Close a frame once the bus has been idle, then drain one frame at a time
  // so the lock is never held while logging.
  taskENTER_CRITICAL(&this->lock_);
  if (this->current_open_ && micros() - this->last_byte_us_ > FRAME_GAP_US)
    this->close_frame_locked_();
  taskEXIT_CRITICAL(&this->lock_);

  Frame frame;
  while (true) {
    taskENTER_CRITICAL(&this->lock_);
    bool have = this->queue_count_ > 0;
    if (have) {
      frame = this->queue_[this->queue_head_];
      this->queue_head_ = (this->queue_head_ + 1) % QUEUE_LEN;
      this->queue_count_--;
    }
    taskEXIT_CRITICAL(&this->lock_);
    if (!have)
      break;
    this->process_frame_(frame);
  }

#ifdef HCPBRIDGE_DEBUG_HAS_PCNT
  if (this->pcnt_unit_ != nullptr) {
    int count = 0;
    if (pcnt_unit_get_count(this->pcnt_unit_, &count) == ESP_OK && count > 0) {
      pcnt_unit_clear_count(this->pcnt_unit_);
      this->period_.rx_edges += count;
    }
  }
#endif

  if (millis() - this->last_report_ >= this->report_interval_) {
    this->last_report_ = millis();
    this->report_();
  }
}

void HCPBridgeDebug::process_frame_(const Frame &frame) {
  char hex[MAX_FRAME_BYTES * 3 + 1];
  size_t shown = std::min<size_t>(frame.len, MAX_FRAME_BYTES);
  for (size_t i = 0; i < shown; i++)
    snprintf(hex + i * 3, 4, "%02X ", frame.data[i]);
  hex[shown ? shown * 3 - 1 : 0] = '\0';
  const char *trunc = frame.len > MAX_FRAME_BYTES ? " ..." : "";

  if (frame.dir == DIR_TX) {
    this->period_.tx_frames++;
    this->last_tx_ = frame;
    this->have_last_tx_ = true;
    if (this->log_frames_)
      ESP_LOGD(TAG, "TX %3u B reply      : %s%s", frame.len, hex, trunc);
    return;
  }

  this->period_.rx_frames++;
  uint32_t since_prev = this->last_rx_frame_ms_ == 0 ? 0 : frame.ms - this->last_rx_frame_ms_;
  this->last_rx_frame_ms_ = frame.ms;

  if (this->have_last_tx_ && frame.len == this->last_tx_.len && frame.ms - this->last_tx_.ms < ECHO_WINDOW_MS &&
      memcmp(frame.data, this->last_tx_.data, std::min<size_t>(frame.len, MAX_FRAME_BYTES)) == 0) {
    this->period_.echoes++;
    if (this->log_frames_)
      ESP_LOGW(TAG, "RX %3u B ECHO of own reply: %s%s", frame.len, hex, trunc);
    return;
  }

  if (frame.len < 4) {
    this->period_.short_frames++;
    if (this->log_frames_)
      ESP_LOGD(TAG, "RX %3u B short/noise (+%" PRIu32 " ms): %s", frame.len, since_prev, hex);
    return;
  }

  const char *crc_state = "n/a";
  bool crc_ok = false;
  if (frame.len <= MAX_FRAME_BYTES) {
    uint16_t crc = modbus_crc16(frame.data, frame.len - 2);
    crc_ok = frame.data[frame.len - 2] == (crc & 0xFF) && frame.data[frame.len - 1] == (crc >> 8);
    crc_state = crc_ok ? "ok" : "BAD";
  }
  if (!crc_ok) {
    this->period_.crc_bad++;
    if (this->log_frames_)
      ESP_LOGD(TAG, "RX %3u B crc=%s (+%" PRIu32 " ms): %s%s", frame.len, crc_state, since_prev, hex, trunc);
    return;
  }
  this->period_.crc_ok++;

  uint8_t addr = frame.data[0];
  uint8_t fc = frame.data[1];
  this->seen_addresses_[addr / 32] |= 1u << (addr % 32);
  if (addr == HCP_SLAVE_ID) {
    this->period_.for_us++;
  } else if (addr == 0) {
    this->period_.broadcast++;
  } else {
    this->period_.other_slave++;
  }

  char desc[96] = "";
  const uint8_t *d = frame.data;
  if (fc == 0x17 && frame.len >= 13) {
    uint16_t rd = be16(d + 2), rd_n = be16(d + 4), wr = be16(d + 6), wr_n = be16(d + 8);
    const char *kind = "";
    if (wr == 0x9C41 && rd == 0x9CB9) {
      if (wr_n == 3 && rd_n == 5) {
        kind = " BUS SCAN";
        this->period_.bus_scans++;
      } else if (wr_n == 2 && rd_n == 8) {
        kind = " command poll";
      } else if (wr_n == 2 && rd_n == 2) {
        kind = " empty command";
      }
    }
    snprintf(desc, sizeof(desc), "rw read %04X x%u write %04X x%u%s", rd, rd_n, wr, wr_n, kind);
  } else if (fc == 0x10 && frame.len >= 8) {
    uint16_t reg = be16(d + 2);
    snprintf(desc, sizeof(desc), "write %04X x%u%s", reg, be16(d + 4), reg == 0x9D31 ? " status broadcast" : "");
  } else if ((fc == 0x03 || fc == 0x04 || fc == 0x06) && frame.len >= 8) {
    snprintf(desc, sizeof(desc), "reg %04X val/count %u", be16(d + 2), be16(d + 4));
  } else if (fc & 0x80) {
    snprintf(desc, sizeof(desc), "exception 0x%02X", frame.len > 2 ? d[2] : 0);
  }

  if (this->log_frames_) {
    const char *who = addr == HCP_SLAVE_ID ? "us" : (addr == 0 ? "bcast" : "other");
    ESP_LOGD(TAG, "RX %3u B addr=%02X(%s) fc=%02X crc=ok (+%" PRIu32 " ms) %s : %s%s", frame.len, addr, who, fc,
             since_prev, desc, hex, trunc);
  }
}

void HCPBridgeDebug::report_() {
  taskENTER_CRITICAL(&this->lock_);
  this->period_.rx_bytes = this->rx_bytes_;
  this->period_.tx_bytes = this->tx_bytes_;
  this->rx_bytes_ = 0;
  this->tx_bytes_ = 0;
  uint32_t dropped = this->dropped_frames_;
  this->dropped_frames_ = 0;
  taskEXIT_CRITICAL(&this->lock_);
  this->period_.err_break = this->err_break_.exchange(0);
  this->period_.err_frame = this->err_frame_.exchange(0);
  this->period_.err_parity = this->err_parity_.exchange(0);
  this->period_.err_overflow = this->err_overflow_.exchange(0);

  const Stats &p = this->period_;
  Stats &t = this->total_;
  t.rx_frames += p.rx_frames;
  t.crc_ok += p.crc_ok;
  t.crc_bad += p.crc_bad;
  t.short_frames += p.short_frames;
  t.for_us += p.for_us;
  t.broadcast += p.broadcast;
  t.other_slave += p.other_slave;
  t.echoes += p.echoes;
  t.tx_frames += p.tx_frames;
  t.bus_scans += p.bus_scans;
  t.rx_edges += p.rx_edges;
  t.rx_bytes += p.rx_bytes;
  t.tx_bytes += p.tx_bytes;
  t.err_break += p.err_break;
  t.err_frame += p.err_frame;
  t.err_parity += p.err_parity;
  t.err_overflow += p.err_overflow;

  HoermannState *state = HoermannGarageEngine::getInstance().state;
  int rx_level = this->rx_pin_ >= 0 ? gpio_get_level(static_cast<gpio_num_t>(this->rx_pin_)) : -1;

  ESP_LOGI(TAG,
           "Last %" PRIu32 " s: RX edges=%" PRIu32 " bytes=%" PRIu32 " frames=%" PRIu32 " (crc ok=%" PRIu32
           " bad=%" PRIu32 " short=%" PRIu32 " echo=%" PRIu32 ") | TX bytes=%" PRIu32 " replies=%" PRIu32,
           this->report_interval_ / 1000, p.rx_edges, p.rx_bytes, p.rx_frames, p.crc_ok, p.crc_bad, p.short_frames,
           p.echoes, p.tx_bytes, p.tx_frames);
  ESP_LOGI(TAG,
           "  to slave %u=%" PRIu32 " broadcast=%" PRIu32 " other slaves=%" PRIu32 " bus scans=%" PRIu32
           " | UART err break=%" PRIu32 " frame=%" PRIu32 " parity=%" PRIu32 " overflow=%" PRIu32 " | dropped=%" PRIu32,
           HCP_SLAVE_ID, p.for_us, p.broadcast, p.other_slave, p.bus_scans, p.err_break, p.err_frame, p.err_parity,
           p.err_overflow, dropped);
  ESP_LOGI(TAG,
           "  totals: RX edges=%" PRIu32 " bytes=%" PRIu32 " crc ok=%" PRIu32 " bad=%" PRIu32 " to us=%" PRIu32
           " replies=%" PRIu32 " | RX idle level=%d | engine valid=%s last request age=%ld s",
           t.rx_edges, t.rx_bytes, t.crc_ok, t.crc_bad, t.for_us, t.tx_frames, rx_level, YESNO(state->valid),
           state->responseAge());

  std::string addrs;
  for (int a = 0; a < 256; a++) {
    if (this->seen_addresses_[a / 32] & (1u << (a % 32))) {
      char buf[6];
      snprintf(buf, sizeof(buf), "%s%02X", addrs.empty() ? "" : " ", a);
      addrs += buf;
    }
  }
  if (!addrs.empty())
    ESP_LOGI(TAG, "  slave addresses seen on bus: %s", addrs.c_str());

  // One hint per report, ordered from the lowest layer upwards.
  if (!this->tap_installed_) {
    ESP_LOGW(TAG, "  hint: Modbus stream tap not installed; HCPBridge engine was not started");
  } else if (t.rx_edges == 0 && t.rx_bytes == 0 && this->rx_pin_ >= 0) {
    ESP_LOGW(TAG, "  hint: no signal on RX GPIO%d since boot. Check HCP bus power/connection, A/B wiring, "
                  "and that RE is enabled (DE/RE GPIO idle low)",
             this->rx_pin_);
  } else if (p.err_break > 0 || (rx_level == 0 && p.rx_bytes == 0)) {
    ESP_LOGW(TAG, "  hint: RX line held low (break). Likely A/B swapped, missing bias, or transceiver stuck");
  } else if (p.err_frame + p.err_parity > 0 && p.crc_ok == 0) {
    ESP_LOGW(TAG, "  hint: framing/parity errors without valid frames. Check 57600 8E1, A/B polarity, noise");
  } else if (p.rx_edges > 0 && p.rx_bytes == 0) {
    ESP_LOGW(TAG, "  hint: RX edges but no UART bytes. Baud mismatch or UART not attached to RX pin");
  } else if (p.crc_bad > p.crc_ok) {
    ESP_LOGW(TAG, "  hint: mostly CRC failures. Check termination, grounding, A/B polarity and baud rate");
  } else if (p.echoes > 0) {
    ESP_LOGW(TAG, "  hint: own replies are read back. Transceiver receiver is not disabled while transmitting");
  } else if (t.crc_ok > 0 && t.for_us == 0) {
    ESP_LOGW(TAG, "  hint: motor talks on the bus but never to slave %u. Run a bus scan from the motor menu",
             HCP_SLAVE_ID);
  } else if (p.for_us > 0 && p.tx_frames == 0) {
    ESP_LOGW(TAG, "  hint: requests for slave %u are not answered by the Modbus engine", HCP_SLAVE_ID);
  } else if (p.tx_frames > 0 && t.bus_scans > 0 && p.for_us > 0) {
    ESP_LOGI(TAG, "  hint: replies are sent. If the motor still sees no device, verify DE (RTS) drives the "
                  "transceiver and replies reach the bus");
  }

  this->period_ = Stats{};
}

void HCPBridgeDebug::dump_config() {
  ESP_LOGCONFIG(TAG,
                "HCPBridge debug:\n"
                "  Stream tap: %s\n"
                "  RS485 UART baud: %" PRIu32 " (HCP expects 57600 8E1)\n"
                "  Slave address: %u\n"
                "  Log frames: %s\n"
                "  Report interval: %" PRIu32 " ms",
                this->tap_installed_ ? "installed" : "NOT installed", RS485.baudRate(), HCP_SLAVE_ID,
                YESNO(this->log_frames_), this->report_interval_);
#ifdef HCPBRIDGE_DEBUG_HAS_PCNT
  if (this->rx_pin_ >= 0)
    ESP_LOGCONFIG(TAG, "  RX edge counter: GPIO%d (%s)", this->rx_pin_, this->pcnt_unit_ ? "PCNT" : "failed");
#endif
}

}  // namespace esphome::hcpbridge_debug
