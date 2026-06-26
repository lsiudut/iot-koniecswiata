#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_eth.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_eth_w6100.h"
#include "w6100.h"

static const char *TAG = "w6100-mac";

#define MAC_CHECK(a, str, goto_tag, ret_value, ...) \
  do { \
    if (!(a)) { \
      ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
      ret = ret_value; \
      goto goto_tag; \
    } \
  } while (0)

#define W6100_SPI_LOCK_TIMEOUT_MS (50)

typedef struct {
  esp_eth_mac_t parent;
  esp_eth_mediator_t *eth;
  spi_device_handle_t spi_hdl;
  SemaphoreHandle_t spi_lock;
  TaskHandle_t rx_task_hdl;
  uint32_t sw_reset_timeout_ms;
  int int_gpio_num;
  uint32_t poll_period_ms;
  uint8_t addr[6];
  uint8_t mcast_cnt;
  bool packets_remain;
} emac_w6100_t;

static inline bool w6100_lock(emac_w6100_t *emac) {
  return xSemaphoreTake(emac->spi_lock, pdMS_TO_TICKS(W6100_SPI_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static inline void w6100_unlock(emac_w6100_t *emac) { xSemaphoreGive(emac->spi_lock); }

static esp_err_t w6100_write(emac_w6100_t *emac, uint32_t address, const void *value, uint32_t len) {
  esp_err_t ret = ESP_OK;
  spi_transaction_t trans = {
      .cmd = (address >> W6100_ADDR_OFFSET),
      .addr = ((address & 0xFF) | (W6100_ACCESS_MODE_WRITE << W6100_RWB_OFFSET) | W6100_SPI_OP_MODE_VDM),
      .length = 8 * len,
      .tx_buffer = value,
  };
  if (w6100_lock(emac)) {
    ret = spi_device_polling_transmit(emac->spi_hdl, &trans);
    w6100_unlock(emac);
  } else {
    ret = ESP_ERR_TIMEOUT;
  }
  return ret;
}

static esp_err_t w6100_read(emac_w6100_t *emac, uint32_t address, void *value, uint32_t len) {
  esp_err_t ret = ESP_OK;
  spi_transaction_t trans = {
      .cmd = (address >> W6100_ADDR_OFFSET),
      .addr = ((address & 0xFF) | (W6100_ACCESS_MODE_READ << W6100_RWB_OFFSET) | W6100_SPI_OP_MODE_VDM),
      .length = 8 * len,
      .rx_buffer = value,
  };
  if (w6100_lock(emac)) {
    ret = spi_device_polling_transmit(emac->spi_hdl, &trans);
    w6100_unlock(emac);
  } else {
    ret = ESP_ERR_TIMEOUT;
  }
  return ret;
}

static esp_err_t w6100_send_command(emac_w6100_t *emac, uint8_t command, uint32_t timeout_ms) {
  esp_err_t ret = ESP_OK;
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_CR(0), &command, sizeof(command)) == ESP_OK, "write SCR failed", err,
            ESP_FAIL);
  uint32_t to = 0;
  for (to = 0; to < timeout_ms / 10; to++) {
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_CR(0), &command, sizeof(command)) == ESP_OK, "read SCR failed", err,
              ESP_FAIL);
    if (!command)
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  MAC_CHECK(to < timeout_ms / 10, "send command timeout", err, ESP_ERR_TIMEOUT);
err:
  return ret;
}

static esp_err_t w6100_get_tx_free_size(emac_w6100_t *emac, uint16_t *size) {
  esp_err_t ret = ESP_OK;
  uint16_t free0;
  uint16_t free1 = 0;
  do {
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_TX_FSR(0), &free0, sizeof(free0)) == ESP_OK, "read TX FSR failed", err,
              ESP_FAIL);
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_TX_FSR(0), &free1, sizeof(free1)) == ESP_OK, "read TX FSR failed", err,
              ESP_FAIL);
  } while (free0 != free1);
  *size = __builtin_bswap16(free0);
err:
  return ret;
}

static esp_err_t w6100_get_rx_received_size(emac_w6100_t *emac, uint16_t *size) {
  esp_err_t ret = ESP_OK;
  uint16_t received0;
  uint16_t received1 = 0;
  do {
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_RX_RSR(0), &received0, sizeof(received0)) == ESP_OK,
              "read RX RSR failed", err, ESP_FAIL);
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_RX_RSR(0), &received1, sizeof(received1)) == ESP_OK,
              "read RX RSR failed", err, ESP_FAIL);
  } while (received0 != received1);
  *size = __builtin_bswap16(received0);
