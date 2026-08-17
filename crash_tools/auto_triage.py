#!/usr/bin/env python3
"""
auto_triage.py - Pipeline Automático de Triage de Crashes para PS Vita
=====================================================================
Pipeline integral que combina análisis de dump, desensamblado nativo de .so,
extracción de contexto del log de juego y correlación con el código pseudo-C de Ghidra.
"""

import os
import sys
import glob
import subprocess
import argparse

def run_cmd(cmd):
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res.stdout
    except Exception as e:
        return str(e)

def get_latest_files():
    logs = sorted(glob.glob("logs/*.log"), key=os.path.getmtime)
    dmps = sorted(glob.glob("logs/*.psp2dmp"), key=os.path.getmtime)
    return (logs[-1] if logs else None, dmps[-1] if dmps else None)

def triage(log_path=None, dmp_path=None, so_path="ux0_data/illusia2/libgameDSO.so", ghidra_path="decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c", so_base=0x98000000):
    latest_log, latest_dmp = get_latest_files()
    log_file = log_path or latest_log
    dmp_file = dmp_path or latest_dmp

    print("=" * 80)
    print("      PIPELINE AUTOMÁTICO DE TRIAGE DE CRASH (crash_tools/auto_triage)")
    print("=" * 80)
    print(f" Log del juego:    {log_file}")
    print(f" Volcado de crash: {dmp_file}")
    print(f" SO Android:       {so_path}")
    print("=" * 80)

    if not dmp_file or not os.path.exists(dmp_file):
        print("[-] Error: No se encontró archivo .psp2dmp.")
        return

    # 1. Últimas líneas del log
    if log_file and os.path.exists(log_file):
        print("\n--- ÚLTIMAS LÍNEAS DEL LOG DE JUEGO ---")
        with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
            for l in lines[-15:]:
                print(f"  {l.rstrip()}")

    # 2. Ejecutar analyze_crash.py
    print("\n--- ANÁLISIS DEL VOLCADO DE MEMORIA (.psp2dmp) ---")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    analyze_script = os.path.join(script_dir, "analyze_crash.py")
    cmd = [sys.executable, analyze_script, dmp_file, "--so", so_path, "--base", hex(so_base)]
    report = run_cmd(cmd)
    print(report)

    # 3. Extraer PC del reporte
    pc_offset = None
    func_name = None
    for line in report.splitlines():
        if "PC:" in line and "->" in line:
            parts = line.split("->")
            if len(parts) >= 2:
                sym_part = parts[1].strip()
                if " + " in sym_part:
                    func_name = sym_part.split(" + ")[0].strip()
                else:
                    func_name = sym_part.strip()
        if "PC (Program Counter):" in line:
            pass

    # 4. Correlación con Ghidra
    if func_name and os.path.exists(ghidra_path):
        print("\n--- CÓDIGO PSEUDO-C DE GHIDRA PARA LA FUNCIÓN CAUSANTE ---")
        scanner_script = os.path.join(script_dir, "ghidra_scanner.py")
        cmd = [sys.executable, scanner_script, func_name, "--file", ghidra_path, "-n", "35", "-m", "1"]
        ghidra_res = run_cmd(cmd)
        print(ghidra_res)

    print("\n" + "=" * 80)
    print(" [+] TRIAGE AUTOMÁTICO COMPLETADO.")
    print("=" * 80)

def main():
    parser = argparse.ArgumentParser(description="Pipeline Automático de Triage")
    parser.add_argument("--log", help="Ruta al archivo de log")
    parser.add_argument("--dmp", help="Ruta al archivo .psp2dmp")
    parser.add_argument("--so", default="ux0_data/illusia2/libgameDSO.so", help="Ruta al archivo .so")
    parser.add_argument("--ghidra", default="decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c", help="Ruta al out_ghidra.c")
    parser.add_argument("--base", default="0x98000000", help="Dirección base en memoria")

    args = parser.parse_args()
    triage(args.log, args.dmp, args.so, args.ghidra, int(args.base, 16))

if __name__ == "__main__":
    main()
