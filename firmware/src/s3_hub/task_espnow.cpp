#include "task_espnow.h"
#include "hub_state.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static uint8_t  wroomMac[6] = {0};
static bool     hasWroomMac = false;
static uint8_t  lvglMac[6]  = {0};
static bool     hasLvglMac  = false;
static uint16_t hubSeq      = 0;

static volatile bool s3SendDone = false;
static volatile bool s3SendOk   = false;

static bool ensureEspNowPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  esp_err_t res = esp_now_add_peer(&peerInfo);
  if (res == ESP_OK) {
    Serial.print("[ESP-NOW] Da dang ky peer moi: ");
    printMac(mac);
    Serial.println();
    return true;
  }
  return false;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void onDataSent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
#else
static void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
#endif
  s3SendOk   = (status == ESP_NOW_SEND_SUCCESS);
  s3SendDone = true;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
#else
static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(EspNowPacket) || xEspNowRxQueue == nullptr) return;

  EspNowRxItem item;
  memcpy(item.mac, mac, 6);
  memcpy(&item.pkt, data, sizeof(EspNowPacket));
  xQueueSend(xEspNowRxQueue, &item, 0);
}

static bool sendEspNowToPeer(const uint8_t* mac, bool hasPeer, const EspNowPacket& pkt,
                             const char* peerName, uint8_t maxRetries = 5) {
  if (!hasPeer) return false;
  if (!ensureEspNowPeer(mac)) return false;

  for (uint8_t attempt = 1; attempt <= maxRetries; attempt++) {
    s3SendDone = false;
    s3SendOk   = false;
    esp_err_t res = esp_now_send(mac, (const uint8_t*)&pkt, sizeof(pkt));

    if (res == ESP_OK) {
      uint32_t t0 = millis();
      while (!s3SendDone && (millis() - t0 < 50)) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (s3SendDone && s3SendOk) {
        Serial.printf("[ESP-NOW -> %s] OK (lan %u/%u) | type=%u | sw=0x%02X | away=%u\n",
                      peerName, attempt, maxRetries, pkt.msg_type, pkt.switch_mask, pkt.away_mode);
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(45));
  }

  Serial.printf("[ESP-NOW -> %s] THAT BAI sau %u lan thu!\n", peerName, maxRetries);
  return false;
}

static void executeEspNowTx(EspNowTxAction action) {
  SystemState snap = getStateSnapshot();
  EspNowPacket pkt = {};
  pkt.node_id       = NODE_S3_HUB;
  pkt.seq           = ++hubSeq;
  pkt.temperature   = snap.temperature;
  pkt.humidity      = snap.humidity;
  pkt.light_lux     = snap.light_lux;
  pkt.switch_mask   = snap.switch_mask;
  pkt.away_mode     = snap.away_mode ? 1 : 0;
  pkt.alert_code    = snap.alert_code;
  pkt.espnow_online = snap.espnow_online ? 1 : 0;
  pkt.ble_online    = (snap.espnow_online && snap.ble_online) ? 1 : 0;
  strncpy(pkt.alert_msg, snap.alert_msg, sizeof(pkt.alert_msg) - 1);
  pkt.alert_msg[sizeof(pkt.alert_msg) - 1] = '\0';

  if (action == TX_CMD_WROOM) {
    pkt.msg_type = MSG_COMMAND;
    sendEspNowToPeer(wroomMac, hasWroomMac, pkt, "WROOM", 5);
  } else if (action == TX_SYNC_LVGL) {
    if (currentNetState != NET_ONLINE) {
      pkt.alert_code = ALERT_GATEWAY_OFFLINE;
    }
    if (pkt.alert_code == ALERT_NONE || pkt.alert_code == ALERT_GATEWAY_OFFLINE) {
      uint8_t ch = 0;
      char ssidBuf[28] = "--";
      if (currentNetState == NET_WIFI_OFFLINE_AP) {
        ch = FALLBACK_CHANNEL;
        snprintf(ssidBuf, sizeof(ssidBuf), "AP:%s", FALLBACK_AP_SSID);
      } else if (WiFi.status() == WL_CONNECTED) {
        ch = (uint8_t)WiFi.channel();
        String s = WiFi.SSID();
        if (s.length() > 0) {
          snprintf(ssidBuf, sizeof(ssidBuf), "%s", s.c_str());
        }
      } else {
        ch = (uint8_t)WiFi.channel();
        snprintf(ssidBuf, sizeof(ssidBuf), "Connecting...");
      }
      snprintf(pkt.alert_msg, sizeof(pkt.alert_msg), "%u|%s", ch, ssidBuf);
    }
    pkt.msg_type = MSG_STATE_SYNC;
    sendEspNowToPeer(lvglMac, hasLvglMac, pkt, "LVGL", 2);
  } else if (action == TX_ALERT_LVGL) {
    pkt.msg_type = MSG_ALERT;
    sendEspNowToPeer(lvglMac, hasLvglMac, pkt, "LVGL", 3);
  }
}

void initEspNow() {
  Serial.println("===================================================");
  Serial.print("[QUAN TRONG] Copy MAC nay dan vao WROOM: ");
  Serial.println(WiFi.macAddress());
  Serial.println("===================================================");

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Khoi tao that bai!");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
}

void TaskEspNow(void* pvParameters) {
  EspNowRxItem rxItem;
  EspNowTxAction txAction;

  for (;;) {
    while (xQueueReceive(xEspNowRxQueue, &rxItem, 0) == pdTRUE) {
      const EspNowPacket& p = rxItem.pkt;

      if (p.node_id == NODE_WROOM_ROOM) {
        if (!hasWroomMac || memcmp(wroomMac, rxItem.mac, 6) != 0) {
          memcpy(wroomMac, rxItem.mac, 6);
          hasWroomMac = true;
          ensureEspNowPeer(wroomMac);
        }

        if (p.msg_type == MSG_TELEMETRY) {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!sysState.espnow_online) {
              Serial.println("[ESP-NOW] >>> WROOM da ket noi tro lai!");
            }
            sysState.espnow_online = true;
            sysState.temperature   = p.temperature;
            sysState.humidity      = p.humidity;
            sysState.light_lux     = p.light_lux;
            sysState.switch_mask   = p.switch_mask;
            sysState.away_mode     = (p.away_mode != 0);
            sysState.ble_online    = (p.ble_online != 0);
            sysState.last_rx_seq   = p.seq;
            sysState.last_rx_ms    = millis();

            if (sysState.ble_online) {
              pushEnvHistoryLocked(p.temperature, p.humidity);
            }
            xSemaphoreGive(xStateMutex);
          }

          Serial.printf("[ESP-NOW <- WROOM] seq=%u | BLE=%s (T=%.1fC, H=%.1f%%) | Light=%u | Relay=%s\n",
                        p.seq,
                        p.ble_online ? "ON" : "OFF",
                        p.temperature,
                        p.humidity,
                        p.light_lux,
                        (p.switch_mask & SW_BED_LIGHT_BIT) ? "ON" : "OFF");

          notifyWebClients();
          xSemaphoreGive(xMqttPubTelemetrySem);
          executeEspNowTx(TX_SYNC_LVGL);
        }
      } else if (p.node_id == NODE_LVGL_PANEL) {
        if (!hasLvglMac || memcmp(lvglMac, rxItem.mac, 6) != 0) {
          memcpy(lvglMac, rxItem.mac, 6);
          hasLvglMac = true;
          ensureEspNowPeer(lvglMac);
        }

        if (p.msg_type == MSG_COMMAND) {
          uint8_t newMask = p.switch_mask;
          bool newAway    = (p.away_mode != 0);
          if (newAway) {
            newMask &= ~SW_BED_LIGHT_BIT;
          }

          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            sysState.switch_mask = newMask;
            sysState.away_mode   = newAway;
            xSemaphoreGive(xStateMutex);
          }

          Serial.printf("[ESP-NOW <- LVGL] sw=0x%02X | away=%d -> Gui xuong WROOM\n",
                        newMask, newAway);
          notifyWebClients();
          executeEspNowTx(TX_CMD_WROOM);
          xSemaphoreGive(xMqttPubTelemetrySem);
        } else if (p.msg_type == MSG_STATE_SYNC) {
          executeEspNowTx(TX_SYNC_LVGL);
        }
      }
    }

    bool wroomTimedOut = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (sysState.espnow_online && (millis() - sysState.last_rx_ms > ESPNOW_TIMEOUT_MS)) {
        sysState.espnow_online = false;
        sysState.ble_online    = false;
        wroomTimedOut          = true;
      }
      xSemaphoreGive(xStateMutex);
    }

    if (wroomTimedOut) {
      Serial.println("[ESP-NOW] Mat ket noi WROOM (>10s) -> espnow_online=0, ble_online=0");
      notifyWebClients();
      xSemaphoreGive(xMqttPubTelemetrySem);
      executeEspNowTx(TX_SYNC_LVGL);
    }

    while (xQueueReceive(xEspNowTxQueue, &txAction, 0) == pdTRUE) {
      executeEspNowTx(txAction);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}