err:
  return ret;
}

static esp_err_t w6100_write_buffer(emac_w6100_t *emac, const void *buffer, uint32_t len, uint16_t offset) {
  esp_err_t ret = ESP_OK;
  uint32_t remain = len;
  const uint8_t *buf = buffer;
  offset %= W6100_TX_MEM_SIZE;
  if (offset + len > W6100_TX_MEM_SIZE) {
    remain = (offset + len) % W6100_TX_MEM_SIZE;
    len = W6100_TX_MEM_SIZE - offset;
    MAC_CHECK(w6100_write(emac, W6100_MEM_SOCK_TX(0, offset), buf, len) == ESP_OK, "write TX buffer failed", err,
              ESP_FAIL);
    offset += len;
    buf += len;
  }
  MAC_CHECK(w6100_write(emac, W6100_MEM_SOCK_TX(0, offset), buf, remain) == ESP_OK, "write TX buffer failed", err,
            ESP_FAIL);
err:
  return ret;
}

static esp_err_t w6100_read_buffer(emac_w6100_t *emac, void *buffer, uint32_t len, uint16_t offset) {
  esp_err_t ret = ESP_OK;
  uint32_t remain = len;
  uint8_t *buf = buffer;
  offset %= W6100_RX_MEM_SIZE;
  if (offset + len > W6100_RX_MEM_SIZE) {
    remain = (offset + len) % W6100_RX_MEM_SIZE;
    len = W6100_RX_MEM_SIZE - offset;
    MAC_CHECK(w6100_read(emac, W6100_MEM_SOCK_RX(0, offset), buf, len) == ESP_OK, "read RX buffer failed", err,
              ESP_FAIL);
    offset += len;
    buf += len;
  }
  MAC_CHECK(w6100_read(emac, W6100_MEM_SOCK_RX(0, offset), buf, remain) == ESP_OK, "read RX buffer failed", err,
            ESP_FAIL);
err:
  return ret;
}

static esp_err_t w6100_unlock_config(emac_w6100_t *emac) {
  uint8_t unlock = W6100_CHIP_UNLOCK;
  esp_err_t ret = w6100_write(emac, W6100_REG_CHPLCKR, &unlock, sizeof(unlock));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "unlock chip failed");
    return ret;
  }
  unlock = W6100_NET_UNLOCK;
  ret = w6100_write(emac, W6100_REG_NETLCKR, &unlock, sizeof(unlock));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "unlock net failed");
    return ret;
  }
  unlock = W6100_PHY_UNLOCK;
  ret = w6100_write(emac, W6100_REG_PHYLCKR, &unlock, sizeof(unlock));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "unlock phy failed");
    return ret;
  }
  return ESP_OK;
}

static esp_err_t w6100_set_mac_addr(emac_w6100_t *emac) {
  esp_err_t ret = ESP_OK;
  uint8_t value = W6100_NET_UNLOCK;
  uint8_t readback[6] = {0};
  MAC_CHECK(w6100_write(emac, W6100_REG_NETLCKR, &value, sizeof(value)) == ESP_OK, "unlock net failed", err, ESP_FAIL);
  MAC_CHECK(w6100_write(emac, W6100_REG_SHAR, emac->addr, 6) == ESP_OK, "write SHAR failed", err, ESP_FAIL);
  MAC_CHECK(w6100_read(emac, W6100_REG_SHAR, readback, sizeof(readback)) == ESP_OK, "read SHAR failed", err, ESP_FAIL);
  value = W6100_NET_LOCK;
  MAC_CHECK(w6100_write(emac, W6100_REG_NETLCKR, &value, sizeof(value)) == ESP_OK, "lock net failed", err, ESP_FAIL);
  MAC_CHECK(memcmp(emac->addr, readback, sizeof(readback)) == 0, "SHAR readback mismatch", err, ESP_FAIL);
err:
  return ret;
}

