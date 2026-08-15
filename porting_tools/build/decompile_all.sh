#!/bin/bash
# ==============================================================================
# Script para decompilar APK y archivos .so para ILLUSIA 2 (PS Vita)
# ==============================================================================

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BASE_PARENT="$(cd "$PROJECT_DIR/.." && pwd)"
APK_FILE="${PROJECT_DIR}/ILLUSIA-2-v1-0-3.apk"
LIB_DIR="${PROJECT_DIR}/ILLUSIA-2-v1-0-3/lib"
DECOMPILED_DIR="${PROJECT_DIR}/decompiled"
APK_OUT_DIR="${DECOMPILED_DIR}/apk_jadx"

mkdir -p "$APK_OUT_DIR"

echo "Iniciando decompilación con JADX..."
if command -v jadx >/dev/null 2>&1; then
  jadx -d "$APK_OUT_DIR" "$APK_FILE"
else
  echo "[-] Error: jadx no está instalado. Instálalo con 'brew install jadx'."
  exit 1
fi
echo "JADX Finalizado. Resultados en ${APK_OUT_DIR}"

echo "Iniciando decompilación de .so con Ghidra y Angr (devrvk/so-decompiler)..."
if [ ! -d "$LIB_DIR" ]; then
    echo "Directorio de librerías no encontrado en: $LIB_DIR"
else
    find "$LIB_DIR" -type f -name "*.so" -not -name "._*" | while read -r so_file; do
        so_name=$(basename "$so_file")
        abi=$(basename "$(dirname "$so_file")")
        so_out="${DECOMPILED_DIR}/${so_name%.so}_${abi}/ghidra"
        mkdir -p "$so_out"

        rel_so_path="${so_file#$BASE_PARENT/}"
        rel_so_out="${so_out#$BASE_PARENT/}"

        echo "Decompilando $so_name ($abi)..."
        docker run --rm --platform linux/amd64 -v "${BASE_PARENT}:/app" devrvk/so-decompiler decompile "/app/$rel_so_path" "/app/$rel_so_out"
    done
fi

echo "Decompilación de archivos .so finalizada. Resultados en ${DECOMPILED_DIR}/<lib>_<abi>/ghidra/"
