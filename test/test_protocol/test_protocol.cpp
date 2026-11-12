#include <unity.h>
#include <string.h>
#include "protocol.h"
using namespace proto;

static Err P(const char *s, Request &r) { return parseRequest(s, r); }

void test_ping() {
    Request r; TEST_ASSERT_EQUAL(Err::OK, P("{\"id\":13,\"cmd\":\"ping\"}", r));
    TEST_ASSERT_EQUAL(Cmd::PING, r.cmd); TEST_ASSERT_EQUAL_UINT32(13, r.id);
}
void test_pulse_full() {
    Request r; TEST_ASSERT_EQUAL(Err::OK, P("{\"id\":12,\"cmd\":\"pulse\",\"relay\":2,\"ms\":1500}", r));
    TEST_ASSERT_EQUAL(Cmd::PULSE, r.cmd); TEST_ASSERT_EQUAL(2, r.relay); TEST_ASSERT_EQUAL(1500, r.ms);
}
void test_pulse_default_ms_and_ws_and_order() {
    Request r; TEST_ASSERT_EQUAL(Err::OK, P(" { \"relay\" : 1 , \"cmd\":\"pulse\", \"id\": 7 } ", r));
    TEST_ASSERT_EQUAL(1000, r.ms); TEST_ASSERT_EQUAL_UINT32(7, r.id);
}
void test_unknown_keys_ignored() {
    Request r; TEST_ASSERT_EQUAL(Err::OK, P("{\"id\":1,\"x\":\"hi\",\"y\":true,\"cmd\":\"ping\"}", r));
}
void test_bad_json() {
    Request r;
    const char *bad[] = {"", "garbage", "{", "{\"id\":1", "{\"id\":1,}", "{\"id\":1} x",
                         "{\"id\":1,\"cmd\":\"ping\",\"n\":{}}", "[]", "{\"id\" 1}"};
    for (auto s : bad) TEST_ASSERT_EQUAL_MESSAGE(Err::BAD_JSON, P(s, r), s);
}
void test_bad_id() {
    Request r;
    TEST_ASSERT_EQUAL(Err::BAD_ID, P("{\"cmd\":\"ping\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_ID, P("{\"id\":0,\"cmd\":\"ping\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_ID, P("{\"id\":\"a\",\"cmd\":\"ping\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_ID, P("{\"id\":4294967296,\"cmd\":\"ping\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_ID, P("{\"id\":-1,\"cmd\":\"ping\"}", r));
    TEST_ASSERT_EQUAL(Err::OK, P("{\"id\":4294967295,\"cmd\":\"ping\"}", r));
}
void test_unknown_cmd_keeps_id() {
    Request r; TEST_ASSERT_EQUAL(Err::UNKNOWN_CMD, P("{\"id\":9,\"cmd\":\"reboot\"}", r));
    TEST_ASSERT_EQUAL_UINT32(9, r.id);
    TEST_ASSERT_EQUAL(Err::UNKNOWN_CMD, P("{\"id\":9}", r));
}
void test_bad_relay_and_ms() {
    Request r;
    TEST_ASSERT_EQUAL(Err::BAD_RELAY, P("{\"id\":1,\"cmd\":\"pulse\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_RELAY, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":0}", r));
    TEST_ASSERT_EQUAL(Err::BAD_RELAY, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":3}", r));
    TEST_ASSERT_EQUAL(Err::BAD_RELAY, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":\"1\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_MS, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":1,\"ms\":0}", r));
    TEST_ASSERT_EQUAL(Err::BAD_MS, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":1,\"ms\":5001}", r));
    TEST_ASSERT_EQUAL(Err::BAD_MS, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":1,\"ms\":\"abc\"}", r));
    TEST_ASSERT_EQUAL(Err::BAD_MS, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":1,\"ms\":1.5}", r));
    TEST_ASSERT_EQUAL(Err::OK, P("{\"id\":1,\"cmd\":\"pulse\",\"relay\":1,\"ms\":5000}", r));
}
void test_format() {
    char b[MAX_REPLY];
    TEST_ASSERT_GREATER_THAN(0, fmtErr(b, sizeof b, 5, Err::BUSY));
    TEST_ASSERT_EQUAL_STRING("{\"id\":5,\"ok\":false,\"err\":\"BUSY\"}\n", b);
    fmtAck(b, sizeof b, 5);
    TEST_ASSERT_EQUAL_STRING("{\"id\":5,\"ok\":true,\"evt\":\"ack\"}\n", b);
    fmtDone(b, sizeof b, 5, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("{\"id\":5,\"ok\":true,\"evt\":\"done\",\"relay\":1,\"ms\":1000}\n", b);
    PongInfo p{123456789012ULL, "1.1.0", 2, 4, 1, 0, "power_on"};
    TEST_ASSERT_GREATER_THAN(0, fmtPong(b, sizeof b, 6, p));
    TEST_ASSERT_NOT_NULL(strstr(b, "\"relays\":[0,1],\"busy\":true"));
    TEST_ASSERT_NOT_NULL(strstr(b, "\"uptime_ms\":123456789012"));
}
void test_format_truncation() {
    char b[8]; TEST_ASSERT_EQUAL(0, fmtAck(b, sizeof b, 1));
}
void test_fuzz_no_crash() {
    unsigned s = 12345; char line[MAX_LINE]; Request r;
    const char alpha[] = "{}[]\":, \\0123456789abcdeilmnopsuyrt-.";
    for (int i = 0; i < 200000; i++) {
        s = s * 1664525u + 1013904223u;
        size_t n = (s >> 8) % (MAX_LINE - 1);
        for (size_t j = 0; j < n; j++) { s = s * 1664525u + 1013904223u; line[j] = alpha[(s >> 16) % (sizeof alpha - 1)]; }
        line[n] = 0; parseRequest(line, r);
    }
    TEST_PASS();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ping); RUN_TEST(test_pulse_full); RUN_TEST(test_pulse_default_ms_and_ws_and_order);
    RUN_TEST(test_unknown_keys_ignored); RUN_TEST(test_bad_json); RUN_TEST(test_bad_id);
    RUN_TEST(test_unknown_cmd_keeps_id); RUN_TEST(test_bad_relay_and_ms); RUN_TEST(test_format);
    RUN_TEST(test_format_truncation); RUN_TEST(test_fuzz_no_crash);
    return UNITY_END();
}
