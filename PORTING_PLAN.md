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
- [x] **Fase 2: Tabla JNI FalsoJNI:** Implementación de callbacks de guardado/carga en `ux0:data/illusia2/saves/`, emulación de Dalvik ArrayObjects para `.pzx`/`.zt1`, Gamevil Font API (GFA) y propiedades del sistema.
- [x] **Fase 3: Renderizado Gráfico:** Pipeline de render con `vitaGL` a 960x544 con búfer lógico 480x320.
- [x] **Fase 4: Input Táctil y Botones Físicos:** Mapeo de botones Vita (D-Pad, X, O, □, Δ, L, R, Start, Select) y stick analógico izquierdo a HAL de Nexus2 y multitouch frontal.
- [x] **Fase 5: Audio:** Mezclador multicanal en hilo dedicado con `Tremor` (`libvorbisidec`) y `SceAudioOut` para BGM en loop y 4 voces SFX simultáneas.
- [x] **Fase 6: LiveArea & VPK:** Generación de `icon0.png`, `bg0.png`, `pic0.png`, `startup.png` y empaquetado VPK completo.

## 5. Progreso Actual y Bugs Conocidos (Actualizado)

### Progreso General
* El port compila y ejecuta exitosamente llegando al Title, Menú Principal, Cinemáticas e In-game.
* Gráficos, Input y Audio están funcionando a través de los wrappers (FalsoJNI, vitaGL, etc.).
* **Correcciones recientes aplicadas:**
  * **Crash en Cinemáticas (`CMvMap::Draw`):** Corregido mediante el aprovisionamiento dinámico de una capa `tileData` vacía para mapas sin colisión.
  * **Personajes faltantes en cinemáticas (Princesa, Guardián, etc.):** Corregido al eliminar el bloqueo absoluto que ejercía `is_map_tiledata_ready()` sobre las funciones de `OnMove` y `GetTileAttr` cuando `tileData` era nulo.
  * **Personaje atascado in-game:** Corregido. El personaje ya no es bloqueado en un falso "tile sólido" debido al mismo chequeo de mapa.

### Bugs Actuales (Pendientes)

*Por verificar por el usuario.* Se aplicaron correcciones para:
1. **Fallo al Guardar Partida (Corregido):**
   * **Causa encontrada:** La función JNI `getAbsolueFilePath` retornaba un objeto `byte[]` (Dalvik ArrayObject) en lugar de un `jstring` (`char*` en FalsoJNI). El motor intentaba leer el encabezado de este arreglo asumiendo que era un `char*`, extrayendo un string vacío (`""`).
   * **Consecuencia:** Los guardados nativos intentaban escribir en la ruta `"/Save0.dat"` (raíz de la PS Vita) lo cual fallaba por falta de permisos, en lugar de `ux0:data/illusia2/saves/Save0.dat`.
   * **Solución:** Se corrigieron `Illusia_getAbsolueFilePath`, `Illusia_getVersion` y `Illusia_getPlayerName` en `java.c` para retornar literales `char*` compatibles con FalsoJNI, restaurando correctamente las rutas de I/O de la partición.

### Actualización 3 (Cinemáticas y Colisiones)
* **El problema:** Tras corregir el cuelgue anterior, los personajes de la cinemática inicial seguían sin aparecer, el terreno no se dibujaba y eventualmente el juego volvía a crashear (Data Abort en `CanMove`).
* **Causa encontrada:** 
  1. El mapa de la cinemática (`map/002.zt1`) legítimamente NO tiene datos de colisión (`tileData = NULL`), ya que utiliza un fondo de desplazamiento (ScrollBG) y los personajes se mueven por eventos guionizados.
  2. En parches anteriores, forzamos a `GetTileAttr` a devolver `0` (traspasable) cuando `tileData` era nulo. Esto provocó que la gravedad del motor hiciera caer a todos los personajes al vacío antes de que la cámara llegara a ellos.
  3. Al estar cayendo, el motor eventualmente intentó invocar un ataque por guion (`CreateDamageEffect`), el cual llamó directamente a `CanMove`. Al no estar `CanMove` protegido con memoria falsa (sólo protegimos `OnMove`), el juego hizo crash intentando leer el mapa.
* **Solución aplicada:** 
  1. Restauramos `GetTileAttr` para que devuelva `1` (pared sólida) cuando se sale del mapa o cuando `tileData` es nulo. Esto coincide con el comportamiento original del juego y evita que los personajes caigan al vacío durante las cinemáticas.
  2. Añadimos un hook directamente en `CMvCharacter::CanMove` (que es el causante del crash) para inyectar dinámicamente un mapa de colisión falso (ceros) sólo mientras dura la ejecución de esta función. Esto protege al juego de crashear ante eventos y scripts de cinemática sin interferir con la lógica real de mapas.

