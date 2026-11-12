// Hardware-independent wire protocol: strict parser + allocation-free formatter.
// Newline-delimited flat JSON objects. See README.md for the full spec.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace proto {

constexpr uint8_t  VERSION    = 1;
constexpr uint16_t DEFAULT_MS = 1000;
constexpr uint16_t MAX_MS     = 5000;
constexpr size_t   MAX_LINE   = 128;   // incl. terminator
constexpr size_t   MAX_REPLY  = 192;

enum class Err : uint8_t {
    OK = 0, BAD_JSON, UNKNOWN_CMD, BAD_ID, BAD_RELAY, BAD_MS, BUSY, LINE_TOO_LONG,
};
const char *errName(Err e);

enum class Cmd : uint8_t { NONE = 0, PING, PULSE };

struct Request {
    Cmd      cmd   = Cmd::NONE;
    uint32_t id    = 0;   // filled whenever it could be parsed, even on error
    uint8_t  relay = 0;   // 1..2
    uint16_t ms    = DEFAULT_MS;
};

// Parses one line (NUL-terminated, no newline). On error `req.id` still holds
// the id if one was readable, so the error reply can be correlated.
Err parseRequest(const char *line, Request &req);

struct PongInfo {
    uint64_t uptimeMs;
    const char *fw;
    uint8_t  relayMask;      // bit0 = relay1, bit1 = relay2
    uint32_t lastDoneId;     // most recent completed pulse id (0 = none)
    uint32_t errCount;
    uint32_t dropCount;      // replies dropped because TX was full
    const char *resetReason;
};

// Each returns the length written (excluding NUL), or 0 if buffer too small.
size_t fmtErr(char *buf, size_t cap, uint32_t id, Err e);
size_t fmtAck(char *buf, size_t cap, uint32_t id);
size_t fmtDone(char *buf, size_t cap, uint32_t id, uint8_t relay, uint16_t ms);
size_t fmtPong(char *buf, size_t cap, uint32_t id, const PongInfo &p);

}  // namespace proto
