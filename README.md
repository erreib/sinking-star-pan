# Sinking Star Pan

Drag the local area view (the **Q** overhead map) around with the mouse in
*Order of the Sinking Star*. In the game the view is locked to the cell you are
standing in; with this mod you hold the mouse button and drag to look at the
surrounding area, and the view returns to normal when you close it.

Nothing in the game folder is modified. The mod is a DLL that is loaded into the
running game and patched out again when it unloads.

## Install and run

1. Download the release zip and extract it anywhere (not into the game folder).
2. Run `SinkingStarPanInjector.exe`. It starts the game through Steam if it is
   not already running, waits for it, and loads `sinking_star_pan.dll` into it.
   If the game is already running it just loads the mod.
3. In the game, press **Q** to open the local area view, then just move the
   mouse: the view follows it. Opening the view puts the cursor in the middle
   of the window and starts on the player; moving the mouse to the edge of the
   window shows about two thirds of a screen further in that direction, and
   moving back to the middle recentres. Press **Q** again to close the view;
   the camera goes back to the player, and the next time you open the view it
   starts on the player again.

If you prefer to click and drag the map instead, set `mode = drag` in
`config.ini`.

The DLL writes `sinking_star_pan.log` next to itself. If something does not
work, that file says why.

### macOS (Wine wrapper)

If you run the game through the
[sinking-star-mac](https://github.com/erreib/sinking-star-mac) wrapper, use
`mac/play.sh`: it launches the game through the wrapper when needed and injects
the mod with the wrapper's own Wine. `mac/play.sh --unload` removes the hook
from a running game.

## Configuration

Edit `config.ini` next to the DLL (read when the mod loads):

| key | default | meaning |
|---|---|---|
| `mode` | `move` | `move`: the view follows the mouse position, no clicking; `drag`: hold a button and drag the map |
| `gain` | `1.3` | move mode: how far the view goes when the cursor reaches the window edge, in half-screens (`1.0` = half a screen, `2.0` = a full screen) |
| `smoothing` | `0.08` | move mode: easing time in seconds (`0` = instant) |
| `recenter_cursor` | `1` | move mode: warp the cursor to the window centre when the view opens, so the full range is available in every direction |
| `button` | `left` | drag mode: mouse button that drags: `left`, `right` or `middle` |
| `sensitivity` | `1.0` | drag mode: drag speed; `1.0` keeps the map under the cursor |
| `invert_x`, `invert_y` | `0` | set to `1` to flip a direction (either mode) |
| `max_offset` | `500` | how far (in world units) the view may be dragged from where the game put it |
| `modes` | `2` | which overhead views may be dragged: `2` is the local area view (Q); add `1` to try the full map view as well |
| `[log] enabled` | `1` | write the log file |
| `[log] debug` | `0` | also log the camera state every two seconds |

## Uninstall

Close the game and delete the folder. If you want to unload the mod while the
game is running, create an empty file named `unload.flag` next to the DLL; the
mod restores the patched bytes and unloads itself.

## Compatibility

Built against the Steam build of 2026-10-30 (build id 26249670). The mod finds
everything it needs by byte signature rather than fixed addresses, so it should
survive small updates; if a game update changes the camera code the log will say
`hook signature: 0 hits` and the mod does nothing.

Tested on an Apple Silicon Mac through the Wine wrapper. The same files should
work on Windows (the loader is the same technique other injector mods for this
game use), but that has not been tried yet.

Known limits:

- Panning only applies to the local area view. The full map view has its own
  camera path and is not changed.
- In areas where the designers placed camera limiters, the game may clamp the
  view back toward the limiter region while you pan.
- Motion is only as smooth as the game's frame rate. The view is eased once per
  rendered frame with a smoothed frame time, but at 30 fps with uneven frame
  pacing a moving camera still shows some judder; a lower render preset helps.

## How it works

The game is written in Jai and keeps its type information in the executable,
which is how the game-state layout below was recovered (`tools/jai_types.py`
dumps any reflected struct). The camera lives in a block at game-state +0x3d98:

| offset | field |
|---|---|
| +0x3d98 | camera position |
| +0x3db0 | camera rotation |
| +0x3dd4 | view mode: 0 normal, 1 map view, 2 local area view |
| +0x3dd8 | local-area blend (0..1) |
| +0x3e04 | bounds of the framed local area (x0 y0 x1 y1) |

The game's camera routine centres the local area view on the midpoint of those
bounds and eases toward it. The frame code recomputes the bounds from the
player's cell every frame, so writing to them from a thread would race. Instead
the DLL patches the three instructions that read the bounds inside the camera
routine with a jump to a small trampoline that adds the pan offset to both
corners before the game uses them. A polling thread turns mouse drags into that
target (in move mode from the cursor's position relative to the window centre,
in drag mode from accumulated mouse motion; pixels are converted to world units
from the camera height, field of view and window size), scales it with the
blend while the view closes, and clears it afterwards. The trampoline also
calls a small C function once per frame, on the game thread, that eases the
offset toward that target using the real frame time, so the motion is smooth
regardless of how the polling thread's ticks line up with frames.

The pointer to the game state is found from the prologue of the overhead-map
toggle handlers; the hook site is found by its byte pattern.

## Building

Cross-compiled with mingw-w64 (`brew install mingw-w64` on macOS). `./build.sh`
produces `dist/SinkingStarPanInjector.exe` and `dist/sinking_star_pan.dll`.

`tools/` holds the research tooling: `jai_types.py` (dump Jai type info from the
exe), `xref.py` (find code that references a string), the Ghidra headless script
used to decompile the camera routine, and `game.sh` for driving the game from a
Mac terminal during development.

## Credits

The loader approach and the idea of reading the game's Jai type table come from
[hhhxiao/SinkingStarCamera](https://github.com/hhhxiao/SinkingStarCamera). The
first game-state offsets were cross-checked against
[linyanzhi591-afk/order-of-the-sinking-star-local-area-view](https://github.com/linyanzhi591-afk/order-of-the-sinking-star-local-area-view).

MIT licensed. Not affiliated with Thekla.
