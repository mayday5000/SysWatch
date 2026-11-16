#include "capture.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

std::string quote_pipe(std::string_view text) {
    bool quote = false;
    size_t clean = 0;
    for (char c : text) {
        if (c == '\0') continue;
        clean++;
        if (c == '|' || c == '"' || c == '\r' || c == '\n') quote = true;
    }
    if (clean == 0) return {};
    std::string out;
    out.reserve(clean + (quote ? 2 : 0));
    if (quote) out.push_back('"');
    for (char c : text) {
        if (c == '\0') continue;
        if (c == '"' && quote) out.push_back('"');
        out.push_back(c);
    }
    if (quote) out.push_back('"');
    return out;
}

std::string sanitize_host(std::string_view host) {
    std::string out;
    out.reserve(host.size());
    for (char c : host) {
        unsigned char uc = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(uc) || c == '-' || c == '_' ? c : '_');
    }
    return out.empty() ? std::string("host") : out;
}

bool Capture::toggle() {
    if (file_) { file_.reset(); return false; }
    char host[64] = "host";
    if (gethostname(host, sizeof(host) - 1) != 0)
        std::snprintf(host, sizeof(host), "host");
    auto now = std::time(nullptr);
    std::tm tm{};
    char name[160];
    if (!localtime_r(&now, &tm))
        std::snprintf(name, sizeof(name), "strace_%s.log", sanitize_host(host).c_str());
    else
        std::snprintf(name, sizeof(name), "strace_%s_%04d%02d%02d_%02d%02d%02d.log",
                      sanitize_host(host).c_str(), tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    file_.reset(std::fopen(name, "w"));
    if (file_) std::fputs("Time|PID|Process|SysID|Syscall|Parameters\n", file_.get());
    return static_cast<bool>(file_);
}

void Capture::write(const UiEvent &e) {
    if (!file_) return;
    std::fprintf(file_.get(), "%s|%d|%s|%d|%s|%s\n",
                 quote_pipe(std::to_string(e.ts_ns)).c_str(), e.pid,
                 quote_pipe(e.comm).c_str(), e.nr,
                 quote_pipe(e.sys_name).c_str(), quote_pipe(e.extra).c_str());
}
