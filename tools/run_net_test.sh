#!/usr/bin/env bash
# Multiplayer test on one machine: every player is its own OxCity process on 127.0.0.1, all headless (the clients
# render through lavapipe, see run_headless.sh; the dedicated server doesn't render at all).
#
#   tools/run_net_test.sh dedicated   OxCity --server + two clients: "shooter" and "target"   (default)
#   tools/run_net_test.sh listen      a listen server that plays ("host") + one client ("shooter")
#
# Logs go to .sandbox/net-<mode>/, screenshots to captures/net/<mode>/. Each scripted player prints a PASS/FAIL
# checklist (NetAutoplay.cpp); this script collects them and exits non-zero if anything failed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../env.sh
. "$ROOT/env.sh"

MODE="${1:-dedicated}"
PORT="${PORT:-7777}"
CLIENT_FRAMES="${CLIENT_FRAMES:-1600}"
CLIENT_DT="${CLIENT_DT:-0.05}"
# lavapipe clients manage 1-2 frames a second here, so their scripted players live at ~0.07 s of game time per second.
# The dedicated server doesn't render and would run the city (and its cops) in real time around them: slow its
# clock to the clients' pace
SERVER_DT="${SERVER_DT:-0.0012}"
# several lavapipe renderers share the CPU: keep them small
export OXCITY_WIDTH="${OXCITY_WIDTH:-640}" OXCITY_HEIGHT="${OXCITY_HEIGHT:-360}"
BIN_DIR="$ROOT/engine/build/linux/x86_64/release"
LOGS="$ROOT/.sandbox/net-$MODE"
SHOTS="$ROOT/captures/net/$MODE"
mkdir -p "$LOGS" "$SHOTS"
rm -f "$LOGS"/*.log "$SHOTS"/*.png

# the engine's ENet only opens IPv6 (dual stack) sockets. Without an IPv6 stack (this container) nothing can listen
# or connect, so for the test only, preload a shim that falls back to IPv4 (tools/netshim/ipv4_fallback.c)
if ! python3 -c "import socket; socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)" 2>/dev/null; then
  SHIM="$OXCITY_SANDBOX/lib/liboxcity_ipv4_fallback.so"
  if [ ! -f "$SHIM" ] || [ "$ROOT/tools/netshim/ipv4_fallback.c" -nt "$SHIM" ]; then
    mkdir -p "$(dirname "$SHIM")"
    clang -O2 -shared -fPIC -Wall -Wextra -o "$SHIM" "$ROOT/tools/netshim/ipv4_fallback.c" -ldl
  fi
  export LD_PRELOAD="$SHIM"
  echo "no IPv6 here: running with the IPv4 fallback shim"
fi

server_pid=""
cleanup() {
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

client() { # role name log [extra args]
  local role="$1" name="$2" log="$3"
  shift 3
  "$ROOT/tools/run_headless.sh" --join "127.0.0.1:$PORT" --name "$name" --net-autoplay "$role" \
    --frames "$CLIENT_FRAMES" --fixed-dt "$CLIENT_DT" --screenshots "$SHOTS" "$@" > "$log" 2>&1
}

case "$MODE" in
  dedicated)
    (cd "$BIN_DIR" && exec ./OxCity --server "$PORT" --fixed-dt "$SERVER_DT") > "$LOGS/server.log" 2>&1 &
    server_pid=$!
    sleep 2
    client target BOB "$LOGS/target.log" &
    target_pid=$!
    sleep 3
    # the shooter runs out of frames first; the target keeps going long enough to respawn afterwards
    CLIENT_FRAMES=$((CLIENT_FRAMES - 200)) client shooter ALICE "$LOGS/shooter.log" || true
    wait "$target_pid" || true
    ;;
  listen)
    "$ROOT/tools/run_headless.sh" --host "$PORT" --name HOSTESS --net-autoplay host --frames "$((CLIENT_FRAMES + 300))" \
      --fixed-dt "$CLIENT_DT" --screenshots "$SHOTS" > "$LOGS/host.log" 2>&1 &
    server_pid=$!
    sleep 25 # the host renders through lavapipe too, give it time to open the port
    client shooter ALICE "$LOGS/shooter.log" || true
    wait "$server_pid" || true
    server_pid=""
    ;;
  *)
    echo "usage: $0 [dedicated|listen]" >&2
    exit 2
    ;;
esac
cleanup
server_pid=""

status=0
for log in "$LOGS"/*.log; do
  echo "== $(basename "$log")"
  sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -E "OxCity net|OxCity server|OxCity: net autoplay|\[PASS\]|\[FAIL\]|  player |wasted" |
    sed -E 's/^[0-9:. ()s]+\[[^]]*\] +[A-Za-z]+\.cpp:[0-9]+ +[A-Z]+\| //' || true
  if sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -q "\[FAIL\]"; then
    status=1
  fi
done
echo "screenshots: $SHOTS"
exit $status
