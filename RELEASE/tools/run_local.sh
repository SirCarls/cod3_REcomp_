#!/bin/sh
# run_local.sh: runs the heavy work on this computer (all CPU cores, your
# GPU) and writes the results into dev/ in the project folder, where Claude
# can read them. Run it from anywhere:
#
#   sh tools/run_local.sh                  test + check + rip (the default)
#   sh tools/run_local.sh test             build the Rust tools and run their tests
#   sh tools/run_local.sh check            check every CoD3 archive against the format notes
#   sh tools/run_local.sh rip              rip CoD3 textures, meshes and materials
#   sh tools/run_local.sh recomp-test      build the recomp and play Saint-Lo on your GPU
#   sh tools/run_local.sh rip --waw "/path/to/Call of Duty World at War"
#   sh tools/run_local.sh waw360 --waw-xbox360 "/path/to/Call of Duty - World at War.iso"
#                                    copy the fastfiles out of the Xbox 360 disc
#                                    image and try ripping them
#   sh tools/run_local.sh map-pack --waw-map-pack "/path/to/codwaw-v1.5_1.6.zip"
#                                    take the map packs out of an official PC patch
#                                    (never run) and try ripping them
#   Steps can be combined: sh tools/run_local.sh check waw360 map-pack --waw-xbox360 ... --waw-map-pack ...
#
# Options:
#   --jobs N       worker threads (default: every CPU core)
#   --iso PATH     CoD3 extracted folder (default: output/assets, the setup tool's copy)
#   --waw PATH     World at War folder; its fastfiles are ripped too
#                  (its mods/ folder is ignored)
#   --waw-xbox360 PATH   World at War Xbox 360 disc image, for the waw360 step
#   --waw-map-pack PATH  patch download with map packs, for the map-pack step
#                        (default: every patch in input/)
#                        (where to get it: python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py map-pack --list)
#   --seconds N    how long recomp-test runs the game (default 600)
#
# Output, all in dev/:
#   logs/<step>.log, logs/summary.txt, logs/system.txt, logs/shots/*.png
#   ripped/cod3/   textures/, meshes/, materials/
#   ripped/waw/    raw files (scripts) of each fastfile
#   ripped/waw360/ files/ (copied from the Xbox 360 image) and ripped/
#   ripped/map-pack/ files/ (taken out of the patch) and ripped/; logs/map-pack-report.txt
#   recomp-test/   the build made by recomp-test
# Nothing outside the project folder is changed. Your ISO and WaW folder are only read.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEV="$ROOT/dev"
LOGS="$DEV/logs"
ENGINE="$ROOT/engine"
RIPPED="$DEV/ripped"
SETUP="$ROOT/COD_3_WAW_Setup-tool/setup.sh"
TOOL="$ROOT/COD_3_WAW_Setup-tool/cod3_waw_setup.py"

JOBS=""
# The extracted CoD3 disc: the setup tool's copy, or the older assets/ folder.
ISO="$ROOT/output/assets"
[ -d "$ISO/sp" ] || [ ! -d "$ROOT/assets/sp" ] || ISO="$ROOT/assets"
WAW=""
WAW360=""
MAP_PACK=""
SECONDS_TO_RUN=600
STEPS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS="$2"; shift 2 ;;
    --iso) ISO="$2"; shift 2 ;;
    --waw) WAW="$2"; shift 2 ;;
    --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
    test|check|rip|recomp-test|waw360|map-pack) STEPS="$STEPS $1"; shift ;;
    --waw-xbox360) WAW360="$2"; shift 2 ;;
    --waw-map-pack) MAP_PACK="$2"; shift 2 ;;
    -h|--help) sed -n '2,37p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (see: sh tools/run_local.sh --help)"; exit 2 ;;
  esac
done
[ -n "$STEPS" ] || STEPS="test check rip"
SETUP_JOBS=""
if [ -n "$JOBS" ]; then SETUP_JOBS="--jobs $JOBS"; else JOBS="$(nproc 2>/dev/null || echo 4)"; fi

mkdir -p "$LOGS"
SUMMARY="$LOGS/summary.txt"
echo "run_local.sh $(date '+%Y-%m-%d %H:%M:%S') steps:$STEPS jobs:$JOBS" >> "$SUMMARY"

# What this computer is, for reading the logs later.
{
  echo "date: $(date)"
  echo "system: $(uname -srm)"
  echo "cpu threads: $(nproc 2>/dev/null)"
  grep -m1 'model name' /proc/cpuinfo 2>/dev/null
  free -h 2>/dev/null | sed -n 2p
  command -v lspci >/dev/null 2>&1 && lspci | grep -iE 'vga|3d|display'
  command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo|apiVersion' | head -6
} > "$LOGS/system.txt" 2>&1

