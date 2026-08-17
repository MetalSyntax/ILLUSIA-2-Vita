#!/usr/bin/env python3
"""
so_symbol_search.py - Buscador e Inspector de Símbolos y C++ Mangling
====================================================================
Busca funciones, clases, variables globales y cadenas dentro de un .so de Android,
desofusca nombres de C++ (demangling) y muestra los offsets exactos.
"""

import os
import sys
import argparse
import subprocess
import re

VITASDK_BIN = os.environ.get("VITASDK", "/Users/metalsyntax/vitasdk") + "/bin"
if os.path.exists(VITASDK_BIN) and VITASDK_BIN not in os.environ.get("PATH", ""):
    os.environ["PATH"] = f"{VITASDK_BIN}:{os.environ.get('PATH', '')}"

def demangle(name):
    if not name or not name.startswith("_Z"):
        return name
    for cmd in ["arm-vita-eabi-c++filt", "c++filt"]:
        try:
            res = subprocess.run([cmd, name], capture_output=True, text=True)
            if res.returncode == 0 and res.stdout.strip():
                return res.stdout.strip()
        except Exception:
            continue
    return name

def search_symbols(so_path, query, case_insensitive=True):
    if not os.path.exists(so_path):
        print(f"[-] Error: Archivo {so_path} no encontrado.")
        return

    cmd = ["arm-vita-eabi-nm", "-D", so_path]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            cmd = ["arm-vita-eabi-objdump", "-T", so_path]
            res = subprocess.run(cmd, capture_output=True, text=True)
    except Exception as e:
        print(f"[-] Error al ejecutar nm/objdump: {e}")
        return

    matches = []
    q = query.lower() if case_insensitive else query

    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            addr_str = parts[0]
            raw_sym = parts[-1]
            try:
                addr = int(addr_str, 16)
            except ValueError:
                continue
            
            dem = demangle(raw_sym)
            search_target = f"{raw_sym} {dem}".lower() if case_insensitive else f"{raw_sym} {dem}"
            if q in search_target:
                matches.append((addr, raw_sym, dem))

    matches.sort(key=lambda x: x[0])
    print("=" * 90)
    print(f" Símbolos coincidentes con '{query}' en {so_path} ({len(matches)} encontrados)")
    print("=" * 90)
    print(f" {'OFFSET':<12} | {'SÍMBOLO DESMANGLED (C++)':<50} | {'SÍMBOLO RAW'}")
    print("-" * 90)
    for addr, raw, dem in matches:
        print(f" 0x{addr:08x}   | {dem:<50} | {raw}")

def main():
    parser = argparse.ArgumentParser(description="Buscador de símbolos en archivos .so")
    parser.add_argument("query", help="Texto o patrón a buscar (ej: 'CheckRide', 'OnMove', 'Draw')")
    parser.add_argument("--so", default="ux0_data/illusia2/libgameDSO.so", help="Ruta al archivo .so")
    parser.add_argument("-i", "--case-sensitive", action="store_true", help="Búsqueda sensible a mayúsculas")

    args = parser.parse_args()
    search_symbols(args.so, args.query, not args.case_sensitive)

if __name__ == "__main__":
    main()
