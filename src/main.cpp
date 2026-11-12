// KITB-Relay firmware: Raspberry Pi -> ESP32-C3 relay pulse controller over USB CDC.
// Protocol: see lib/protocol/protocol.h and README.md.
// Rules: Serial carries protocol traffic only (no debug prints); loop() never blocks.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include "protocol.h"

static const char *FW_VERSION = "1.1.0";

// Hardware config. Set RELAY_ON to LOW for active-low relay modules.
static constexpr uint8_t RELAY_ON  = HIGH;
static constexpr uint8_t RELAY_OFF = LOW;
static constexpr uint8_t RELAY_COUNT = 2;
static constexpr uint32_t WDT_TIMEOUT_MS = 2000;
static constexpr uint32_t REPLAY_WINDOW_MS = 10000;  // retry-dedup memory

struct RelayState {
    uint8_t  pin;
    bool     active;
    uint32_t startedAt;
    uint16_t ms;
    uint32_t reqId;
};
static RelayState relays[RELAY_COUNT] = {{6, false, 0, 0, 0}, {7, false, 0, 0, 0}};

// Recent pulse requests, so a Pi retry with the same id never fires the relay twice.
struct Recent {
    uint32_t id;       // 0 = empty
    uint8_t  relay;
    uint16_t ms;
    bool     done;
    uint32_t at;
};
static Recent recent[8];
static uint8_t recentNext = 0;

static uint32_t lastDoneId = 0, errCount = 0, dropCount = 0;
static char lineBuf[proto::MAX_LINE];
static size_t lineLen = 0;
static bool overflow = false;

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static const char *resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_USB:       return "usb";
        default:                return "unknown";
    }
}

// Never blocks: if the host is not draining USB, the reply is dropped and counted.
static void send(const char *buf, size_t len) {
    if (len == 0) { dropCount++; return; }
    if ((size_t)Serial.availableForWrite() < len) { dropCount++; return; }
    Serial.write((const uint8_t *)buf, len);
}

static void replyErr(uint32_t id, proto::Err e) {
    char b[proto::MAX_REPLY];
    errCount++;
    send(b, proto::fmtErr(b, sizeof b, id, e));
}
static void replyAck(uint32_t id) {
    char b[proto::MAX_REPLY];
    send(b, proto::fmtAck(b, sizeof b, id));
}
static void replyDone(uint32_t id, uint8_t relay, uint16_t ms) {
    char b[proto::MAX_REPLY];
    send(b, proto::fmtDone(b, sizeof b, id, relay, ms));
}

static uint8_t relayMask() {
    uint8_t m = 0;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) if (relays[i].active) m |= 1u << i;
    return m;
}

static Recent *findRecent(uint32_t id, uint32_t now) {
    for (auto &r : recent)
        if (r.id == id && (now - r.at) < REPLAY_WINDOW_MS) return &r;
    return nullptr;
}

static void handlePing(uint32_t id) {
    char b[proto::MAX_REPLY];
    proto::PongInfo p{(uint64_t)(esp_timer_get_time() / 1000), FW_VERSION, relayMask(),
                      lastDoneId, errCount, dropCount, resetReason()};
    send(b, proto::fmtPong(b, sizeof b, id, p));
}

static void handlePulse(const proto::Request &rq) {
    uint32_t now = nowMs();
    if (Recent *r = findRecent(rq.id, now)) {      // retry of a request we already saw
        if (r->relay == rq.relay && r->ms == rq.ms) {
            replyAck(rq.id);
            if (r->done) replyDone(rq.id, r->relay, r->ms);
            return;
        }
        return replyErr(rq.id, proto::Err::BAD_ID);  // same id, different content
    }
    RelayState &rs = relays[rq.relay - 1];
    if (rs.active) return replyErr(rq.id, proto::Err::BUSY);

    rs = {rs.pin, true, now, rq.ms, rq.id};
    gpio_set_level((gpio_num_t)rs.pin, RELAY_ON);
    recent[recentNext] = {rq.id, rq.relay, rq.ms, false, now};
    recentNext = (recentNext + 1) % (sizeof recent / sizeof recent[0]);
    replyAck(rq.id);
}

static void handleLine(const char *line) {
    proto::Request rq;
    proto::Err e = proto::parseRequest(line, rq);
    if (e != proto::Err::OK) return replyErr(rq.id, e);
    if (rq.cmd == proto::Cmd::PING) handlePing(rq.id);
    else handlePulse(rq);
}

static void readSerial() {
    int budget = 256;  // bound work per loop iteration
    while (budget-- > 0 && Serial.available()) {
        int c = Serial.read();
        if (c < 0 || c == '\r') continue;
        if (c == '\n') {
            if (overflow) replyErr(0, proto::Err::LINE_TOO_LONG);
            else if (lineLen > 0) { lineBuf[lineLen] = '\0'; handleLine(lineBuf); }
            lineLen = 0;
            overflow = false;
        } else if (lineLen < sizeof lineBuf - 1) {
            lineBuf[lineLen++] = (char)c;
        } else {
            overflow = true;
        }
    }
}

static void updateRelays() {
    uint32_t now = nowMs();
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        RelayState &rs = relays[i];
        if (rs.active && (uint32_t)(now - rs.startedAt) >= rs.ms) {
            gpio_set_level((gpio_num_t)rs.pin, RELAY_OFF);
            rs.active = false;
            lastDoneId = rs.reqId;
            for (auto &r : recent) if (r.id == rs.reqId) r.done = true;
            replyDone(rs.reqId, i + 1, rs.ms);
        }
    }
}

void setup() {
    // Drive the output level before enabling the driver so the pin never glitches.
    for (auto &rs : relays) {
        gpio_set_level((gpio_num_t)rs.pin, RELAY_OFF);
        pinMode(rs.pin, OUTPUT);
        gpio_set_level((gpio_num_t)rs.pin, RELAY_OFF);
    }
    Serial.setTxTimeoutMs(0);
    Serial.begin(115200);

    // Hang/crash => reset in 2 s. (Pins then float: use pull-downs / fail-safe wiring.)
    esp_task_wdt_config_t wdt = {.timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
    esp_task_wdt_add(nullptr);
}

void loop() {
    esp_task_wdt_reset();
    readSerial();
    updateRelays();
    delay(1);
}
