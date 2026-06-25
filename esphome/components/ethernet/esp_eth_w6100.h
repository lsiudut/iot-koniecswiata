#pragma once

#include "esp_eth.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  spi_host_device_t spi_host_id;
  spi_device_interface_config_t *spi_devcfg;
  int int_gpio_num;
  uint32_t poll_period_ms;
} eth_w6100_config_t;

#define ETH_W6100_DEFAULT_CONFIG(spi_host, spi_devcfg_p) \
  { .spi_host_id = spi_host, .spi_devcfg = spi_devcfg_p, .int_gpio_num = 4, .poll_period_ms = 0 }

esp_eth_mac_t *esp_eth_mac_new_w6100(const eth_w6100_config_t *w6100_config, const eth_mac_config_t *mac_config);
esp_eth_phy_t *esp_eth_phy_new_w6100(const eth_phy_config_t *config);

#ifdef __cplusplus
}
#endif
