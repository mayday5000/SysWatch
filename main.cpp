#include "filters.hpp"
#include "types.hpp"
#include "view.hpp"
#include "renderer.hpp"
#include "capture.hpp"
#include "mock_generator.hpp"
#include "syscall_table.hpp"
#include "text_util.hpp"
/*
 * main.cpp
 * Purpose: the userspace program. Owns the terminal, the device or the
 * simulator, the filter store, the view, and the poll loop. Keys are
 * dispatched from a map of lambdas. Records arrive as 176-byte blobs and
 * are turned into UiEvent rows.
 */
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <cstdio>
#include <expected>
#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

// Compile-time check that the ring size is a power of two. The kernel
// indexes the ring with a mask; a non-power-of-two size would wrap wrong.
consteval bool sm_is_pow2(unsigned n) { return n && (n & (n - 1)) == 0; }
static_assert(sm_is_pow2(SM_EVENT_RING_SIZE), "ring size must be a power of two");
static_assert(SM_EVENT_SIZE == 176, "wire record drifted from 176 bytes");

// ==========================================
// Core RAII Hardware & Environment Wrappers
// ==========================================

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }

    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    
    Fd(Fd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd &operator=(Fd &&o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
private:
    int fd_;
};

class Terminal {
public:
    Terminal() {
        if (tcgetattr(STDIN_FILENO, &orig_) == 0) {
            termios t = orig_;
            t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            ok_ = true;
        }
        std::fwrite(tui::ansi::enter_tui_mode.data(), 1, tui::ansi::enter_tui_mode.size(), stdout);
        std::fflush(stdout);
    }
    
    ~Terminal() {
        if (ok_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
        }
        std::fwrite(tui::ansi::exit_tui_mode.data(), 1, tui::ansi::exit_tui_mode.size(), stdout);
        std::fflush(stdout);
    }

    Terminal(const Terminal &) = delete;
    Terminal &operator=(const Terminal &) = delete;
private:
    termios orig_{};
    bool ok_ = false;
};

// ==========================================
// Input parser
// ==========================================

static int read_key() {
    unsigned char b = 0;
    ssize_t n = ::read(STDIN_FILENO, &b, sizeof(b));
    if (n <= 0) return 0;
    if (b != 0x1b) return b;
    
    std::array<unsigned char, 3> seq{};
    ssize_t n2 = ::read(STDIN_FILENO, seq.data(), seq.size());
    
    if (n2 >= 2 && seq[0] == '[') {
        if (seq[1] == 'A') return std::to_underlying(Key::Up);
        if (seq[1] == 'B') return std::to_underlying(Key::Down);
        if (seq[1] == 'H') return std::to_underlying(Key::Home);
        if (seq[1] == 'F') return std::to_underlying(Key::End);
        
        if (n2 >= 3 && seq[2] == '~') {
            if (seq[1] == '5') return std::to_underlying(Key::PgUp);
            if (seq[1] == '6') return std::to_underlying(Key::PgDn);
        }
    }
    return 0x1b; 
}

struct EventLoopContext {
    IFilterStore *filters = nullptr;
    Capture *cap = nullptr;
    IView *view = nullptr;
};

// decode_events walks a read() buffer as whole records. std::span carries
// the pointer and the length together, so a short count cannot be passed
// with the wrong size. A partial record is rejected, not split.
static bool decode_events(std::span<const uint8_t> buf, int (*fn)(const sm_event &, void *), void *ud) {
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

static int on_raw_kernel_event(const sm_event &raw, void *user_data) {
    auto *ctx = static_cast<EventLoopContext*>(user_data);
    
    UiEvent e;
    e.pid = raw.pid;
    e.nr = raw.nr;
    e.ts_ns = raw.ts_ns;
    // comm and the path argument are attacker-controlled; never let them
    // reach the terminal (or the capture file) as raw control bytes.
    e.comm = sanitize_terminal(std::string_view(raw.comm, strnlen(raw.comm, SM_COMM_LEN)));
    e.sys_name = syscall_name(raw.nr);
    e.extra = sanitize_terminal(std::string_view(raw.arg_str, strnlen(raw.arg_str, SM_ARG_STR_LEN)));

    if (ctx->filters->hidden(e.comm, e.pid, e.nr, e.sys_name)) {
        return 0; 
    }
    if (ctx->cap->active()) {
        ctx->cap->write(e);
    }
    ctx->view->append(std::move(e));
    return 0; 
}

static void clear_overlay_area(IView &view, tui::UiState &state) {
    state.overlay = tui::OverlayMode::None;
    state.frame_cache.clear(); 
    std::fputs("\033[5;1H\033[J", stdout);
    std::fflush(stdout);
    view.set_rows(view.count()); 
}

static bool check_lsof_presence() {
    if (std::filesystem::exists("/usr/bin/lsof") || 
        std::filesystem::exists("/bin/lsof") || 
        std::filesystem::exists("/usr/sbin/lsof")) {
        return true;
    }
    
    FILE* pipe = popen("command -v lsof 2>/dev/null", "r");
    if (!pipe) return false;
    std::array<char, 16> buffer{};
    char* result = fgets(buffer.data(), static_cast<int>(buffer.size()), pipe);
    pclose(pipe);
    return (result != nullptr);
}

static std::string lsof_command(int32_t pid) {
    if (pid <= 0) return {};
    return "lsof -n -P -p " + std::to_string(pid);
}

static void populate_lsof_buffer(const IView &view, tui::UiState &state) {
    const UiEvent *selected = nullptr;
    for (int32_t i = 0; i < view.count(); ++i) {
        if (view.top() + i == view.sel()) {
            selected = view.row(i);
            break;
        }
    }
    if (!selected || selected->pid <= 0) return;

    state.lsof_lines.clear();
    state.lsof_sel = 0;
    state.lsof_top = 0;

    std::string cmd = lsof_command(selected->pid) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return;

    std::array<char, 512> chunk{};
    while (fgets(chunk.data(), static_cast<int>(chunk.size()), pipe)) {
        std::string line(chunk.data());
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) state.lsof_lines.push_back(sanitize_terminal(line));
    }
    pclose(pipe);

    if (!state.lsof_lines.empty()) {
        state.overlay = tui::OverlayMode::InspectLsof;
        state.frame_cache.clear(); 
        std::fputs("\033[5;1H\033[J", stdout);
        std::fflush(stdout);
    }
}

// =============================================================================
// Local Command Processing Dispatchers
// =============================================================================

static bool already(const IFilterStore &filters, RuleKind kind, int32_t nr, const std::string &text) {
    for (const auto &r : filters.rules()) {
        if (r.kind != kind) continue;
        if (kind == RuleKind::SysId || kind == RuleKind::Pid) {
            if (r.nr == nr) return true;
        } else if (r.text == text) {
            return true;
        }
    }
    return false;
}

static void fill_pick_list(const IView &view, const IFilterStore &filters, tui::UiState &state) {
    state.seen_pids.clear();
    state.seen_procs.clear();
    state.seen_sysids.clear();
    state.seen_syscalls.clear();
    for (int32_t i = 0; i < view.count(); ++i) {
        const UiEvent *e = view.row(i);
        if (!e) continue;
        if (!already(filters, RuleKind::Pid, e->pid, {}) &&
            std::find(state.seen_pids.begin(), state.seen_pids.end(), e->pid) == state.seen_pids.end())
            state.seen_pids.push_back(e->pid);
        if (!e->comm.empty() && !already(filters, RuleKind::Process, 0, e->comm) &&
            std::find(state.seen_procs.begin(), state.seen_procs.end(), e->comm) == state.seen_procs.end())
            state.seen_procs.push_back(e->comm);
    }
    for (int32_t id = 0; id < syscall_count(); ++id) {
        if (!already(filters, RuleKind::SysId, id, {}))
            state.seen_sysids.push_back(id);
    }
    for (const auto &name : state.syscall_names) {
        if (!already(filters, RuleKind::Syscall, 0, name))
            state.seen_syscalls.push_back(name);
    }
}

static void handle_overlay_confirmation(IView &view, IFilterStore &filters, tui::UiState &state) {
    switch (state.overlay) {
        case tui::OverlayMode::SelectOrder:
            view.set_desc(state.overlay_sel == 1);
            break;
            
        case tui::OverlayMode::SelectSort: {
            std::array<Sort, 6> choices = {Sort::Time, Sort::Pid, Sort::Process, Sort::SysId, Sort::Syscall, Sort::Param};
            if (state.overlay_sel >= 0 && state.overlay_sel < static_cast<int32_t>(choices.size())) {
                view.set_sort(choices[static_cast<size_t>(state.overlay_sel)]);
            }
            break;
        }
        case tui::OverlayMode::EditRefreshMs:
            state.refresh_ms = tui::clamp_refresh_ms(state.refresh_edit);
            break;
            
        case tui::OverlayMode::MainFilterHub:
            if (state.overlay_sel == 0)      state.overlay = tui::OverlayMode::FilterKind;
            else if (state.overlay_sel == 1) state.overlay = tui::OverlayMode::RemoveFilter;
            else                             state.overlay = tui::OverlayMode::ShowFilters;
            state.overlay_sel = 0;
            return;

        case tui::OverlayMode::FilterKind:
            fill_pick_list(view, filters, state);
            if (state.overlay_sel == 0)      state.overlay = tui::OverlayMode::PickPid;
            else if (state.overlay_sel == 1) state.overlay = tui::OverlayMode::PickProcess;
            else if (state.overlay_sel == 2) state.overlay = tui::OverlayMode::PickSysId;
            else                             state.overlay = tui::OverlayMode::PickSyscall;
            state.overlay_sel = 0;
            state.overlay_top = 0;
            return;

        case tui::OverlayMode::PickProcess:
            if (state.overlay_sel >= 0 && state.overlay_sel < static_cast<int32_t>(state.seen_procs.size()))
                filters.add(FilterRule{RuleKind::Process, 0, state.seen_procs[static_cast<size_t>(state.overlay_sel)]});
            break;
        case tui::OverlayMode::PickSysId:
            if (state.overlay_sel >= 0 && state.overlay_sel < static_cast<int32_t>(state.seen_sysids.size())) {
                int32_t id = state.seen_sysids[static_cast<size_t>(state.overlay_sel)];
                filters.add(FilterRule{RuleKind::SysId, id, std::to_string(id)});
            }
            break;
        case tui::OverlayMode::PickSyscall:
            if (state.overlay_sel >= 0 && state.overlay_sel < static_cast<int32_t>(state.seen_syscalls.size()))
                filters.add(FilterRule{RuleKind::Syscall, 0, state.seen_syscalls[static_cast<size_t>(state.overlay_sel)]});
            break;
        case tui::OverlayMode::PickPid:
            if (state.overlay_sel >= 0 && state.overlay_sel < static_cast<int32_t>(state.seen_pids.size())) {
                int32_t pid = state.seen_pids[static_cast<size_t>(state.overlay_sel)];
                filters.add(FilterRule{RuleKind::Pid, pid, std::to_string(pid)});
            }
            break;
        case tui::OverlayMode::RemoveFilter:
            filters.remove_at(state.overlay_sel);
            break;
            
        default:
            break;
    }
    clear_overlay_area(view, state);
}

static void handle_active_overlay_input(int key, IView &view, IFilterStore &filters, tui::UiState &state) {
    if (key == 0x1b) {
        if (state.overlay == tui::OverlayMode::PickProcess ||
            state.overlay == tui::OverlayMode::PickSysId ||
            state.overlay == tui::OverlayMode::PickSyscall ||
            state.overlay == tui::OverlayMode::PickPid) {
            state.overlay = tui::OverlayMode::FilterKind;
            state.overlay_sel = 0;
            return;
        }
        if (state.overlay == tui::OverlayMode::FilterKind) {
            state.overlay = tui::OverlayMode::MainFilterHub;
            state.overlay_sel = 0;
            return;
        }
        clear_overlay_area(view, state);
        return;
    }

    // Full-Screen Lsof Scrollable List Pipeline Logic
    if (state.overlay == tui::OverlayMode::InspectLsof) {
        struct winsize w{};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        const int32_t visible_rows = (w.ws_row > 5) ? (w.ws_row - 5) : 3;
        const int32_t total_items = static_cast<int32_t>(state.lsof_lines.size());

        switch (key) {
            case static_cast<int>(Key::Up):   
                if (state.lsof_sel > 0) state.lsof_sel--; 
                break;
            case static_cast<int>(Key::Down): 
                if (state.lsof_sel < total_items - 1) state.lsof_sel++; 
                break;
            case static_cast<int>(Key::Home): 
                state.lsof_sel = 0; 
                break;
            case static_cast<int>(Key::End):  
                state.lsof_sel = total_items > 0 ? total_items - 1 : 0; 
                break;
            case static_cast<int>(Key::PgUp): 
                state.lsof_sel = (state.lsof_sel - visible_rows > 0) ? state.lsof_sel - visible_rows : 0; 
                break;
            case static_cast<int>(Key::PgDn): 
                state.lsof_sel = (state.lsof_sel + visible_rows < total_items) ? state.lsof_sel + visible_rows : (total_items > 0 ? total_items - 1 : 0); 
                break;
            default: 
                break;
        }

        // Maintain view scroll viewport clamping limits safely
        if (state.lsof_sel < state.lsof_top) {
            state.lsof_top = state.lsof_sel;
        }
        if (state.lsof_sel >= state.lsof_top + visible_rows) {
            state.lsof_top = state.lsof_sel - visible_rows + 1;
        }
        return;
    }

    // Refresh Scale Rate Rate Up/Down 100ms Jump Arithmetic Hooks
    if (state.overlay == tui::OverlayMode::EditRefreshMs) {
        if (key == static_cast<int>(Key::Up)) {
            if (state.refresh_edit <= 9900) {
                state.refresh_edit += 100;
            }
            return;
        }
        if (key == static_cast<int>(Key::Down)) {
            if (state.refresh_edit >= 200) {
                state.refresh_edit -= 100;
            }
            return;
        }
    }
    
    // Standard Right-Side Popups Interface Controls
    int32_t total = 0;
    if (state.overlay == tui::OverlayMode::PickProcess) total = static_cast<int32_t>(state.seen_procs.size());
    else if (state.overlay == tui::OverlayMode::PickPid) total = static_cast<int32_t>(state.seen_pids.size());
    else if (state.overlay == tui::OverlayMode::PickSysId) total = static_cast<int32_t>(state.seen_sysids.size());
    else if (state.overlay == tui::OverlayMode::PickSyscall) total = static_cast<int32_t>(state.seen_syscalls.size());
    else if (state.overlay == tui::OverlayMode::RemoveFilter || state.overlay == tui::OverlayMode::ShowFilters)
        total = static_cast<int32_t>(filters.rules().size());
    struct winsize w{};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int32_t page = (w.ws_row > 7) ? (w.ws_row - 7) : 4;

    switch (key) {
        case static_cast<int>(Key::Up):
            if (state.overlay_sel > 0) state.overlay_sel--;
            break;
        case static_cast<int>(Key::Down):
            if (total == 0 || state.overlay_sel < total - 1) state.overlay_sel++;
            break;
        case static_cast<int>(Key::Home):
            state.overlay_sel = 0;
            break;
        case static_cast<int>(Key::End):
            state.overlay_sel = total > 0 ? total - 1 : 0;
            break;
        case static_cast<int>(Key::PgUp):
            state.overlay_sel = state.overlay_sel > page ? state.overlay_sel - page : 0;
            break;
        case static_cast<int>(Key::PgDn):
            state.overlay_sel = (total > 0 && state.overlay_sel + page < total)
                ? state.overlay_sel + page : (total > 0 ? total - 1 : 0);
            break;
        case '\n':
        case '\r':
            handle_overlay_confirmation(view, filters, state);
            break;
            
        default:
            // Maintain raw numeric text entry append arrays capabilities for the refresh utility box
            if (state.overlay == tui::OverlayMode::EditRefreshMs) {
                if (key >= '0' && key <= '9') {
                    if (state.refresh_edit >= 10000) state.refresh_edit = 0;
                    state.refresh_edit = (state.refresh_edit * 10) + (key - '0');
                } else if (key == 127 || key == 8) { // 127 represents standard backspace character
                    state.refresh_edit /= 10;
                }
            }
            break;
    }
}

using DashFn = std::function<void(IView &, Capture &, tui::UiState &)>;

static const std::map<int, DashFn> kDashMap = {
    {'f', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::MainFilterHub; state.overlay_sel = 0; }},
    {'F', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::MainFilterHub; state.overlay_sel = 0; }},
    {'s', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::SelectSort; state.overlay_sel = 0; }},
    {'S', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::SelectSort; state.overlay_sel = 0; }},
    {'o', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::SelectOrder; state.overlay_sel = 0; }},
    {'O', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::SelectOrder; state.overlay_sel = 0; }},
    {'r', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::EditRefreshMs; state.refresh_edit = state.refresh_ms; state.overlay_sel = 0; }},
    {'R', [](IView &, Capture &, tui::UiState &state) { state.overlay = tui::OverlayMode::EditRefreshMs; state.refresh_edit = state.refresh_ms; state.overlay_sel = 0; }},
    {'l', [](IView &view, Capture &, tui::UiState &state) { if (state.has_lsof) populate_lsof_buffer(view, state); }},
    {'L', [](IView &view, Capture &, tui::UiState &state) { if (state.has_lsof) populate_lsof_buffer(view, state); }},
    {' ', [](IView &view, Capture &, tui::UiState &) { view.on_key(Key::Pause); }},
    {'c', [](IView &, Capture &cap, tui::UiState &state) { cap.toggle(); state.is_capturing = cap.active(); }},
    {'C', [](IView &, Capture &cap, tui::UiState &state) { cap.toggle(); state.is_capturing = cap.active(); }}
};

