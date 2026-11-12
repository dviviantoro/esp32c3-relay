#include "protocol.h"

#include <stdio.h>
#include <string.h>

namespace proto {

const char *errName(Err e) {
    switch (e) {
        case Err::OK:            return "OK";
        case Err::BAD_JSON:      return "BAD_JSON";
        case Err::UNKNOWN_CMD:   return "UNKNOWN_CMD";
        case Err::BAD_ID:        return "BAD_ID";
        case Err::BAD_RELAY:     return "BAD_RELAY";
        case Err::BAD_MS:        return "BAD_MS";
        case Err::BUSY:          return "BUSY";
        case Err::LINE_TOO_LONG: return "LINE_TOO_LONG";
    }
    return "UNKNOWN";
}

namespace {

struct Cursor {
    const char *p;
    void ws() { while (*p == ' ' || *p == '\t') p++; }
    bool eat(char c) { ws(); if (*p == c) { p++; return true; } return false; }
};

// String without escapes (escapes are rejected: nothing in our protocol needs them).
bool readString(Cursor &c, char *out, size_t cap) {
    if (!c.eat('"')) return false;
    size_t n = 0;
    while (*c.p && *c.p != '"') {
        if (*c.p == '\\' || (unsigned char)*c.p < 0x20) return false;
        if (n + 1 >= cap) return false;
        out[n++] = *c.p++;
    }
    if (*c.p != '"') return false;
    c.p++;
    out[n] = '\0';
    return true;
}

// Unsigned decimal, no sign/fraction/exponent, no leading zeros, fits uint32.
bool readUInt(Cursor &c, uint32_t &out) {
    c.ws();
    if (*c.p < '0' || *c.p > '9') return false;
    if (*c.p == '0' && c.p[1] >= '0' && c.p[1] <= '9') return false;
    uint64_t v = 0;
    while (*c.p >= '0' && *c.p <= '9') {
        v = v * 10 + (uint32_t)(*c.p++ - '0');
        if (v > UINT32_MAX) return false;
    }
    if (*c.p == '.' || *c.p == 'e' || *c.p == 'E') return false;  // fraction/exponent
    out = (uint32_t)v;
    return true;
}

// Skips an unknown value (string, number, true/false/null). Nesting is rejected.
bool skipValue(Cursor &c) {
    c.ws();
    if (*c.p == '"') {
        c.p++;
        while (*c.p && *c.p != '"') {
            if (*c.p == '\\') return false;
            c.p++;
        }
        if (*c.p != '"') return false;
        c.p++;
        return true;
    }
    if (*c.p == '{' || *c.p == '[' || *c.p == '\0') return false;
    const char *s = c.p;
    while (*c.p && *c.p != ',' && *c.p != '}' && *c.p != ' ') c.p++;
    return c.p != s;
}

}  // namespace

Err parseRequest(const char *line, Request &req) {
    req = Request{};
    Cursor c{line};
    char key[12], cmdStr[12] = "";
    bool haveId = false, haveCmd = false, haveRelay = false, haveMs = false;
    bool idBad = false, relayBad = false, msBad = false;
    uint32_t id = 0, relay = 0, ms = DEFAULT_MS;

    if (!c.eat('{')) return Err::BAD_JSON;
    c.ws();
    if (*c.p != '}') {
        for (;;) {
            if (!readString(c, key, sizeof key)) {
                return Err::BAD_JSON;
            }
            if (!c.eat(':')) return Err::BAD_JSON;
            c.ws();
            if (!strcmp(key, "id") || !strcmp(key, "relay") || !strcmp(key, "ms")) {
                uint32_t v;
                bool ok = readUInt(c, v);
                if (!strcmp(key, "id"))        { haveId = true;    idBad = !ok;    id = ok ? v : 0; }
                else if (!strcmp(key, "relay")){ haveRelay = true; relayBad = !ok; relay = ok ? v : 0; }
                else                           { haveMs = true;    msBad = !ok;    ms = ok ? v : 0; }
                if (!ok) {
                    // Resync: skip the malformed scalar so remaining keys still parse.
                    while (*c.p && *c.p != ',' && *c.p != '}') c.p++;
                }
            } else if (!strcmp(key, "cmd")) {
                if (!readString(c, cmdStr, sizeof cmdStr)) {
                    cmdStr[0] = '?'; cmdStr[1] = '\0';
                    while (*c.p && *c.p != ',' && *c.p != '}') c.p++;
                }
                haveCmd = true;
            } else if (!skipValue(c)) {
                return Err::BAD_JSON;
            }
            if (c.eat(',')) continue;
            break;
        }
    }
    if (!c.eat('}')) return Err::BAD_JSON;
    c.ws();
    if (*c.p != '\0') return Err::BAD_JSON;

    if (haveId && !idBad) req.id = id;
    if (!haveId || idBad || id == 0) return Err::BAD_ID;

    if (!haveCmd) return Err::UNKNOWN_CMD;
    if (!strcmp(cmdStr, "ping")) { req.cmd = Cmd::PING; return Err::OK; }
    if (strcmp(cmdStr, "pulse") != 0) return Err::UNKNOWN_CMD;

    if (!haveRelay || relayBad || relay < 1 || relay > 2) return Err::BAD_RELAY;
    if (haveMs && (msBad || ms < 1 || ms > MAX_MS)) return Err::BAD_MS;
    req.cmd = Cmd::PULSE;
    req.relay = (uint8_t)relay;
    req.ms = (uint16_t)(haveMs ? ms : DEFAULT_MS);
    return Err::OK;
}

namespace {
size_t done(int n, size_t cap) { return (n > 0 && (size_t)n < cap) ? (size_t)n : 0; }
}

size_t fmtErr(char *b, size_t cap, uint32_t id, Err e) {
    return done(snprintf(b, cap, "{\"id\":%lu,\"ok\":false,\"err\":\"%s\"}\n",
                         (unsigned long)id, errName(e)), cap);
}
size_t fmtAck(char *b, size_t cap, uint32_t id) {
    return done(snprintf(b, cap, "{\"id\":%lu,\"ok\":true,\"evt\":\"ack\"}\n", (unsigned long)id), cap);
}
size_t fmtDone(char *b, size_t cap, uint32_t id, uint8_t relay, uint16_t ms) {
    return done(snprintf(b, cap, "{\"id\":%lu,\"ok\":true,\"evt\":\"done\",\"relay\":%u,\"ms\":%u}\n",
                         (unsigned long)id, relay, ms), cap);
}
size_t fmtPong(char *b, size_t cap, uint32_t id, const PongInfo &p) {
    return done(snprintf(b, cap,
        "{\"id\":%lu,\"ok\":true,\"evt\":\"pong\",\"proto\":%u,\"fw\":\"%s\",\"uptime_ms\":%llu,"
        "\"relays\":[%u,%u],\"busy\":%s,\"last_done\":%lu,\"errs\":%lu,\"drops\":%lu,\"reset\":\"%s\"}\n",
        (unsigned long)id, VERSION, p.fw, (unsigned long long)p.uptimeMs,
        p.relayMask & 1, (p.relayMask >> 1) & 1, p.relayMask ? "true" : "false",
        (unsigned long)p.lastDoneId, (unsigned long)p.errCount, (unsigned long)p.dropCount,
        p.resetReason), cap);
}

}  // namespace proto
