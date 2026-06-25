#include "ethernet_component.h"

#if defined(USE_ETHERNET) && defined(USE_ESP32)

#include "esp_eth_w6100.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>
#include <lwip/dns.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_rom_gpio.h"

namespace esphome::ethernet {

static const char *const TAG = "ethernet";

static void configure_optional_input_pin(int pin) {
  if (pin < 0)
    return;
  esp_rom_gpio_pad_select_gpio(pin);
  gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT);
  gpio_set_pull_mode(static_cast<gpio_num_t>(pin), GPIO_FLOATING);
}

static const char *active_low_state(int pin, const char *active, const char *inactive) {
  if (pin < 0)
    return "not configured";
  return gpio_get_level(static_cast<gpio_num_t>(pin)) == 0 ? active : inactive;
}

void EthernetComponent::log_error_and_mark_failed_(esp_err_t err, const char *message) {
  ESP_LOGE(TAG, "%s: (%d) %s", message, err, esp_err_to_name(err));
  this->mark_failed();
}

#define ESPHL_ERROR_CHECK(err, message) \
  if ((err) != ESP_OK) { \
    this->log_error_and_mark_failed_(err, message); \
    return; \
  }

void EthernetComponent::setup() {
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
    delay(300);  // NOLINT
  }
  this->ethernet_lazy_init_();
  if (!this->ethernet_initialized_) {
    return;
  }
  esp_err_t err = esp_eth_start(this->eth_handle_);
  ESPHL_ERROR_CHECK(err, "ETH start error");
}

void EthernetComponent::loop() {
  const uint32_t now = App.get_loop_component_start_time();

  switch (this->state_) {
    case EthernetComponentState::STOPPED:
      if (this->started_) {
        ESP_LOGI(TAG, "Starting connection");
        this->state_ = EthernetComponentState::CONNECTING;
        this->start_connect_();
      }
      break;
    case EthernetComponentState::CONNECTING:
      if (!this->started_) {
        ESP_LOGI(TAG, "Stopped connection");
        this->state_ = EthernetComponentState::STOPPED;
      } else if (this->connected_) {
        ESP_LOGI(TAG, "Connected");
        this->state_ = EthernetComponentState::CONNECTED;
        this->dump_connect_params_();
        this->status_clear_warning();
#ifdef USE_ETHERNET_CONNECT_TRIGGER
        this->connect_trigger_.trigger();
#endif
      } else if (now - this->connect_begin_ > 15000) {
        ESP_LOGW(TAG, "Connecting failed; reconnecting");
        this->start_connect_();
      }
      break;
    case EthernetComponentState::CONNECTED:
      if (!this->started_) {
        ESP_LOGI(TAG, "Stopped connection");
        this->state_ = EthernetComponentState::STOPPED;
#ifdef USE_ETHERNET_DISCONNECT_TRIGGER
        this->disconnect_trigger_.trigger();
#endif
      } else if (!this->connected_) {
        ESP_LOGW(TAG, "Connection lost; reconnecting");
        this->state_ = EthernetComponentState::CONNECTING;
        this->start_connect_();
#ifdef USE_ETHERNET_DISCONNECT_TRIGGER
        this->disconnect_trigger_.trigger();
#endif
      } else {
        this->finish_connect_();
        this->disable_loop();
      }
      break;
  }
}

