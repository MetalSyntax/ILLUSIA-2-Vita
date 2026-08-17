#!/usr/bin/env python3
"""
analyze_crash.py - PS Vita Crash Dump (.psp2dmp) Analyzer
==========================================================
Analiza volcados de memoria (.psp2dmp) generados por la PS Vita / kubridge
e identifica la causa raíz, hilos, registros, desensamblado en PC/LR y backtrace.
"""

import os
import sys
import subprocess
import argparse
import glob
from collections import defaultdict, Counter

VITASDK_BIN = os.environ.get("VITASDK", "/Users/metalsyntax/vitasdk") + "/bin"
if os.path.exists(VITASDK_BIN) and VITASDK_BIN not in os.environ.get("PATH", ""):
    os.environ["PATH"] = f"{VITASDK_BIN}:{os.environ.get('PATH', '')}"

VITA_PARSE_CORE_DIR = os.environ.get("VITA_PARSE_CORE_DIR", "/Users/metalsyntax/vita-tools/vita-parse-core")
if os.path.exists(VITA_PARSE_CORE_DIR) and VITA_PARSE_CORE_DIR not in sys.path:
    sys.path.insert(0, VITA_PARSE_CORE_DIR)

try:
    from core import CoreParser
    from elf import ElfParser
except ImportError:
    CoreParser = None
    ElfParser = None

STR_STOP_REASON = defaultdict(lambda: "Razón desconocida", {
    0: "No reason (Ejecución normal)",
    0x30002: "Undefined instruction exception (Instrucción no válida / corrupta)",
    0x30003: "Prefetch abort exception (Fallo al buscar instrucción en memoria)",
    0x30004: "Data abort exception (Fallo de acceso a memoria / Puntero nulo o inválido)",
    0x60080: "Division by zero (División por cero en operación entera)",
})

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

class SymbolTable:
    def __init__(self, so_path):
        self.so_path = so_path
        self.symbols = []
        self.load_symbols()

    def load_symbols(self):
        if not self.so_path or not os.path.exists(self.so_path):
            return
        cmd = ["arm-vita-eabi-objdump", "-T", self.so_path]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                cmd = ["arm-vita-eabi-nm", "-D", self.so_path]
                res = subprocess.run(cmd, capture_output=True, text=True)
                for line in res.stdout.splitlines():
                    parts = line.split()
                    if len(parts) >= 3:
                        try:
                            addr = int(parts[0], 16)
                            raw = parts[2]
                            self.symbols.append((addr, addr + 4, demangle(raw), raw))
                        except ValueError:
                            pass
            else:
                for line in res.stdout.splitlines():
                    parts = line.split()
                    if len(parts) >= 6 and parts[1] in ['g', 'l'] and 'F' in parts[2]:
                        try:
                            addr = int(parts[0], 16)
                            size = int(parts[4], 16)
                            raw_name = parts[-1]
                            self.symbols.append((addr, addr + size, demangle(raw_name), raw_name))
                        except ValueError:
                            continue
            self.symbols.sort(key=lambda x: x[0])
        except Exception as e:
            print(f"[!] Warning: No se pudieron extraer símbolos de {self.so_path}: {e}")

    def lookup(self, offset):
        for start, end, demangled, raw in self.symbols:
            if start <= offset < end or (start <= offset <= end + 4 and end > start):
                return f"{demangled} + 0x{offset - start:x}"
        prev = None
        for start, end, demangled, raw in self.symbols:
            if start <= offset:
                prev = (start, demangled)
            else:
                break
        if prev:
            return f"{prev[1]} + 0x{offset - prev[0]:x}"
        return f"0x{offset:x}"

def disassemble_range(bin_path, start_addr, count=16, is_thumb=True):
    if not bin_path or not os.path.exists(bin_path):
        return []
    mode_flag = ["-M", "force-thumb"] if is_thumb else ["-M", "force-arm"]
    stop_addr = start_addr + (count * 4)
    cmd = [
        "arm-vita-eabi-objdump", "-d",
        *mode_flag,
        f"--start-address={hex(start_addr)}",
        f"--stop-address={hex(stop_addr)}",
        bin_path
    ]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
        lines = []
        for line in res.stdout.splitlines():
            if ":" in line and ("\t" in line or "  " in line):
                lines.append(line)
        return lines[:count]
    except Exception:
        return []