static esp_err_t w6100_reset(emac_w6100_t *emac) {
  esp_err_t ret = ESP_OK;
  uint8_t sycr0 = W6100_SYCR0_RST;
  MAC_CHECK(w6100_unlock_config(emac) == ESP_OK, "unlock failed", err, ESP_FAIL);
  MAC_CHECK(w6100_write(emac, W6100_REG_SYCR0, &sycr0, sizeof(sycr0)) == ESP_OK, "write SYCR0 failed", err, ESP_FAIL);
  vTaskDelay(pdMS_TO_TICKS(10));
  MAC_CHECK(w6100_unlock_config(emac) == ESP_OK, "unlock after reset failed", err, ESP_FAIL);
err:
  return ret;
}

static esp_err_t w6100_verify_id(emac_w6100_t *emac) {
  esp_err_t ret = ESP_OK;
  uint16_t cidr = 0;
  uint16_t ver = 0;
  MAC_CHECK(w6100_read(emac, W6100_REG_CIDR, &cidr, sizeof(cidr)) == ESP_OK, "read CIDR failed", err, ESP_FAIL);
  MAC_CHECK(w6100_read(emac, W6100_REG_VER, &ver, sizeof(ver)) == ESP_OK, "read VER failed", err, ESP_FAIL);
  cidr = __builtin_bswap16(cidr);
  ver = __builtin_bswap16(ver);
  ESP_LOGI(TAG, "cidr=0x%04x version=0x%04x", cidr, ver);
  MAC_CHECK(cidr == 0x6100, "unexpected chip id 0x%04x", err, ESP_FAIL, cidr);
err:
  return ret;
}

static esp_err_t w6100_setup_default(emac_w6100_t *emac) {
  esp_err_t ret = ESP_OK;
  uint8_t reg_value = 16;
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_RXBUF_SIZE(0), &reg_value, sizeof(reg_value)) == ESP_OK,
            "set rx buffer size failed", err, ESP_FAIL);
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_TXBUF_SIZE(0), &reg_value, sizeof(reg_value)) == ESP_OK,
            "set tx buffer size failed", err, ESP_FAIL);
  reg_value = 0;
  for (int i = 1; i < 8; i++) {
    MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_RXBUF_SIZE(i), &reg_value, sizeof(reg_value)) == ESP_OK,
              "set rx buffer size failed", err, ESP_FAIL);
    MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_TXBUF_SIZE(i), &reg_value, sizeof(reg_value)) == ESP_OK,
              "set tx buffer size failed", err, ESP_FAIL);
  }

  reg_value = W6100_NETXMR_PB;
  MAC_CHECK(w6100_write(emac, W6100_REG_NET4MR, &reg_value, sizeof(reg_value)) == ESP_OK, "write NET4MR failed", err,
            ESP_FAIL);
  MAC_CHECK(w6100_write(emac, W6100_REG_NET6MR, &reg_value, sizeof(reg_value)) == ESP_OK, "write NET6MR failed", err,
            ESP_FAIL);
  reg_value = 0;
  MAC_CHECK(w6100_write(emac, W6100_REG_NETMR, &reg_value, sizeof(reg_value)) == ESP_OK, "write NETMR failed", err,
            ESP_FAIL);
  MAC_CHECK(w6100_write(emac, W6100_REG_SIMR, &reg_value, sizeof(reg_value)) == ESP_OK, "write SIMR failed", err,
            ESP_FAIL);

  reg_value = W6100_SMR_MAC_RAW | W6100_SMR_MAC_FILTER | W6100_SMR_MAC_BLOCK_MCAST;
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_MR(0), &reg_value, sizeof(reg_value)) == ESP_OK, "write SMR failed", err,
            ESP_FAIL);
  reg_value = W6100_SIR_RECV | W6100_SIR_SEND;
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_IMR(0), &reg_value, sizeof(reg_value)) == ESP_OK,
            "write SOCK0 IMR failed", err, ESP_FAIL);
  reg_value = W6100_SYCR1_IEN;
  MAC_CHECK(w6100_write(emac, W6100_REG_SYCR1, &reg_value, sizeof(reg_value)) == ESP_OK, "write SYCR1 failed", err,
            ESP_FAIL);
err:
  return ret;
}

