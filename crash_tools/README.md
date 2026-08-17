# Suite de Herramientas de Triage y Diagnóstico de Crashes para PS Vita

Esta suite contiene todas las herramientas de Python desarrolladas y utilizadas para el diagnóstico rápido, análisis de memoria, desensamblado y corrección de bugs en ports de Android a PS Vita (*so-loader / FalsoJNI*).

---

## 🛠️ Herramientas Incluidas

### 1. `auto_triage.py` (Pipeline de Diagnóstico Integral)
Ejecuta automáticamente el triage completo:
1. Lee las últimas líneas del log más reciente en `logs/`.
2. Analiza el volcado `.psp2dmp` más reciente.
3. Extrae PC, LR, registros CPU y stack trace.
4. Desensambla el punto exacto del crash en `libgameDSO.so`.
5. Busca y muestra la función causante en el código pseudo-C de Ghidra (`out_ghidra.c`).

**Uso:**
```bash
python3 crash_tools/auto_triage.py
```
O especificando archivos específicos:
```bash
python3 crash_tools/auto_triage.py --log logs/game_018.log --dmp logs/Illusia2-psp2core-*.psp2dmp
```

---

### 2. `analyze_crash.py` (Analizador de Core Dumps)
Analiza archivos de volcado de memoria de PS Vita (`.psp2dmp`):
- Muestra el hilo en crash y la razón de parada (Data Abort, Prefetch Abort, etc.).
- Desofusca símbolos de C++ (Demangling de `_Z...`).
- Muestra los registros CPU y resolución de direcciones.
- Desensambla en PC y LR.
- Muestra el stack backtrace anotado con símbolos.

**Uso:**
```bash
python3 crash_tools/analyze_crash.py logs/Illusia2-psp2core-1786908134-0x000362366d-eboot.bin.psp2dmp
```

---

### 3. `so_disasm.py` (Desensamblador Inteligente)
Desensambla cualquier dirección de runtime (0x98...) u offset (0x000...) de una librería `.so`:
- Soporta modo Thumb (por defecto) o modo ARM.
- Calcula offsets a partir de la base de memoria (`--base 0x98000000`).

**Uso:**
```bash
# Desensamblar 30 instrucciones en el offset 0x96530:
python3 crash_tools/so_disasm.py 0x96530 -n 30

# Desensamblar a partir de una dirección virtual de runtime (0x98096530):
python3 crash_tools/so_disasm.py 0x98096530 -n 30
```

---

### 4. `so_symbol_search.py` (Buscador de Símbolos en `.so`)
Busca funciones, clases o mangled names dentro de la tabla de símbolos de la librería `.so`:
- Muestra el offset exacto y el nombre C++ desmangled.

**Uso:**
```bash
python3 crash_tools/so_symbol_search.py DrawMiniMap
python3 crash_tools/so_symbol_search.py CheckRide
python3 crash_tools/so_symbol_search.py OnMove
```

---

### 5. `ghidra_scanner.py` (Buscador en Código Pseudo-C de Ghidra)
Busca funciones o variables dentro de `out_ghidra.c` mostrando el bloque de código C y números de línea exactos.

**Uso:**
```bash
python3 crash_tools/ghidra_scanner.py "CMvMap::DrawMiniMap" -n 50
python3 crash_tools/ghidra_scanner.py "m_pTileData" -n 20
```

---

### 6. `pattern_audit.py` (Auditor de Patrones Peligrosos)
Audita el código decompilado para encontrar patrones de vulnerabilidad o falta de verificación de punteros nulos:
- Desreferencias no verificadas de `layer[0].m_pTileData` (`+0xf0` y `+0xc`).
- Acceso a singletons no verificados.

**Uso:**
```bash
python3 crash_tools/pattern_audit.py
```
