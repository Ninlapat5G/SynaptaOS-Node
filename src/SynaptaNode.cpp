#include "SynaptaNode.h"
#include "SynaptaRegistry.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include <cstring>

// mDNS — มีเมื่อโปรเจคใส่ component espressif/mdns (idf_component.yml)
// ไม่มีก็ compile ผ่าน แค่ใช้ชื่อ .local ไม่ได้ (ต้องใส่ IP ตรง ๆ แบบเดิม)
#if __has_include("mdns.h")
#include "mdns.h"
#define SYN_HAS_MDNS 1
#endif

SynaptaNodeClass Synapta;

// ต้องเก็บ string ไว้ให้ pointer ยังชี้ถึงได้ตลอด lifetime ของ client
// (file-scope เพราะ _maybeReResolve ต้องแก้ s_uri/s_host ได้หลังบูต)
static std::string s_uri, s_clientId, s_statusTopic, s_user, s_pass, s_host;

// ── NVS init ──────────────────────────────────────────────────────────────────

static void _nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

// ── WiFi event handler ────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifiEG;
#define WIFI_CONNECTED_BIT BIT0

static void wifiEventHandler(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(2000));   // หน่วงก่อน retry — กัน reconnect รัว ๆ ตอน AP ล่ม/รหัสผิด
        esp_wifi_connect();
    }
    if (base == IP_EVENT   && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifiEG, WIFI_CONNECTED_BIT);
}

// ── mDNS resolve — broker เป็นชื่อ "xxx.local" → ถามหา IP ปัจจุบันในวง LAN ──────
// DHCP แจกเลขใหม่เมื่อไหร่ก็ไม่พัง เพราะถามด้วย "ชื่อ" ใหม่ทุกครั้งที่ต่อ

static const char* NET_NS = "syn-net";

static std::string _loadCachedBrokerIp() {
    nvs_handle_t h;
    if (nvs_open(NET_NS, NVS_READONLY, &h) != ESP_OK) return "";
    char buf[16] = {};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, "broker_ip", buf, &len);
    nvs_close(h);
    return err == ESP_OK ? std::string(buf) : "";
}

static void _saveCachedBrokerIp(const std::string& ip) {
    nvs_handle_t h;
    if (nvs_open(NET_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "broker_ip", ip.c_str());
    nvs_commit(h);
    nvs_close(h);
}

// คืน IP ของ host (ชื่อไม่รวม .local) — ว่าง = หาไม่เจอ
static std::string _mdnsResolve(const std::string& hostname) {
#ifdef SYN_HAS_MDNS
    static bool inited = false;
    if (!inited) {
        if (mdns_init() != ESP_OK) return "";
        inited = true;
    }
    esp_ip4_addr_t addr = {};
    if (mdns_query_a(hostname.c_str(), 4000, &addr) != ESP_OK) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&addr));
    return std::string(buf);
#else
    printf("[Synapta] WARN: broker เป็นชื่อ .local แต่โปรเจคไม่มี component mdns\n"
           "          เพิ่ม idf_component.yml: dependencies: espressif/mdns\n");
    return "";
#endif
}

// แปลง broker ใน config → host ที่ใช้ต่อจริง (resolve .local + cache ลง NVS)
std::string SynaptaNodeClass::_resolveBrokerHost() {
    const std::string& host = _cfg.mqttBroker;
    const std::string suffix = ".local";
    if (host.size() <= suffix.size() ||
        host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return host;   // เป็น IP/โดเมนปกติ — ใช้ตรง ๆ
    }
#ifdef SYN_HAS_MDNS
    // ทางหลัก (มี component mdns): resolve เอง + จำ IP ลง NVS ไว้เป็นแผนสำรอง
    std::string name = host.substr(0, host.size() - suffix.size());
    std::string ip = _mdnsResolve(name);
    if (!ip.empty()) {
        printf("[Synapta] mDNS: %s → %s\n", host.c_str(), ip.c_str());
        _saveCachedBrokerIp(ip);
        return ip;
    }
    std::string cached = _loadCachedBrokerIp();
    if (!cached.empty()) {
        printf("[Synapta] mDNS หาไม่เจอ — ใช้ IP ล่าสุดที่จำไว้: %s\n", cached.c_str());
        return cached;
    }
    printf("[Synapta] mDNS หาไม่เจอและไม่มี IP เก่า — ใช้ชื่อตรง ๆ (อาจต่อไม่ติด)\n");
    return host;
#elif defined(CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES)
    // ไม่มี component แต่ lwip resolve .local ได้เอง — ส่งชื่อให้ esp-mqtt ตรง ๆ
    // (ดีกว่าด้วยซ้ำ: ทุกครั้งที่ reconnect มันถามหา IP ใหม่เอง broker ย้ายเครื่องก็ตามเจอ)
    printf("[Synapta] broker %s — ให้ lwip resolve ตอนเชื่อมต่อ (mDNS in-stack)\n", host.c_str());
    return host;
#else
    printf("[Synapta] WARN: ใช้ชื่อ .local ต้องเปิด CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y\n"
           "          ใน sdkconfig.defaults (หรือเพิ่ม component espressif/mdns)\n");
    std::string cached = _loadCachedBrokerIp();
    return cached.empty() ? host : cached;
#endif
}

