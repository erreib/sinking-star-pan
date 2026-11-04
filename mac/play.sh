#!/bin/zsh
# Mac (Wine wrapper) launcher: starts Order of the Sinking Star through the sinking-star-mac wrapper
# if it is not running, waits for the game process, and injects the pan mod into it.
#   ./mac/play.sh            launch (if needed) + inject
#   ./mac/play.sh --unload   ask a running mod to remove its hook and unload
set -e
HERE="${0:A:h}/.."
WRAPPER="${SS_WRAPPER_DIR:-$HOME/dev/sinking-star-mac}"
[[ -f "$WRAPPER/env.sh" ]] || { echo "sinking-star-mac wrapper not found at $WRAPPER (set SS_WRAPPER_DIR)"; exit 1; }
if [[ "$1" == "--unload" ]]; then echo unload > "$HERE/dist/unload.flag"; echo "unload requested"; exit 0; fi
if ! pgrep -f '^C:.*sinking_star\.exe' >/dev/null; then
    "$WRAPPER/launch.sh"
    echo "waiting for the game..."
    for i in {1..180}; do pgrep -f '^C:.*sinking_star\.exe' >/dev/null && break; sleep 2; done
fi
source "$WRAPPER/env.sh"
cd "$HERE/dist"
ss_wine "Z:$(pwd | tr / '\\')\\SinkingStarPanInjector.exe" --no-launch 2>&1 | grep -v -E '^[0-9a-f]{4}:(fixme|warn|err):' || true
echo "log: $HERE/dist/sinking_star_pan.log"