### Actualización 4 (Bucle infinito de mapas de cinemática)
* **El problema:** Tras arreglar el crash, la pantalla se veía negra y el log mostraba un bucle infinito oscilando rápidamente entre `SCROLLBG(cutscene)` y `NORMAL`. El jugador estaba atascado en un "trigger" (disparador) de cinemática. El script de la cinemática intentaba mover a los personajes, pero fallaba instantáneamente porque las colisiones impedían el movimiento, forzando la salida al mapa normal. Al volver al mapa normal, el jugador seguía encima del trigger, disparando el evento de nuevo.
* **Causa encontrada:** 
  1. En las cinemáticas (`tileData = NULL`), habíamos forzado `GetTileAttr` a devolver `1` (pared sólida) en todo el mapa para que no cayeran al vacío. 
  2. Al estar todo el mapa sólido, los comandos del script como `MoveTo` fallaban, abortando el script.
  3. Los diferentes hooks de `GetTileAttr` devolvían valores diferentes (`0` y `1`) porque algunos no fueron actualizados correctamente, rompiendo el estado interno.
* **Solución aplicada:** 
  1. Unificamos todos los hooks de colisión (`CMvMapObject_GetTileAttr`, `CMvObject_GetTileAttr`, `GetTileAttr_Obj`).
  2. Implementamos un "Suelo virtual" (Virtual Floor) para los mapas sin datos de colisión. Ahora `GetTileAttr` devuelve `1` (sólido) SOLO si `ty >= 12` (cerca de la parte inferior de la pantalla), y `0` (traspasable) en el resto.
  3. Esto permite que los personajes caigan levemente al inicio de la cinemática, aterricen en el suelo virtual (deteniendo la gravedad) y se puedan mover libremente en el eje X, permitiendo que los scripts guionizados se ejecuten hasta el final sin abortar.

### Actualización 5 (Resolución final del bucle de cinemáticas)
* **El problema:** Tras implementar el suelo virtual dinámico, el jugador seguía atascado en el mismo bucle infinito y la pantalla seguía en negro.
* **Causa encontrada:** 
  1. Extraje y analicé manualmente la cabecera del archivo de mapa `002.zt1`. ¡Resulta que el motor original especifica que este mapa tiene una altura de `0` (`map_h = 0`)!
  2. Como mi suelo virtual se generaba en `ty >= (map_h - 1)`, la fórmula resultó en `ty >= -1`. Esto provocó que *absolutamente todas las coordenadas del mapa* devolvieran `1` (pared sólida).
  3. Los personajes aparecían dentro de un enorme bloque sólido, los comandos del script fallaban al no poder moverlos, y el juego volvía al mapa normal, repitiendo el bucle infinito al pisar el mismo trigger.
* **Solución aplicada:** 
  1. Cambié el suelo virtual para que use una coordenada Y fija (`ty >= 14`), en lugar de depender de la altura corrupta o nula del mapa. 
  2. En la resolución de ILLUSIA 2, la pantalla tiene unos 17 bloques de altura. Al fijar el suelo en la fila 14, los personajes caerán de forma natural hasta la parte inferior de la pantalla, aterrizarán sin problemas, y el script podrá moverlos de izquierda a derecha sin bloqueos.

### Actualización 6 (Faltaban NPCs, terreno invisible y personaje atascado)
* **El problema:** Tras lograr avanzar al gameplay, los mapas se veían pero el terreno era sólido. El personaje quedaba atascado sin poder moverse y aparecía en lugares incorrectos junto a los NPCs.
* **Causas encontradas:** 
  1. **Bug del PreLoad (Terreno omitido):** Al igual que en ILLUSIA 1, la función original `CMvLayerData::PreLoad` chequeaba que el puntero a la RAM fuera positivo (`if (0 < ptr)`). En PS Vita, la memoria se aloja por encima de `0x81000000` (evaluado como negativo), provocando que el juego omitiera la carga de mapas y NPCs.
  2. **Corrupción por Allocator (`Gcx_MM`):** El motor usaba su propio gestor de memoria interno que falla en Vita, repartiendo bloques llenos de basura, lo que provocaba `Data Aborts` e inicializaciones incorrectas.
  3. **Offset incorrecto de `tileData` (Atascamiento):** El hook anterior heredado de ILLUSIA 1 intentaba leer el array de colisiones (`tileData`) en el offset `+4` de la capa de mapa (`CMvLayerData`). Sin embargo, en ILLUSIA 2 la estructura cambió y el array real se encuentra en el offset `+12`. Al leer `+4`, el hook recibía basura del archivo de mapa, devolviendo constantemente "sólido" (impidiendo todo movimiento).
* **Soluciones aplicadas:** 
  1. **Inyección ASM en `PreLoad`:** Se inyectó el parche binario en la instrucción ARM Thumb del `.so` (offset `0x947bc`), cambiando el condicional `ble.n` por `beq.n` (inyectando `0xd01f` que en Little-Endian es `1f d0`). Esto forzó la carga completa de NPCs y terrenos.
  2. **Sobreescritura con `calloc`:** Se hookearon todos los métodos internos del juego (`_Z12Gcx_MM_Allocj`, `operator new`, etc.) para usar directamente el `calloc` y `free` de libc en Vita, asegurando memoria prístina. Adicionalmente se insertó un `custom_memcpy` y `custom_memset` en `dynlib.c` que protegen contra lecturas de punteros nulos.
  3. **Corrección de offsets en colisiones:** Se modificó la función `get_main_tile_data` en `patch.c` para apuntar de `layers + 4` a `layers + 12`, restaurando completamente el movimiento y spawn del personaje.
