#!/bin/zsh
# Cross-builds the mod with mingw-w64 (brew install mingw-w64). Output lands in dist/.
set -e
cd "${0:A:h}"
CC=x86_64-w64-mingw32-gcc
CFLAGS=(-O2 -Wall -Wextra -std=gnu11 -municode)
mkdir -p dist
$CC -O2 -Wall -o dist/SinkingStarPanInjector.exe src/injector.c -static -lpsapi
if [[ -f src/mod.c ]]; then
    $CC -O2 -Wall -Wextra -std=gnu11 -shared -o dist/sinking_star_pan.dll src/mod.c -static -lpsapi -luser32 -Wl,--subsystem,windows
fi
[[ -f src/config.ini ]] && cp src/config.ini dist/
ls -la dist
