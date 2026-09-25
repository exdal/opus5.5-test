# Source this file (`. ./env.sh`) to use the sandboxed toolchain. Nothing here touches system paths:
# every tool, cache and temp file lives in .sandbox/ (created by tools/sandbox/bootstrap.sh).
OXCITY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export OXCITY_ROOT
OXCITY_SANDBOX="$OXCITY_ROOT/.sandbox"
export OXCITY_SANDBOX
export OXCITY_LLVM="$OXCITY_SANDBOX/llvm23"
OXCITY_SYSROOT="$OXCITY_SANDBOX/sysroot"
export PATH="$OXCITY_SANDBOX/bin:$OXCITY_SANDBOX/xmake/bin:$OXCITY_LLVM/bin:$PATH"
# lavapipe (mesa) + the libllvm20 it links against
export LD_LIBRARY_PATH="$OXCITY_SANDBOX/runtime-libs:$OXCITY_SYSROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# cmake-based packages (SDL3) look for the X11 dev libs bootstrap.sh extracted here, xmake's cmake glue forwards
# include dirs of package deps but not their link dirs
export CMAKE_LIBRARY_PATH="$OXCITY_SYSROOT/usr/lib/x86_64-linux-gnu"
export TMPDIR="$OXCITY_SANDBOX/tmp"
export XMAKE_GLOBALDIR="$OXCITY_SANDBOX/xmake-global"
export XMAKE_ROOT=y
export XMAKE_STATS=n
export XDG_CACHE_HOME="$OXCITY_SANDBOX/cache"
export XDG_CONFIG_HOME="$OXCITY_SANDBOX/config"
export XDG_DATA_HOME="$OXCITY_SANDBOX/data"
export XDG_RUNTIME_DIR="$OXCITY_SANDBOX/run"
export VK_ICD_FILENAMES="$OXCITY_SANDBOX/lvp_icd.json"
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
mkdir -p "$TMPDIR" "$XMAKE_GLOBALDIR" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
