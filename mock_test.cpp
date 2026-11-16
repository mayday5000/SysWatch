// Unit checks that need no kernel: the wire ABI, the real record decoder,
// filters.xml parsing/matching, terminal sanitizing, the generated syscall
// table, the capture quoting and the view's sort order. Run with ctest.
#include "capture.hpp"
#include "decode.hpp"
#include "filters.hpp"
#include "syscall_monitor_shared.h"
#include "syscall_table.hpp"
#include "text_util.hpp"
#include "view.hpp"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

static int count_cb(const sm_event &, void *ud) {
    *static_cast<int *>(ud) += 1;
    return 0;
}

int main() {
    int fail = 0;
    auto check = [&](bool ok, const char *name) {
        std::cout << (ok ? "ok   " : "FAIL ") << name << "\n";
        if (!ok) fail++;
    };

    // ABI
    check(sizeof(sm_event) == 176, "event is 176 bytes");
    check(offsetof(sm_event, ts_ns) == 8 && offsetof(sm_event, arg_str) == 48, "field offsets");
    check((SM_EVENT_RING_SIZE & (SM_EVENT_RING_SIZE - 1)) == 0, "ring is a power of two");
    check((SM_NR_COMPAT & 0xFFFF) == 0, "compat flag does not overlap syscall numbers");

    // Decoder (the one main.cpp uses)
    std::vector<uint8_t> three(3 * SM_EVENT_SIZE, 0);
    int n = 0;
    check(decode_events(three, count_cb, &n) && n == 3, "decodes three records");
    std::vector<uint8_t> bad(SM_EVENT_SIZE - 1, 0);
    n = 0;
    check(!decode_events(bad, count_cb, &n) && n == 0, "rejects a partial record");
    check(!decode_events(std::span<const uint8_t>(), count_cb, &n), "rejects an empty buffer");
    std::vector<uint8_t> unterminated(SM_EVENT_SIZE, 'A');
    std::string seen;
    auto grab = [](const sm_event &ev, void *ud) -> int {
        *static_cast<std::string *>(ud) = std::string(ev.comm);
        return 0;
    };
    check(decode_events(unterminated, grab, &seen) && seen.size() == SM_COMM_LEN - 1,
          "unterminated comm is cut at 15 bytes");

    // Filters
    auto rules = FilterStore::parse("<hide kind=\"sysid\">0</hide><hide kind=\"process\">nginx</hide>");
    check(rules.size() == 2, "parse two rules");
    check(rules.size() == 2 && rules[0].kind == RuleKind::SysId && rules[0].nr == 0, "sysid 0");
    check(rules.size() == 2 && rules[1].kind == RuleKind::Process && rules[1].text == "nginx", "process nginx");
    check(FilterStore::parse("<hide kind=\"process\"></hide>").empty(), "empty process rule is skipped");
    check(FilterStore::parse("<hide kind=\"pid\">12abc</hide>").empty(), "non-numeric pid is skipped");
    check(FilterStore::parse("<hidden kind=\"pid\">5</hidden>").empty(), "<hidden> is not <hide>");
    check(FilterStore::parse("<!-- <hide kind=\"pid\">7</hide> -->").empty(), "comments are ignored");
    auto esc = FilterStore::parse("<hide kind=\"process\">a&lt;b&amp;c</hide>");
    check(esc.size() == 1 && esc[0].text == "a<b&c", "entities are unescaped");

    const char *path = "/tmp/syswatch_mock_test_filters.xml";
    std::remove(path);
    {
        FilterStore f(path);
        while (!f.rules().empty()) f.remove_at(0);
        f.add(FilterRule{RuleKind::Process, 0, "x</hide><hide kind=\"pid\">1"});
        f.add(FilterRule{RuleKind::Process, 0, "node"});
        f.add(FilterRule{RuleKind::Process, 0, "gnome-terminal-server"});
        f.add(FilterRule{RuleKind::Process, 0, "kw*"});
        f.add(FilterRule{RuleKind::Syscall, 0, "openat"});
        check(!f.add(FilterRule{RuleKind::Process, 0, ""}), "add rejects an empty process rule");
    }
    {
        FilterStore g(path);
        check(g.rules().size() == 5 && g.rules()[0].text == "x</hide><hide kind=\"pid\">1",
              "save/load round-trips markup in a name");
        check(g.hidden("node", 1, 1, "x") && !g.hidden("nodejs", 1, 1, "x"), "process rules match exactly");
        check(g.hidden("gnome-terminal-", 1, 1, "x"), "long rule names match the 15-byte comm");
        check(g.hidden("kwin_x11", 1, 1, "x"), "trailing * is a prefix match");
        check(g.hidden("bash", 1, SM_NR_OPENAT, syscall_name(SM_NR_OPENAT)), "syscall-name rule matches");
        check(!g.hidden("bash", 1, 1, syscall_name(1)), "other syscalls pass");
    }
    std::remove(path);

    // Syscall table (generated for this architecture)
    check(syscall_name(SM_NR_OPENAT) == "openat", "openat number resolves to its name");
    check(syscall_name(SM_NR_EXECVE) == "execve", "execve number resolves to its name");
    check(syscall_name(99999) == "sys_99999", "unknown number falls back to sys_<nr>");
    check(syscall_count() > SM_NR_OPENAT, "table covers openat");

    // Terminal sanitizing
    check(sanitize_terminal("/etc/passwd") == "/etc/passwd", "plain text unchanged");
    check(sanitize_terminal("a\x1b]0;x\x07" "b") == "a?]0;x?b", "ESC and BEL replaced");
    check(sanitize_terminal("\xc2\x9b" "31m") == "??31m", "C1 CSI replaced");
    check(sanitize_terminal("caf\xc3\xa9") == "caf\xc3\xa9", "valid UTF-8 kept");
    check(sanitize_terminal("\xff\xfe") == "??", "invalid bytes replaced");

    // Capture quoting
    check(quote_pipe("a|b") == "\"a|b\"", "pipe is quoted");
    check(quote_pipe("say \"hi\"") == "\"say \"\"hi\"\"\"", "quotes are doubled");

    // View: lazily sorted, newest first by default
    View v(10);
    for (uint64_t t : {3u, 1u, 2u}) {
        UiEvent e;
        e.ts_ns = t;
        v.append(e);
    }
    check(v.count() == 3 && v.row(0)->ts_ns == 3 && v.row(2)->ts_ns == 1, "view sorts by time, descending");
    v.set_desc(false);
    check(v.row(0)->ts_ns == 1, "view re-sorts after set_desc");

    std::cout << (fail ? "FAILED" : "PASSED") << " (" << fail << " failures)\n";
    return fail ? 1 : 0;
}
