#!/bin/bash
# Restart the compositor fresh.
#
# Use pgrep -x, which matches the EXECUTABLE NAME only. Never grep ps output for
# the process name: the ssh command line, a heredoc being written, and even this
# script s own filename all contain that string, and each one makes the kill
# match itself. That has killed the ssh session three times.
#   usage: restart_compositor.sh [socket] [port] [width] [height]
SOCK=${1:-wayland-combo-20}; PORT=${2:-5920}; W=${3:-1920}; H=${4:-1080}
pgrep -x weston >/dev/null && { pkill -x -9 weston; sleep 2; }
rm -f "/run/user/1000/$SOCK" "/run/user/1000/$SOCK.lock"
nohup weston --backends=vnc-backend.so,headless-backend.so --renderer=gl \
  --socket="$SOCK" --width="$W" --height="$H" --port="$PORT" \
  --vnc-tls-cert=/data/tmp/vnc_tls/cert.pem --vnc-tls-key=/data/tmp/vnc_tls/key.pem \
  > "/data/tmp/compositor_$SOCK.log" 2>&1 &
disown
sleep 6
[ -S "/run/user/1000/$SOCK" ] && echo "socket OK: $SOCK" || echo "SOCKET MISSING"
ss -lntp 2>/dev/null | grep -q ":$PORT" && echo "vnc listening on $PORT" || echo "VNC NOT LISTENING"
pgrep -xc weston | sed "s/^/compositor processes: /"
