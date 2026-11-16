#include "renderer.hpp"

namespace tui {
int32_t clamp_refresh_ms(int32_t ms) {
    if (ms < 100) return 100;
    if (ms > 10000) return 10000;
    return ms;
}

bool refresh_due(int64_t now_ms, int64_t last_ms, bool dirty, int64_t interval_ms) {
    if (!dirty) return false;
    if (last_ms == 0) return true;
    return (now_ms - last_ms) >= interval_ms;
}
}


#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <array>

namespace tui {

void move_cursor(int32_t row, int32_t col) {
    std::printf("\033[%d;%dH", row, col);
}

void clear_line() {
    std::fwrite(ansi::clear_line.data(), 1, ansi::clear_line.size(), stdout);
}

void style_highlight() {
    std::fwrite(ansi::style_highlight.data(), 1, ansi::style_highlight.size(), stdout);
}

void style_reset() {
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
}

static const char* get_sort_name(Sort s) {
    switch (s) {
        case Sort::Pid:     return "PID";
        case Sort::Process: return "PROCESS";
        case Sort::SysId:   return "SYSID";
        case Sort::Syscall: return "SYSCALL";
        case Sort::Param:   return "PARAMETERS";
        default:            return "TIME";
    }
}

static void render_top_title_panel(const IView &view, int32_t screen_cols, const UiState &state) {
    std::string clock_str = "00:00:00";
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    if (localtime_r(&now, &tm_buf)) {
        clock_str.resize(16);
        size_t len = std::strftime(clock_str.data(), clock_str.size(), "%H:%M:%S", &tm_buf);
        clock_str.resize(len);
    }

    move_cursor(1, 1);
    std::fwrite(ansi::bg_blue_white_bold.data(), 1, ansi::bg_blue_white_bold.size(), stdout);
    
    static constexpr std::string_view title = "  Syscall Monitor v" SYSWATCH_VERSION " │ ";
    std::fwrite(title.data(), 1, title.size(), stdout);
    std::fputs(clock_str.c_str(), stdout);
    std::fputs(" │ ", stdout);

    // "│" is three bytes but one column wide; three spare columns as before.
    int32_t used = static_cast<int32_t>(title.size()) - 2 + 3 + 3
                 + static_cast<int32_t>(clock_str.length());

    if (state.overlay == OverlayMode::InspectLsof) {
        std::fputs("INSPECTING OPEN FILES (lsof)", stdout);
        used += 28;
    } else {
        std::fputs(get_sort_name(view.sort()), stdout);
        std::fputs(view.desc() ? " DESC" : " ASC", stdout);
        used += static_cast<int32_t>(std::strlen(get_sort_name(view.sort()))) + 5;

        if (view.paused()) {
			std::fputs(" │", stdout);
            std::fwrite(ansi::text_orange_bold.data(), 1, ansi::text_orange_bold.size(), stdout);
            std::fputs(" PAUSED", stdout);
            std::fwrite(ansi::bg_blue_white_bold.data(), 1, ansi::bg_blue_white_bold.size(), stdout);
            used += 9;
        }
        
        int32_t written = std::snprintf(nullptr, 0, " │ %d ms", state.refresh_ms);
        std::string refresh_str;
        refresh_str.resize(static_cast<size_t>(written));
        std::snprintf(refresh_str.data(), refresh_str.size() + 1, " │ %d ms", state.refresh_ms);
        std::fputs(refresh_str.c_str(), stdout);
        used += written;
    }

    if (state.overlay != OverlayMode::InspectLsof && state.is_capturing) {
		std::fputs(" │", stdout);		
        std::fwrite(ansi::bg_orange_white_bold.data(), 1, ansi::bg_orange_white_bold.size(), stdout);
        std::fputs(" Capturing...              ", stdout);
        std::fwrite(ansi::bg_blue_white_bold.data(), 1, ansi::bg_blue_white_bold.size(), stdout);
        used += 15;
    }
    
    if ((screen_cols + 2) > used) {
        std::string padding(static_cast<size_t>((screen_cols + 2) - used), ' ');
        std::fputs(padding.c_str(), stdout);
    }
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
    std::putchar('\n');
}

static void render_applied_filters_banner(const IFilterStore &filters, int32_t fpid, const UiState &state) {
    move_cursor(2, 1);
    clear_line();
    if (state.overlay == OverlayMode::InspectLsof) {
        std::fwrite(ansi::text_cyan_bold.data(), 1, ansi::text_cyan_bold.size(), stdout);
        std::fputs("  Output Rows Tracking Metrics Table View Panel:", stdout);
        std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
        return;
    }

    const auto &rules_vec = filters.rules();
    std::fwrite(ansi::text_yellow_bold.data(), 1, ansi::text_yellow_bold.size(), stdout);
    std::printf("  Hide(%zu)", rules_vec.size());
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
    
    for (size_t i = 0; i < rules_vec.size() && i < 4; ++i) {
        if (rules_vec[i].kind == RuleKind::Process)
            std::printf(" │ PROC: %s", rules_vec[i].text.c_str());
        else if (rules_vec[i].kind == RuleKind::Syscall)
            std::printf(" │ SYS: %s", rules_vec[i].text.c_str());
        else if (rules_vec[i].kind == RuleKind::Pid)
            std::printf(" │ PID: %d", rules_vec[i].nr);
        else
            std::printf(" │ ID: %d", rules_vec[i].nr);
    }
    if (fpid) {
        std::printf(" │ PID %d", fpid);
    }
}

static void render_table_headers(int32_t screen_cols, const UiState &state) {
    move_cursor(3, 1);
    clear_line();
    if (state.overlay == OverlayMode::InspectLsof) {
        std::fwrite(ansi::text_bold.data(), 1, ansi::text_bold.size(), stdout);
        std::fputs("   COMMAND     PID     USER   FD      TYPE     DEVICE SIZE/OFF       NODE NAME", stdout);
        std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
        
        move_cursor(4, 1);
        clear_line();
        std::string divider;
        divider.reserve(static_cast<size_t>(screen_cols) * 3);
        for (int32_t col = 0; col < screen_cols; ++col) {
            divider += "─"; 
        }
        std::printf("\033[38;5;51m%s\033[0m", divider.c_str());
        return;
    }

    std::fwrite(ansi::text_cyan_bold.data(), 1, ansi::text_cyan_bold.size(), stdout);
    std::fputs("  Time                  │ PID    │ Process          │ SysID  │ Syscall      │ Parameters", stdout);
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
    
    move_cursor(4, 1);
    clear_line();
    
    std::string divider;
    divider.reserve(static_cast<size_t>(screen_cols) * 3);
    for (int32_t col = 0; col < screen_cols; ++col) {
        if (col == 24 || col == 33 || col == 52 || col == 61 || col == 76) {
            divider += "┼";
        } else {
            divider += "─";
        }
    }
    std::fwrite(ansi::text_cyan_bold.data(), 1, ansi::text_cyan_bold.size(), stdout);
    std::printf("%s", divider.c_str());
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
}

static void render_core_trace_records(const IView &view, int32_t body, int32_t screen_cols, const UiState &state) {
    if (state.frame_cache.size() != static_cast<size_t>(body)) {
        state.frame_cache.resize(static_cast<size_t>(body));
    }

    for (int32_t i = 0; i < body; ++i) {
        std::string current_line_str;

        if (state.overlay == OverlayMode::InspectLsof) {
            int32_t index = state.lsof_top + i;
            if (index < static_cast<int32_t>(state.lsof_lines.size())) {
                const std::string &raw_line = state.lsof_lines[static_cast<size_t>(index)];
                std::string processed = raw_line;
                if (processed.length() > static_cast<size_t>(screen_cols - 4)) {
                    processed = processed.substr(0, static_cast<size_t>(screen_cols - 4));
                }
                
                if (index == state.lsof_sel) {
                    current_line_str = " > " + processed;
                } else {
                    current_line_str = "   " + processed;
                }
            }
        } else {
            const UiEvent *e = view.row(i);
            int32_t abs_idx = view.top() + i;

            if (e) {
                int32_t extra_w = (screen_cols > 78) ? (screen_cols - 78) : 10;
                
                // ts_ns is wall-clock time (ktime_get_real_ns); show it in
                // local time, like the clock in the title bar.
                const time_t secs = static_cast<time_t>(e->ts_ns / 1'000'000'000ULL);
                const uint32_t msec = static_cast<uint32_t>((e->ts_ns / 1'000'000ULL) % 1000);
                std::tm ltm{};
                if (!localtime_r(&secs, &ltm)) ltm = std::tm{};

                std::string time_stamp_str;
                time_stamp_str.resize(16);
                std::snprintf(time_stamp_str.data(), time_stamp_str.size(), "%02d:%02d:%02d.%03u",
                              ltm.tm_hour, ltm.tm_min, ltm.tm_sec, msec);

                int32_t line_size = std::snprintf(nullptr, 0, "%c %-21.21s | %-6d | %-16.16s | %-6d | %-12.12s | %.*s",
                                                  (abs_idx == view.sel()) ? '*' : ' ', time_stamp_str.c_str(),
                                                  e->pid, e->comm.c_str(), e->nr, e->sys_name.c_str(), extra_w, e->extra.c_str());
                
                current_line_str.resize(static_cast<size_t>(line_size));
                std::snprintf(current_line_str.data(), current_line_str.size() + 1, "%c %-21.21s | %-6d | %-16.16s | %-6d | %-12.12s | %.*s",
                              (abs_idx == view.sel()) ? '*' : ' ', time_stamp_str.c_str(),
                              e->pid, e->comm.c_str(), e->nr, e->sys_name.c_str(), extra_w, e->extra.c_str());
            }
        }

        // Compare strings to skip unmodified lines before touching stdout
        if (state.frame_cache[static_cast<size_t>(i)] == current_line_str) {
            continue; 
        }
        state.frame_cache[static_cast<size_t>(i)] = current_line_str;

        // Move cursor and paint row ONLY when layout variations occur
        move_cursor(5 + i, 1);
        
        if (state.overlay == OverlayMode::InspectLsof) {
            int32_t index = state.lsof_top + i;
            if (index == state.lsof_sel && !current_line_str.empty()) {
                std::fwrite(ansi::style_highlight.data(), 1, ansi::style_highlight.size(), stdout);
                std::fputs(current_line_str.c_str(), stdout);
                int32_t length = static_cast<int32_t>(current_line_str.length());
                if (screen_cols > length) {
                    std::string padding(static_cast<size_t>(screen_cols - length), ' ');
                    std::fputs(padding.c_str(), stdout);
                }
                std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
            } else {
                std::fputs(current_line_str.c_str(), stdout);
                std::fwrite(ansi::clear_line.data(), 1, ansi::clear_line.size(), stdout);
            }
        } else {
            // Type-safe selection lookup mapping character arrays
            const bool is_row_highlighted = (!current_line_str.empty() && current_line_str[0] == '*');
            if (is_row_highlighted) {
                std::fwrite(ansi::bg_green_white_bold.data(), 1, ansi::bg_green_white_bold.size(), stdout);
            }
            std::fputs(current_line_str.c_str(), stdout);
            if (is_row_highlighted) {
                int32_t length = static_cast<int32_t>(current_line_str.length());
                if (screen_cols > length) {
                    std::string padding(static_cast<size_t>(screen_cols - length), ' ');
                    std::fputs(padding.c_str(), stdout);
                }
            }
            std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
            std::fwrite(ansi::clear_line.data(), 1, ansi::clear_line.size(), stdout);
        }
    }
}

static void render_interactive_footer_dock(int32_t body, int32_t screen_cols, const UiState &state) {
    std::string footer_msg;

    // Dynamically choose hotkey labels based on the active mode
    if (state.overlay == OverlayMode::InspectLsof) {
        footer_msg = "  ESC: Exit Inspector │ Home/End: Snap Limits │ Up/Down: Scroll │ PageUp/PageDown: Flip Page";
    } else {
        footer_msg = "  F Filter  S Sort  O Order  Space Pause/Go  Home/End  PgUp/PgDown  R Refresh  ";
        footer_msg += state.is_capturing ? "C Stop Capture  " : "C Capture  ";
        if (state.has_lsof) {
            footer_msg += "L List Open Files  ";
        }
        footer_msg += "Q Quit";
    }

    // Safely prevent spoiling past the terminal window boundaries
    if (static_cast<size_t>(screen_cols) < footer_msg.length()) {
        footer_msg = footer_msg.substr(0, static_cast<size_t>(screen_cols));
    }

    // Anchor the cursor to the designated final line row of the viewport matrix
    move_cursor(5 + body, 1);
    std::fwrite(ansi::bg_blue_white_bold.data(), 1, ansi::bg_blue_white_bold.size(), stdout);
    std::fputs(footer_msg.c_str(), stdout);
    
    // Fill out remaining width with padded empty spaces to cleanly stretch the background tint
    int32_t active_len = static_cast<int32_t>(footer_msg.length());
    if (screen_cols > active_len) {
        std::string fill(static_cast<size_t>(screen_cols - active_len), ' ');
        std::fputs(fill.c_str(), stdout);
    }
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
}

static void render_right_side_modal_overlays(const IFilterStore &filters, int32_t body, int32_t screen_cols, UiState &state) {
    int32_t x_pos = (screen_cols > 44) ? (screen_cols - 40) : 2;
    
    std::vector<std::string> labels;
    labels.reserve(32);

    if (state.overlay == OverlayMode::SelectOrder) {
        labels.push_back("ASC");
        labels.push_back("DESC");
    } else if (state.overlay == OverlayMode::SelectSort) {
        labels.insert(labels.end(), {"TIME", "PID", "PROCESS", "SYSID", "SYSCALL", "PARAMETERS"});
    } else if (state.overlay == OverlayMode::EditRefreshMs) {
        labels.push_back("REFRESH: " + std::to_string(state.refresh_edit) + " MS");
        labels.push_back("UP/DOWN: JUMP 100MS (100-10000)");
    } else if (state.overlay == OverlayMode::MainFilterHub) {
        labels.insert(labels.end(), {"ADD FILTER", "REMOVE FILTER", "SHOW FILTERS"});
    } else if (state.overlay == OverlayMode::FilterKind) {
        labels.insert(labels.end(), {"PID", "PROCESS", "SYSID", "SYSCALL"});
    } else if (state.overlay == OverlayMode::PickProcess) {
        labels.insert(labels.end(), state.seen_procs.begin(), state.seen_procs.end());
    } else if (state.overlay == OverlayMode::PickSysId) {
        for (int32_t id : state.seen_sysids)
            labels.push_back("SYSID " + std::to_string(id));
    } else if (state.overlay == OverlayMode::PickSyscall) {
        labels.insert(labels.end(), state.seen_syscalls.begin(), state.seen_syscalls.end());
    } else if (state.overlay == OverlayMode::PickPid) {
        for (int32_t pid : state.seen_pids)
            labels.push_back("PID " + std::to_string(pid));
    } else if (state.overlay == OverlayMode::AddFilter) {
        labels.insert(labels.end(), {"PROCESS gnome-terminal", "PROCESS gnome-shell", "PROCESS Xorg", "PROCESS vmtoolsd"});
    } else {
        const auto &rules_vec = filters.rules();
        for (const auto &rule : rules_vec) {
            if (rule.kind == RuleKind::Process)
                labels.push_back("PROCESS " + rule.text);
            else if (rule.kind == RuleKind::Syscall)
                labels.push_back("SYSCALL " + rule.text);
            else if (rule.kind == RuleKind::Pid)
                labels.push_back("PID " + std::to_string(rule.nr));
            else
                labels.push_back("SYSID " + std::to_string(rule.nr));
        }
    }

    int32_t n = static_cast<int32_t>(labels.size());
    int32_t vis = (body > 4) ? (body - 2) : 4;
    
    UiState& mutable_state = state;
    
    // Anti-Flicker Switch: Lock scrolling bounds to row 0 if inside the rate rate text-modifier
    if (state.overlay != OverlayMode::EditRefreshMs) {
        if (n == 0) {
            mutable_state.overlay_sel = 0;
        } else if (mutable_state.overlay_sel >= n) {
            mutable_state.overlay_sel = n - 1;
        }
        
        if (mutable_state.overlay_sel < mutable_state.overlay_top) 
            mutable_state.overlay_top = mutable_state.overlay_sel;
        if (mutable_state.overlay_sel >= mutable_state.overlay_top + vis) 
            mutable_state.overlay_top = mutable_state.overlay_sel - vis + 1;
        if (mutable_state.overlay_top < 0) 
            mutable_state.overlay_top = 0;
    } else {
        mutable_state.overlay_sel = 0;
        mutable_state.overlay_top = 0;
    }

    move_cursor(2, x_pos);
    std::fwrite(ansi::bg_blue_white_bold.data(), 1, ansi::bg_blue_white_bold.size(), stdout);
    std::printf(" %-38.38s ", 
               state.overlay == OverlayMode::SelectOrder   ? "ORDER  ENTER/ESC" :
               state.overlay == OverlayMode::SelectSort    ? "SORT  ENTER/ESC" :
               state.overlay == OverlayMode::EditRefreshMs ? "REFRESH SCALE RATE MODIFIER" :
               state.overlay == OverlayMode::MainFilterHub ? "FILTER  R=REMOVE  ENTER/ESC" :
               state.overlay == OverlayMode::FilterKind    ? "KIND  ENTER/ESC" :
               state.overlay == OverlayMode::PickProcess   ? "PROCESS  ENTER/ESC" :
               state.overlay == OverlayMode::PickSysId     ? "SYSID  ENTER/ESC" :
               state.overlay == OverlayMode::PickSyscall   ? "SYSCALL  ENTER/ESC" :
               state.overlay == OverlayMode::PickPid       ? "PID  ENTER/ESC" :
               state.overlay == OverlayMode::AddFilter     ? "ADD FILTER  ENTER/ESC" :
               state.overlay == OverlayMode::RemoveFilter  ? "REMOVE FILTER  ENTER/ESC" : "SHOW FILTERS  ESC");
    std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);

    for (int32_t y = 0; y < vis; ++y) {
        int32_t idx = mutable_state.overlay_top + y;
        move_cursor(3 + y, x_pos);
        if (idx < n) {
            if (state.overlay != OverlayMode::EditRefreshMs && idx == mutable_state.overlay_sel) {
                std::fwrite(ansi::style_highlight.data(), 1, ansi::style_highlight.size(), stdout);
                std::printf("%-38.38s", labels[static_cast<size_t>(idx)].c_str());
                std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
            } else if (state.overlay == OverlayMode::EditRefreshMs && idx == 0) {
                std::fwrite(ansi::style_highlight.data(), 1, ansi::style_highlight.size(), stdout);
                std::printf("%-38.38s", labels[static_cast<size_t>(idx)].c_str());
                std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
            } else {
                std::fwrite(ansi::text_cyan.data(), 1, ansi::text_cyan.size(), stdout);
                std::printf("%-38.38s", labels[static_cast<size_t>(idx)].c_str());
                std::fwrite(ansi::style_reset.data(), 1, ansi::style_reset.size(), stdout);
            }
        } else {
            std::printf("%-38.38s", " ");
        }
    }
}

// =================================================================------------
// Main Orchestrator Definition
// =================================================================------------

void render_dashboard(IView &view, const IFilterStore &filters, int32_t fpid, UiState &state) {
    struct winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_row == 0 || w.ws_col == 0) {
        return; // Guard backdrop structures if window dimensions are completely lost
    }

