#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/network/ip_address.h"

#ifdef USE_ETHERNET

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "hal/spi_types.h"

namespace esphome::ethernet {

enum EthernetType : uint8_t {
  ETHERNET_TYPE_UNKNOWN = 0,
  ETHERNET_TYPE_W6100,
};

struct ManualIP {
  network::IPAddress static_ip;
  network::IPAddress gateway;
  network::IPAddress subnet;
  network::IPAddress dns1;
  network::IPAddress dns2;
};

enum class EthernetComponentState : uint8_t {
  STOPPED,
  CONNECTING,
  CONNECTED,
};

class EthernetComponent final : public Component {
 public:
  EthernetComponent();
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;
  void on_powerdown() override { this->powerdown(); }

  bool is_connected() { return this->state_ == EthernetComponentState::CONNECTED; }
  bool powerdown();

  void set_type(EthernetType type) { this->type_ = type; }
  void set_clk_pin(uint8_t clk_pin) { this->clk_pin_ = clk_pin; }
  void set_miso_pin(uint8_t miso_pin) { this->miso_pin_ = miso_pin; }
  void set_mosi_pin(uint8_t mosi_pin) { this->mosi_pin_ = mosi_pin; }
  void set_cs_pin(uint8_t cs_pin) { this->cs_pin_ = cs_pin; }
  void set_interrupt_pin(uint8_t interrupt_pin) { this->interrupt_pin_ = interrupt_pin; }
  void set_reset_pin(uint8_t reset_pin) { this->reset_pin_ = reset_pin; }
  void set_ncol_pin(uint8_t ncol_pin) { this->ncol_pin_ = ncol_pin; }
  void set_nspd_pin(uint8_t nspd_pin) { this->nspd_pin_ = nspd_pin; }
  void set_ndpx_pin(uint8_t ndpx_pin) { this->ndpx_pin_ = ndpx_pin; }
  void set_clock_speed(int clock_speed) { this->clock_speed_ = clock_speed; }
  void set_interface(spi_host_device_t interface) { this->interface_ = interface; }
#ifdef USE_ETHERNET_SPI_POLLING_SUPPORT
  void set_polling_interval(uint32_t polling_interval) { this->polling_interval_ = polling_interval; }
#endif
#ifdef USE_ETHERNET_MANUAL_IP
  void set_manual_ip(const ManualIP &manual_ip) { this->manual_ip_ = manual_ip; }
#endif
  void set_fixed_mac(const std::array<uint8_t, 6> &mac) { this->fixed_mac_ = mac; }

  const char *get_use_address() const { return this->use_address_; }
  void set_use_address(const char *use_address) { this->use_address_ = use_address; }
  network::IPAddresses get_ip_addresses();
  network::IPAddress get_dns_address(uint8_t num);
  void get_eth_mac_address_raw(uint8_t *mac);
  std::string get_eth_mac_address_pretty();
  const char *get_eth_mac_address_pretty_into_buffer(std::span<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> buf);
  eth_duplex_t get_duplex_mode();
  eth_speed_t get_link_speed();

#ifdef USE_ETHERNET_CONNECT_TRIGGER
  Trigger<> *get_connect_trigger() { return &this->connect_trigger_; }
#endif
#ifdef USE_ETHERNET_DISCONNECT_TRIGGER
  Trigger<> *get_disconnect_trigger() { return &this->disconnect_trigger_; }
#endif

 protected:
  void start_connect_();
  void finish_connect_();
  void dump_connect_params_();
  void ethernet_lazy_init_();
  void log_error_and_mark_failed_(esp_err_t err, const char *message);

  static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
  static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
#if LWIP_IPV6
  static void got_ip6_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
#endif

  EthernetType type_{ETHERNET_TYPE_UNKNOWN};
  EthernetComponentState state_{EthernetComponentState::STOPPED};
  bool started_{false};
  bool connected_{false};
  bool ethernet_initialized_{false};
  uint32_t connect_begin_{0};
  const char *use_address_{nullptr};

  uint8_t clk_pin_{0};
  uint8_t miso_pin_{0};
  uint8_t mosi_pin_{0};
  uint8_t cs_pin_{0};
  int interrupt_pin_{-1};
  int reset_pin_{-1};
  int ncol_pin_{-1};
  int nspd_pin_{-1};
  int ndpx_pin_{-1};
  int clock_speed_{26670000};
  spi_host_device_t interface_{SPI2_HOST};
#ifdef USE_ETHERNET_SPI_POLLING_SUPPORT
  uint32_t polling_interval_{0};
#endif

  esp_eth_handle_t eth_handle_{nullptr};
  esp_eth_phy_t *phy_{nullptr};
  esp_netif_t *eth_netif_{nullptr};
  optional<std::array<uint8_t, 6>> fixed_mac_{};
#ifdef USE_ETHERNET_MANUAL_IP
  optional<ManualIP> manual_ip_{};
#endif

#ifdef USE_ETHERNET_CONNECT_TRIGGER
  Trigger<> connect_trigger_{};
#endif
#ifdef USE_ETHERNET_DISCONNECT_TRIGGER
  Trigger<> disconnect_trigger_{};
#endif
};

extern EthernetComponent *global_eth_component;

}  // namespace esphome::ethernet

#endif  // USE_ETHERNET