static void handle_standard_dashboard_input(int key, IView &view, Capture &cap, tui::UiState &state) {
    auto it = kDashMap.find(key);
    if (it != kDashMap.end()) { it->second(view, cap, state); return; }
    view.on_key(static_cast<Key>(key));
}

// Both sources hand the main loop a pollable fd that yields whole 176-byte
// records. Everything after read() runs on the main thread.
class EventSource {
public:
    virtual ~EventSource() = default;
    virtual int fd() const = 0;
    void ingest(EventLoopContext &ctx) {
        const int src = fd();
        if (src < 0) return;
        ssize_t n = ::read(src, buf_.data(), buf_.size());
        if (n > 0)
            decode_events(std::span<const uint8_t>(buf_.data(), static_cast<size_t>(n)),
                          on_raw_kernel_event, &ctx);
    }
private:
    // Allocated once, sized for a full ring, reused for every read.
    std::vector<uint8_t> buf_ = std::vector<uint8_t>(SM_EVENT_RING_SIZE * SM_EVENT_SIZE);
};

class DeviceSource final : public EventSource {
public:
    explicit DeviceSource(Fd &dev) : dev_(dev) {}
    int fd() const override { return dev_.get(); }
private:
    Fd &dev_;
};

// The generator thread only writes records into a pipe; see mock_generator.hpp.
class SimulatorSource final : public EventSource {
public:
    SimulatorSource() { mock_.start(); }
    int fd() const override { return mock_.fd(); }
private:
    MockEventGenerator mock_;
};

