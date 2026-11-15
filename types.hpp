#pragma once
#include "syscall_monitor_shared.h"
#include <cstdint>
#include <string>

struct UiEvent {
    int32_t pid = 0;
    int32_t nr = 0;
    uint64_t ts_ns = 0;
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

enum class Key : int32_t {
    Up = 1, Down = 2, PgUp = 3, PgDn = 4,
    Pause = 5, Follow = 6, Home = 7, End = 8
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