void EthernetComponent::ethernet_lazy_init_() {
  if (this->ethernet_initialized_)
    return;

  gpio_install_isr_service(0);

  spi_bus_config_t buscfg = {
      .mosi_io_num = this->mosi_pin_,
      .miso_io_num = this->miso_pin_,
      .sclk_io_num = this->clk_pin_,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .data4_io_num = -1,
      .data5_io_num = -1,
      .data6_io_num = -1,
      .data7_io_num = -1,
      .max_transfer_sz = 0,
      .flags = 0,
      .intr_flags = 0,
  };

  esp_err_t err = spi_bus_initialize(this->interface_, &buscfg, SPI_DMA_CH_AUTO);
  ESPHL_ERROR_CHECK(err, "SPI bus initialize error");

  configure_optional_input_pin(this->ncol_pin_);
  configure_optional_input_pin(this->nspd_pin_);
  configure_optional_input_pin(this->ndpx_pin_);

  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
  this->eth_netif_ = esp_netif_new(&cfg);
  if (this->eth_netif_ == nullptr) {
    this->log_error_and_mark_failed_(ESP_ERR_NO_MEM, "ETH netif create error");
    return;
  }

#ifdef USE_ETHERNET_MANUAL_IP
  if (this->manual_ip_.has_value()) {
    esp_netif_dhcpc_stop(this->eth_netif_);
    esp_netif_ip_info_t info;
    info.ip = this->manual_ip_->static_ip;
    info.gw = this->manual_ip_->gateway;
    info.netmask = this->manual_ip_->subnet;
    ESPHL_ERROR_CHECK(esp_netif_set_ip_info(this->eth_netif_, &info), "ETH static IP config error");
  }
#endif

  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.reset_gpio_num = this->reset_pin_;

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

  spi_device_interface_config_t devcfg = {
      .command_bits = 16,
      .address_bits = 8,
      .dummy_bits = 0,
      .mode = 0,
      .duty_cycle_pos = 0,
      .cs_ena_pretrans = 0,
      .cs_ena_posttrans = 0,
      .clock_speed_hz = this->clock_speed_,
      .input_delay_ns = 0,
      .spics_io_num = this->cs_pin_,
      .flags = 0,
      .queue_size = 20,
      .pre_cb = nullptr,
      .post_cb = nullptr,
  };

  eth_w6100_config_t w6100_config = ETH_W6100_DEFAULT_CONFIG(this->interface_, &devcfg);
  w6100_config.int_gpio_num = this->interrupt_pin_;
#ifdef USE_ETHERNET_SPI_POLLING_SUPPORT
  w6100_config.poll_period_ms = this->polling_interval_;
#endif

  esp_eth_mac_t *mac = esp_eth_mac_new_w6100(&w6100_config, &mac_config);
  this->phy_ = esp_eth_phy_new_w6100(&phy_config);
  if (mac == nullptr || this->phy_ == nullptr) {
    this->log_error_and_mark_failed_(ESP_ERR_NO_MEM, "W6100 driver create error");
    return;
  }

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, this->phy_);
  err = esp_eth_driver_install(&eth_config, &this->eth_handle_);
  ESPHL_ERROR_CHECK(err, "ETH driver install error");

  uint8_t mac_addr[6];
  if (this->fixed_mac_.has_value()) {
    memcpy(mac_addr, this->fixed_mac_->data(), 6);
  } else {
    esp_read_mac(mac_addr, ESP_MAC_ETH);
  }
  err = esp_eth_ioctl(this->eth_handle_, ETH_CMD_S_MAC_ADDR, mac_addr);
  ESPHL_ERROR_CHECK(err, "set mac address error");

  err = esp_netif_attach(this->eth_netif_, esp_eth_new_netif_glue(this->eth_handle_));
  ESPHL_ERROR_CHECK(err, "ETH netif attach error");

  err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &EthernetComponent::eth_event_handler, nullptr);
  ESPHL_ERROR_CHECK(err, "ETH event handler register error");
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &EthernetComponent::got_ip_event_handler, nullptr);
  ESPHL_ERROR_CHECK(err, "GOT IP event handler register error");
#if LWIP_IPV6
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &EthernetComponent::got_ip6_event_handler, nullptr);
  ESPHL_ERROR_CHECK(err, "GOT IPv6 event handler register error");
#endif

  this->ethernet_initialized_ = true;
}

void EthernetComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Ethernet:\n"
                "  Connected: %s\n"
                "  Type: W6100\n"
                "  CLK Pin: %u\n"
                "  MISO Pin: %u\n"
                "  MOSI Pin: %u\n"
                "  CS Pin: %u",
                YESNO(this->is_connected()), this->clk_pin_, this->miso_pin_, this->mosi_pin_, this->cs_pin_);
  ESP_LOGCONFIG(TAG, "  Interface: %s", this->interface_ == SPI2_HOST ? "spi2" : "spi3");
#ifdef USE_ETHERNET_SPI_POLLING_SUPPORT
  if (this->polling_interval_ != 0) {
    ESP_LOGCONFIG(TAG, "  Polling Interval: %" PRIu32 " ms", this->polling_interval_);
  } else
#endif
  {
    ESP_LOGCONFIG(TAG, "  IRQ Pin: %d", this->interrupt_pin_);
  }
  ESP_LOGCONFIG(TAG,
                "  Reset Pin: %d\n"
                "  Clock Speed: %d MHz",
                this->reset_pin_, this->clock_speed_ / 1000000);
  if (this->ncol_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  nCOL Pin: %d (%s)", this->ncol_pin_, active_low_state(this->ncol_pin_, "collision", "idle"));
  }
  if (this->nspd_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  nSPD Pin: %d (%s)", this->nspd_pin_, active_low_state(this->nspd_pin_, "100M", "10M"));
  }
  if (this->ndpx_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  nDPX Pin: %d (%s)", this->ndpx_pin_, active_low_state(this->ndpx_pin_, "full", "half"));
  }
  this->dump_connect_params_();
}

void EthernetComponent::start_connect_() {
  this->connect_begin_ = millis();
  this->status_set_warning();
}

