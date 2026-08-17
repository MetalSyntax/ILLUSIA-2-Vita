#!/usr/bin/env python3
"""
so_disasm.py - Disensamblador Inteligente para librerías .so de Android / PS Vita
================================================================================
Desensambla rangos de direcciones, offsets, o funciones completas con soporte
para Thumb / ARM mode, cálculo automático de runtime base y detección de símbolos.
"""

import os
import sys
import argparse
import subprocess

VITASDK_BIN = os.environ.get("VITASDK", "/Users/metalsyntax/vitasdk") + "/bin"
if os.path.exists(VITASDK_BIN) and VITASDK_BIN not in os.environ.get("PATH", ""):
    os.environ["PATH"] = f"{VITASDK_BIN}:{os.environ.get('PATH', '')}"

def parse_target_address(target_str, base=0):
    val = int(target_str, 16) if target_str.startswith("0x") else int(target_str, 10)
    if val >= 0x80000000 and base > 0:
        return val - base
    return val

def disassemble(so_path, target_offset, count=32, thumb=True, stop_addr=None):
    if not os.path.exists(so_path):
        print(f"[-] Error: Archivo {so_path} no encontrado.")
        return

    start_addr = target_offset
    if stop_addr is None:
        stop_addr = start_addr + (count * 2 if thumb else count * 4)

    mode = ["-M", "force-thumb"] if thumb else ["-M", "force-arm"]
    cmd = [
        "arm-vita-eabi-objdump", "-d",
        *mode,
        f"--start-address={hex(start_addr)}",
        f"--stop-address={hex(stop_addr)}",
        so_path
    ]

    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[-] Error en objdump: {res.stderr}")
            return
        
        print("=" * 80)
        print(f" Desensamblado de {so_path} en [0x{start_addr:x} .. 0x{stop_addr:x}] (Mode: {'Thumb' if thumb else 'ARM'})")
        print("=" * 80)
        for line in res.stdout.splitlines():
            if ":" in line:
                print(line)
    except Exception as e:
        print(f"[-] Error al ejecutar objdump: {e}")

def main():
    parser = argparse.ArgumentParser(description="Desensamblador rápido para .so de PS Vita / Android")
    parser.add_argument("addr", help="Dirección virtual (0x98...) u offset (0x000...) a desensamblar")
    parser.add_argument("--so", default="ux0_data/illusia2/libgameDSO.so", help="Ruta al archivo .so")
    parser.add_argument("--base", default="0x98000000", help="Dirección base en memoria (por defecto 0x98000000)")
    parser.add_argument("-n", "--count", type=int, default=32, help="Número de instrucciones a mostrar")
    parser.add_argument("--arm", action="store_true", help="Forzar modo ARM en lugar de Thumb")
    parser.add_argument("--stop", help="Dirección final opcional")

    args = parser.parse_args()
    base_val = int(args.base, 16)
    offset = parse_target_address(args.addr, base_val)
    stop_val = parse_target_address(args.stop, base_val) if args.stop else None

    disassemble(args.so, offset, args.count, not args.arm, stop_val)

if __name__ == "__main__":
    main()
