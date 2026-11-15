#pragma once
#include "types.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

std::string quote_pipe(std::string_view text);
std::string sanitize_host(std::string_view host);

class Capture {
public:
    bool toggle();
    bool active() const { return static_cast<bool>(file_); }
    void write(const UiEvent &e);
private:
    struct Close { void operator()(FILE *f) const { if (f) std::fclose(f); } };
    std::unique_ptr<FILE, Close> file_;
};
