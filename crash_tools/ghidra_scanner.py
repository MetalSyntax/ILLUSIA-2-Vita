#!/usr/bin/env python3
"""
ghidra_scanner.py - Buscador e Inspector en el Código Pseudo-C de Ghidra
========================================================================
Permite buscar rápidamente funciones decompiladas, variables, offsets y llamadas
dentro del archivo decompilado `out_ghidra.c` mostrando el código C exacto y número de líneas.
"""

import os
import sys
import argparse
import re

DEFAULT_GHIDRA_PATH = "decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c"

def scan_ghidra(file_path, query, show_lines=40, max_results=5):
    if not os.path.exists(file_path):
        print(f"[-] Error: Archivo {file_path} no encontrado.")
        return

    print(f"[*] Escaneando '{query}' en {file_path}...")

    matches = []
    with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
        for line_no, line in enumerate(f, 1):
            if query.lower() in line.lower():
                matches.append((line_no, line.rstrip()))

    print("=" * 80)
    print(f" Resultados encontrados para '{query}': {len(matches)}")
    print("=" * 80)

    if not matches:
        return

    for idx, (line_no, text) in enumerate(matches[:max_results], 1):
        print(f"\n--- Coincidencia #{idx} (Línea {line_no}) ---")
        print(f"    > {text}")
        
        start_line = max(1, line_no - 5)
        end_line = line_no + show_lines
        print(f"--- Contexto de código [Líneas {start_line} - {end_line}] ---")
        
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            for cur_no, cur_line in enumerate(f, 1):
                if start_line <= cur_no <= end_line:
                    marker = ">>" if cur_no == line_no else "  "
                    print(f" {marker} {cur_no:6d}: {cur_line.rstrip()}")
                elif cur_no > end_line:
                    break

def main():
    parser = argparse.ArgumentParser(description="Buscador en el código decompilado de Ghidra")
    parser.add_argument("query", help="Nombre de función, símbolo o patrón (ej: 'CheckRide', 'm_pTileData', 'CreateMiniMap')")
    parser.add_argument("--file", default=DEFAULT_GHIDRA_PATH, help=f"Ruta al archivo out_ghidra.c (por defecto {DEFAULT_GHIDRA_PATH})")
    parser.add_argument("-n", "--lines", type=int, default=40, help="Número de líneas de contexto a mostrar")
    parser.add_argument("-m", "--max", type=int, default=5, help="Número máximo de coincidencias a mostrar detalladas")

    args = parser.parse_args()
    scan_ghidra(args.file, args.query, args.lines, args.max)

if __name__ == "__main__":
    main()
