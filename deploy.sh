#!/usr/bin/env bash
# deploy: detect the compiler, seed filters.xml if there is none,
# build userspace and the kernel module, load the module. Does not start the TUI.
#   ./deploy.sh                    build both, load the module
#   ./deploy.sh --no-run           build only
#   ./deploy.sh --refresh-filters  also rewrite an existing filters.xml
# Run it as a normal user (it calls sudo where root is needed) or as root.
set -euo pipefail
cd "$(dirname "$0")"

BOLD="\033[1m"; GREEN="\033[32m"; YELLOW="\033[33m"; RED="\033[31m"; RESET="\033[0m"
log_info() { echo -e "${BOLD}${GREEN}[INFO]${RESET} $1"; }
log_warn() { echo -e "${BOLD}${YELLOW}[WARN]${RESET} $1"; }
log_err()  { echo -e "${BOLD}${RED}[ERROR]${RESET} $1" >&2; }

MODULE_NAME="syscall_monitor"
UDEV_RULE_FILE="/etc/udev/rules.d/99-${MODULE_NAME}.rules"
BUILD_DIR="build"
KMOD_DIR="${BUILD_DIR}/kmod"
NO_RUN=0
REFRESH_FILTERS=0
for arg in "$@"; do
    case "$arg" in
        --no-run)          NO_RUN=1 ;;
        --refresh-filters) REFRESH_FILTERS=1 ;;
        -h|--help)         sed -n '2,7p' "$0"; exit 0 ;;
        *)                 log_err "unknown option: $arg"; exit 2 ;;
    esac
done

# Root needs no sudo; everyone else needs sudo for packages and insmod.
if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=()
elif command -v sudo >/dev/null 2>&1; then
    SUDO=(sudo)
else
    SUDO=(need_root)
fi
need_root() {
    log_err "'$*' needs root and sudo is not installed; run ./deploy.sh as root"
    return 1
}

ensure_pkg() {
    local bin="$1" pkg="$2"
    if command -v "$bin" >/dev/null 2>&1; then
        log_info "$bin already installed"
        return 0
    fi
    log_info "installing $pkg"
    if command -v apt-get >/dev/null 2>&1; then
        "${SUDO[@]}" apt-get update && "${SUDO[@]}" apt-get install -y "$pkg"
    elif command -v dnf >/dev/null 2>&1; then
        "${SUDO[@]}" dnf install -y "$pkg"
    elif command -v yum >/dev/null 2>&1; then
        "${SUDO[@]}" yum install -y "$pkg"
    else
        log_warn "no package manager; install $pkg manually"
        return 1
    fi
}

# ps shows comm, which the kernel truncates to 15 bytes; compare on those.
running() {
    ps -eo comm= 2>/dev/null | awk -v n="${1:0:15}" '$0==n {found=1} END {exit !found}'
}

emit_filters() {
    local out="filters.xml"
    local -a procs=()
    local term="${TERM_PROGRAM:-}${TERMINAL:-}"
    for name in gnome-terminal gnome-terminal-server konsole xterm kitty alacritty \
                tilix terminator xfce4-terminal mate-terminal lxterminal wezterm foot; do
        if running "$name" || [[ "$term" == *"$name"* ]]; then
            procs+=("$name")
        fi
    done
    for name in gnome-shell plasmashell kwin_x11 kwin_wayland Xorg Xwayland vmtoolsd VBoxClient; do
        if running "$name"; then
            procs+=("$name")
        fi
    done
    {
        echo '<?xml version="1.0"?>'
        echo '<filters>'
        echo '  <!-- written by deploy.sh from this session -->'
        local p
        for p in "${procs[@]}"; do
            echo "  <hide kind=\"process\">${p}</hide>"
        done
        echo '  <hide kind="sysid">0</hide>'
        echo '</filters>'
    } > "$out"
    log_info "wrote ${out} (${#procs[@]} processes)"
}

ensure_pkg cmake cmake
ensure_pkg g++ g++ || ensure_pkg clang++ clang
ensure_pkg lsof lsof || true
ensure_pkg make build-essential || true

CXX_BIN="g++"
command -v g++ >/dev/null 2>&1 || CXX_BIN="clang++"
if ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    log_err "no C++ compiler"
    exit 1
fi

# filters.xml holds the user's own rules (the TUI saves to it), so an
# existing file is only rewritten on request.
if [[ -f filters.xml && "$REFRESH_FILTERS" -eq 0 ]]; then
    log_info "keeping existing filters.xml (--refresh-filters rewrites it)"
else
    log_info "writing filters.xml"
    emit_filters || log_warn "filter probe failed; the TUI seeds a default filters.xml"
fi

log_info "removing old objects"
rm -f ./*.o ./*.cmd ./*.ko ./*.mod ./*.mod.c ./Module.symvers ./modules.order
rm -rf "${KMOD_DIR}"

log_info "configuring userspace"
cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
CORES=$(nproc 2>/dev/null || echo 2)
log_info "building userspace with ${CORES} cores"
cmake --build "${BUILD_DIR}" --config Release -j "${CORES}"

log_info "building kernel module into ./${KMOD_DIR}"
if [[ -d "/lib/modules/$(uname -r)/build" ]]; then
    make module
else
    log_warn "kernel headers missing; module not built"
fi

if [[ "$NO_RUN" -eq 1 ]]; then
    log_info "build only"
    exit 0
fi

if [[ ! -f "${KMOD_DIR}/${MODULE_NAME}.ko" ]]; then
    log_warn "no ${KMOD_DIR}/${MODULE_NAME}.ko; TUI can still run in simulator mode"
    exit 0
fi

# Older versions installed a udev rule that made the node world-readable
# (0666). The stream exposes every process's syscalls, so remove it and
# keep the kernel default: root-only 0600, and CAP_PERFMON/CAP_SYS_ADMIN
# is checked again in open().
if [[ -f "${UDEV_RULE_FILE}" ]]; then
    log_info "removing legacy world-readable udev rule ${UDEV_RULE_FILE}"
    "${SUDO[@]}" rm -f "${UDEV_RULE_FILE}"
    "${SUDO[@]}" udevadm control --reload-rules || true
fi
if grep -q "^${MODULE_NAME} " /proc/modules 2>/dev/null; then
    log_info "unloading existing ${MODULE_NAME}"
    "${SUDO[@]}" rmmod "${MODULE_NAME}" || true
fi
log_info "insmod ${KMOD_DIR}/${MODULE_NAME}.ko"
"${SUDO[@]}" insmod "${KMOD_DIR}/${MODULE_NAME}.ko"
if [[ "$(id -u)" -eq 0 ]]; then
    log_info "installed. next: ./${BUILD_DIR}/syswatch"
else
    log_info "installed. next: sudo ./${BUILD_DIR}/syswatch"
fi