static esp_err_t emac_w6100_start(esp_eth_mac_t *mac) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint8_t reg_value = W6100_SIMR_SOCK0;
  uint8_t state = 0;
  MAC_CHECK(w6100_send_command(emac, W6100_SCR_OPEN, 100) == ESP_OK, "issue OPEN command failed", err, ESP_FAIL);
  MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_SR(0), &state, sizeof(state)) == ESP_OK, "read socket state failed", err,
            ESP_FAIL);
  MAC_CHECK(state == W6100_SSR_MACRAW, "socket 0 not in MACRAW mode (0x%02x)", err, ESP_FAIL, state);
  MAC_CHECK(w6100_write(emac, W6100_REG_SIMR, &reg_value, sizeof(reg_value)) == ESP_OK, "write SIMR failed", err,
            ESP_FAIL);
err:
  return ret;
}

static esp_err_t emac_w6100_stop(esp_eth_mac_t *mac) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint8_t reg_value = 0;
  MAC_CHECK(w6100_write(emac, W6100_REG_SIMR, &reg_value, sizeof(reg_value)) == ESP_OK, "write SIMR failed", err,
            ESP_FAIL);
  MAC_CHECK(w6100_send_command(emac, W6100_SCR_CLOSE, 100) == ESP_OK, "issue CLOSE command failed", err, ESP_FAIL);
err:
  return ret;
}

IRAM_ATTR static void w6100_isr_handler(void *arg) {
  emac_w6100_t *emac = (emac_w6100_t *) arg;
  BaseType_t high_task_wakeup = pdFALSE;
  vTaskNotifyGiveFromISR(emac->rx_task_hdl, &high_task_wakeup);
  if (high_task_wakeup != pdFALSE) {
    portYIELD_FROM_ISR();
  }
}

static void emac_w6100_task(void *arg) {
  emac_w6100_t *emac = (emac_w6100_t *) arg;
  uint8_t status = 0;
  uint8_t *buffer = NULL;
  uint32_t length = 0;
  while (1) {
    if (emac->int_gpio_num >= 0) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0 && gpio_get_level(emac->int_gpio_num) != 0) {
        continue;
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(emac->poll_period_ms));
    }
    if (w6100_read(emac, W6100_REG_SOCK_IR(0), &status, sizeof(status)) != ESP_OK) {
      continue;
    }
    if (status & W6100_SIR_RECV) {
      uint8_t clear = W6100_SIR_RECV;
      w6100_write(emac, W6100_REG_SOCK_IRCLR(0), &clear, sizeof(clear));
      do {
        length = ETH_MAX_PACKET_SIZE;
        buffer = heap_caps_malloc(length, MALLOC_CAP_DMA);
        if (!buffer) {
          ESP_LOGE(TAG, "no mem for receive buffer");
          break;
        } else if (emac->parent.receive(&emac->parent, buffer, &length) == ESP_OK) {
          if (length) {
            emac->eth->stack_input(emac->eth, buffer, length);
          } else {
            free(buffer);
          }
        } else {
          free(buffer);
        }
      } while (emac->packets_remain);
    }
  }
}

static esp_err_t emac_w6100_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth) {
  esp_err_t ret = ESP_OK;
  MAC_CHECK(eth, "can't set mac's mediator to null", err, ESP_ERR_INVALID_ARG);
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  emac->eth = eth;
err:
  return ret;
}

static esp_err_t emac_w6100_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value) {
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint8_t value = reg_value;
  return w6100_write(emac, phy_reg, &value, sizeof(value));
}

static esp_err_t emac_w6100_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value) {
  esp_err_t ret = ESP_OK;
  MAC_CHECK(reg_value, "can't set reg_value to null", err, ESP_ERR_INVALID_ARG);
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint8_t value = 0;
  MAC_CHECK(w6100_read(emac, phy_reg, &value, sizeof(value)) == ESP_OK, "read PHY register failed", err, ESP_FAIL);
  *reg_value = value;
err:
  return ret;
}

static esp_err_t emac_w6100_set_addr(esp_eth_mac_t *mac, uint8_t *addr) {
  esp_err_t ret = ESP_OK;
  MAC_CHECK(addr, "invalid argument", err, ESP_ERR_INVALID_ARG);
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  memcpy(emac->addr, addr, 6);
  MAC_CHECK(w6100_set_mac_addr(emac) == ESP_OK, "set mac address failed", err, ESP_FAIL);
err:
  return ret;
}

static esp_err_t emac_w6100_get_addr(esp_eth_mac_t *mac, uint8_t *addr) {
  esp_err_t ret = ESP_OK;
  MAC_CHECK(addr, "invalid argument", err, ESP_ERR_INVALID_ARG);
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  memcpy(addr, emac->addr, 6);
err:
  return ret;
}

