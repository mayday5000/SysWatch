#pragma once
/*
 * text_util.hpp
 * Purpose: make untrusted text safe to print on a terminal. comm (set by
 * any process through prctl(PR_SET_NAME)) and syscall path arguments are
 * attacker-controlled bytes; written raw they could carry ESC/CSI/OSC
 * sequences that recolour, move, retitle or otherwise drive the terminal
 * of whoever runs the TUI (usually root).
 */
#include <string>
#include <string_view>

// Printable ASCII and well-formed UTF-8 (excluding C1 controls U+0080..
// U+009F) pass through; every other byte becomes '?'.
std::string sanitize_terminal(std::string_view text);
