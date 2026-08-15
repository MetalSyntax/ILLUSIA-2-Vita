# Plan de Port — ILLUSIA 2 (PS Vita)

> Documento de Arquitectura, Análisis Estático Verificado de `libgameDSO.so`, `libARMPlugin.so` y `decompiled/apk_jadx/`, Mapeo de Símbolos JNI y Hoja de Ruta para el port de **ILLUSIA 2** (Android) a **PS Vita**.

---

## 0. Contexto y Estrategia General

* **Juego:** ILLUSIA 2 (일루시아 2)
* **Desarrollador / Publisher:** Gamevil
* **Paquete Java:** `com.gamevil.illusia2.global` (motor: `com.gamevil.nexus2`)
* **Activity Principal:** `com.gamevil.illusia2.global.SkeletonLauncher`
* **Librerías Nativas:**
  * `libgameDSO.so` (1.44 MB, arquitectura ARM 32-bit `armeabi`) — Motor principal Gamevil Nexus2
  * `libARMPlugin.so` (11 KB, arquitectura ARM 32-bit `armeabi`) — Plugin SKT DRM / Anti-tamper
* **TITLEID asignado:** `ILLUSIA02`
* **Ruta de Datos en PS Vita:** `ux0:data/illusia2/`
* **Ruta de la Librería:** `ux0:data/illusia2/libgameDSO.so`
* **Ruta de Guardado:** `ux0:data/illusia2/saves/`

### Arquitectura del Motor (Gamevil Nexus2)
1. **Motor Compartido:** ILLUSIA 2 utiliza el motor **Gamevil Nexus2**, la misma familia de motor presente en Zenonia 2/3/4 y Advena.
2. **Ciclo de Vida JNI:**
   * `Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef`: Configura punteros de JNIEnv y referencias a la clase `Natives`.
   * `Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo`: Registra resolución de pantalla (`width`, `height`), DPI, paths y modelo de dispositivo.
   * `Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize`: Inicializa búferes gráficos y audio.
   * `Java_com_gamevil_nexus2_Natives_NativeRender`: Ejecuta un tick del motor y dibuja la escena.
   * `Java_com_gamevil_nexus2_Natives_handleCletEvent`: Procesa eventos de entrada táctil y teclado físico.
   * `Java_com_gamevil_nexus2_Natives_NativePauseClet` / `NativeResumeClet` / `NativeDestroyClet`: Maneja el ciclo de vida de la aplicación.

---

## 1. Detección de Arquitectura y Gráficos

* **Arquitectura Binaria:** ARMv5TE / ARMv6 (`armeabi`), código Thumb/ARM. Compatible de forma nativa con el CPU Cortex-A9 de la PS Vita en modo `softfp`.
* **Gráficos:** Gamevil Nexus2 2D Engine (GLES1 / Blit de framebuffer / texturas vitaGL).
* **Resolución PS Vita:** 960 x 544.

---

## 2. Catálogo de Símbolos JNI Exportados (`libgameDSO.so`)

| Símbolo Exportado | Firma JNI / Descripción |
|---|---|
| `Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef` | Inicializa referencias globales JNI |
| `Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo` | Envía resolución, DPI y datos del dispositivo |
| `Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize` | Inicializa motor con tamaño de búfer de pantalla |
| `Java_com_gamevil_nexus2_Natives_NativeRender` | Renderiza el frame actual |
| `Java_com_gamevil_nexus2_Natives_handleCletEvent` | Envía eventos de touch / keys |
| `Java_com_gamevil_nexus2_Natives_NativePauseClet` | Pausa el ciclo del motor |
| `Java_com_gamevil_nexus2_Natives_NativeResumeClet` | Reanuda el ciclo del motor |
| `Java_com_gamevil_nexus2_Natives_NativeDestroyClet` | Limpia recursos al salir |
| `Java_com_gamevil_nexus2_Natives_NativeResize` | Maneja cambios de resolución |
| `Java_com_gamevil_nexus2_Natives_NativeAsyncTimerCallBack` | Callback de timers asíncronos |
| `Java_com_gamevil_nexus2_Natives_NativeAsyncTimerCallBackTimeStemp` | Callback de timers con timestamp |
| `Java_com_gamevil_nexus2_Natives_NativeResponseIAP` | Respuesta de compras In-App |
| `Java_com_gamevil_nexus2_Natives_NativeHandleInAppBiiling` | Manejo de facturación In-App |
| `Java_com_gamevil_nexus2_Natives_NativeNetTimeOut` | Timeout de red |
| `Java_com_gamevil_nexus2_Natives_NativeIsNexusOne` | Detección de dispositivo |
| `Java_com_gamevil_nexus2_Natives_NativeGetPublicKey` | Clave pública de verificación |

---

## 3. Guía de Decompilación Reproducible

```bash
# 1. Java del APK
jadx -d "decompiled/apk_jadx" "ILLUSIA-2-v1-0-3.apk"

# 2. Decompilar .so con Ghidra y Angr (Docker devrvk/so-decompiler)
docker run --rm --platform linux/amd64 -v "/Volumes/Seagate/PSVITA Develop:/app" devrvk/so-decompiler \
  decompile "/app/ILLUSIA-2-Vita/ILLUSIA-2-v1-0-3/lib/armeabi/libgameDSO.so" \
  "/app/ILLUSIA-2-Vita/decompiled/libgameDSO_armeabi/ghidra"

docker run --rm --platform linux/amd64 -v "/Volumes/Seagate/PSVITA Develop:/app" devrvk/so-decompiler \
  decompile "/app/ILLUSIA-2-Vita/ILLUSIA-2-v1-0-3/lib/armeabi/libARMPlugin.so" \
  "/app/ILLUSIA-2-Vita/decompiled/libARMPlugin_armeabi/ghidra"
```

Re-ejecutable en cualquier momento con `porting_tools/build/decompile_all.sh`.

---

## 4. Fases del Port (Checklist)

- [x] Repo creado, `git init`, `.gitignore` anti-DMCA.
- [x] `porting_tools/` adaptado a ILLUSIA 2 (`manage_vita.py`, `build.sh`, `build_and_install.sh`, etc.).
- [x] Skills instaladas en `.agents/skills/`, `.claude/skills/` y configuración del usuario (`psvita-porting`, `so-crash-triage`).
- [x] APK decompilado (`decompiled/apk_jadx/`) y `.so` decompilados (`decompiled/libgameDSO_armeabi/ghidra/`, `decompiled/libARMPlugin_armeabi/ghidra/`).
- [x] **Fase 1: Configuración del Loader:** Mapeo de `libgameDSO.so` con `so_util`, relocalización, tabla de dynlib / stubs libc y compilación exitosa de `build/illusia2.vpk`.
- [ ] **Fase 2: Tabla JNI FalsoJNI:** Implementación de callbacks de audio, input, file I/O y emulación de `com/gamevil/nexus2/Natives`.
- [ ] **Fase 3: Renderizado Gráfico:** Pipeline de render con `vitaGL` para el framebuffer de Nexus2.
- [ ] **Fase 4: Input Táctil y Botones Físicos:** Mapeo de controles Vita a eventos táctiles y teclado virtual.
- [ ] **Fase 5: Audio:** Reproducción de SFX y BGM (`res/raw/*.ogg`).
- [ ] **Fase 6: LiveArea & VPK:** Empaquetado final y pruebas en hardware real.
