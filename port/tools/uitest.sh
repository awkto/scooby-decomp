#!/bin/sh
# Saves-overlay UI test (#28), on a VM with xvfb + xdotool:  port/tools/uitest.sh
# Starts the real window under Xvfb, plays the title for a while, then drives the F8 GAME SAVES menu with real keyboard and
# mouse events: opens it (screenshot), saves slot 1 with the key S, loads it with a mouse click on LOAD, erases it with DEL.
# Screenshots land in runs/uitest/ (menu1.png ...); the game log is runs/uitest/game.log.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/uitest; rm -rf "$out"; mkdir -p "$out/saves"
command -v xdotool >/dev/null || apt-get install -y -qq xdotool >/dev/null 2>&1
cat > "$out/inner.sh" <<'IN'
bin=build/port/scoobyport
out=runs/uitest
# start in a mystery through a short harness-style script: the first story route, then the live window takes over
$bin --savedir $out/saves --mute --script tools/scripts/story/w0_cafe_items.txt --no-throttle > $out/game.log 2>&1 &
pid=$!
sleep 12
win=$(xdotool search --name "Scooby" | head -1)
echo "window $win"; xdotool getwindowgeometry $win
xdotool key --window $win F8; sleep 1
import -window root $out/menu1.png
xdotool key --window $win s; sleep 1          # save into the selected slot (1)
xdotool key --window $win Down; xdotool key --window $win s; sleep 1
import -window root $out/menu2.png
ls $out/saves
# mouse: click the LOAD button of row 1: layout in hostmenu.c (box centred in the 256x224 frame, 3x scale)
eval $(xdotool getwindowgeometry --shell $win)
# game px -> screen px: the frame (256x224) is centred in the window, whole-number scale
sc=$((HEIGHT / 224)); fx=$((X + (WIDTH - 256 * sc) / 2)); fy=$Y
bx=$(( (256 - 236) / 2 )); by=$(( (224 - 216) / 2 ))
lx=$(( bx + 236 - 66 + 14 )); ly=$(( by + 18 + 10 ))
xdotool mousemove $((fx + lx * sc)) $((fy + ly * sc)); sleep 0.5
import -window root $out/menu3.png
xdotool click 1; sleep 20
import -window root $out/after_load.png
kill $pid
IN
xvfb-run -a -s "-screen 0 1280x1024x24" sh "$out/inner.sh"
cat "$out/game.log" | tail -5