# Runs one step, logging its output and how it went.
#   step <name> <command...>
# (POSIX sh has no local variables, so step_* names are used only here.)
step() {
  step_name="$1"; shift
  echo "== $step_name (log: dev/logs/$step_name.log)"
  step_start=$(date +%s)
  "$@" > "$LOGS/$step_name.log" 2>&1
  step_status=$?
  step_took=$(( $(date +%s) - step_start ))
  tail -n 5 "$LOGS/$step_name.log"
  if [ $step_status -eq 0 ]; then step_result=ok; else step_result="FAILED ($step_status)"; fi
  echo "  $step_name: $step_result in ${step_took}s" | tee -a "$SUMMARY"
  return $step_status
}

find_cargo() {
  if command -v cargo >/dev/null 2>&1; then echo cargo; return 0; fi
  if [ -x "$HOME/.cargo/bin/cargo" ]; then echo "$HOME/.cargo/bin/cargo"; return 0; fi
  return 1
}

CARGO="$(find_cargo || true)"
RIP="$ENGINE/target/release/rip"

# Installs Rust (and a C linker) through the setup tool when they're missing.
install_rust() {
  echo "Rust isn't installed; installing it now (into your home folder, no admin rights needed)..."
  python3 "$TOOL" doctor --fix --no-recomp
  CARGO="$(find_cargo || true)"
}

build_tools() {
  [ -n "$CARGO" ] || install_rust
  if [ -z "$CARGO" ]; then
    echo "Rust couldn't be installed automatically. Install it from https://rustup.rs, then run this again."
    return 1
  fi
  (cd "$ENGINE" && "$CARGO" build --release --jobs "$JOBS" && "$CARGO" test --release --jobs "$JOBS")
}

# Removes an output folder this script made before (it holds .rip-output),
# never anything else.
clear_output() {
  if [ -f "$1/.rip-output" ]; then rm -rf -- "$1"; fi
  mkdir -p "$1" && : > "$1/.rip-output"
}

rip_cod3() {
  clear_output "$RIPPED/cod3"
  "$RIP" cod3-all "$ISO/sp" "$RIPPED/cod3" --jobs "$JOBS"
}

rip_waw() {
  [ -n "$WAW" ] || return 0
  clear_output "$RIPPED/waw"
  # Fastfiles live under zone/; the mods/ folder is the user's own and skipped.
  list="$RIPPED/waw/fastfiles.txt"
  find "$WAW/zone" -name '*.ff' -not -path '*/mods/*' | sort > "$list"
  failed=0
  while read -r ff; do
    "$RIP" waw-ff "$ff" "$RIPPED/waw/$(basename "$ff" .ff)" || failed=1
  done < "$list"
  return $failed
}

# check and rip read the extracted disc, not the .iso itself.
need_extracted() {
  [ -d "$ISO/sp" ] && return 0
  echo "$ISO/sp not found. check and rip need the folder extracted from the ISO: build once with the setup"
  echo "tool (it puts it in output/assets), or give it with --iso PATH."
  return 1
}

build_only() {
  [ -n "$CARGO" ] || install_rust
  [ -n "$CARGO" ] || { build_tools; return; }
  (cd "$ENGINE" && "$CARGO" build --release --jobs "$JOBS")
}

# Takes a screenshot of the whole screen with whatever tool is installed.
screenshot() {
  if command -v gnome-screenshot >/dev/null 2>&1; then gnome-screenshot -f "$1"
  elif command -v import >/dev/null 2>&1; then import -window root "$1"
  elif command -v scrot >/dev/null 2>&1; then scrot -o "$1"
  elif command -v grim >/dev/null 2>&1; then grim "$1"
  elif command -v spectacle >/dev/null 2>&1; then spectacle -b -n -o "$1"
  else return 1
  fi
}

