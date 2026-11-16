#include "filters.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdint>

/*
 * filters.cpp
 * Purpose: hide rules and their filters.xml file.
 *
 * A rule is <hide kind="process|pid|sysid|syscall">value</hide>. Values are
 * XML-escaped on save and unescaped on load. Process rules match comm
 * exactly; a trailing '*' makes the rule a prefix match. The kernel keeps
 * only the first 15 bytes of comm, so longer rule names are compared on
 * those 15 bytes ("gnome-terminal-server" matches comm "gnome-terminal-").
 */

namespace {

constexpr size_t kCommMax = 15;      // TASK_COMM_LEN - 1
constexpr size_t kValueMax = 31;

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

bool is_numeric_kind(RuleKind k) { return k == RuleKind::SysId || k == RuleKind::Pid; }

// Whole-string decimal parse; "12abc", "" and out-of-range are rejected.
bool parse_int(std::string_view s, int32_t &out) {
    if (s.empty()) return false;
    const char *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc() && ptr == end;
}

std::string xml_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string xml_unescape(std::string_view s) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool matched = false;
        if (s[i] == '&') {
            for (const auto &[ent, ch] : kEntities) {
                if (s.substr(i, ent.size()) == ent) {
                    out.push_back(ch);
                    i += ent.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) out.push_back(s[i++]);
    }
    return out;
}

// Drops <!-- ... --> so commented-out rules are not parsed.
std::string strip_comments(std::string_view xml) {
    std::string out;
    size_t pos = 0;
    while (pos < xml.size()) {
        auto open = xml.find("<!--", pos);
        if (open == std::string_view::npos) { out.append(xml.substr(pos)); break; }
        out.append(xml.substr(pos, open - pos));
        auto close = xml.find("-->", open + 4);
        if (close == std::string_view::npos) break;
        pos = close + 3;
    }
    return out;
}

bool process_matches(std::string_view comm, std::string_view rule) {
    if (!rule.empty() && rule.back() == '*') {
        rule.remove_suffix(1);
        if (rule.size() > kCommMax) rule = rule.substr(0, kCommMax);
        return !rule.empty() && comm.size() >= rule.size() && comm.compare(0, rule.size(), rule) == 0;
    }
    if (rule.size() > kCommMax) rule = rule.substr(0, kCommMax);
    return comm == rule;
}
}

std::vector<FilterRule> FilterStore::seed() {
    std::vector<FilterRule> out;
    out.reserve(5);
    for (const char *p : {"gnome-terminal*", "gnome-shell*", "Xorg", "vmtoolsd*"}) {
        out.push_back(FilterRule{RuleKind::Process, 0, p});
    }
    out.push_back(FilterRule{RuleKind::SysId, 0, "0"});
    return out;
}

std::vector<FilterRule> FilterStore::parse(std::string_view input, int cap) {
    std::vector<FilterRule> out;
    const std::string xml = strip_comments(input);
    const std::string_view x(xml);
    size_t pos = 0;

    while (static_cast<int>(out.size()) < cap) {
        auto at = x.find("<hide", pos);
        if (at == std::string_view::npos) break;
        const size_t after = at + 5;
        // "<hide" must be the whole tag name: not <hidden>, <hideX>, ...
        if (after >= x.size() || (x[after] != ' ' && x[after] != '\t' && x[after] != '\n' &&
                                  x[after] != '\r' && x[after] != '>')) {
            pos = after;
            continue;
        }
        auto gt = x.find('>', after);
        if (gt == std::string_view::npos) break;
        auto vend = x.find("</hide>", gt);
        if (vend == std::string_view::npos) break;
        pos = vend + 7;

        // The kind attribute must sit inside this start tag.
        const std::string_view tag = x.substr(after, gt - after);
        auto attr = tag.find("kind=\"");
        if (attr == std::string_view::npos) continue;
        auto k_start = attr + 6;
        auto k_end = tag.find('"', k_start);
        if (k_end == std::string_view::npos) continue;
        const RuleKind k = kind_of(tag.substr(k_start, k_end - k_start));
        if (static_cast<int>(k) == 0) continue;

        std::string value = xml_unescape(x.substr(gt + 1, vend - gt - 1));
        if (auto nul = value.find('\0'); nul != std::string::npos) value.resize(nul);
        if (value.size() > kValueMax) value.resize(kValueMax);

        FilterRule r;
        r.kind = k;
        if (is_numeric_kind(k)) {
            if (!parse_int(value, r.nr)) continue;     // "12abc" is not pid 12
            r.text = std::to_string(r.nr);
        } else {
            // An empty process/syscall rule would hide everything.
            if (value.empty() || value == "*") continue;
            r.text = std::move(value);
        }
        out.push_back(std::move(r));
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

bool FilterStore::hidden(std::string_view comm, int32_t pid, int32_t nr, std::string_view name) const {
    for (const auto &r : rules_) {
        if (r.kind == RuleKind::Process && process_matches(comm, r.text)) return true;
        if (r.kind == RuleKind::SysId   && nr == r.nr)                    return true;
        if (r.kind == RuleKind::Syscall && name == r.text)                return true;
        if (r.kind == RuleKind::Pid     && pid == r.nr)                   return true;
    }
    return false;
}

bool FilterStore::add(FilterRule rule) {
    if (rules_.size() >= 32) return false;
    if ((rule.kind == RuleKind::Syscall || rule.kind == RuleKind::Process) &&
        (rule.text.empty() || rule.text == "*"))
        return false;
    for (const auto &r : rules_) {
        if (r.kind != rule.kind) continue;
        if (is_numeric_kind(rule.kind)) {
            if (r.nr == rule.nr) return false;
        } else if (r.text == rule.text) {
            return false;
        }
    }
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
        const std::string value = is_numeric_kind(r.kind) ? std::to_string(r.nr) : xml_escape(r.text);
        out << "  <hide kind=\"" << kind_name(r.kind) << "\">" << value << "</hide>\n";
    }
    out << "</filters>\n";
    out.flush();
    return out.good();
}
