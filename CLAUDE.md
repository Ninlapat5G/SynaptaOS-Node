# SynaptaNode — Project Guide

ESP32 library สำหรับ Synapta smart home — เชื่อม ESP32 เข้ากับ AI dashboard ผ่าน **MQTT 5**
Repo: https://github.com/Ninlapat5G/SynaptaNode-V0

## สถานะปัจจุบัน (branch: main, v3.0.0)

ใช้ **ESP-IDF framework** (ไม่ใช่ Arduino) — migration เสร็จแล้วทั้ง core library + examples ทั้ง 9 ตัว

ทำไมต้อง ESP-IDF: Arduino ESP32 SDK ส่ง `libmqtt.a` ที่ pre-compile มาด้วย `CONFIG_MQTT_PROTOCOL_311` → ใช้ MQTT5 API ไม่ได้ ส่วน ESP-IDF compile `libmqtt.a` จาก source ทำให้เปิด `CONFIG_MQTT_PROTOCOL_5=y` ได้ ซึ่งจำเป็นจริงสำหรับระบบนี้ (manifest/state expiry กัน zombie device เมื่อ node ตาย, responseTopic/correlationData สำหรับ config ACK)

## โครงสร้าง

| ไฟล์ | หน้าที่ |
|---|---|
| `src/SynaptaNode.cpp/.h` | core: WiFi + MQTT5 client, background FreeRTOS task, manifest/status publish |
| `src/SynaptaDevice.cpp/.h` | logic ของอุปกรณ์แต่ละตัว: parse cmd, PWM/GPIO, NVS pin config, publish state |
| `src/SynaptaDevices.h` | wrapper API: `SynaptaDigital` / `SynaptaAnalog` / `SynaptaSensor` |
| `src/SynaptaRegistry.h` | static registry — อุปกรณ์ auto-register ผ่าน constructor |
| `src/NodeConfig.cpp/.h` | เก็บ/โหลด credential จาก NVS |

## API (user-facing)

```cpp
#include <Synapta.h>

SynaptaDigital lamp("bedroom/lamp");

extern "C" void app_main() {        // ← ESP-IDF ใช้ app_main ไม่ใช่ setup()/loop()
    Synapta.wifi("SSID", "PASS");
    Synapta.baseTopic("Mylab/smarthome");
    Synapta.start();                // background task จัดการ loop เอง ไม่ต้องเรียก loop()
}
```

Pin ตั้งจาก Web App (Edit device → Pin → Save) ส่งผ่าน `{base}/{topic}/config` แล้ว save ลง NVS

## MQTT 5 Protocol (ต้องตรงกับ Web App + Hub)

- **cmd**:     `{base}/{topic}/set`     — รับคำสั่ง (digital: `true/false/on/off/1/0/toggle` · analog: 0–255)
- **state**:   `{base}/{topic}/state`   — รายงานสถานะ (retain + expiry 300s)
- **config**:  `{base}/{topic}/config`  — รับ pin config (`{"pin":N}`), ตอบ ACK ผ่าน responseTopic
- **status**:  `{base}/nodes/{nodeId}/status`   — `online` (retained) / `offline` (LWT)
- **manifest**: `{base}/nodes/{nodeId}/manifest` — auto-discovery (retain + expiry 3600s)

## Build requirements

ทุกโปรเจค/example ต้องมี `sdkconfig.defaults`:
```
CONFIG_MQTT_PROTOCOL_5=y
CONFIG_MQTT_TRANSPORT_SSL=y
CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y
```
⚠️ ถ้าลืม `CONFIG_MQTT_PROTOCOL_5=y` โค้ดที่อยู่ใน `#ifdef CONFIG_MQTT_PROTOCOL_5` (อ่าน responseTopic, ตั้ง message expiry) จะถูกตัดทิ้งเงียบ ๆ → manifest ไม่หมดอายุ + config ACK ไม่ทำงาน

## Broker แบบชื่อ (mDNS) — รองรับ DHCP

ตั้ง broker เป็นชื่อ `.local` แทน IP: `Synapta.broker("synapta-hub.local", 1883, false)`
- lwip resolve ให้เองตอนเชื่อมต่อ (ต้องมี `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`) — DHCP แจกเลขใหม่ก็หาเจอ เพราะ resolve ใหม่ทุกครั้งที่ reconnect
- เครื่อง Hub จริง (RPi/Linux): ตั้ง hostname เป็น `synapta-hub` → Avahi ตอบ `synapta-hub.local` เองในตัว · Windows ตอบเฉพาะ "ชื่อเครื่องตัวเอง.local" (เช่น `NINV2.local`) — zeroconf ฝั่ง Hub ประกาศ alias ได้แต่ Windows Firewall มักบล็อกคำถามจากเครื่องอื่น ใช้ชื่อเครื่องตรง ๆ ชัวร์กว่า
- ทดสอบบนบอร์ดจริงแล้ว (2026-06-11): resolve `NINV2.local` → MQTT5 ต่อสำเร็จ
- ถ้าโปรเจคมี component `espressif/mdns` library จะ resolve เอง + cache IP ลง NVS เป็นแผนสำรอง — แต่กับ PlatformIO header component มองไม่เห็นจาก lib_deps ให้พึ่งเส้นทาง lwip เป็นหลัก

`platformio.ini`: `framework = espidf`, `lib_deps` ชี้ `https://github.com/Ninlapat5G/SynaptaNode-V0.git`

## หมายเหตุ

- Example 03/06 ใช้ mock sensor (return ค่าคงที่) เพราะ DHT Arduino library ใช้กับ ESP-IDF ตรง ๆ ไม่ได้ — ถ้าต้องการ sensor จริงใช้ ESP-IDF driver แทน
- ค้าง: ยังไม่ได้ทำ Arduino/MQTT3 version แยก (ถ้าจะรองรับคน Arduino IDE ต้องคุยเรื่อง branch/feature ที่จะ drop เช่น manifest expiry, responseTopic)
