#!/bin/bash
set -e

# ==============================================================================
# Script de Build Automático para ILLUSIA 2 (PS Vita)
# ==============================================================================

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_SH="$PROJECT_DIR/build.sh"

echo "================================================================"
echo "  Script de Build Automático para ILLUSIA 2 (PS Vita)"
echo "================================================================"

if [ ! -x "$BUILD_SH" ]; then
    echo "Error: no se encontró (o no es ejecutable) $BUILD_SH"
    exit 1
fi

flag_desc() {
    case "$1" in
        --debug) echo "Compilar en modo Debug con símbolos y logs adicionales" ;;
        --clean) echo "Limpiar carpetas temporales de compilación antes de construir" ;;
        *) echo "(sin descripción corta -- ver build.sh)" ;;
    esac
}

FLAGS=()
while IFS= read -r line; do
    [ -n "$line" ] && FLAGS+=("$line")
done < <(grep -o '"\-\-[a-z0-9-]\+"' "$BUILD_SH" 2>/dev/null | tr -d '"' | awk '!seen[$0]++' || true)

echo ""
echo "[1/4] Elegí qué build de $(basename "$BUILD_SH") querés compilar:"
echo "  0) (build normal, Release) -- build/illusia2.vpk"
i=1
for f in "${FLAGS[@]}"; do
    printf "  %d) %-24s -- %s\n" "$i" "$f" "$(flag_desc "$f")"
    i=$((i + 1))
done
echo "  q) [ Cancelar / Regresar al menú principal ]"
echo ""
read -p "Número de opción [0, q para cancelar]: " FLAG_CHOICE
FLAG_CHOICE="${FLAG_CHOICE:-0}"

if [ "$FLAG_CHOICE" = "q" ] || [ "$FLAG_CHOICE" = "Q" ] || [ "$FLAG_CHOICE" = "c" ] || [ "$FLAG_CHOICE" = "cancel" ]; then
    echo "[*] Operación cancelada. Regresando al menú principal..."
    exit 0
fi

BUILD_FLAG=""
if [ "$FLAG_CHOICE" != "0" ]; then
    idx=$((FLAG_CHOICE - 1))
    if [ "$idx" -lt 0 ] || [ "$idx" -ge "${#FLAGS[@]}" ]; then
        echo "Opción inválida. Regresando..."
        exit 0
    fi
    BUILD_FLAG="${FLAGS[$idx]}"
fi

BUILD_ARGS=()
if [ -n "$BUILD_FLAG" ]; then
    BUILD_ARGS+=("$BUILD_FLAG")
fi

echo ""
echo "[2/4] Ejecutando build.sh ${BUILD_ARGS[*]} ..."
echo "(build.sh ya se encarga del staging en /tmp para evitar el bug de"
echo " vita-pack-vpk con rutas con espacios -- ver toolchain_gotchas.md)"
"$BUILD_SH" "${BUILD_ARGS[@]}"

VPK_OUTPUT_NAME="illusia2"
case "$BUILD_FLAG" in
    --debug) VPK_OUTPUT_NAME="illusia2_debug" ;;
    *) VPK_OUTPUT_NAME="illusia2" ;;
esac
VPK_NAME="${VPK_OUTPUT_NAME}.vpk"
VPK_PATH="$PROJECT_DIR/build/$VPK_NAME"

if [ ! -f "$VPK_PATH" ]; then
    if [ -f "$PROJECT_DIR/build/illusia2.vpk" ]; then
        VPK_PATH="$PROJECT_DIR/build/illusia2.vpk"
        VPK_NAME="illusia2.vpk"
    else
        echo "Error: build.sh terminó pero no encuentro $VPK_PATH"
        exit 1
    fi
fi
echo "Build exitoso: $VPK_PATH"

echo ""
echo "[3/4] Exportando el ELF sin firmar (para simbolizar un .psp2dmp con vita-parse-core)..."
TMP_BUILD_DIR="/tmp/illusia2-build"
if [ -f "$TMP_BUILD_DIR/illusia2" ]; then
    cp "$TMP_BUILD_DIR/illusia2" "$PROJECT_DIR/build/${VPK_OUTPUT_NAME}.elf"
    echo "ELF con símbolos exportado a: $PROJECT_DIR/build/${VPK_OUTPUT_NAME}.elf"
elif [ -f "$PROJECT_DIR/build/illusia2.elf" ]; then
    echo "ELF con símbolos presente en: $PROJECT_DIR/build/illusia2.elf"
fi

echo ""
echo "[4/4] Instalación (opcional)"
VITA3K_APP="/Applications/Vita3K.app/Contents/MacOS/Vita3K"
if [ -x "$VITA3K_APP" ]; then
    read -p "¿Deseas instalar y ejecutar $VPK_NAME en Vita3K ahora? [s/N] " INSTALL_VITA3K
    if [[ "$INSTALL_VITA3K" =~ ^[sS]$ ]]; then
        echo "Instalando VPK y lanzando el emulador (backend OpenGL)..."
        "$VITA3K_APP" -B OpenGL "$VPK_PATH" > /dev/null 2>&1 &
        echo "Listo."
    else
        echo "Omitiendo instalación automática en Vita3K."
    fi
else
    echo "Vita3K no encontrado en la ruta por defecto (/Applications/Vita3K.app)."
fi
echo "Para instalar en un Vita real (FTP+comando de VitaShell), usar manage_vita.py o 'make send' desde el árbol de build."