// ── V1 Fluent config ──────────────────────────────────────────────────────────

SynaptaNodeClass& SynaptaNodeClass::wifi(const char* ssid, const char* pass) {
    _cfg.wifiSSID = ssid; _cfg.wifiPassword = pass; return *this;
}
SynaptaNodeClass& SynaptaNodeClass::broker(const char* host, int port, bool tls) {
    _cfg.mqttBroker = host; _cfg.mqttPort = port; _cfg.mqttTLS = tls; return *this;
}
SynaptaNodeClass& SynaptaNodeClass::mqttAuth(const char* user, const char* pass) {
    _cfg.mqttUser = user; _cfg.mqttPassword = pass; return *this;
}
SynaptaNodeClass& SynaptaNodeClass::baseTopic(const char* base) {
    _cfg.baseTopic = base; return *this;
}
SynaptaNodeClass& SynaptaNodeClass::nodeId(const char* id) {
    _cfg.nodeId = id; return *this;
}

void SynaptaNodeClass::start() {
    _nvs_init();
    if (!_cfg.isValid()) {
        printf("[Synapta] ERROR: call wifi() and baseTopic() before start()\n");
        return;
    }
    _init();
}

// ── Legacy API ────────────────────────────────────────────────────────────────

void SynaptaNodeClass::begin(const char* ssid, const char* pass, const char* base) {
    _nvs_init();
    _cfg = NodeConfig(ssid, pass, base);
    _init();
}

void SynaptaNodeClass::begin() {
    _nvs_init();
    _cfg.load();
    if (!_cfg.isValid()) {
        printf("[Synapta] ERROR: no saved credentials — call configure() first\n");
        return;
    }
    _init();
}

void SynaptaNodeClass::configure(const char* ssid, const char* pass, const char* base) {
    _nvs_init();
    _cfg = NodeConfig(ssid, pass, base);
    _cfg.save();
    _init();
}

// ── Init ──────────────────────────────────────────────────────────────────────

