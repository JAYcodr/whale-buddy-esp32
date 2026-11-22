#include "net/net.h"
#include "model/settings.h"
#include "model/state.h"
#include "net/proto.h"

#include <Arduino.h>
#include <MQTT.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

namespace {

struct NetTxMsg {
  char topic[48];
  char payload[320];
  bool retained;
  uint8_t qos;
};

WiFiClient      s_wifiClient;
MQTTClient      s_mqtt(4096);
WiFiManager*    s_wm = nullptr;
QueueHandle_t   s_txQ = nullptr;
TaskHandle_t    s_netTask = nullptr;

volatile bool   s_reprovisionReq = false;
volatile bool   s_cancelPortalReq = false;
volatile bool   s_wifiConnectReq = false;
char            s_reqSsid[64] = "";
char            s_reqPass[64] = "";
volatile NetPsMode s_desiredPs = NetPsMode::LOW_POWER;
NetPsMode       s_appliedPs = NetPsMode::PERFORMANCE;
bool            s_portalActive = false;

char s_apName[24] = "Nova-0000";
volatile uint16_t s_statIntervalMs = 30000;
char s_clientId[24] = "nova-000000";
char s_topicPrefix[24] = "nova/000000";

char s_paramHost[40] = "192.168.31.15";
char s_paramPort[8]  = "1883";
WiFiManagerParameter* s_pHost = nullptr;
WiFiManagerParameter* s_pPort = nullptr;

void applyWifiPsIfConnected() {
  if (WiFi.status() != WL_CONNECTED || s_portalActive) return;
  if (s_desiredPs == s_appliedPs) return;
  wifi_ps_type_t ps = (s_desiredPs == NetPsMode::LOW_POWER)
      ? WIFI_PS_MAX_MODEM
      : WIFI_PS_NONE;
  if (esp_wifi_set_ps(ps) == ESP_OK) {
    s_appliedPs = s_desiredPs;
  }
}

void configureListenInterval() {
  wifi_config_t cfg = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
    if (cfg.sta.listen_interval != 10) {
      cfg.sta.listen_interval = 10;
      esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
  }
}

void onSaveParams() {
  NovaSettings& cfg = settingsMut();
  if (s_pHost && s_pHost->getValue()[0]) {
    strncpy(cfg.brokerHost, s_pHost->getValue(), sizeof(cfg.brokerHost) - 1);
    cfg.brokerHost[sizeof(cfg.brokerHost) - 1] = '\0';
  }
  if (s_pPort && s_pPort->getValue()[0]) {
    long p = strtol(s_pPort->getValue(), nullptr, 10);
    if (p > 0 && p <= 65535) cfg.brokerPort = (uint16_t)p;
  }
  settingsSave();
  Serial.printf("net: saved broker %s:%u\n", cfg.brokerHost, cfg.brokerPort);
}

void onApStart(WiFiManager*) {
  s_portalActive = true;
  novaStateWithLock([&](NovaState& st) {
    st.wifiApMode = true;
    st.wifiConnected = false;
    st.mqttConnected = false;
    strncpy(st.apSsid, s_apName, sizeof(st.apSsid) - 1);
  });
  Serial.printf("net: AP portal started: %s\n", s_apName);
}

void onMqttMessage(String& topic, String& payload) {
  const char* t = topic.c_str();
  const size_t plen = strlen(s_topicPrefix);
  if (strncmp(t, s_topicPrefix, plen) != 0 || t[plen] != '/') return;
  const char* suffix = t + plen + 1;
  const uint32_t now = millis();
  // 心跳类 topic（time/tts）的 handler 不碰 NovaState，但它们证明链路活着。
  // 在这里统一刷新存活时间，否则每 30s 的 time 心跳不算数，状态栏会误报「滞后」。
  if (protoApplyMessage(suffix, payload.c_str(), payload.length(), now)) {
    novaStateWithLock([&](NovaState& st) { st.lastMqttRxMs = now; });
  }
}

void subscribeTopics() {
  static const char* SUFFIXES[] = {
    "state", "quota", "rank", "tokens", "approval", "approval_end", "time", "choreo", "tts"
  };
  char topic[48];
  for (const char* sfx : SUFFIXES) {
    snprintf(topic, sizeof(topic), "%s/%s", s_topicPrefix, sfx);
    s_mqtt.subscribe(topic, 1);
  }
}

void publishHello(bool online) {
  static NovaState s_tmpSt;
  novaStateRead(&s_tmpSt);
  const NovaSettings& cfg = settingsGet();
  char topic[48];
  char payload[320];
  snprintf(topic, sizeof(topic), "%s/hello", s_topicPrefix);
  size_t n = protoBuildHello(payload, sizeof(payload), s_tmpSt, cfg, online);
  s_mqtt.publish(topic, payload, (int)n, true, 1);
  if (online) {
    snprintf(topic, sizeof(topic), "%s/up", s_topicPrefix);
    s_mqtt.publish(topic, "{\"evt\":\"sync\"}", 14, false, 1);
  }
}

void netTaskFn(void*) {
  char devId[16] = "";
  novaStateWithLock([&](NovaState& st) {
    strncpy(devId, st.deviceId, sizeof(devId) - 1);
    devId[sizeof(devId) - 1] = '\0';
  });
  snprintf(s_apName, sizeof(s_apName), "Nova-%s", strlen(devId) >= 4 ? devId + strlen(devId) - 4 : devId);
  snprintf(s_clientId, sizeof(s_clientId), "nova-%s", devId);
  snprintf(s_topicPrefix, sizeof(s_topicPrefix), "nova/%s", devId);

  const NovaSettings& cfg = settingsGet();
  strncpy(s_paramHost, cfg.brokerHost, sizeof(s_paramHost) - 1);
  snprintf(s_paramPort, sizeof(s_paramPort), "%u", cfg.brokerPort);

  s_wm = new WiFiManager();
  s_pHost = new WiFiManagerParameter("mqtt_host", "MQTT Broker IP", s_paramHost, sizeof(s_paramHost) - 1);
  s_pPort = new WiFiManagerParameter("mqtt_port", "MQTT Port", s_paramPort, sizeof(s_paramPort) - 1);
  s_wm->addParameter(s_pHost);
  s_wm->addParameter(s_pPort);
  s_wm->setDebugOutput(false);
  s_wm->setConfigPortalBlocking(false);
  s_wm->setBreakAfterConfig(true);
  s_wm->setAPCallback(onApStart);
  s_wm->setSaveParamsCallback(onSaveParams);
  s_wm->setSaveConfigCallback([]() {
    onSaveParams();
    s_portalActive = false;
  });

  WiFi.mode(WIFI_STA);
  if (s_wm->autoConnect(s_apName)) {
    s_portalActive = false;
    configureListenInterval();
    s_appliedPs = NetPsMode::PERFORMANCE;
    applyWifiPsIfConnected();
  }

  s_mqtt.onMessage(onMqttMessage);
  s_mqtt.setKeepAlive(30);
  s_mqtt.setTimeout(1000);

  uint32_t retryBackoffMs = 2000;
  uint32_t nextConnectAttemptMs = 0;
  uint32_t lastStatMs = 0;
  char configuredHost[40] = "";
  uint16_t configuredPort = 0;

  for (;;) {
    uint32_t now = millis();

    if (s_reprovisionReq) {
      s_reprovisionReq = false;
      s_mqtt.disconnect();
      // 必须先断开 STA 再开 portal：下面有一段
      //   if (wifiUp && s_portalActive) { stopConfigPortal(); s_portalActive = false; }
      // STA 还连着时 wifiUp 恒为 true，portal 会在启动后的下一轮就被关掉，
      // 表现就是「点重新配网没反应」。断开后 WiFi.status() 不再是 CONNECTED，
      // portal 才能活下来。
      s_wm->disconnect();
      s_wm->startConfigPortal(s_apName);
    }
    if (s_wifiConnectReq) {
      s_wifiConnectReq = false;
      if (s_portalActive) {
        s_wm->stopConfigPortal();
        s_portalActive = false;
      }
      s_mqtt.disconnect();
      WiFi.mode(WIFI_STA);
      WiFi.persistent(true);
      WiFi.begin(s_reqSsid, s_reqPass);
      novaStateWithLock([](NovaState& st) { st.wifiApMode = false; });
      Serial.printf("net: connecting STA to %s...\n", s_reqSsid);
    }
    if (s_cancelPortalReq && s_portalActive) {
      s_cancelPortalReq = false;
      s_wm->stopConfigPortal();
      s_portalActive = false;
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      novaStateWithLock([](NovaState& st) { st.wifiApMode = false; });
    }

    if (s_portalActive) {
      s_wm->process();
    }

    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    if (wifiUp && s_portalActive) {
      s_wm->stopConfigPortal();
      s_portalActive = false;
      configureListenInterval();
    }

    if (wifiUp) {
      applyWifiPsIfConnected();
    }

    novaStateWithLock([&](NovaState& st) {
      st.wifiConnected = wifiUp;
      st.wifiApMode = s_portalActive;
      if (wifiUp) {
        String ipStr = WiFi.localIP().toString();
        strncpy(st.ip, ipStr.c_str(), sizeof(st.ip) - 1);
        st.rssi = (int8_t)WiFi.RSSI();
      } else {
        st.ip[0] = '\0';
        st.rssi = 0;
      }
      st.mqttConnected = s_mqtt.connected();
    });

    if (wifiUp && !s_portalActive) {
      const NovaSettings& curCfg = settingsGet();
      if (strcmp(configuredHost, curCfg.brokerHost) != 0 || configuredPort != curCfg.brokerPort) {
        if (s_mqtt.connected()) s_mqtt.disconnect();
        strncpy(configuredHost, curCfg.brokerHost, sizeof(configuredHost) - 1);
        configuredPort = curCfg.brokerPort;
        s_mqtt.begin(configuredHost, configuredPort, s_wifiClient);
        char willTopic[48];
        snprintf(willTopic, sizeof(willTopic), "%s/hello", s_topicPrefix);
        s_mqtt.setWill(willTopic, "{\"online\":false}", true, 1);
        nextConnectAttemptMs = 0;
      }

      if (!s_mqtt.connected()) {
        if (now >= nextConnectAttemptMs && configuredHost[0]) {
          Serial.printf("net: mqtt connecting %s:%u as %s...\n", configuredHost, configuredPort, s_clientId);
          if (s_mqtt.connect(s_clientId)) {
            Serial.println("net: mqtt connected");
            retryBackoffMs = 2000;
            subscribeTopics();
            publishHello(true);
            novaStateWithLock([&](NovaState& st) {
              st.mqttConnected = true;
              st.lastMqttRxMs = now;
            });
          } else {
            nextConnectAttemptMs = now + retryBackoffMs;
            retryBackoffMs = (retryBackoffMs < 30000) ? (retryBackoffMs * 2) : 30000;
            if (retryBackoffMs > 30000) retryBackoffMs = 30000;
          }
        }
      } else {
        s_mqtt.loop();

        // Drain outbound queue
        NetTxMsg tx;
        while (s_txQ && xQueueReceive(s_txQ, &tx, 0) == pdTRUE) {
          s_mqtt.publish(tx.topic, tx.payload, (int)strlen(tx.payload), tx.retained, tx.qos);
        }

        // Periodic device telemetry (default 30 s; `{"cmd":"statrate"}` 可临时改密)
        if (now - lastStatMs >= s_statIntervalMs) {
          lastStatMs = now;
          static NovaState s_statSt;
          novaStateRead(&s_statSt);
          char topic[48];
          char payload[320];
          snprintf(topic, sizeof(topic), "%s/up", s_topicPrefix);
          size_t n = protoBuildStat(payload, sizeof(payload), s_statSt);
          s_mqtt.publish(topic, payload, (int)n, false, 0);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(s_portalActive ? 10 : 25));
  }
}

}  // namespace

bool netStart() {
  if (s_netTask) return true;
  s_txQ = xQueueCreate(8, sizeof(NetTxMsg));
  if (!s_txQ) return false;
  BaseType_t ok = xTaskCreatePinnedToCore(
      netTaskFn, "task_net", 12288, nullptr, 4, &s_netTask, 0);
  return ok == pdPASS;
}

void netRequestReprovision() { s_reprovisionReq = true; }
void netRequestCancelPortal() { s_cancelPortalReq = true; }
void netRequestWifiConnect(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  strncpy(s_reqSsid, ssid, sizeof(s_reqSsid) - 1);
  s_reqSsid[sizeof(s_reqSsid) - 1] = '\0';
  if (pass) {
    strncpy(s_reqPass, pass, sizeof(s_reqPass) - 1);
    s_reqPass[sizeof(s_reqPass) - 1] = '\0';
  } else {
    s_reqPass[0] = '\0';
  }
  s_wifiConnectReq = true;
}
void netSetPowerSave(NetPsMode mode) { s_desiredPs = mode; }

void netSetStatIntervalMs(uint16_t ms) {
  if (ms == 0) { s_statIntervalMs = 30000; return; }
  if (ms < 5000) ms = 5000;
  if (ms > 300000) ms = 300000;
  s_statIntervalMs = ms;
}

void netPublishDecision(const char* approvalId, bool accept, bool toPc) {
  if (!s_txQ || !approvalId || !approvalId[0]) return;
  NetTxMsg tx{};
  snprintf(tx.topic, sizeof(tx.topic), "%s/up", s_topicPrefix);
  protoBuildDecision(tx.payload, sizeof(tx.payload), approvalId, accept, toPc);
  tx.retained = false;
  tx.qos = 1;
  xQueueSend(s_txQ, &tx, 0);
}

void netPublishPrefs() {
  if (!s_txQ) return;
  NetTxMsg tx{};
  snprintf(tx.topic, sizeof(tx.topic), "%s/up", s_topicPrefix);
  protoBuildPrefs(tx.payload, sizeof(tx.payload), settingsGet());
  tx.retained = false;
  tx.qos = 1;
  xQueueSend(s_txQ, &tx, 0);
}

void netPublishSync() {
  if (!s_txQ) return;
  NetTxMsg tx{};
  snprintf(tx.topic, sizeof(tx.topic), "%s/up", s_topicPrefix);
  snprintf(tx.payload, sizeof(tx.payload), "{\"evt\":\"sync\"}");
  tx.retained = false;
  tx.qos = 1;
  xQueueSend(s_txQ, &tx, 0);
}
