#include "filters.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdint>

namespace {

bool starts(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

RuleKind kind_of(std::string_view s) {
    if (s == "process") return RuleKind::Process;
    if (s == "sysid")   return RuleKind::SysId;
    if (s == "syscall") return RuleKind::Syscall;
    if (s == "pid")     return RuleKind::Pid;
    return static_cast<RuleKind>(0);
}

const char *kind_name(RuleKind k) {
    switch (k) {
        case RuleKind::Process: return "process";
        case RuleKind::SysId:   return "sysid";
        case RuleKind::Syscall: return "syscall";
        case RuleKind::Pid:     return "pid";
        default:                return "process";
    }
}
}

bool FilterStore::default_hidden(std::string_view comm, int32_t nr) {
    if (nr == 0) return true;
    auto starts = [](std::string_view s, std::string_view p) {
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    };
    return starts(comm, "gnome-terminal") || starts(comm, "gnome-shell") ||
           comm == "Xorg" || starts(comm, "vmtoolsd");
}

std::vector<FilterRule> FilterStore::seed() {
    std::vector<FilterRule> out;
    out.reserve(5); // Pre-allocate initial boundary size
    for (const char *p : {"gnome-terminal", "gnome-shell", "Xorg", "vmtoolsd"}) {
        out.push_back(FilterRule{RuleKind::Process, 0, p});
    }
    out.push_back(FilterRule{RuleKind::SysId, 0, "0"});
    return out;
}

std::vector<FilterRule> FilterStore::parse(std::string_view xml, int cap) {
    std::vector<FilterRule> out;
    size_t pos = 0;

    while (static_cast<int>(out.size()) < cap) {
        auto at = xml.find("<hide", pos);
        if (at == std::string_view::npos) break;

        // Targeted hunting: Hunt directly for the kind attribute token boundaries
        auto attr = xml.find("kind=\"", at);
        if (attr == std::string_view::npos) break;
        
        auto k_start = attr + 6;
        auto k_end = xml.find('"', k_start);
        if (k_end == std::string_view::npos) break;

        auto vend = xml.find("</hide>", k_end);
        auto gt = xml.find('>', k_end);
        if (vend == std::string_view::npos || gt == std::string_view::npos || gt > vend) break;

        auto kind_str = xml.substr(k_start, k_end - k_start);
        auto val_str = xml.substr(gt + 1, vend - gt - 1);

        // Sanitize string contents efficiently without breaking bounds
        if (val_str.size() > 31) val_str = val_str.substr(0, 31);
        if (auto null_idx = val_str.find('\0'); null_idx != std::string_view::npos) {
            val_str = val_str.substr(0, null_idx);
        }

        RuleKind k = kind_of(kind_str);
        if (static_cast<int>(k) != 0) {
            FilterRule r;
            r.kind = k;
            r.text = std::string(val_str);
            try { 
                r.nr = std::stoi(r.text); 
            } catch (...) { 
                r.nr = 0; 
            }
            out.push_back(std::move(r));
        }
        pos = vend + 7;
    }
    return out;
}

FilterStore::FilterStore(std::string path) : path_(std::move(path)) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path_, ec)) {
        rules_ = seed();
        save();
        return;
    }
    std::ifstream in(path_);
    std::stringstream buf;
    buf << in.rdbuf();
    rules_ = parse(buf.str());
}

// Aligned types explicitly to 32-bit width ABI specification targets
bool FilterStore::hidden(std::string_view comm, int32_t pid, int32_t nr, std::string_view name) const {
    if (rules_.empty()) return false;
    
    for (const auto &r : rules_) {
        if (r.kind == RuleKind::Process && starts(comm, r.text)) return true;
        if (r.kind == RuleKind::SysId   && nr == r.nr)           return true;
        if (r.kind == RuleKind::Syscall && name == r.text)       return true;
        if (r.kind == RuleKind::Pid     && pid == r.nr)          return true;
    }
    return false;
}

bool FilterStore::add(FilterRule rule) {
    if (rules_.size() >= 32) return false;
    for (const auto &r : rules_) {
        if (r.kind != rule.kind) continue;
        if (rule.kind == RuleKind::SysId || rule.kind == RuleKind::Pid) {
            if (r.nr == rule.nr) return false;
        } else if (!rule.text.empty() && r.text == rule.text) {
            return false;
        }
    }
    if ((rule.kind == RuleKind::Syscall || rule.kind == RuleKind::Process) && rule.text.empty())
        return false;
    rules_.push_back(std::move(rule));
    return save();
}

bool FilterStore::remove_at(int index) {
    if (index < 0 || index >= static_cast<int>(rules_.size())) return false;
    
    rules_.erase(rules_.begin() + index);
    return save();
}

bool FilterStore::save() const {
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return false;
    
    out << "<?xml version=\"1.0\"?>\n<filters>\n";
    for (const auto &r : rules_) {
        const std::string value = (r.kind == RuleKind::SysId || r.kind == RuleKind::Pid)
            ? std::to_string(r.nr) : r.text;
        out << "  <hide kind=\"" << kind_name(r.kind) << "\">" << value << "</hide>\n";
    }
    out << "</filters>\n";
    return true;
}