static int32_t parse_args(int argc, char **argv) {
    int32_t fpid = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            fpid = static_cast<int32_t>(std::strtol(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "-h") == 0)
            std::exit(2);
    }
    return fpid;
}

// open_device returns the descriptor, or the errno. An empty unique_ptr
// used to mean both "failed" and "use the simulator". expected makes the
// failure explicit; the caller still falls back to the simulator.
static std::expected<std::unique_ptr<Fd>, int> open_device(int32_t fpid) {
    auto dev = std::make_unique<Fd>(::open("/dev/syscall_monitor", O_RDONLY | O_NONBLOCK));
    if (dev->get() < 0) {
        int err = errno;
        std::cout << tui::ansi::warn_yellow << "Cannot open /dev/syscall_monitor: "
                  << std::strerror(err) << tui::ansi::style_reset << "\n";
        if (err == EPERM || err == EACCES)
            std::cout << "The device needs root, CAP_PERFMON or CAP_SYS_ADMIN "
                         "(sudo ./build/syswatch).\n";
        std::cout << tui::ansi::info_green << "Simulator mode in 2 seconds..."
                  << tui::ansi::style_reset << "\n";
        ::sleep(2);
        return std::unexpected(err);
    }
    sm_s32 self = static_cast<sm_s32>(::getpid());
    ioctl(dev->get(), SET_EXCLUDE_PID, &self);
    if (fpid) {
        sm_s32 id = fpid;
        ioctl(dev->get(), SET_FILTER_PID, &id);
    }
    return dev;
}