static esp_err_t emac_w6100_set_block_ip4_mcast(esp_eth_mac_t *mac, bool block) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint8_t smr = 0;
  MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_MR(0), &smr, sizeof(smr)) == ESP_OK, "read SMR failed", err, ESP_FAIL);
  if (block) {
    smr |= W6100_SMR_MAC_BLOCK_MCAST;
  } else {
    smr &= ~W6100_SMR_MAC_BLOCK_MCAST;
  }
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_MR(0), &smr, sizeof(smr)) == ESP_OK, "write SMR failed", err, ESP_FAIL);
err:
  return ret;
}

static esp_err_t emac_w6100_add_mac_filter(esp_eth_mac_t *mac, uint8_t *addr) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);

  if (addr[0] == 0x01 && addr[1] == 0x00 && addr[2] == 0x5e) {
    MAC_CHECK(emac_w6100_set_block_ip4_mcast(mac, false) == ESP_OK, "set block multicast failed", err, ESP_FAIL);
    emac->mcast_cnt++;
  } else if (addr[0] == 0x33 && addr[1] == 0x33) {
    ESP_LOGW(TAG, "IPv6 multicast filter is not configurable by W6100");
  } else {
    ESP_LOGW(TAG, "W6100 supports IP multicast filtering only");
    ret = ESP_ERR_NOT_SUPPORTED;
  }
err:
  return ret;
}

static esp_err_t emac_w6100_del_mac_filter(esp_eth_mac_t *mac, uint8_t *addr) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);

  if (addr[0] == 0x01 && addr[1] == 0x00 && addr[2] == 0x5e && emac->mcast_cnt > 0) {
    emac->mcast_cnt--;
  }
  if (emac->mcast_cnt == 0) {
    MAC_CHECK(emac_w6100_set_block_ip4_mcast(mac, true) == ESP_OK, "set block multicast failed", err, ESP_FAIL);
  }
err:
  return ret;
}

static esp_err_t emac_w6100_set_link(esp_eth_mac_t *mac, eth_link_t link) {
  switch (link) {
    case ETH_LINK_UP:
      return mac->start(mac);
    case ETH_LINK_DOWN:
      return mac->stop(mac);
    default:
      return ESP_ERR_INVALID_ARG;
  }
}

static esp_err_t emac_w6100_set_speed(esp_eth_mac_t *mac, eth_speed_t speed) { return ESP_OK; }

static esp_err_t emac_w6100_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex) { return ESP_OK; }

static esp_err_t emac_w6100_set_promiscuous(esp_eth_mac_t *mac, bool enable) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint8_t smr = 0;
  MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_MR(0), &smr, sizeof(smr)) == ESP_OK, "read SMR failed", err, ESP_FAIL);
  if (enable) {
    smr &= ~W6100_SMR_MAC_FILTER;
  } else {
    smr |= W6100_SMR_MAC_FILTER;
  }
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_MR(0), &smr, sizeof(smr)) == ESP_OK, "write SMR failed", err, ESP_FAIL);
err:
  return ret;
}

static esp_err_t emac_w6100_set_all_multicast(esp_eth_mac_t *mac, bool enable) {
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  esp_err_t ret = emac_w6100_set_block_ip4_mcast(mac, !enable);
  if (ret == ESP_OK) {
    emac->mcast_cnt = 0;
  }
  if (enable) {
    ESP_LOGW(TAG, "W6100 supports IP multicast filtering only");
  }
  return ret;
}

static esp_err_t emac_w6100_enable_flow_ctrl(esp_eth_mac_t *mac, bool enable) { return ESP_ERR_NOT_SUPPORTED; }