def analyze(dmp_path, elf_path=None, so_path=None, so_base=0x98000000, output_file=None):
    if not CoreParser:
        print("[-] Error: vita-parse-core no está disponible.")
        return

    print(f"[*] Analizando crash dump: {dmp_path}")
    core = CoreParser(dmp_path)
    sym_table = SymbolTable(so_path) if so_path else None

    out = []
    out.append("=" * 80)
    out.append("           PS VITA CRASH DUMP ANALYSIS REPORT (crash_tools)")
    out.append("=" * 80)
    out.append(f" Core Dump (.psp2dmp): {dmp_path}")
    if elf_path: out.append(f" ELF (Vita):            {elf_path}")
    if so_path:  out.append(f" SO (Android):          {so_path}")
    out.append(f" SO Base Address:       0x{so_base:08x}")
    out.append("=" * 80)

    crashed_thread = None
    for thread in core.threads:
        if thread.stop_reason != 0:
            crashed_thread = thread
            break
    if not crashed_thread and core.threads:
        crashed_thread = core.threads[0]

    if not crashed_thread:
        out.append("[-] No se encontraron hilos en el volcado de memoria.")
        report = "\n".join(out)
        print(report)
        return report

    t = crashed_thread
    reason = STR_STOP_REASON[t.stop_reason]
    pc = t.registers.get('pc', 0)
    lr = t.registers.get('lr', 0)
    sp = t.registers.get('sp', 0)

    out.append(f"\n[!] HILO EN CRASH: '{t.name}' (ID: 0x{t.thread_id:x})")
    out.append(f"    Razón: 0x{t.stop_reason:x} ({reason})")
    
    so_pc_offset = pc - so_base if pc >= so_base else None
    so_lr_offset = lr - so_base if lr >= so_base else None

    pc_sym = sym_table.lookup(so_pc_offset) if (sym_table and so_pc_offset is not None) else f"0x{pc:08x}"
    lr_sym = sym_table.lookup(so_lr_offset) if (sym_table and so_lr_offset is not None) else f"0x{lr:08x}"

    out.append(f"    PC: 0x{pc:08x} -> {pc_sym}")
    out.append(f"    LR: 0x{lr:08x} -> {lr_sym}")
    out.append(f"    SP: 0x{sp:08x}")

    out.append("\n--- REGISTROS CPU ---")
    for r in range(13):
        reg_name = f"r{r}"
        val = t.registers.get(reg_name, 0)
        sym_str = ""
        if val >= so_base and sym_table:
            sym_str = f" -> {sym_table.lookup(val - so_base)}"
        out.append(f"    {reg_name.upper():<3}: 0x{val:08x}{sym_str}")
    out.append(f"    SP : 0x{sp:08x}")
    out.append(f"    LR : 0x{lr:08x} -> {lr_sym}")
    out.append(f"    PC : 0x{pc:08x} -> {pc_sym}")

    if so_path and so_pc_offset is not None and so_pc_offset >= 0:
        out.append("\n--- DESENSAMBLADO EN PC (Punto del Crash) ---")
        start_dasm = max(0, so_pc_offset - 16)
        dasm_lines = disassemble_range(so_path, start_dasm, 12, is_thumb=True)
        for line in dasm_lines:
            marker = "==>" if f"{so_pc_offset:x}:" in line.lower() else "   "
            out.append(f" {marker} {line}")

    if so_path and so_lr_offset is not None and so_lr_offset >= 0:
        out.append("\n--- DESENSAMBLADO EN LR (Dirección de Retorno) ---")
        start_dasm = max(0, so_lr_offset - 16)
        dasm_lines = disassemble_range(so_path, start_dasm, 8, is_thumb=True)
        for line in dasm_lines:
            marker = "==>" if f"{so_lr_offset:x}:" in line.lower() else "   "
            out.append(f" {marker} {line}")

    out.append("\n--- STACK BACKTRACE ---")
    stack_data = core.get_stack(t) if hasattr(core, 'get_stack') else None
    if stack_data:
        for idx in range(0, min(len(stack_data), 128), 4):
            addr = sp + idx
            val = int.from_bytes(stack_data[idx:idx+4], byteorder='little')
            sym_str = ""
            if val >= so_base and sym_table:
                sym_str = f" -> [so + 0x{val - so_base:x} -> {sym_table.lookup(val - so_base)}]"
            out.append(f"    SP+0x{idx:03x} (0x{addr:08x}): 0x{val:08x}{sym_str}")

    report = "\n".join(out)
    print(report)

    if output_file:
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(report)
        print(f"\n[+] Reporte guardado en: {output_file}")

    return report

def main():
    parser = argparse.ArgumentParser(description="Analizador de Crash Dumps de PS Vita")
    parser.add_argument("dmp", nargs="?", help="Ruta al archivo .psp2dmp")
    parser.add_argument("--so", default="ux0_data/illusia2/libgameDSO.so", help="Ruta a libgameDSO.so")
    parser.add_argument("--elf", default="build/illusia2.elf", help="Ruta a illusia2.elf")
    parser.add_argument("--base", default="0x98000000", help="Dirección base de memoria del .so (por defecto 0x98000000)")
    parser.add_argument("--out", help="Archivo donde escribir el análisis")
    args = parser.parse_args()

    dmp_file = args.dmp
    if not dmp_file:
        dmp_files = sorted(glob.glob("logs/*.psp2dmp"), key=os.path.getmtime)
        if dmp_files:
            dmp_file = dmp_files[-1]
            print(f"[*] Seleccionado volcado más reciente: {dmp_file}")
        else:
            print("[-] No se encontró ningún archivo .psp2dmp en logs/")
            sys.exit(1)

    so_base = int(args.base, 16)
    out_file = args.out or (dmp_file + ".analysis.txt")
    analyze(dmp_file, args.elf, args.so, so_base, out_file)

if __name__ == "__main__":
    main()
