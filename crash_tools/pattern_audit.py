#!/usr/bin/env python3
"""
pattern_audit.py - Auditor Estático de Patrones Peligrosos y Desreferencias Nulas
================================================================================
Escanea el código decompilado de Ghidra en busca de accesos a memoria peligrosos
sin verificación de puntero NULL (como desreferencias de buffers de tiles +0xf0/+0xc,
singletons sin verificar, o accesos directos a arrays no inicializados).
"""

import os
import sys
import argparse
import re
from collections import defaultdict

DEFAULT_GHIDRA_PATH = "decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c"

PATTERNS = [
    {
        "name": "Unchecked TileData Dereference (+0xf0 -> +0xc)",
        "desc": "Lectura directa del buffer de tiles de mapa sin comprobar si m_pLayers o m_pTileData es NULL",
        "regex": r"\+\s*0xf0[^\n]*\+\s*0xc",
        "severity": "CRITICAL"
    },
    {
        "name": "Direct Singleton Dereference without check",
        "desc": "Desreferencia directa de un singleton de juego",
        "regex": r"\*\s*\(\s*int\s*\*\s*\)\s*\(\s*CGsSingleton<[^>]+>::ms_pSingleton\s*\+\s*0x[0-9a-fA-F]+\s*\)",
        "severity": "MEDIUM"
    },
    {
        "name": "Potential Unchecked VTable Call",
        "desc": "Invocación a través de tabla de funciones virtuales",
        "regex": r"\(\*\*\s*\(code\s*\*\*\)\s*\(\s*\*in_r0\s*\+\s*0x[0-9a-fA-F]+\s*\)\)",
        "severity": "LOW"
    }
]

def audit(file_path, pattern_filter=None):
    if not os.path.exists(file_path):
        print(f"[-] Error: Archivo {file_path} no encontrado.")
        return

    print(f"[*] Auditando patrones en: {file_path}")

    results = defaultdict(list)
    current_func = "Global"

    with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
        for line_no, line in enumerate(f, 1):
            if line.startswith("// ") and ("::" in line or "(" in line):
                current_func = line.strip()[3:]

            for p in PATTERNS:
                if pattern_filter and pattern_filter.lower() not in p["name"].lower():
                    continue
                if re.search(p["regex"], line):
                    results[p["name"]].append((line_no, current_func, line.strip()))

    print("=" * 80)
    print(" REPORTE DE AUDITORÍA ESTÁTICA DE PATRONES")
    print("=" * 80)

    for p in PATTERNS:
        matches = results[p["name"]]
        print(f"\n[!] {p['name']} [{p['severity']}]")
        print(f"    Descripción: {p['desc']}")
        print(f"    Total de ocurrencias encontradas: {len(matches)}")
        print("-" * 80)
        
        funcs = defaultdict(list)
        for line_no, func, snippet in matches:
            funcs[func].append(line_no)
        
        for func, lines in funcs.items():
            lines_str = ", ".join(str(l) for l in lines[:5])
            if len(lines) > 5: lines_str += f" ... (+{len(lines)-5} más)"
            print(f"    • {func} (Líneas: {lines_str})")

def main():
    parser = argparse.ArgumentParser(description="Auditor de patrones de crash en código Ghidra")
    parser.add_argument("--file", default=DEFAULT_GHIDRA_PATH, help="Ruta al archivo out_ghidra.c")
    parser.add_argument("--pattern", help="Filtrar por nombre de patrón específico")

    args = parser.parse_args()
    audit(args.file, args.pattern)

if __name__ == "__main__":
    main()