    const int32_t screen_rows = w.ws_row;
    const int32_t screen_cols = w.ws_col;
    const int32_t body = (screen_rows > 5) ? (screen_rows - 5) : 3;
    view.set_rows(body);

    // Route A: Full-Screen 'lsof' Process Details Viewer Mode
    if (state.overlay == OverlayMode::InspectLsof) {
        render_top_title_panel(view, screen_cols, state);
        render_applied_filters_banner(filters, fpid, state);
        render_table_headers(screen_cols, state);
        render_core_trace_records(view, body, screen_cols, state);
        render_interactive_footer_dock(body, screen_cols, state);
        std::fflush(stdout);
        view.mark_drawn();
        return;
    }

    // Route B: Modular Right-Side Configuration Popups Sidebar Overlay Mode
    if (state.overlay != OverlayMode::None) {
        render_right_side_modal_overlays(filters, body, screen_cols, state);
        std::fflush(stdout);
        view.mark_drawn();
        return;
    }

    // Route C: Standard Real-Time System Call Trace Dashboard Mode
    render_top_title_panel(view, screen_cols, state);
    render_applied_filters_banner(filters, fpid, state);
    render_table_headers(screen_cols, state);
    render_core_trace_records(view, body, screen_cols, state);
    render_interactive_footer_dock(body, screen_cols, state);

    std::fflush(stdout);
    view.mark_drawn();
}

} // namespace tui
