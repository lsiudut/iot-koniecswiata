#include <stdlib.h>
#include <sys/cdefs.h>

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_eth_w6100.h"
#include "w6100.h"

static const char *TAG = "w6100-phy";

#define PHY_CHECK(a, str, goto_tag, ...) \
  do { \
    if (!(a)) { \
      ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
      goto goto_tag; \
    } \
  } while (0)

typedef struct {
  esp_eth_phy_t parent;
  esp_eth_mediator_t *eth;
  uint32_t addr;
  eth_link_t link_status;
  int reset_gpio_num;
} phy_w6100_t;

static esp_err_t w6100_update_link_duplex_speed(phy_w6100_t *w6100) {
  esp_eth_mediator_t *eth = w6100->eth;
  uint32_t physr = 0;
  PHY_CHECK(eth->phy_reg_read(eth, w6100->addr, W6100_REG_PHYSR, &physr) == ESP_OK, "read PHYSR failed", err);

  eth_link_t link = (physr & W6100_PHYSR_LINK) ? ETH_LINK_UP : ETH_LINK_DOWN;
  if (w6100->link_status != link) {
    if (link == ETH_LINK_UP) {
      eth_speed_t speed = (physr & W6100_PHYSR_SPEED_10M) ? ETH_SPEED_10M : ETH_SPEED_100M;
      eth_duplex_t duplex = (physr & W6100_PHYSR_DUPLEX_HALF) ? ETH_DUPLEX_HALF : ETH_DUPLEX_FULL;
      PHY_CHECK(eth->on_state_changed(eth, ETH_STATE_SPEED, (void *) speed) == ESP_OK, "change speed failed", err);
      PHY_CHECK(eth->on_state_changed(eth, ETH_STATE_DUPLEX, (void *) duplex) == ESP_OK, "change duplex failed", err);
    }
    PHY_CHECK(eth->on_state_changed(eth, ETH_STATE_LINK, (void *) link) == ESP_OK, "change link failed", err);
    w6100->link_status = link;
  }
  return ESP_OK;
err:
  return ESP_FAIL;
}

static esp_err_t w6100_set_mediator(esp_eth_phy_t *phy, esp_eth_mediator_t *eth) {
  PHY_CHECK(eth, "can't set mediator to null", err);
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  w6100->eth = eth;
  return ESP_OK;
err:
  return ESP_ERR_INVALID_ARG;
}

static esp_err_t w6100_get_link(esp_eth_phy_t *phy) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  PHY_CHECK(w6100_update_link_duplex_speed(w6100) == ESP_OK, "update link duplex speed failed", err);
  return ESP_OK;
err:
  return ESP_FAIL;
}

static esp_err_t w6100_reset(esp_eth_phy_t *phy) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  esp_eth_mediator_t *eth = w6100->eth;
  w6100->link_status = ETH_LINK_DOWN;
  PHY_CHECK(eth->phy_reg_write(eth, w6100->addr, W6100_REG_PHYLCKR, W6100_PHY_UNLOCK) == ESP_OK, "unlock PHY failed",
            err);
  PHY_CHECK(eth->phy_reg_write(eth, w6100->addr, W6100_REG_PHYCR0, W6100_PHYCR0_AUTO) == ESP_OK,
            "write PHYCR0 failed", err);
  PHY_CHECK(eth->phy_reg_write(eth, w6100->addr, W6100_REG_PHYCR1, W6100_PHYCR1_RST) == ESP_OK,
            "write PHYCR1 failed", err);
  vTaskDelay(pdMS_TO_TICKS(70));
  return ESP_OK;
err:
  return ESP_FAIL;
}

static esp_err_t w6100_reset_hw(esp_eth_phy_t *phy) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  if (w6100->reset_gpio_num >= 0) {
    esp_rom_gpio_pad_select_gpio(w6100->reset_gpio_num);
    gpio_set_direction(w6100->reset_gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(w6100->reset_gpio_num, 0);
    esp_rom_delay_us(100);
    gpio_set_level(w6100->reset_gpio_num, 1);
    vTaskDelay(pdMS_TO_TICKS(70));
  }
  return ESP_OK;
}

