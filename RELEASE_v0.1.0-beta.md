# ILLUSIA 2 (PS Vita) — v0.1.0-beta

Primera build **beta** jugable del port de ILLUSIA 2 a PS Vita: arranca sin crashes, carga terreno/NPCs/personaje en el mapa principal, guarda partida en la ruta correcta y corre gráficos/input/audio a través de los wrappers nativos (vitaGL, FalsoJNI, Tremor). Sigue en **beta** porque hay bugs de colisión y cinemáticas todavía por confirmar en consola física.

## ✅ Qué funciona
- Arranque completo: Title → Menú Principal → Cinemáticas → In-game, sin crashes de arranque.
- Gráficos vía vitaGL, input táctil + botones físicos (D-Pad, X/O/□/Δ, L/R, stick analógico) e input mapeado al HAL de Nexus2/Illusia.
- Audio: mezclador multicanal en hilo dedicado (`Tremor`/`libvorbisidec`) con BGM en loop y 4 voces SFX simultáneas vía `SceAudioOut`.
- Terreno, NPCs y personaje se cargan y renderizan correctamente en el mapa principal (fix del bug de `PreLoad` + allocator `calloc` + offset de colisión).
- Guardado de partida en `ux0:data/illusia2/saves/` (antes escribía en la raíz de la Vita y fallaba por permisos).
- LiveArea (`icon0`/`bg0`/`pic0`/`startup`) y empaquetado VPK completos.

## 🐛 Bugs conocidos (por qué sigue siendo beta)
- **Colisión de terreno** (fix recién aplicado, aún no confirmado en consola): se corrigió una lectura de la capa equivocada del array de colisión (`tileData`) que hacía que el personaje atravesara plataformas o quedara atascado contra terreno que no debía ser sólido.
- **Posicionamiento en cinemáticas**: algunos objetos/personajes en escenas con fondo de desplazamiento (ScrollBG, mapas sin datos de colisión propios) pueden aparecer fuera de posición o fuera de pantalla.
- Posible inestabilidad puntual en transiciones entre cinemática y mapa normal, aún bajo investigación activa.

## Notas
Build sin probar en hardware físico más allá de la sesión de logs que motivó estos fixes — se recomienda reportar cualquier crash o comportamiento raro con el log correspondiente (`ux0:data/illusia2/logs/`) para seguir iterando.

**Commit**: `68db1db`