static esp_err_t emac_w6100_set_peer_pause_ability(esp_eth_mac_t *mac, uint32_t ability) {
  return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t emac_w6100_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint16_t offset = 0;
  uint16_t free_size = 0;
  MAC_CHECK(w6100_get_tx_free_size(emac, &free_size) == ESP_OK, "get free size failed", err, ESP_FAIL);
  MAC_CHECK(length <= free_size, "free size (%d) < send length (%" PRIu32 ")", err, ESP_FAIL, free_size, length);
  MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_TX_WR(0), &offset, sizeof(offset)) == ESP_OK, "read TX WR failed", err,
            ESP_FAIL);
  offset = __builtin_bswap16(offset);
  MAC_CHECK(w6100_write_buffer(emac, buf, length, offset) == ESP_OK, "write frame failed", err, ESP_FAIL);
  offset += length;
  offset = __builtin_bswap16(offset);
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_TX_WR(0), &offset, sizeof(offset)) == ESP_OK, "write TX WR failed", err,
            ESP_FAIL);
  MAC_CHECK(w6100_send_command(emac, W6100_SCR_SEND, 100) == ESP_OK, "issue SEND command failed", err, ESP_FAIL);

  uint8_t status = 0;
  do {
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_IR(0), &status, sizeof(status)) == ESP_OK, "read SOCK0 IR failed", err,
              ESP_FAIL);
  } while (!(status & W6100_SIR_SEND));
  status = W6100_SIR_SEND;
  MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_IRCLR(0), &status, sizeof(status)) == ESP_OK, "write SOCK0 IR failed",
            err, ESP_FAIL);
err:
  return ret;
}

static esp_err_t emac_w6100_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  uint16_t offset = 0;
  uint16_t rx_len = 0;
  uint16_t remain_bytes = 0;
  emac->packets_remain = false;

  MAC_CHECK(w6100_get_rx_received_size(emac, &remain_bytes) == ESP_OK, "get received size failed", err, ESP_FAIL);
  if (remain_bytes) {
    MAC_CHECK(w6100_read(emac, W6100_REG_SOCK_RX_RD(0), &offset, sizeof(offset)) == ESP_OK, "read RX RD failed", err,
              ESP_FAIL);
    offset = __builtin_bswap16(offset);
    MAC_CHECK(w6100_read_buffer(emac, &rx_len, sizeof(rx_len), offset) == ESP_OK, "read frame header failed", err,
              ESP_FAIL);
    rx_len = __builtin_bswap16(rx_len) - 2;
    offset += 2;
    MAC_CHECK(*length >= rx_len, "buffer size too small, needs %u", err, ESP_ERR_INVALID_SIZE, rx_len);
    MAC_CHECK(w6100_read_buffer(emac, buf, rx_len, offset) == ESP_OK, "read payload failed", err, ESP_FAIL);
    offset += rx_len;
    offset = __builtin_bswap16(offset);
    MAC_CHECK(w6100_write(emac, W6100_REG_SOCK_RX_RD(0), &offset, sizeof(offset)) == ESP_OK, "write RX RD failed", err,
              ESP_FAIL);
    MAC_CHECK(w6100_send_command(emac, W6100_SCR_RECV, 100) == ESP_OK, "issue RECV command failed", err, ESP_FAIL);
    remain_bytes -= rx_len + 2;
    emac->packets_remain = remain_bytes > 0;
  }
  *length = rx_len;
err:
  return ret;
}