static esp_err_t w6100_negotiate(esp_eth_phy_t *phy) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  PHY_CHECK(w6100_update_link_duplex_speed(w6100) == ESP_OK, "update link duplex speed failed", err);
  return ESP_OK;
err:
  return ESP_FAIL;
}

static esp_err_t w6100_pwrctl(esp_eth_phy_t *phy, bool enable) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  esp_eth_mediator_t *eth = w6100->eth;
  uint32_t phycr1 = 0;
  PHY_CHECK(eth->phy_reg_read(eth, w6100->addr, W6100_REG_PHYCR1, &phycr1) == ESP_OK, "read PHYCR1 failed", err);
  if (enable) {
    phycr1 &= ~W6100_PHYCR1_PWDN;
  } else {
    phycr1 |= W6100_PHYCR1_PWDN;
  }
  PHY_CHECK(eth->phy_reg_write(eth, w6100->addr, W6100_REG_PHYLCKR, W6100_PHY_UNLOCK) == ESP_OK, "unlock PHY failed",
            err);
  PHY_CHECK(eth->phy_reg_write(eth, w6100->addr, W6100_REG_PHYCR1, phycr1) == ESP_OK, "write PHYCR1 failed", err);
  return ESP_OK;
err:
  return ESP_FAIL;
}

static esp_err_t w6100_set_addr(esp_eth_phy_t *phy, uint32_t addr) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  w6100->addr = addr;
  return ESP_OK;
}

static esp_err_t w6100_get_addr(esp_eth_phy_t *phy, uint32_t *addr) {
  PHY_CHECK(addr, "addr can't be null", err);
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  *addr = w6100->addr;
  return ESP_OK;
err:
  return ESP_ERR_INVALID_ARG;
}

static esp_err_t w6100_del(esp_eth_phy_t *phy) {
  phy_w6100_t *w6100 = __containerof(phy, phy_w6100_t, parent);
  free(w6100);
  return ESP_OK;
}

static esp_err_t w6100_advertise_pause_ability(esp_eth_phy_t *phy, uint32_t ability) { return ESP_OK; }

static esp_err_t w6100_init(esp_eth_phy_t *phy) {
  PHY_CHECK(w6100_pwrctl(phy, true) == ESP_OK, "power control failed", err);
  PHY_CHECK(w6100_reset(phy) == ESP_OK, "reset failed", err);
  return ESP_OK;
err:
  return ESP_FAIL;
}

static esp_err_t w6100_deinit(esp_eth_phy_t *phy) {
  PHY_CHECK(w6100_pwrctl(phy, false) == ESP_OK, "power control failed", err);
  return ESP_OK;
err:
  return ESP_FAIL;
}

esp_eth_phy_t *esp_eth_phy_new_w6100(const eth_phy_config_t *config) {
  PHY_CHECK(config, "invalid arguments", err);
  phy_w6100_t *w6100 = calloc(1, sizeof(phy_w6100_t));
  PHY_CHECK(w6100, "no mem for PHY instance", err);
  w6100->addr = config->phy_addr;
  w6100->reset_gpio_num = config->reset_gpio_num;
  w6100->link_status = ETH_LINK_DOWN;
  w6100->parent.reset = w6100_reset;
  w6100->parent.reset_hw = w6100_reset_hw;
  w6100->parent.init = w6100_init;
  w6100->parent.deinit = w6100_deinit;
  w6100->parent.set_mediator = w6100_set_mediator;
  w6100->parent.get_link = w6100_get_link;
  w6100->parent.pwrctl = w6100_pwrctl;
  w6100->parent.get_addr = w6100_get_addr;
  w6100->parent.set_addr = w6100_set_addr;
  w6100->parent.advertise_pause_ability = w6100_advertise_pause_ability;
  w6100->parent.del = w6100_del;
  return &w6100->parent;
err:
  return NULL;
}
