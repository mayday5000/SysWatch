#pragma once
#include "types.hpp"
#include "filters.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

namespace tui {

    enum class OverlayMode : int32_t {
        None          = 0,
        AddFilter     = 1,
        RemoveFilter  = 2,
        ShowFilters   = 3,
        MainFilterHub = 4,
        EditRefreshMs = 5,
        SelectSort    = 6,
        SelectOrder   = 7,
        InspectLsof   = 8,
        FilterKind    = 9,
        PickProcess   = 10,
        PickSysId     = 11,
        PickSyscall   = 12,
        PickPid       = 13
    };

    struct UiState {
        OverlayMode overlay     = OverlayMode::None;
        int32_t overlay_sel     = 0;
        int32_t overlay_top     = 0;
        int32_t refresh_ms      = 1000;
        int32_t refresh_edit    = 1000;
        bool is_capturing       = false;
        bool has_lsof           = false;
        int32_t lsof_sel        = 0;
        int32_t lsof_top        = 0;
		
        std::vector<std::string> lsof_lines;
        std::vector<std::string> syscall_names;
        std::vector<std::string> seen_procs;
        std::vector<std::string> seen_syscalls;
        std::vector<int32_t> seen_pids;
        std::vector<int32_t> seen_sysids;
        mutable std::vector<std::string> frame_cache; 
    };

    namespace ansi {
        constexpr std::string_view style_reset          = "\033[0m";      
        constexpr std::string_view text_bold            = "\033[1m";      
        constexpr std::string_view text_cyan            = "\033[38;5;44m"; 
        constexpr std::string_view style_highlight      = "\033[7m";      
        constexpr std::string_view bg_blue_white_bold   = "\033[48;5;21m\033[38;5;15m\033[1m";  
        constexpr std::string_view bg_green_white_bold  = "\033[48;5;34m\033[38;5;15m\033[1m";  
        constexpr std::string_view text_yellow_bold     = "\033[1m\033[38;5;226m";             
        constexpr std::string_view text_cyan_bold       = "\033[1m\033[38;5;51m";              
        constexpr std::string_view enter_tui_mode       = "\033[?1049h\033[?25l\033[2J\033[H"; 
        constexpr std::string_view exit_tui_mode        = "\033[?1049l\033[?25h\033[0m\033[2J\033[H"; 
        constexpr std::string_view clear_line           = "\033[K";                             
        constexpr std::string_view warn_yellow          = "\n\033[1;33m[WARN] "; 
        constexpr std::string_view info_green           = "\033[1;32m[INFO] ";
        constexpr std::string_view text_orange_bold     = "\033[38;5;202m";
        constexpr std::string_view bg_orange_white_bold = "\033[48;5;202m\033[38;5;15m\033[1m";		
    }

    void move_cursor(int32_t row, int32_t col);
    void clear_line();
    void style_highlight();
    void style_reset();
    
    void render_dashboard(const IView &view, const IFilterStore &filters, int32_t fpid, const UiState &state);
    int32_t clamp_refresh_ms(int32_t ms);
    bool refresh_due(int64_t now_ms, int64_t last_ms, bool dirty, int64_t interval_ms);
}