static esp_err_t emac_w6100_init(esp_eth_mac_t *mac) {
  esp_err_t ret = ESP_OK;
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  esp_eth_mediator_t *eth = emac->eth;
  if (emac->int_gpio_num >= 0) {
    esp_rom_gpio_pad_select_gpio(emac->int_gpio_num);
    gpio_set_direction(emac->int_gpio_num, GPIO_MODE_INPUT);
    gpio_set_pull_mode(emac->int_gpio_num, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(emac->int_gpio_num, GPIO_INTR_NEGEDGE);
    gpio_intr_enable(emac->int_gpio_num);
    gpio_isr_handler_add(emac->int_gpio_num, w6100_isr_handler, emac);
  }
  MAC_CHECK(eth->on_state_changed(eth, ETH_STATE_LLINIT, NULL) == ESP_OK, "lowlevel init failed", err, ESP_FAIL);
  MAC_CHECK(w6100_reset(emac) == ESP_OK, "reset w6100 failed", err, ESP_FAIL);
  MAC_CHECK(w6100_verify_id(emac) == ESP_OK, "verify chip ID failed", err, ESP_FAIL);
  MAC_CHECK(w6100_setup_default(emac) == ESP_OK, "w6100 default setup failed", err, ESP_FAIL);
  return ESP_OK;
err:
  if (emac->int_gpio_num >= 0) {
    gpio_isr_handler_remove(emac->int_gpio_num);
    gpio_reset_pin(emac->int_gpio_num);
  }
  eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
  return ret;
}

static esp_err_t emac_w6100_deinit(esp_eth_mac_t *mac) {
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  mac->stop(mac);
  if (emac->int_gpio_num >= 0) {
    gpio_isr_handler_remove(emac->int_gpio_num);
    gpio_reset_pin(emac->int_gpio_num);
  }
  emac->eth->on_state_changed(emac->eth, ETH_STATE_DEINIT, NULL);
  return ESP_OK;
}

static esp_err_t emac_w6100_del(esp_eth_mac_t *mac) {
  emac_w6100_t *emac = __containerof(mac, emac_w6100_t, parent);
  if (emac->rx_task_hdl) {
    vTaskDelete(emac->rx_task_hdl);
  }
  if (emac->spi_lock) {
    vSemaphoreDelete(emac->spi_lock);
  }
  if (emac->spi_hdl) {
    spi_bus_remove_device(emac->spi_hdl);
  }
  free(emac);
  return ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_w6100(const eth_w6100_config_t *w6100_config, const eth_mac_config_t *mac_config) {
  esp_eth_mac_t *ret = NULL;
  emac_w6100_t *emac = NULL;
  MAC_CHECK(w6100_config && mac_config && w6100_config->spi_devcfg, "invalid argument", err, NULL);
  emac = calloc(1, sizeof(emac_w6100_t));
  MAC_CHECK(emac, "no mem for MAC instance", err, NULL);
  MAC_CHECK(spi_bus_add_device(w6100_config->spi_host_id, w6100_config->spi_devcfg, &emac->spi_hdl) == ESP_OK,
            "add spi device failed", err, NULL);
  MAC_CHECK(w6100_config->int_gpio_num >= 0 || w6100_config->poll_period_ms > 0, "missing IRQ or polling interval", err,
            NULL);

  emac->sw_reset_timeout_ms = mac_config->sw_reset_timeout_ms;
  emac->int_gpio_num = w6100_config->int_gpio_num;
  emac->poll_period_ms = w6100_config->poll_period_ms;
  emac->parent.set_mediator = emac_w6100_set_mediator;
  emac->parent.init = emac_w6100_init;
  emac->parent.deinit = emac_w6100_deinit;
  emac->parent.start = emac_w6100_start;
  emac->parent.stop = emac_w6100_stop;
  emac->parent.del = emac_w6100_del;
  emac->parent.write_phy_reg = emac_w6100_write_phy_reg;
  emac->parent.read_phy_reg = emac_w6100_read_phy_reg;
  emac->parent.set_addr = emac_w6100_set_addr;
  emac->parent.get_addr = emac_w6100_get_addr;
  emac->parent.add_mac_filter = emac_w6100_add_mac_filter;
  emac->parent.rm_mac_filter = emac_w6100_del_mac_filter;
  emac->parent.set_speed = emac_w6100_set_speed;
  emac->parent.set_duplex = emac_w6100_set_duplex;
  emac->parent.set_link = emac_w6100_set_link;
  emac->parent.set_promiscuous = emac_w6100_set_promiscuous;
  emac->parent.set_all_multicast = emac_w6100_set_all_multicast;
  emac->parent.set_peer_pause_ability = emac_w6100_set_peer_pause_ability;
  emac->parent.enable_flow_ctrl = emac_w6100_enable_flow_ctrl;
  emac->parent.transmit = emac_w6100_transmit;
  emac->parent.receive = emac_w6100_receive;

  emac->spi_lock = xSemaphoreCreateMutex();
  MAC_CHECK(emac->spi_lock, "create lock failed", err, NULL);
  BaseType_t core_num = tskNO_AFFINITY;
  if (mac_config->flags & ETH_MAC_FLAG_PIN_TO_CORE) {
    core_num = xPortGetCoreID();
  }
  BaseType_t xReturned = xTaskCreatePinnedToCore(emac_w6100_task, "w6100_tsk", mac_config->rx_task_stack_size, emac,
                                                 mac_config->rx_task_prio, &emac->rx_task_hdl, core_num);
  MAC_CHECK(xReturned == pdPASS, "create w6100 task failed", err, NULL);
  return &emac->parent;

err:
  if (emac) {
    if (emac->rx_task_hdl) {
      vTaskDelete(emac->rx_task_hdl);
    }
    if (emac->spi_lock) {
      vSemaphoreDelete(emac->spi_lock);
    }
    if (emac->spi_hdl) {
      spi_bus_remove_device(emac->spi_hdl);
    }
    free(emac);
  }
  return ret;
}
