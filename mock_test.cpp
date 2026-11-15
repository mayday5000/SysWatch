// Mock checks for the ABI, the span decoder, and filter parse.
#include "filters.hpp"
#include "syscall_monitor_shared.h"
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

static int count_cb(const sm_event &, void *ud) {
    *static_cast<int *>(ud) += 1;
    return 0;
}

static bool decode(std::span<const uint8_t> buf, int (*fn)(const sm_event &, void *), void *ud) {
    if (buf.empty() || buf.size() % SM_EVENT_SIZE != 0) return false;
    for (size_t off = 0; off < buf.size(); off += SM_EVENT_SIZE) {
        sm_event ev{};
        std::memcpy(&ev, buf.data() + off, SM_EVENT_SIZE);
        if (fn && fn(ev, ud)) return false;
    }
    return true;
}

int main() {
    int fail = 0;
    auto check = [&](bool ok, const char *name) {
        std::cout << (ok ? "ok  " : "FAIL ") << name << "\n";
        if (!ok) fail++;
    };
    check(sizeof(sm_event) == 176, "event is 176");
    check(SM_EVENT_RING_SIZE == 512, "ring is 512");
    check((SM_EVENT_RING_SIZE & (SM_EVENT_RING_SIZE - 1)) == 0, "ring power of two");

    std::vector<uint8_t> one(SM_EVENT_SIZE, 0);
    int n = 0;
    check(decode(one, count_cb, &n) && n == 1, "one record");
    std::vector<uint8_t> bad(SM_EVENT_SIZE - 1, 0);
    check(!decode(bad, count_cb, &n), "reject partial");

    auto rules = FilterStore::parse("<hide kind=\"sysid\">0</hide><hide kind=\"process\">nginx</hide>");
    check(rules.size() == 2, "parse two rules");
    check(rules[0].kind == RuleKind::SysId && rules[0].nr == 0, "sysid 0");
    check(rules[1].kind == RuleKind::Process && rules[1].text == "nginx", "process nginx");
    return fail ? 1 : 0;
}