recomp_test() {
  out="$DEV/recomp-test"
  echo "Building the recomp into $out (the first build takes a while)..."
  # The setup tool picks its compile threads from the RAM (about 2 GB each);
  # --jobs is passed on only when given explicitly.
  sh "$SETUP" build --iso "$ISO" --output "$out" --no-appimage --keep-sdk --no-waw \
    $SETUP_JOBS || return 1
  [ -x "$out/bin/cod3" ] || { echo "no game executable in $out/bin"; return 1; }
  mkdir -p "$LOGS/shots"
  rm -f "$LOGS"/shots/recomp-*.png
  echo "Running Saint-Lo for ${SECONDS_TO_RUN}s; screenshots every 30s into dev/logs/shots/"
  (cd "$out" && ./bin/cod3 --fullscreen=false --log_file="$LOGS/recomp-game.log" \
      --cod3_autoexec="spmap saint_lo" --cod3_autoexec_delay=60 --cod3_debug_frames=true) &
  pid=$!
  waited=0
  while kill -0 $pid 2>/dev/null && [ $waited -lt "$SECONDS_TO_RUN" ]; do
    sleep 30
    waited=$((waited + 30))
    screenshot "$LOGS/shots/recomp-$(printf %04d $waited).png" 2>/dev/null || true
  done
  if kill -0 $pid 2>/dev/null; then
    kill $pid
    echo "stopped the game after ${waited}s"
  else
    wait $pid
    echo "the game exited by itself after about ${waited}s (code $?)"
  fi
}

# World at War for Xbox 360: lists the disc image, copies its fastfiles out
# (the image is only read) and tries the ripper on each one.
waw360() {
  [ -n "$WAW360" ] || { echo "Give the disc image: --waw-xbox360 PATH"; return 1; }
  out="$RIPPED/waw360"
  clear_output "$out"
  python3 "$TOOL" xiso "$WAW360" > "$LOGS/waw360-files.txt" || return 1
  python3 "$TOOL" xiso "$WAW360" --extract "$out/files" --only "*.ff" || return 1
  find "$out/files" -name '*.ff' | sort > "$out/fastfiles.txt"
  ripped=0; total=0
  while read -r ff; do
    total=$((total + 1))
    if "$RIP" waw-ff "$ff" "$out/ripped/$(basename "$ff" .ff)"; then ripped=$((ripped + 1)); fi
  done < "$out/fastfiles.txt"
  echo "ripped $ripped of $total Xbox 360 fastfiles"
}

# Map packs: describes the patch download (dev/logs/map-pack-report.txt), takes
# its fastfiles and .iwd files out (its installer is never run) and tries the
# ripper on each fastfile.
map_pack() {
  out="$RIPPED/map-pack"
  list="$DEV/map-pack-downloads.txt"
  mkdir -p "$DEV"
  # The download given with --waw-map-pack, or every patch in input/.
  if [ -n "$MAP_PACK" ]; then
    printf '%s\n' "$MAP_PACK" > "$list"
  else
    find "$ROOT/input" -maxdepth 2 -type f \( -iname 'codwaw-v*.zip' -o -iname 'codwaw*patch*.exe' \) 2>/dev/null \
      | sort > "$list"
  fi
  if [ ! -s "$list" ]; then
    echo "No map pack download: put the patches in input/ or give one with --waw-map-pack PATH"
    python3 "$TOOL" map-pack --list
    return 1
  fi
  clear_output "$out"
  : > "$LOGS/map-pack-report.txt"
  failed=0
  while read -r download; do
    python3 "$TOOL" map-pack "$download" >> "$LOGS/map-pack-report.txt" 2>&1 || failed=1
    python3 "$TOOL" map-pack "$download" --extract "$out/files" || failed=1
  done < "$list"
  find "$out/files" -name '*.ff' | sort > "$out/fastfiles.txt"
  ripped=0; total=0
  while read -r ff; do
    total=$((total + 1))
    if "$RIP" waw-ff "$ff" "$out/ripped/$(basename "$ff" .ff)"; then ripped=$((ripped + 1)); fi
  done < "$out/fastfiles.txt"
  echo "ripped $ripped of $total map pack fastfiles"
  return $failed
}

# check and rip use the freshly built tools even when "test" isn't asked for.
case "$STEPS" in
  *test*) ;;
  *check*|*rip*|*waw360*|*map-pack*) step build build_only || exit 1 ;;
esac

for s in $STEPS; do
  case "$s" in
    test) step test build_tools || exit 1 ;;
    check) need_extracted && step check "$RIP" cod3-check "$ISO/sp" --jobs "$JOBS" ;;
    rip)
      need_extracted && step rip-cod3 rip_cod3
      [ -z "$WAW" ] || step rip-waw rip_waw
      ;;
    recomp-test) step recomp-test recomp_test ;;
    waw360) step waw360 waw360 ;;
    map-pack) step map-pack map_pack ;;
  esac
done
echo "Done. Results: $LOGS/summary.txt"
