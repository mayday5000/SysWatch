#pragma once
#include "syscall_monitor_shared.h"
#include <cstdint>
#include <string>

struct UiEvent {
    int32_t pid = 0;
    int32_t nr = 0;
    uint64_t ts_ns = 0;
    bool compat = false;   // 32-bit task on a 64-bit kernel (SM_NR_COMPAT)
    std::string comm;
    std::string sys_name;
    std::string extra;
};

enum class Sort : int32_t {
    Time = 4,
    Pid = 1,
    Process = 5,
    SysId = 6,
    Syscall = 2,
    Param = 3
};

// Values above 0xff so no byte read from the terminal (Ctrl-A is 1,
// Ctrl-H is 8, ...) can be mistaken for a navigation key.
enum class Key : int32_t {
    Up = 0x101, Down = 0x102, PgUp = 0x103, PgDn = 0x104,
    Pause = 0x105, Follow = 0x106, Home = 0x107, End = 0x108
};

class IView {
public:
    virtual ~IView() = default;
    virtual void append(UiEvent ev) = 0;
    virtual void on_key(Key key) = 0;
    virtual void set_sort(Sort sort) = 0;
    virtual Sort sort() const = 0;
    virtual void set_desc(bool desc) = 0;
    virtual bool desc() const = 0;
    virtual void set_rows(int32_t rows) = 0;
    virtual const UiEvent *row(int32_t visible) const = 0;
    virtual int32_t count() const = 0;
    virtual int32_t top() const = 0;
    virtual int32_t sel() const = 0;
    virtual bool paused() const = 0;
    virtual bool dirty() const = 0;
    virtual void mark_drawn() = 0;
};
