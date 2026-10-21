#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
prefix=${FPV_PREFIX:-/root/fpv-sdk}
desktop_user=${FPV_DESKTOP_USER:-bianbu}
desktop_uid=$(id -u "$desktop_user")
runtime_dir="/run/user/$desktop_uid"

if [[ $EUID -ne 0 ]]; then
  echo "Run this launcher as root; it opens the viewer in $desktop_user's desktop." >&2
  exit 1
fi
if [[ ! -S "$runtime_dir/wayland-0" ]]; then
  echo "No Wayland desktop for $desktop_user at $runtime_dir/wayland-0" >&2
  exit 1
fi
if pgrep -x fpv_ground >/dev/null; then
  echo "Stop the existing fpv_ground before starting the preview." >&2
  exit 1
fi
source "$prefix/fpv_setup.bash"
export MPP_PLUGIN_DIR="$prefix/lib"

preview_dir=$(mktemp -d /tmp/fpv-ground-preview.XXXXXX)
chmod 755 "$preview_dir"
fifo="$preview_dir/video.h265"
mkfifo -m 644 "$fifo"
viewer_pid=
ground_pid=
cleanup() {
  if [[ -n "$ground_pid" ]] && kill -0 "$ground_pid" 2>/dev/null; then
    kill -INT "$ground_pid" 2>/dev/null || true
    wait "$ground_pid" 2>/dev/null || true
  fi
  if [[ -n "$viewer_pid" ]]; then
    kill "$viewer_pid" 2>/dev/null || true
    wait "$viewer_pid" 2>/dev/null || true
  fi
  rm -rf "$preview_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

runuser -u "$desktop_user" -- env \
  XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland \
  ffplay -hide_banner -loglevel warning -an -framedrop \
  -window_title 'FPV Ground' -f hevc -i "$fifo" \
  >"$preview_dir/ffplay.log" 2>&1 &
viewer_pid=$!

echo "Opening FPV preview on $desktop_user's local screen."
echo "ffplay log: $preview_dir/ffplay.log"
fpv_ground --config "$script_dir/ground.yaml" --preview-fifo "$fifo" &
ground_pid=$!
wait "$ground_pid"