static void load_syscall_names(tui::UiState &state) {
    for (const SyscallName &s : syscall_table())
        state.syscall_names.emplace_back(s.name);
}

static int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

class App {
public:
    int run(int argc, char **argv) {
        state_.has_lsof = check_lsof_presence();
        fpid_ = parse_args(argc, argv);
        auto opened = open_device(fpid_);
        if (opened) dev_ = std::move(*opened);
        filters_ = std::make_unique<FilterStore>("filters.xml");
        load_syscall_names(state_);
        view_ = std::make_unique<View>(20);
        term_ = std::make_unique<Terminal>();
        ctx_ = EventLoopContext{filters_.get(), &cap_, view_.get()};
        if (dev_) source_ = std::make_unique<DeviceSource>(*dev_);
        else      source_ = std::make_unique<SimulatorSource>();

        tui::render_dashboard(*view_, *filters_, fpid_, state_);
        view_->mark_drawn();
        int64_t last_draw = now_ms();

        while (true) {
            pollfd pf[2]{};
            nfds_t nfd = 1;
            pf[0].fd = STDIN_FILENO;
            pf[0].events = POLLIN;
            int src = source_->fd();
            if (src >= 0) { pf[1].fd = src; pf[1].events = POLLIN; nfd = 2; }
            poll(pf, nfd, 10);

            if (pf[0].revents & POLLIN) {
                int key = read_key();
                if (key == 'q' || key == 'Q') break;
                if (state_.overlay != tui::OverlayMode::None)
                    handle_active_overlay_input(key, *view_, *filters_, state_);
                else
                    handle_standard_dashboard_input(key, *view_, cap_, state_);
                tui::render_dashboard(*view_, *filters_, fpid_, state_);
                view_->mark_drawn();
                last_draw = now_ms();
                continue;
            }
            if (src >= 0 && (pf[1].revents & POLLIN))
                source_->ingest(ctx_);
            int64_t now = now_ms();
            if (view_->dirty() && now - last_draw >= state_.refresh_ms) {
                tui::render_dashboard(*view_, *filters_, fpid_, state_);
                view_->mark_drawn();
                last_draw = now;
            }
        }
        // Stop the simulator thread (if any) while everything it could
        // reference is still alive.
        source_.reset();
        return 0;
    }
private:
    tui::UiState state_;
    int32_t fpid_ = 0;
    std::unique_ptr<Fd> dev_;
    std::unique_ptr<FilterStore> filters_;
    std::unique_ptr<View> view_;
    std::unique_ptr<Terminal> term_;
    Capture cap_;
    EventLoopContext ctx_{};
    // Declared last so it is destroyed first.
    std::unique_ptr<EventSource> source_;
};

int main(int argc, char **argv) {
    return App{}.run(argc, argv);
}
