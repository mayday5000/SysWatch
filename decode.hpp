#pragma once
/*
 * decode.hpp
 * Purpose: split a read() buffer into sm_event records. Shared by the TUI
 * and mock_test, so the test exercises the real decoder.
 */
#include "syscall_monitor_shared.h"
#include <cstdint>
#include <cstring>
#include <span>

// decode_events walks a read() buffer as whole records. std::span carries
// the pointer and the length together, so a short count cannot be passed
// with the wrong size. A partial record is rejected, not split. Returns
// false for a malformed buffer or when fn returns non-zero.
inline bool decode_events(std::span<const uint8_t> buf, int (*fn)(const sm_event &, void *), void *ud) {
    if (buf.empty() || buf.size() % SM_EVENT_SIZE != 0)
        return false;
    for (size_t off = 0; off < buf.size(); off += SM_EVENT_SIZE) {
        sm_event ev{};
        std::memcpy(&ev, buf.data() + off, SM_EVENT_SIZE);
        ev.comm[SM_COMM_LEN - 1] = '\0';
        ev.arg_str[SM_ARG_STR_LEN - 1] = '\0';
        if (fn && fn(ev, ud))
            return false;
    }
    return true;
}