void SynaptaNodeClass::_init() {
    if (_loopTask) {                      // กันเรียก start()/begin() ซ้ำ — init รอบเดียวพอ
        printf("[Synapta] already started — ignoring duplicate init\n");
        return;
    }
    _mutex    = xSemaphoreCreateMutex();
    _pubMutex = xSemaphoreCreateMutex();

    _devices.clear();
    for (auto* d : _SynaptaRegistry::devices()) {
        _devices.push_back(d);
        d->_loadPinConfig();
    }

    // WiFi
    s_wifiEG = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifiInitCfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifiInitCfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,   wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr);

    wifi_config_t wCfg = {};
    strncpy((char*)wCfg.sta.ssid,     _cfg.wifiSSID.c_str(),     sizeof(wCfg.sta.ssid) - 1);
    strncpy((char*)wCfg.sta.password, _cfg.wifiPassword.c_str(), sizeof(wCfg.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wCfg);
    esp_wifi_start();

    printf("[Synapta] WiFi connecting...\n");
    xEventGroupWaitBits(s_wifiEG, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    printf("[Synapta] WiFi connected\n");

    // MQTT — broker เป็นชื่อ .local ให้ถาม mDNS หา IP ปัจจุบันก่อน (รองรับ DHCP)
    s_host = _resolveBrokerHost();
    std::string uri = (_cfg.mqttTLS ? "mqtts://" : "mqtt://")
                    + s_host + ":" + std::to_string(_cfg.mqttPort);
    std::string clientId    = "synapta-" + _macSuffix();
    std::string statusTopic = _statusTopic();

    s_uri         = uri;
    s_clientId    = clientId;
    s_statusTopic = statusTopic;
    s_user        = _cfg.mqttUser;
    s_pass        = _cfg.mqttPassword;

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri              = s_uri.c_str();
    cfg.session.protocol_ver            = MQTT_PROTOCOL_V_5;
    // default 120 วิ = broker กว่าจะรู้ว่า node ตาย (LWT ยิง offline) ช้าสุด ~3 นาที
    // 15 วิ → รู้ภายใน ~25 วิ — กฎแจ้งเตือน "อุปกรณ์หลุด" จะเด้งเร็วขึ้นมาก
    cfg.session.keepalive               = 15;
    cfg.credentials.client_id           = s_clientId.c_str();
    cfg.session.last_will.topic         = s_statusTopic.c_str();
    cfg.session.last_will.msg           = "offline";
    cfg.session.last_will.qos           = 1;
    cfg.session.last_will.retain        = 1;
    cfg.session.last_will.msg_len       = 7;

    if (!s_user.empty()) {
        cfg.credentials.username                = s_user.c_str();
        cfg.credentials.authentication.password = s_pass.c_str();
        // password_len removed in ESP-IDF 5.x — password is null-terminated
    }
    if (_cfg.mqttTLS) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    _client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, _eventHandler, this);
    esp_mqtt_client_start(_client);

    printf("[Synapta] MQTT connecting (MQTT 5)...\n");

    // Background task — รัน message dispatch + device loop โดยไม่ต้องมี loop() ใน user code
    xTaskCreate([](void* arg) {
        auto* self = static_cast<SynaptaNodeClass*>(arg);
        uint32_t downTicks = 0;   // นับเวลาที่หลุดต่อเนื่อง (รอบละ ~10ms)
        while (true) {
            std::vector<_SynaptaMsg> inbox;
            xSemaphoreTake(self->_mutex, portMAX_DELAY);
            inbox.swap(self->_inbox);
            xSemaphoreGive(self->_mutex);
            for (auto& msg : inbox) self->_dispatch(msg);
            for (auto* d : self->_devices) d->_loop();

            // หลุดนานเกิน ~30 วิ → broker อาจย้าย IP (DHCP) — ถาม mDNS ใหม่
            if (self->_connected) {
                downTicks = 0;
            } else if (++downTicks >= 3000) {
                downTicks = 0;
                self->_maybeReResolve();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }, "synapta_main", 8192, this, 5, &_loopTask);
}

// ── MQTT event handler — runs in esp-mqtt internal FreeRTOS task ──────────────

void SynaptaNodeClass::_eventHandler(void* arg, esp_event_base_t,
                                     int32_t id, void* data) {
    auto* self  = static_cast<SynaptaNodeClass*>(arg);
    auto* event = static_cast<esp_mqtt_event_handle_t>(data);

    switch (id) {
        case MQTT_EVENT_CONNECTED:    self->_onConnected();    break;
        case MQTT_EVENT_DISCONNECTED: self->_onDisconnected(); break;
        case MQTT_EVENT_DATA:         self->_onData(event);    break;
        default: break;
    }
}

void SynaptaNodeClass::_onConnected() {
    _connected = true;
    printf("[Synapta] MQTT 5 connected\n");

    _publish(_statusTopic().c_str(), "online", true, 1);

    for (auto* d : _devices) {
        if (d->getType() == NODE_SENSOR) continue;   // sensor อ่านอย่างเดียว ไม่มี cmd/config
        std::string cmd = d->_cmdTopic(_cfg.baseTopic);
        std::string cfg = d->_configTopic(_cfg.baseTopic);
        esp_mqtt_client_subscribe(_client, cmd.c_str(), 1);
        esp_mqtt_client_subscribe(_client, cfg.c_str(), 1);
        printf("[Synapta] Sub: %s\n", cmd.c_str());
    }

    for (auto* d : _devices) d->_reportState();
    _publishManifest();

    if (_cbConnect) _cbConnect();
}

void SynaptaNodeClass::_onDisconnected() {
    _connected = false;
    printf("[Synapta] MQTT disconnected — auto-reconnecting\n");
    if (_cbDisconnect) _cbDisconnect();
}

void SynaptaNodeClass::_onData(esp_mqtt_event_handle_t event) {
    _SynaptaMsg msg;
    msg.topic   = std::string(event->topic,   event->topic_len);
    msg.payload = std::string(event->data,    event->data_len);

#ifdef CONFIG_MQTT_PROTOCOL_5
    if (event->property) {
        if (event->property->response_topic && event->property->response_topic_len > 0) {
            msg.responseTopic = std::string(event->property->response_topic,
                                            event->property->response_topic_len);
        }
        if (event->property->correlation_data && event->property->correlation_data_len > 0) {
            const auto* cd = reinterpret_cast<const uint8_t*>(event->property->correlation_data);
            msg.corrData.assign(cd, cd + event->property->correlation_data_len);
        }
    }
#endif

    xSemaphoreTake(_mutex, portMAX_DELAY);
    _inbox.push_back(std::move(msg));
    xSemaphoreGive(_mutex);
}

// ── Dispatch — runs in background loop task ───────────────────────────────────

void SynaptaNodeClass::_dispatch(const _SynaptaMsg& msg) {
    for (auto* d : _devices) {
        if (msg.topic == d->_cmdTopic(_cfg.baseTopic)) {
            d->_handleMessage(msg.payload.c_str());
            return;
        }
        if (msg.topic == d->_configTopic(_cfg.baseTopic)) {
            d->_handleConfig(
                msg.payload.c_str(),
                msg.responseTopic.empty() ? nullptr : msg.responseTopic.c_str(),
                msg.corrData.empty()      ? nullptr : msg.corrData.data(),
                msg.corrData.size()
            );
            return;
        }
    }
}

// ── Publish ───────────────────────────────────────────────────────────────────

bool SynaptaNodeClass::_publish(const char* topic, const char* payload,
                                bool retain, int qos, uint32_t expirySeconds) {
    if (!_client || !_connected) return false;

    // ส่งทีละข้อความ — set_publish_property เป็น state ของ client ทั้งตัว
    // ถ้า 2 task ส่งพร้อมกัน expiry อาจไปติดผิดข้อความ
    if (_pubMutex) xSemaphoreTake(_pubMutex, portMAX_DELAY);

#ifdef CONFIG_MQTT_PROTOCOL_5
    // ต้องเป็น static — esp-mqtt เก็บ "pointer" ของ struct นี้ไว้ ไม่ได้ก็อปค่า
    // ถ้าใช้ตัวแปร stack ชั่วคราว พอออกจาก scope library จะอ่านหน่วยความจำขยะ
    // → publish ครั้งถัดไปแครช (LoadProhibited ใน mqtt5_msg_publish)
    static esp_mqtt5_publish_property_config_t s_pubProp;
    s_pubProp = {};
    s_pubProp.message_expiry_interval = expirySeconds;   // 0 = ไม่หมดอายุ
    esp_mqtt5_client_set_publish_property(_client, &s_pubProp);
#endif

    int ret = esp_mqtt_client_publish(_client, topic, payload, 0, qos, retain ? 1 : 0);

    if (_pubMutex) xSemaphoreGive(_pubMutex);
    return ret >= 0;
}

// ── Manifest ──────────────────────────────────────────────────────────────────

void SynaptaNodeClass::_publishManifest() {
    bool allCfg = true;
    std::string nId = _nodeId();

    std::string json = "{\"nodeId\":\"";
    json += nId;
    json += "\",\"baseTopic\":\"";
    json += _cfg.baseTopic;
    json += "\",\"fw\":\"3.0.0\",\"devices\":[";

    for (size_t i = 0; i < _devices.size(); i++) {
        if (i > 0) json += ',';
        json += _devices[i]->_manifestEntry(_cfg.baseTopic);
        if (!_devices[i]->isConfigured()) allCfg = false;
    }

    json += "],\"configured\":";
    json += allCfg ? "true" : "false";
    json += "}";

    std::string topic = _manifestTopic();
    // retain + 1 ชั่วโมง — manifest เก่าหายเองถ้า node ไม่ reconnect
    bool ok = _publish(topic.c_str(), json.c_str(), true, 1, 3600);
    printf("[Synapta] Manifest → %s %s\n", topic.c_str(), ok ? "OK" : "FAILED");
}

// broker ชื่อ .local อาจย้าย IP (DHCP แจกใหม่) — ถามซ้ำ ถ้าได้เลขใหม่ให้ย้ายตาม
void SynaptaNodeClass::_maybeReResolve() {
    const std::string& host = _cfg.mqttBroker;
    const std::string suffix = ".local";
    if (host.size() <= suffix.size() ||
        host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) return;

    std::string ip = _mdnsResolve(host.substr(0, host.size() - suffix.size()));
    if (ip.empty() || ip == s_host) return;

    printf("[Synapta] broker ย้าย IP → %s — เชื่อมใหม่\n", ip.c_str());
    s_host = ip;
    _saveCachedBrokerIp(ip);
    s_uri = (_cfg.mqttTLS ? "mqtts://" : "mqtt://") + s_host + ":" + std::to_string(_cfg.mqttPort);
    esp_mqtt_client_set_uri(_client, s_uri.c_str());
    esp_mqtt_client_reconnect(_client);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string SynaptaNodeClass::_macSuffix() const {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[7];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    return std::string(buf);
}

std::string SynaptaNodeClass::_nodeId() const {
    return !_cfg.nodeId.empty() ? _cfg.nodeId : "node-" + _macSuffix();
}
std::string SynaptaNodeClass::_statusTopic()   const { return _cfg.baseTopic + "/nodes/" + _nodeId() + "/status"; }
std::string SynaptaNodeClass::_manifestTopic() const { return _cfg.baseTopic + "/nodes/" + _nodeId() + "/manifest"; }
