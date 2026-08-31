# ILLUSIA 2 (PS Vita) — v0.1.0-beta

First playable **beta** build of the ILLUSIA 2 PS Vita port: boots without crashing, loads terrain/NPCs/player on the main map, saves to the correct location, and runs graphics/input/audio through the native wrappers (vitaGL, FalsoJNI, Tremor). It's still **beta** because collision and cutscene bugs remain to be confirmed on physical hardware.

## ✅ What works
- Full boot flow: Title → Main Menu → Cutscenes → In-game, no startup crashes.
- Graphics via vitaGL, touch input + physical buttons (D-Pad, X/O/□/Δ, L/R, analog stick), input mapped to the Nexus2/Illusia HAL.
- Audio: multi-channel mixer on a dedicated thread (`Tremor`/`libvorbisidec`) with looping BGM and 4 simultaneous SFX voices via `SceAudioOut`.
- Terrain, NPCs, and the player character now load and render correctly on the main map (fix for the `PreLoad` bug + `calloc`-based allocator + collision offset fix).
- Save games now write to `ux0:data/illusia2/saves/` (previously wrote to the Vita's root and failed due to permissions).
- LiveArea (`icon0`/`bg0`/`pic0`/`startup`) and VPK packaging complete.

## 🐛 Known bugs (why this is still beta)
- **Terrain collision** (fix just applied, not yet confirmed on console): fixed a read from the wrong collision-array (`tileData`) layer that let the player walk through platforms or get stuck against terrain that shouldn't have been solid.
- **Cutscene positioning**: some objects/characters in scrolling-background scenes (ScrollBG, maps with no collision data of their own) can appear out of position or off-screen.
- Possible occasional instability during transitions between cutscenes and the normal map, still under active investigation.

## Notes
This build hasn't been tested on physical hardware beyond the log session that prompted these fixes — please report any crash or odd behavior along with the matching log (`ux0:data/illusia2/logs/`) so work can keep iterating.

**Commit**: `68db1db`
