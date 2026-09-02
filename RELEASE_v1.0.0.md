# ILLUSIA 2 (PS Vita) — v1.0.0

First full release of the ILLUSIA 2 PS Vita port. Since the `v0.1.0-beta` build, the boot flow, terrain/collision, save system, and the entire audio pipeline went through multiple rounds of log-driven, disassembly-verified fixes. The game now boots, saves/loads, and plays through cutscenes and normal gameplay with the terrain, NPCs, player, and audio all behaving correctly.

## ✅ What works
- Full boot flow: Title → Main Menu → Cutscenes → In-game, no startup crashes.
- Graphics via vitaGL, touch input + physical buttons (D-Pad, X/O/□/Δ, L/R, analog stick), input mapped to the Nexus2/Illusia HAL.
- Terrain, NPCs, and the player character load and render correctly, both in normal gameplay and in scrolling-background (ScrollBG) cutscenes.
- Movement and collision match the original engine's behavior (no falling through platforms, no false "solid tile" lockups, no infinite cutscene loops).
- Save games write to and load from `ux0:data/illusia2/saves/`, including a fix for a struct layout bug that made the save-size check silently fail.
- Audio: multi-channel mixer on a dedicated thread (`Tremor`/`libvorbisidec`) via `SceAudioOut`, with looping ambient/BGM correctly separated from one-shot sound effects, a soft limiter instead of hard clipping under heavy simultaneous sounds, and correct sound-id resolution (see below).
- LiveArea (`icon0`/`bg0`/`pic0`/`startup`) and VPK packaging complete.

## 🔧 Major fixes since v0.1.0-beta

### Collision & cutscenes
- Removed a fabricated "prefer collision layer 1" heuristic that made the player fall through platforms or get stuck on the wrong layer's tiles on any map with more than one layer — verified against real disassembly that the original engine always reads layer 0 for collision.
- Fixed the infinite cutscene loop (map oscillating between `SCROLLBG`/`NORMAL`) caused by a bad "virtual floor" formula that made scrollBG-only maps (which ship with no real collision data) either solid everywhere or bottomless — now uses a fixed virtual-floor row matching the original engine's screen geometry.
- Fixed a crash in scripted cutscene events (`CanMove` reading unallocated collision memory) by injecting a safe, temporary all-zero collision buffer only for the duration of that call.

### Save system
- Fixed `getAbsolueFilePath`/`getVersion`/`getPlayerName` returning a Dalvik array object instead of a real string, which made native saves silently write to the Vita's read-only root instead of `ux0:data/illusia2/saves/`.
- Found and fixed a struct-layout bug in the SoLoader's `stat64_bionic` (Android `stat()` emulation): `nlink_t` is 4 bytes on Android/Bionic but 2 bytes on the Vita's `newlib`, shifting every field after it and making the reported file size always read `0` — this made save/load validation fail with a bogus "file is stale/corrupt" error even though the file was written correctly. Rebuilt the struct with fixed-width types and exact Bionic ABI padding.
- Bypassed a per-slot save-summary check (`CMvSystemMenu`) that produced an "Abnormal save file" popup even for freshly-written saves.

### Audio
- Removed an invented "loop if id >= 100" rule that had no basis in the real engine and made one-shot sounds with high ids loop forever.
- Split the "main track" audio channel into two independent voices — one for looping ambient/BGM, one for one-shot cues — matching the real engine's `mBgmPlayer`/`mPlayer` split. Previously, any scene-transition sound effect would kill and replace the currently playing ambient music.
- Replaced the mixer's hard clamp-to-range with a soft saturation curve, eliminating audible distortion when several sounds overlap (e.g. during combat).
- **Root cause of "wrong/missing music" found and fixed**: the game never sends a real sound file number over its JNI sound-play bridge — it sends the *position* of that sound within an internal 84-entry registered-sound list, which only worked by accident on real Android because the resource compiler (`aapt`) numbers `res/raw/*.ogg` resources in the same order. The port was treating that position as a literal file number, so it played the wrong file for every sound past the first gap in the numbering. The port now rebuilds the same translation table from the actual sound files and resolves the correct file every time.

## Notes
Report any crash or odd behavior along with the matching log (`ux0:data/illusia2/logs/`) so work can keep iterating.

**Commit**: `4311b72`
