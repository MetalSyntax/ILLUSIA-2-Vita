#!/bin/bash
# ==============================================================================
# Despliega eboot.bin directo en Vita3K para iteración rápida en ILLUSIA 2
# ==============================================================================
set -e

VITASDK="${VITASDK:-$HOME/vitasdk}"
export VITASDK
export PATH="$VITASDK/bin:$PATH"

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TITLE_ID="ILLUSIA02"
BUILD_DIR="/tmp/illusia2-build"
APP_FS="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/app/$TITLE_ID"
SCRIPT_DIR="$PROJECT_DIR/porting_tools/automation"

echo "[1/4] Compilando en $BUILD_DIR..."
if [ ! -d "$BUILD_DIR" ]; then
    echo "[-] Error: Directorio $BUILD_DIR no existe. Ejecuta build.sh primero."
    exit 1
fi
cd "$BUILD_DIR"
make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "[2/4] Desplegando eboot.bin en $APP_FS..."
mkdir -p "$APP_FS"
cp "$BUILD_DIR/eboot.bin" "$APP_FS/eboot.bin"

echo "[3/4] Relanzando Vita3K limpio..."
pkill -9 -x Vita3K 2>/dev/null || true
sleep 1
open -a Vita3K
sleep 3
osascript -e 'tell application "System Events" to tell process "Vita3K" to set frontmost to true' 2>/dev/null || true
sleep 1

echo "[4/4] Doble clic en el ícono del juego..."
if [ -f "$SCRIPT_DIR/click_helper.py" ]; then
    ROW_X=$(osascript -e '
    tell application "System Events"
        tell process "Vita3K"
            set p to position of row 1 of table 1 of window 1
            return item 1 of p
        end tell
    end tell' 2>/dev/null || echo "100")
    ROW_Y=$(osascript -e '
    tell application "System Events"
        tell process "Vita3K"
            set p to position of row 1 of table 1 of window 1
            return item 2 of p
        end tell
    end tell' 2>/dev/null || echo "100")
    CLICK_X=$((ROW_X + 60))
    CLICK_Y=$((ROW_Y + 38))
    python3 "$SCRIPT_DIR/click_helper.py" "$CLICK_X" "$CLICK_Y" 2 2>/dev/null || true
fi

echo "Listo."