void EthernetComponent::finish_connect_() {
#ifdef USE_ETHERNET_MANUAL_IP
  if (this->manual_ip_.has_value()) {
    ip_addr_t d1;
    ip_addr_t d2;
    d1.type = IPADDR_TYPE_V4;
    d2.type = IPADDR_TYPE_V4;
    d1.u_addr.ip4 = this->manual_ip_->dns1;
    d2.u_addr.ip4 = this->manual_ip_->dns2;
    dns_setserver(0, &d1);
    dns_setserver(1, &d2);
  }
#endif
}

void EthernetComponent::dump_connect_params_() {
  auto ips = this->get_ip_addresses();
  char ip[network::IP_ADDRESS_BUFFER_SIZE];
  char dns[network::IP_ADDRESS_BUFFER_SIZE];
  ESP_LOGCONFIG(TAG, "  IP Address: %s", ips[0].str_to(ip));
  ESP_LOGCONFIG(TAG, "  DNS Address: %s", this->get_dns_address(0).str_to(dns));
}

network::IPAddresses EthernetComponent::get_ip_addresses() {
  network::IPAddresses addresses;
  if (!this->ethernet_initialized_)
    return addresses;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(this->eth_netif_, &ip) == ESP_OK) {
    addresses[0] = network::IPAddress(&ip.ip);
  }
#if LWIP_IPV6
  esp_ip6_addr_t if_ip6s[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  uint8_t count = esp_netif_get_all_ip6(this->eth_netif_, if_ip6s);
  for (int i = 0; i < count && i + 1 < addresses.size(); i++) {
    addresses[i + 1] = network::IPAddress(&if_ip6s[i]);
  }
#endif
  return addresses;
}

network::IPAddress EthernetComponent::get_dns_address(uint8_t num) {
  LwIPLock lock;
  return dns_getserver(num);
}

void EthernetComponent::get_eth_mac_address_raw(uint8_t *mac) {
  if (this->eth_handle_ != nullptr) {
    esp_eth_ioctl(this->eth_handle_, ETH_CMD_G_MAC_ADDR, mac);
  }
}

std::string EthernetComponent::get_eth_mac_address_pretty() {
  std::array<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> buf;
  return this->get_eth_mac_address_pretty_into_buffer(buf);
}

const char *EthernetComponent::get_eth_mac_address_pretty_into_buffer(
    std::span<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> buf) {
  uint8_t mac[6] = {};
  this->get_eth_mac_address_raw(mac);
  snprintf(buf.data(), buf.size(), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf.data();
}

eth_duplex_t EthernetComponent::get_duplex_mode() {
  eth_duplex_t duplex = ETH_DUPLEX_HALF;
  if (this->eth_handle_ != nullptr) {
    esp_eth_ioctl(this->eth_handle_, ETH_CMD_G_DUPLEX_MODE, &duplex);
  }
  return duplex;
}

eth_speed_t EthernetComponent::get_link_speed() {
  eth_speed_t speed = ETH_SPEED_10M;
  if (this->eth_handle_ != nullptr) {
    esp_eth_ioctl(this->eth_handle_, ETH_CMD_G_SPEED, &speed);
  }
  return speed;
}

bool EthernetComponent::powerdown() {
  if (this->eth_handle_ == nullptr)
    return true;
  return esp_eth_stop(this->eth_handle_) == ESP_OK;
}

void EthernetComponent::eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event, void *event_data) {
  switch (event) {
    case ETHERNET_EVENT_START:
      global_eth_component->started_ = true;
      global_eth_component->enable_loop_soon_any_context();
      ESP_LOGV(TAG, "[Ethernet event] ETH started");
      break;
    case ETHERNET_EVENT_STOP:
      global_eth_component->started_ = false;
      global_eth_component->connected_ = false;
      global_eth_component->enable_loop_soon_any_context();
      ESP_LOGV(TAG, "[Ethernet event] ETH stopped");
      break;
    case ETHERNET_EVENT_CONNECTED:
      ESP_LOGV(TAG, "[Ethernet event] ETH connected");
#ifdef USE_ETHERNET_MANUAL_IP
      if (global_eth_component->manual_ip_.has_value()) {
        global_eth_component->connected_ = true;
        global_eth_component->enable_loop_soon_any_context();
      }
#endif
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      global_eth_component->connected_ = false;
      global_eth_component->enable_loop_soon_any_context();
      ESP_LOGV(TAG, "[Ethernet event] ETH disconnected");
      break;
    default:
      break;
  }
}

void EthernetComponent::got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                             void *event_data) {
  auto *event = static_cast<ip_event_got_ip_t *>(event_data);
  ESP_LOGV(TAG, "[Ethernet event] ETH Got IP " IPSTR, IP2STR(&event->ip_info.ip));
  global_eth_component->connected_ = true;
  global_eth_component->enable_loop_soon_any_context();
}

#if LWIP_IPV6
void EthernetComponent::got_ip6_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                              void *event_data) {
  global_eth_component->connected_ = true;
  global_eth_component->enable_loop_soon_any_context();
}
#endif

}  // namespace esphome::ethernet

#endif
