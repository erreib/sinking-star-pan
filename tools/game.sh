#!/bin/zsh
# Dev helper for driving the game inside the Wine wrapper (Mac).
#   game.sh inject            inject dist/sinking_star_pan.dll into the running game
#   game.sh shot [name]       screenshot (downscaled) to the scratchpad
#   game.sh key <k> [..]      press keys via cliclick (e.g. q, tab, esc, return)
#   game.sh drag x1 y1 x2 y2  left-button drag in screen coordinates
#   game.sh log               tail the mod log
set -e
HERE="${0:A:h}/.."
SHOTS="${SS_SHOTS:-$HERE/shots}"
mkdir -p "$SHOTS"
case "$1" in
    inject)
        source ~/dev/sinking-star-mac/env.sh
        cd "$HERE/dist" && ss_wine "Z:$(pwd | tr / '\\')\\SinkingStarPanInjector.exe" --no-launch 2>&1 | grep -v -E '^[0-9a-f]{4}:(fixme|warn|err):' ;;
    shot)
        f="$SHOTS/${2:-shot-$(date +%H%M%S)}.jpg"; screencapture -x -t jpg "$f"; sips -Z 1600 "$f" >/dev/null; echo "$f" ;;
    key)
        shift; for k in "$@"; do cliclick "kp:$k" 2>/dev/null || cliclick "t:$k"; done ;;
    type)
        shift; cliclick "t:$*" ;;
    click)
        cliclick "c:$2,$3" ;;
    drag)
        cliclick "dd:$2,$3" "w:150" "dm:$(( ($2+$4)/2 )),$(( ($3+$5)/2 ))" "w:150" "dm:$4,$5" "w:150" "du:$4,$5" ;;
    log)
        tail -n ${2:-40} "$HERE/dist/sinking_star_pan.log" ;;
    *) echo "usage: $0 inject|shot|key|type|click|drag|log"; exit 1 ;;
esac
