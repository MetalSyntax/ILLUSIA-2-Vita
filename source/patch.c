
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kubridge.h>
#include <so_util/so_util.h>
#include "utils/logger.h"

#include <stdint.h>
#include <stdlib.h>



static void *hooked_Gcx_MM_Alloc(uint32_t size) {
    return calloc(1, size);
}
static void *hooked_Gcx_MM_Calloc(uint32_t size) {
    return calloc(1, size);
}
static void hooked_Gcx_MM_Free(void *ptr) {
    if (ptr) free(ptr);
}
static void *hooked_Gcx_MM_Realloc(void *ptr, uint32_t size) {
    return realloc(ptr, size);
}
static void *hooked_operator_new(uint32_t size) {
    return calloc(1, size);
}
static void *hooked_operator_new_array(uint32_t size) {
    return calloc(1, size);
}
static void hooked_operator_delete(void *ptr) {
    if (ptr) free(ptr);
}
static void hooked_operator_delete_array(void *ptr) {
    if (ptr) free(ptr);
}

extern so_module so_mod;

void patch_symbol(const char *symbol, uintptr_t dest) {
    uintptr_t sym = (uintptr_t)so_symbol(&so_mod, symbol);
    if (sym) hook_thumb(sym | 1, dest);
}


#define SO_CONTINUE(ret, h, ...) ({ \
  kuKernelCpuUnrestrictedMemcpy((void *)h.addr, h.orig_instr, sizeof(h.orig_instr)); \
  kuKernelFlushCaches((void *)h.addr, sizeof(h.orig_instr)); \
  ret _ret = 0; \
  if (h.thumb_addr) { \
      _ret = ((ret(*)())h.thumb_addr)(__VA_ARGS__); \
  } else { \
      _ret = ((ret(*)())h.addr)(__VA_ARGS__); \
  } \
  kuKernelCpuUnrestrictedMemcpy((void *)h.addr, h.patch_instr, sizeof(h.patch_instr)); \
  kuKernelFlushCaches((void *)h.addr, sizeof(h.patch_instr)); \
  _ret; \
})

#define SO_CONTINUE_VOID(h, ...) do { \
  kuKernelCpuUnrestrictedMemcpy((void *)h.addr, h.orig_instr, sizeof(h.orig_instr)); \
  kuKernelFlushCaches((void *)h.addr, sizeof(h.orig_instr)); \
  if (h.thumb_addr) { \
      ((void(*)())h.thumb_addr)(__VA_ARGS__); \
  } else { \
      ((void(*)())h.addr)(__VA_ARGS__); \
  } \
  kuKernelCpuUnrestrictedMemcpy((void *)h.addr, h.patch_instr, sizeof(h.patch_instr)); \
  kuKernelFlushCaches((void *)h.addr, sizeof(h.patch_instr)); \
} while (0)



static uint8_t *get_main_tile_data(void *map) {
    if (!map) return NULL;
    void *layers = *(void **)((char *)map + 0xf0);
    if (!layers) return NULL;
    /* Real ARM disassembly of CMvObject::GetMapAttrib, CMvObject::GetTileAttr
     * (FUN_000b31ec) and CMvMapObject::GetTileAttribute (FUN_000cc128) all
     * unconditionally read the tile-attribute array from `layers + 0xc`
     * (layer 0) — none of them ever look at layer_count or index into a
     * second layer at `layers + 24 + 12`. A previous "prefer layer 1"
     * heuristic here had no basis in the real code and fed movement/
     * collision queries the wrong layer's data for any map with more than
     * one layer, making terrain read as solid. Always use layer 0. */
    uint8_t *l0 = *(uint8_t **)((char *)layers + 12);
    if (l0 && (uintptr_t)l0 >= 0x10000) return l0;
    return NULL;
}

static so_hook hook_map_draw;
static so_hook hook_create_minimap;
static so_hook hook_layer_draw;
static so_hook hook_draw_numbers;
static so_hook hook_draw_damage;
static so_hook hook_num_draw;
static so_hook hook_draw_damage_obj;
static so_hook hook_draw_npc_head;
static so_hook hook_load_tile_image;
static so_hook hook_objmgr_draw;
static so_hook hook_is_drawable;
static so_hook hook_check_rect_in_screen;
static so_hook hook_encryptfile_loadbegin;
static so_hook hook_invaliddata_popup;
static so_hook hook_gsfile_save;
static so_hook hook_gsfile_load;
static so_hook hook_gsfsfilesize;
static so_hook hook_mc_fsfileattribute;

static void *(*MC_grpCreateOffScreenFrameBuffer)(int w, int h) = NULL;
static uint16_t (*MC_grpGetPixelFromRGB)(uint8_t r, uint8_t g, uint8_t b) = NULL;

static void hook_thumb_func(uintptr_t addr, uintptr_t dst) {
    if (addr) {
        hook_thumb(addr | 1, dst);
    }
}

/*
 * Direction delta lookup table (dx, dy) for EnumDir (0 to 7):
 * 0: Right ( 1,  0)
 * 1: Left  (-1,  0)
 * 2: Up    ( 0, -1)
 * 3: Down  ( 0,  1)
 * 4: UpRight   ( 1, -1)
 * 5: DownRight ( 1,  1)
 * 6: UpLeft    (-1, -1)
 * 7: DownLeft  (-1,  1)
 */
static const int8_t dir_deltas[8][2] = {
    {  1,  0 },
    { -1,  0 },
    {  0, -1 },
    {  0,  1 },
    {  1, -1 },
    {  1,  1 },
    { -1, -1 },
    { -1,  1 }
};

/*
 * Helper to check if current map layers and tile data are fully loaded and accessible.
 */
static uint8_t *swap_dummy_tile_data(void) {
    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x10000) return NULL;
    char *map = (char *)*pMapSingleton;
    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x10000) return (uint8_t *)-1;
    /* Layer 0 only — see get_main_tile_data() for why. */
    uint8_t **pTileData = (uint8_t **)((char *)layers + 12);

    if (*pTileData) return NULL;

    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);
    if (map_w < 0) map_w = 0;
    if (map_h < 0) map_h = 0;
    int required_size = map_w * map_h * 3 + 16;
    static uint8_t *dummy_tile_data = NULL;
    static int dummy_tile_size = 0;
    if (required_size > dummy_tile_size) {
        if (dummy_tile_data) free(dummy_tile_data);
        dummy_tile_data = calloc(1, required_size);
        dummy_tile_size = required_size;
    }
    *pTileData = dummy_tile_data;
    return dummy_tile_data;
}

static void restore_dummy_tile_data(uint8_t *dummy_ptr) {
    if (!dummy_ptr) return;
    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    char *map = (char *)*pMapSingleton;
    void *layers = *(void **)(map + 0xf0);
    if (!layers) return;
    uint8_t **pTileData = (uint8_t **)((char *)layers + 12);
    if (*pTileData == dummy_ptr) *pTileData = NULL;
}

/*
 * Hook for GsPZxSubFrame(CGxPZxFrame *pFrame, int subframe_index)
 * _Z13GsPZxSubFrameP11CGxPZxFramei
 */
static int gpzxsf_log_count = 0;
#define GPZXSF_LOG_MAX 50

static void *hooked_GsPZxSubFrame(void *pFrame, int subframe_index) {
    /* Original GsPZxSubFrame has NO range check at all (just pFrame->subframes
     * + index*0x18) — this hook's [0x10000,0xa0000000) window was added
     * defensively and no caller in the .so was written to expect a NULL
     * back from it. If real sprite/animation frame pointers ever legitimately
     * fall outside this window, every caller silently draws nothing instead
     * of crashing — reported as "static cutscenes / missing graphics", not a
     * crash. Logging (rate-limited) instead of widening the range blind,
     * since the same window is confirmed correct for CMvMap/CMvLayer
     * pointers (rendering works) and may simply be a different case here. */
    if (!pFrame || (uintptr_t)pFrame < 0x10000 || (uintptr_t)pFrame == 0xFFFFFFFF) {
        if (gpzxsf_log_count < GPZXSF_LOG_MAX) {
            gpzxsf_log_count++;
            game_log("[GsPZxSubFrame] out-of-range pFrame=%p (idx=%d)\n", pFrame, subframe_index);
        }
        return NULL;
    }
    uintptr_t subframes = *(uintptr_t *)((char *)pFrame + 8);
    if (!subframes || subframes < 0x10000 || subframes == 0xFFFFFFFF) {
        if (gpzxsf_log_count < GPZXSF_LOG_MAX) {
            gpzxsf_log_count++;
            game_log("[GsPZxSubFrame] out-of-range subframes=%p (pFrame=%p idx=%d)\n", (void *)subframes, pFrame, subframe_index);
        }
        return NULL;
    }
    if (subframe_index < 0) {
        return NULL;
    }
    return (void *)(subframes + subframe_index * 24);
}

/*
 * Hook for CMvGraphics::DrawDamage()
 * _ZN11CMvGraphics10DrawDamageEiiiiii10enumDrawOPi
 */
static void hooked_CMvGraphics_DrawDamage(void *self, int font_idx, int x, int y, int amount,
                                         int param_5, int param_6, int draw_op, int param_8) {
    if (!self || font_idx < 0 || amount < 0) return;

    void **pResourceMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI14CMvResourceMgrE13ms_pSingletonE");
    if (!pResourceMgrSingleton || !*pResourceMgrSingleton) return;
    char *resMgr = (char *)*pResourceMgrSingleton;

    void *pCommonRes = *(void **)(resMgr + 4);
    if (!pCommonRes) return;

    void *pResInfoArray = *(void **)((char *)pCommonRes + 0x10);
    if (!pResInfoArray) return;

    void *pNumberImage = *(void **)((char *)pResInfoArray + 5 * 12 + 4);
    if (!pNumberImage) return;

    void *pAni = *(void **)((char *)pNumberImage + 8);
    if (!pAni) return;

    void *pFrameMgr = *(void **)((char *)pAni + 12);
    if (!pFrameMgr) return;

    int frameCount = *(int *)((char *)pFrameMgr + 12);
    void **ppFrames = *(void ***)((char *)pFrameMgr + 8);
    if (!ppFrames || frameCount <= 0) return;

    if (font_idx >= frameCount) {
        font_idx = 0;
    }
    void *pFrame = ppFrames[font_idx];
    if (!pFrame || (uintptr_t)pFrame < 0x10000 || (uintptr_t)pFrame == 0xFFFFFFFF) return;

    char str[32];
    snprintf(str, sizeof(str), "%d", amount);
    int len = strlen(str);
    if (len <= 0) return;

    int cur_x = 0;
    int (*RandomFunc)(int) = (int (*)(int))so_symbol(&so_mod, "_Z6Randoml");

    for (int i = 0; i < len; i++) {
        int digit = (unsigned char)str[i] - '0';
        void *pSubFrame = hooked_GsPZxSubFrame(pFrame, digit);
        if (pSubFrame) {
            void *pSprite = *(void **)pSubFrame;
            if (pSprite && (uintptr_t)pSprite >= 0x10000 && (uintptr_t)pSprite != 0xFFFFFFFF) {
                void (*sprite_draw)(void *, int, int, int, int, int) = *(void **)(*(int *)pSprite + 0x14);
                int (*sprite_width)(void *) = *(int (**)(void *))(*(int *)pSprite + 0x1c);
                int r = RandomFunc ? RandomFunc(param_5 * 2 + 1) : 0;
                int dy = (y - param_5) + r;
                if (sprite_draw) {
                    sprite_draw(pSprite, cur_x + x, dy, draw_op, param_8, 0);
                }
                int w = sprite_width ? sprite_width(pSprite) : 0;
                cur_x += param_6 + w;
            }
        }
    }
}

/*
 * Hook for CMvGraphics::DrawNumbers()
 * _ZN11CMvGraphics11DrawNumbersEiiiPKcijbb
 */
static int hooked_CMvGraphics_DrawNumbers(void *self, int font_idx, int x, int y, const char *str,
                                         int param_5, uint32_t param_6, int param_7, int param_8) {
    if (!self || !str) return 0;
    return SO_CONTINUE(int, hook_draw_numbers, self, font_idx, x, y, str, param_5, param_6, param_7, param_8);
}

/*
 * Hook for CMvNumberObject::Draw()
 * _ZN15CMvNumberObject4DrawEv
 */
static void hooked_CMvNumberObject_Draw(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    SO_CONTINUE_VOID(hook_num_draw, self);
}

/*
 * Hook for CMvObjectMgr::DrawDamageObject()
 * _ZN12CMvObjectMgr16DrawDamageObjectEv
 */
static void hooked_CMvObjectMgr_DrawDamageObject(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    SO_CONTINUE_VOID(hook_draw_damage_obj, self);
}

/*
 * Hook for CMvObjectMgr::DrawNpcHeadInfo()
 * _ZN12CMvObjectMgr15DrawNpcHeadInfoEv
 */
static void hooked_CMvObjectMgr_DrawNpcHeadInfo(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    SO_CONTINUE_VOID(hook_draw_npc_head, self);
}

/*
 * Hook for CMvObject::GetMapAttrib(int tx, int ty, EnumDir dir, int step)
 * _ZN9CMvObject12GetMapAttribEii7EnumDiri
 */
static uint16_t hooked_CMvObject_GetMapAttrib(void *self, int tx, int ty, int dir, int step) {
    if (tx == -1 && self && (uintptr_t)self >= 0x10000) {
        tx = *(uint8_t *)((char *)self + 0x26);
    }
    if (ty == -1 && self && (uintptr_t)self >= 0x10000) {
        ty = *(uint8_t *)((char *)self + 0x27);
    }

    int target_x = tx;
    int target_y = ty;
    if (dir != -1 && dir >= 0 && dir < 8) {
        target_x += (int)dir_deltas[dir][0] * step;
        target_y += (int)dir_deltas[dir][1] * step;
    }

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x10000) return 1;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) {
        return (target_y >= 14) ? 1 : 0;
    }

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x10000) {
        return (target_y >= 14) ? 1 : 0;
    }

    uint8_t *tileData = get_main_tile_data(map);
    if (!tileData || (uintptr_t)tileData < 0x10000) {
        /* FIX (2026-08-30): this null-tileData case previously always
         * returned 0 ("empty, no floor here") with no fallback, unlike the
         * sibling hooked_CMvMapObject_GetTileAttr/hooked_CMvObject_GetTileAttr/
         * hooked_CMvObjectMgr_GetTileAttr_Obj hooks (same file), which already
         * use this exact ty>=14 virtual-floor convention for the identical
         * null-tileData case (ScrollBG-only cutscene maps with no real tile
         * grid, e.g. map_id=2/92). CMvObject::UpdateFalling/DoFallProc/
         * CMvMapObject::DoUpdateFalling all gate "keep applying gravity" on
         * this function ever reporting a non-zero (uVar & 0xf)/(uVar & 0x30)
         * bit -- returning 0 unconditionally meant gravity NEVER found a
         * floor for these maps, so any object whose vertical movement is
         * driven through DoFallProc/UpdateFalling (not just the tile-render
         * path) fell forever. Confirmed live via game_046.log: a falling
         * object's CheckRectInScreen rect.y0 grew monotonically frame after
         * frame (437 -> 1642 px, never landing) for exactly this map. Return
         * the same bit-0 "floor" value the other hooks use once target_y
         * reaches the virtual floor row, so UpdateFalling's floor-detection
         * loop (`uVar1 & 0xf`) actually fires and calls SetWorldPosY to land
         * the object instead of falling indefinitely.
         *
         * SUSPECTED REGRESSION (game_047.log): user reports the character
         * stopped falling (fix confirmed working) but then got stuck unable
         * to move at all. This function is a general-purpose "attribute at
         * (tx,ty) offset by dir/step" query -- it's not just called for
         * straight-down gravity probes, so if landing places the character's
         * own row exactly at the ty>=14 boundary, EVERY subsequent query
         * (including horizontal CanMove-style probes that only vary tx) would
         * also read "solid" here, since this check ignores tx entirely.
         * Not yet confirmed whether this function is actually in the
         * horizontal-movement call path -- logging every call while this
         * branch is active (capped) to find out before changing the
         * condition further. */
        static int map_attrib_floor_log_count = 0;
        if (map_attrib_floor_log_count < 60) {
            map_attrib_floor_log_count++;
            game_log("[GetMapAttrib] tx=%d ty=%d dir=%d step=%d target=(%d,%d) -> %s\n",
                tx, ty, dir, step, target_x, target_y, (target_y >= 14) ? "SOLID(1)" : "empty(0)");
        }
        return (target_y >= 14) ? 1 : 0;
    }

    if (target_x < 0 || target_x >= map_w || target_y < 0 || target_y >= map_h) {
        return 1;
    }

    int tile_idx = (target_y * map_w + target_x) * 3;
    uint8_t f1 = tileData[tile_idx + 1];
    uint8_t f2 = tileData[tile_idx + 2];
    return (uint16_t)f1 | ((uint16_t)f2 << 8);
}

/*
 * Hook for CMvPlayer::CheckRide()
 * _ZN9CMvPlayer9CheckRideEv
 */
static int checkride_log_count = 0;
#define CHECKRIDE_LOG_MAX 50

static int hooked_CMvPlayer_CheckRide(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return 0;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x10000) return 0;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);
    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) return 0;

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x10000) return 0;

    uint8_t *tileData = get_main_tile_data(map);
    if (!tileData || (uintptr_t)tileData < 0x10000) return 0;

    typedef void *(*GetOccupyFixedObjectFunc)(void *this, int x, int y, int dir, int step);
    GetOccupyFixedObjectFunc GetOccupyFixedObject = (GetOccupyFixedObjectFunc)so_symbol(&so_mod, "_ZN12CMvObjectMgr20GetOccupyFixedObjectEii7EnumDiri");
    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    void *pObjMgr = pObjMgrSingleton ? *pObjMgrSingleton : NULL;

    typedef void (*ReleaseRideFunc)(void *this, int b);
    ReleaseRideFunc ReleaseRide = (ReleaseRideFunc)so_symbol(&so_mod, "_ZN9CMvPlayer11ReleaseRideEb");
    typedef void (*SetRideObjectFunc)(void *this, void *obj);
    SetRideObjectFunc SetRideObject = (SetRideObjectFunc)so_symbol(&so_mod, "_ZN16CMvMapObjectRide13SetRideObjectEP9CMvObject");
    typedef void (*CheckRideMoveMapObjFunc)(void);
    CheckRideMoveMapObjFunc CheckRideMoveMapObj = (CheckRideMoveMapObjFunc)so_symbol(&so_mod, "_ZN9CMvPlayer19CheckRideMoveMapObjEv");

    uint32_t py = (uint32_t)*(uint8_t *)((char *)self + 0x27);
    int c6 = *(int *)((char *)self + 0x318);
    uint32_t uVar7 = py - (c6 >> 4);

    void *pRideTarget = NULL;

    if (*(int *)((char *)self + 0x48) == 0) {
        uint32_t px = (uint32_t)*(uint8_t *)((char *)self + 0x26);
        int dir = -1;
        const int8_t offsets[4] = { 0, 0, 1, 1 };
        int off_idx = 0;

        do {
            uint32_t local_34 = (uVar7 == 0xffffffff) ? py : uVar7;
            if (dir == -1) {
                if ((int)px < map_w && (int)local_34 >= 0 && (int)local_34 < map_h) {
                    int iVar5 = (int)local_34 * map_w + (int)px;
                    uint16_t uVar2 = *(uint16_t *)(tileData + iVar5 * 3 + 1);
                    if ((uVar2 & 0xf) == 0 && (uVar2 & 0x30) == 0 && ((int)(uVar2 << 25) >= 0)) {
                        dir++;
                        off_idx += 2;
                        continue;
                    }
                }
                if (GetOccupyFixedObject && pObjMgr) {
                    pRideTarget = GetOccupyFixedObject(pObjMgr, px, uVar7, dir, 1);
                }
                break;
            } else {
                int ox = (int)offsets[off_idx];
                int oy = (int)offsets[off_idx + 1];
                int tx = (int)px + ox;
                int ty = (int)local_34 + oy;
                if (tx >= 0 && tx < map_w && ty >= 0 && ty < map_h) {
                    int iVar5 = ty * map_w + tx;
                    uint16_t uVar2 = *(uint16_t *)(tileData + iVar5 * 3 + 1);
                    if ((uVar2 & 0xf) == 0 && (uVar2 & 0x30) == 0 && ((int)(uVar2 << 25) >= 0)) {
                        dir++;
                        off_idx += 2;
                        continue;
                    }
                }
                if (GetOccupyFixedObject && pObjMgr) {
                    pRideTarget = GetOccupyFixedObject(pObjMgr, px, uVar7, dir, 1);
                }
                break;
            }
        } while (dir < (*(int *)((char *)self + 0x410) != 0 ? 2 : 0));
    } else {
        uint32_t px25 = (uint32_t)*(uint8_t *)((char *)self + 0x25);
        if ((int)px25 < (int)uVar7) {
            uint32_t px = (uint32_t)*(uint8_t *)((char *)self + 0x26);
            for (uint32_t u = px25; (int)u <= (int)uVar7; u++) {
                if (GetOccupyFixedObject && pObjMgr) {
                    pRideTarget = GetOccupyFixedObject(pObjMgr, px, u, -1, 1);
                    if (pRideTarget) break;
                }
            }
        } else {
            uint32_t px = (uint32_t)*(uint8_t *)((char *)self + 0x26);
            if (GetOccupyFixedObject && pObjMgr) {
                pRideTarget = GetOccupyFixedObject(pObjMgr, px, uVar7, -1, 1);
            }
        }
    }

    if (pRideTarget && (uintptr_t)pRideTarget >= 0x10000) {
        if (*(uint8_t *)((char *)pRideTarget + 4) == 8) {
            int t54 = *(int *)((char *)pRideTarget + 0x54);
            if (t54 != 8 && t54 != 6 && t54 != 0x1b && t54 != 0x20) {
                if (ReleaseRide) ReleaseRide(self, 0);
                if (checkride_log_count < CHECKRIDE_LOG_MAX) {
                    checkride_log_count++;
                    game_log("[Move] CheckRide: released (t54=%d), return 0\n", t54);
                }
                return 0;
            }
            if (*(int *)((char *)self + 0x410) == 0) {
                if (*(uint8_t *)((char *)self + 0x414) == 0 || *(uint8_t *)((char *)self + 0x415) == 0) {
                    *(uint8_t *)((char *)self + 0x414) = *(uint8_t *)((char *)self + 0x24);
                    *(uint8_t *)((char *)self + 0x415) = *(uint8_t *)((char *)self + 0x25);
                }
                void (**vtable)(void *, int, int, int) = *(void (***)(void *, int, int, int))self;
                if (vtable && vtable[0x21]) {
                    vtable[0x21](self, 0, -1, -1);
                }
                if (SetRideObject) SetRideObject(pRideTarget, self);
                *(void **)((char *)self + 0x410) = pRideTarget;
                if (*(int *)((char *)self + 0x48) != 0) {
                    int16_t ry = *(int16_t *)((char *)pRideTarget + 0x2c);
                    void (*SetWorldPosY)(void *, int, int, int) = (void *)so_symbol(&so_mod, "_ZN9CMvObject12SetWorldPosYEsbb");
                    if (SetWorldPosY) SetWorldPosY(self, ry, 1, 0);
                    if (checkride_log_count < CHECKRIDE_LOG_MAX) {
                        checkride_log_count++;
                        game_log("[Move] CheckRide: newly mounted, return 1\n");
                    }
                    return 1;
                }
            } else {
                if (CheckRideMoveMapObj) CheckRideMoveMapObj();
                void *curRide = *(void **)((char *)self + 0x410);
                if (pRideTarget != curRide) {
                    if (SetRideObject && curRide) SetRideObject(curRide, NULL);
                    if (SetRideObject) SetRideObject(pRideTarget, self);
                    *(void **)((char *)self + 0x410) = pRideTarget;
                    if (checkride_log_count < CHECKRIDE_LOG_MAX) {
                        checkride_log_count++;
                        game_log("[Move] CheckRide: switched ride target, return 1\n");
                    }
                    return 1;
                }
            }
            if (checkride_log_count < CHECKRIDE_LOG_MAX) {
                checkride_log_count++;
                game_log("[Move] CheckRide: still riding (self+0x410=%p), return 1\n", *(void **)((char *)self + 0x410));
            }
            return 1;
        }
    }

    if (ReleaseRide) ReleaseRide(self, 1);
    return 0;
}

/*
 * Hook for CMvPlayer::DoFallProc()
 * _ZN9CMvPlayer10DoFallProcEv
 */
static int hooked_CMvPlayer_DoFallProc(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return 0;

    int c9 = *(int *)((char *)self + 0x324);
    if (c9 != 0) {
        if (*(uint8_t *)((char *)self + 0x20) != 0xb) {
            if (*(uint8_t *)((char *)self + 4) == 0) {
                void (**vtable)(void *, int, int, int, int, int) = *(void (***)(void *, int, int, int, int, int))self;
                if (vtable && vtable[0x17]) {
                    vtable[0x17](self, 0xb, -1, 1, 1, 0);
                }
            }
            void (*DoFallAttackAfterBlur)(void) = (void *)so_symbol(&so_mod, "_ZN9CMvPlayer20DoFallAttackAfterBlurEv");
            if (DoFallAttackAfterBlur) DoFallAttackAfterBlur();
        }

        uint32_t uVar6 = (uint32_t)*(uint8_t *)((char *)self + 0x25);
        uint32_t uVar4 = (uint32_t)*(uint8_t *)((char *)self + 0x27);
        uint32_t uVar3 = (uint32_t)*(uint8_t *)((char *)self + 0x26);

        if (uVar4 <= uVar6) {
            uint16_t uVar1 = hooked_CMvObject_GetMapAttrib(self, uVar3, uVar4, -1, 1);
            if ((uVar1 & 0x30) == 0) {
                int (*IsFloorFunc)(uint16_t) = (int (*)(uint16_t))so_symbol(&so_mod, "_Z8IS_FLOORt");
                int iVar2 = IsFloorFunc ? IsFloorFunc(uVar1) : 0;
                if (iVar2 == 0 || (uVar1 & 1) != 0) {
                    return 0;
                }
            } else {
                void (**vtable)(void *, int, int) = *(void (***)(void *, int, int))self;
                if (vtable && vtable[0x11]) {
                    vtable[0x11](self, uVar3, uVar4);
                }
            }
            void (**vtable)(void *, int, int) = *(void (***)(void *, int, int))self;
            if (vtable && vtable[0x3e]) {
                vtable[0x3e](self, 0, 0);
            }
            return 1;
        }

        if (uVar6 <= uVar4) {
            for (uint32_t uVar5 = uVar6; (int)uVar5 <= (int)uVar4; uVar5++) {
                uint16_t uVar1 = hooked_CMvObject_GetMapAttrib(self, uVar3, uVar5, -1, 1);
                if ((uVar1 & 0x10) || (uVar1 & 0x20)) {
                    void (**vtable)(void *, int, int) = *(void (***)(void *, int, int))self;
                    if (vtable && vtable[0x11]) {
                        vtable[0x11](self, uVar3, uVar5);
                    }
                    if (vtable && vtable[0x3e]) {
                        vtable[0x3e](self, 0, 0);
                    }
                    return 1;
                }
                if (((uVar1 & 0xf) || (uVar1 & 0x40)) && ((uVar1 & 1) == 0 && (uVar1 & 0x40) == 0)) {
                    void (*SetWorldPosY)(void *, int, int, int) = (void *)so_symbol(&so_mod, "_ZN9CMvObject12SetWorldPosYEsbb");
                    if (SetWorldPosY) {
                        SetWorldPosY(self, (int16_t)((uVar5 << 20) >> 16), 1, 0);
                    }
                    void (**vtable)(void *, int, int) = *(void (***)(void *, int, int))self;
                    if (vtable && vtable[0x3e]) {
                        vtable[0x3e](self, 0, 0);
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

/*
 * Hook for CMvObject::UpdateFalling()
 * _ZN9CMvObject13UpdateFallingEv
 */
static int hooked_CMvObject_UpdateFalling(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return 0;

    uint32_t uVar6 = (uint32_t)*(uint8_t *)((char *)self + 0x27);
    uint32_t px = (uint32_t)*(uint8_t *)((char *)self + 0x26);
    uint16_t uVar2 = hooked_CMvObject_GetMapAttrib(self, px, uVar6, -1, 1);

    if ((uVar2 & 0xf) == 0 && (uVar2 & 0x30) == 0 && ((int)(uVar2 << 25) >= 0)) {
        int v12 = *(int *)((char *)self + 0x48);
        uint32_t speed = 0x20;
        if (v12 * 2 < 0x10) {
            speed = (uint32_t)(v12 * 2 + 0x10) & 0xffff;
        }

        uint16_t b_val = *(uint16_t *)((char *)self + 0x2c);
        void (*SetWorldPosY)(void *, int, int, int) = (void *)so_symbol(&so_mod, "_ZN9CMvObject12SetWorldPosYEsbb");
        if (SetWorldPosY) {
            SetWorldPosY(self, (int16_t)((speed + b_val) & 0xffff), 1, 0);
        }

        uint8_t cur_y = *(uint8_t *)((char *)self + 0x27);
        if (uVar6 <= (uint32_t)cur_y) {
            uint32_t uVar4 = (uint32_t)*(uint8_t *)((char *)self + 0x26);
            for (uint32_t y = uVar6; (int)y <= (int)cur_y; y++) {
                uint16_t uVar1 = hooked_CMvObject_GetMapAttrib(self, uVar4, y, -1, 1);
                if ((uVar1 & 0x10) || (uVar1 & 0x20)) {
                    void (**vtable)(void *, int) = *(void (***)(void *, int))self;
                    if (vtable && vtable[0x11]) {
                        vtable[0x11](self, uVar4);
                    }
                    break;
                }
                if ((uVar1 & 0xf) || (uVar1 & 0x40)) {
                    if (SetWorldPosY) {
                        SetWorldPosY(self, (int16_t)((y << 20) >> 16), 1, 1);
                    }
                    break;
                }
            }
        }
        *(int *)((char *)self + 0x48) = v12 + 1;
        return 1;
    }
    return 0;
}

/*
 * Hook for CMvMapObject::DoUpdateFalling()
 * _ZN12CMvMapObject15DoUpdateFallingEv
 */
static void hooked_CMvMapObject_DoUpdateFalling(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;

    if (*(int *)((char *)self + 0x54) == 0x17) {
        int (*GetWidth)(void *) = *(int (**)(void *))(*(int *)self + 0x9c);
        int w = GetWidth ? GetWidth(self) : 0;
        uint8_t px = *(uint8_t *)((char *)self + 0x26);
        uint8_t py = *(uint8_t *)((char *)self + 0x27);

        if (w > 1) {
            for (int i = 1; i < w; i++) {
                uint32_t uVar8 = px + i;
                uint16_t uVar3 = hooked_CMvObject_GetMapAttrib(self, uVar8, py, -1, 1);
                if ((uVar3 & 0xf) != 0 || (uVar3 & 0x30) != 0 || ((int)(uVar3 << 25) < 0)) {
                    return;
                }
            }
        }

        uint8_t prev_y = py;
        int falling = hooked_CMvObject_UpdateFalling(self);
        if (falling != 0) {
            uint8_t cur_x = *(uint8_t *)((char *)self + 0x26);
            void (**vtable)(void *, int, int, int, int, int, int) = *(void (***)(void *, int, int, int, int, int, int))self;
            int cur_w = GetWidth ? GetWidth(self) : 0;
            if (vtable && vtable[7]) {
                vtable[7](self, cur_x, prev_y, 2, 0, cur_w, 1);
            }
            void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
            if (pObjMgrSingleton && *pObjMgrSingleton) {
                void *pPlayer = *(void **)((char *)*pObjMgrSingleton + 8);
                void (*OnReleaseLift)(void *, uint32_t) = (void *)so_symbol(&so_mod, "_ZN9CMvPlayer19OnReleaseLiftObjectEj");
                if (OnReleaseLift && pPlayer) {
                    OnReleaseLift(pPlayer, 0xffffffff);
                }
            }
        }
    }
}

/*
 * Hook for CMvCharacter::DoJumpingSlpDownStair()
 * _ZN12CMvCharacter21DoJumpingSlpDownStairEv
 */
static void hooked_CMvCharacter_DoJumpingSlpDownStair(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x10000) return;
    char *map = (char *)*pMapSingleton;

    int map_h = *(int *)(map + 0xe0);
    uint32_t uVar5 = (uint32_t)*(uint8_t *)((char *)self + 0x27);
    uint32_t uVar4 = (uint32_t)*(uint8_t *)((char *)self + 0x26);

    uint16_t uVar2 = hooked_CMvObject_GetMapAttrib(self, uVar4, uVar5, -1, 1);
    uint32_t local_30 = 0;
    if ((uVar2 & 0x30) != 0) {
        int16_t pos_y = *(int16_t *)((char *)self + 0x2c);
        uint32_t shift = (uint32_t)((int)pos_y >> 31) >> 28;
        local_30 = (shift - (((int)pos_y + shift) & 0xf) + 0x10) & 0xff;
    }

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (pObjMgrSingleton && *pObjMgrSingleton) {
        void *pPlayer = *(void **)((char *)*pObjMgrSingleton + 8);
        if (*(uint8_t *)((char *)self + 4) == 0 && pPlayer && *(int *)((char *)pPlayer + 0x410) != 0) {
            local_30 = (local_30 + 8) & 0xff;
        }
    }

    uint32_t jump_dist = 0;
    if ((int)(uVar5 + 1) < map_h) {
        int iVar6 = 1;
        uint16_t uVar1 = 0;
        do {
            uint32_t target_y = iVar6 + uVar5;
            uVar1 = hooked_CMvObject_GetMapAttrib(self, uVar4, target_y, -1, 1);
            if ((uVar1 & 1) != 0 && (uVar1 & 0x30) == 0) {
                return;
            }
            if (!(((uVar1 & 2) != 0 && (uVar1 & 0x30) == 0) || ((uVar1 & 0xfff) != 0 && (uVar1 & 0x180) == 0))) {
                break;
            }
            iVar6++;
        } while (iVar6 != map_h - (int)uVar5);
        jump_dist = (uint32_t)((iVar6 - 1) * 0x100000) >> 16;
    }

    if (*(uint8_t *)((char *)self + 4) == 0) {
        void (**vtable)(void *, int, int, int, int, int) = *(void (***)(void *, int, int, int, int, int))self;
        if (vtable && vtable[0x17]) {
            vtable[0x17](self, 9, -1, 0, 0, 0);
        }
    }

    int16_t cur_y = *(int16_t *)((char *)self + 0x2c);
    void (*SetWorldPosY)(void *, int, int, int) = (void *)so_symbol(&so_mod, "_ZN9CMvObject12SetWorldPosYEsbb");
    if (SetWorldPosY) {
        SetWorldPosY(self, (int16_t)(((jump_dist + local_30 + cur_y + 8) * 0x10000) >> 16), 1, 0);
    }
}

/*
 * Hook for CMvItemMgr::DropablePos()
 * _ZN10CMvItemMgr11DropablePosEiiP12tagTILEPOINT15EnumObjectLayer
 */
static int hooked_CMvItemMgr_DropablePos(void *self, int param_2, int param_3, void *param_4, int layer) {
    if (!param_4) return 0;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x10000) return 0;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) return 0;

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x10000) return 0;

    uint8_t *tileData = get_main_tile_data(map);
    if (!tileData || (uintptr_t)tileData < 0x10000) return 0;

    typedef uint32_t (*GetOccupyMaskFunc)(void *this, int x, int y, uint32_t mask, int b);
    GetOccupyMaskFunc GetOccupyMask = (GetOccupyMaskFunc)so_symbol(&so_mod, "_ZN12CMvObjectMgr13GetOccupyMaskEiijb");
    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    void *pObjMgr = pObjMgrSingleton ? *pObjMgrSingleton : NULL;

    uint32_t uVar4 = (uint32_t)param_3 & 0xff;
    uint32_t uVar1 = 0;

    for (int step = 0; step < 3; step++) {
        *(char *)((char *)param_4 + 1) = (char)param_3;
        uVar1 = (param_2 & 0xff) + uVar1;
        uint32_t uVar2 = uVar1 & 0xff;
        *(char *)param_4 = (char)uVar1;

        if ((int)uVar2 >= map_w || (int)uVar4 >= map_h) {
            uVar1 = 1;
        } else {
            int tile_idx = (uVar2 + uVar4 * map_w) * 3;
            uVar1 = (uint32_t)*(uint16_t *)(tileData + tile_idx + 1);
        }

        uint32_t occ = (GetOccupyMask && pObjMgr) ? GetOccupyMask(pObjMgr, uVar2, uVar4, 0xffffffff, 1) : 0;
        if ((occ & 0xc) == 0) {
            if (((uint8_t)*(char *)param_4 == param_2) &&
                ((uint8_t)*((char *)param_4 + 1) == (uint8_t)param_3)) {
                if ((uVar1 & 1) == 0 || (uVar1 & 0x30) != 0) {
                    return 1;
                }
            } else if (((((uVar1 & 1) == 0) || ((uVar1 & 0x30) != 0)) &&
                        (((uVar1 & 0x40) == 0 || ((uVar1 & 0x30) != 0)))) &&
                       (((uVar1 & 0xf) != 0 || (((uVar1 & 0x30) != 0 || ((uVar1 & 0x40) != 0)))))) {
                if (((int)(uVar1 << 17) >= 0) && ((int)(uVar1 * 0x10000) >= 0)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/*
 * Hook for FUN_000cc128 / CMvMapObject::GetTileAttribute(void *map, int tx, int ty)
 * Located at text_base + 0x000bc128
 */
static uint16_t hooked_CMvMapObject_GetTileAttr(void *map, int tx, int ty) {
    if (!map || (uintptr_t)map < 0x10000) return 1;

    void *layers = *(void **)((char *)map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x10000) return 1;

    uint8_t *tileData = get_main_tile_data(map);
    int map_h = *(int *)((char *)map + 0xe0);

    if (!tileData || (uintptr_t)tileData < 0x10000) {
        if (ty >= 14) return 1;
        return 0;
    }

    int layer_count = *(int *)((char *)map + 0xd8);
    int map_w = *(int *)((char *)map + 0xdc);

    if (layer_count < 1 || tx < 0 || tx >= map_w || ty < 0 || ty >= map_h) {
        return 1;
    }

    int tile_idx = (ty * map_w + tx) * 3;
    uint8_t f1 = tileData[tile_idx + 1];
    uint8_t f2 = tileData[tile_idx + 2];
    return (uint16_t)f1 | ((uint16_t)f2 << 8);
}
static void hooked_CreateMiniMap(void *self) {
    if (!self) return;
    void *layers = *(void **)((char *)self + 0xf0);
    if (!layers) return;
    uint8_t *tileData = get_main_tile_data(self);
    if (!tileData) return;
    SO_CONTINUE_VOID(hook_create_minimap, self);
}

/*
 * Diagnostic-only passthrough for CMvMap::LoadTileImage(int mapId).
 * _ZN6CMvMap13LoadTileImageEi
 *
 * Per the Ghidra decompile, this is the ONLY function that writes self+0xc8
 * ("pTileImages"): it releases the previous map's tile-image resource (a
 * virtual call through that resource's own vtable), then calls
 * MvLoadPzx("map/%03d.pzx", mapId) and stores the result at self+0xc8 --
 * possibly NULL if the load/parse failed even though the underlying file was
 * read successfully via our readAssets JNI bridge (isAssetExist/readAssets
 * logs only prove the bytes were delivered, not that MvLoadPzx accepted
 * them). Logs the map id and whether pTileImages ended up populated for
 * every map load (uncapped -- map loads are rare, not per-frame), pairs with
 * the [MapDraw] state-transition log above to pinpoint exactly which map
 * transition leaves rendering permanently blocked.
 */
static void hooked_CMvMap_LoadTileImage(void *self, int map_id) {
    if (!self) return;
    SO_CONTINUE_VOID(hook_load_tile_image, self, map_id);
    void *pTileImages = *(void **)((char *)self + 200);
    game_log("[LoadTileImage] map=%03d -> pTileImages=%p (%s)\n",
        map_id, pTileImages, pTileImages ? "OK" : "NULL/FAILED");
}

/*
 * Hook for CMvMap::DrawMiniMap()
 * _ZN6CMvMap11DrawMiniMapEv
 */

static so_hook hook_draw_minimap;
static void hooked_CMvMap_DrawMiniMap(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    void *layers = *(void **)((char *)self + 0xf0);
    if (!layers) return;
    uint8_t *tileData = get_main_tile_data(self);
    if (!tileData) return;

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (!pObjMgrSingleton || !*pObjMgrSingleton || (uintptr_t)*pObjMgrSingleton < 0x10000) return;
    void *pPlayer = *(void **)((char *)*pObjMgrSingleton + 8);
    if (!pPlayer || (uintptr_t)pPlayer < 0x10000) return;

    SO_CONTINUE_VOID(hook_draw_minimap, self);
}

/*
 * Diagnostic passthrough hook for CMvScrollBG::DrawH(int, int)
 * _ZN11CMvScrollBG5DrawHEii
 *
 * User-reported: story cutscenes that use a scrolling background (as opposed
 * to a plain tile map) look completely static -- no parallax/cloud motion --
 * after picking a character. CMvMap::Draw calls this every frame with a
 * scroll offset; the offset itself, and ambient effects layered on top
 * (DrawDistantView, the type-5 sprite-cycle block further down in this same
 * function), are driven by CMvScrollBG::Update() advancing this+0xc from the
 * global CGxFrameT1 tick (GxGetFrameT1()->+0xd0). Neither CMvScrollBG::DrawH
 * nor ::Update is hooked/reimplemented by this port -- both run as pristine
 * original ARM code -- so if the background truly never moves, this log
 * should reveal whether it's (a) the tick itself never advancing, (b) DrawH
 * being called with an unchanging (x,y) despite the tick advancing, or (c)
 * neither -- in which case "static" is more likely the 480x320-canvas
 * scaling regression (see project memory) making motion imperceptible in a
 * squeezed corner rather than a real animation freeze.
 */
static so_hook hook_scrollbg_drawh;
static int scrollbg_log_count = 0;
#define SCROLLBG_LOG_MAX 40

static void hooked_CMvScrollBG_DrawH(void *self, int x, int y) {
    if (scrollbg_log_count < SCROLLBG_LOG_MAX) {
        scrollbg_log_count++;
        typedef void *(*GxGetFrameT1Func)(void);
        GxGetFrameT1Func GxGetFrameT1 = (GxGetFrameT1Func)so_symbol(&so_mod, "_Z12GxGetFrameT1v");
        void *frameT1 = GxGetFrameT1 ? GxGetFrameT1() : NULL;
        int tick = (frameT1 && (uintptr_t)frameT1 >= 0x10000) ? *(int *)((char *)frameT1 + 0xd0) : -1;
        int bg_type = self ? *(int *)self : -1;
        game_log("[ScrollBG] DrawH(x=%d, y=%d) type=%d tick=%d\n", x, y, bg_type, tick);
    }
    SO_CONTINUE_VOID(hook_scrollbg_drawh, self, x, y);
}

/*
 * Hook for CMvLayer::Draw()
 * _ZN8CMvLayer4DrawEiiP12tagTILEPOINTb10enumDrawOPi
 *
 * FIX (2026-08-30): CMvMap::Draw (verified via arm-vita-eabi-objdump at
 * 0x96e46/0x96e78/0x96f12/0x9708a) calls this once per Z-layer (layers+0,
 * +24, +48) and then, completely unconditionally, calls the real
 * CMvObjectMgr::Draw (0x96e56/0x96e82/0x96f20/0x97096) for the SAME
 * z-layer, regardless of what this function did -- this happens whether
 * CMvMap::Draw took the CMvScrollBG cutscene branch or the plain-tile
 * branch, they both fall through into the exact same layer+object draw
 * sequence. So this function returning early was never the reason
 * CMvObjectMgr::Draw (player/NPC sprites) failed to draw -- but it WAS,
 * previously, a guaranteed no-op for terrain any time self+12 (this
 * particular layer's own tile-attribute byte array, a SEPARATE field from
 * the map's pTileImages checked below) was null. That happens for real:
 * some maps -- confirmed via a separate investigation of the intro cutscene
 * map (0x5c/92) -- have map_w/map_h == 0, so CMvMap::PreLoad's
 * calloc(map_w*map_h*3) is a calloc(0) and self+12 legitimately never gets
 * a real buffer. Verified via disassembly (0x98436 loads self+12, 0x984a8
 * dereferences it with NO null check at all) that the pristine original
 * function really does crash on a genuinely null self+12 -- so the old
 * `if (!pTileData) return;` guard here was preventing a real crash, not
 * blocking otherwise-safe rendering, and simply deleting it would trade
 * "draws nothing" for "crashes". Real fix: swap in a correctly-sized
 * all-zero dummy tile-attribute buffer for the duration of the call (same
 * pattern hooked_CMvMap_Draw below already uses for layer 0 only,
 * generalized here to whichever layer `self` actually is, since all 3
 * layers can hit this) so the original function runs to completion instead
 * of being skipped outright.
 */
static uint8_t *dummy_layer_tile_data = NULL;
static int dummy_layer_tile_size = 0;

static void hooked_CMvLayer_Draw(void *self, int screen_x, int screen_y, void *tile_pt,
                                 int arg4, int draw_op, int arg6) {
    if (!self) return;
    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    void *pMap = *pMapSingleton;
    void *pTileImages = *(void **)((char *)pMap + 200);
    if (!pTileImages) return;

    uint8_t **pTileData = (uint8_t **)((char *)self + 12);
    uint8_t *old_tileData = *pTileData;

    if (!old_tileData) {
        int map_w = *(int *)((char *)pMap + 0xdc);
        int map_h = *(int *)((char *)pMap + 0xe0);
        if (map_w < 0) map_w = 0;
        if (map_h < 0) map_h = 0;
        int required_size = map_w * map_h * 3 + 16;
        if (required_size > dummy_layer_tile_size) {
            uint8_t *grown = realloc(dummy_layer_tile_data, required_size);
            if (grown) {
                dummy_layer_tile_data = grown;
                dummy_layer_tile_size = required_size;
            }
        }
        if (dummy_layer_tile_data) {
            memset(dummy_layer_tile_data, 0, dummy_layer_tile_size);
            *pTileData = dummy_layer_tile_data;
        }
    }

    if (*pTileData) {
        SO_CONTINUE_VOID(hook_layer_draw, self, screen_x, screen_y, tile_pt, arg4, draw_op, arg6);
    }

    if (!old_tileData) {
        *pTileData = NULL;
    }
}

/*
 * Diagnostic-only passthrough for CMvObjectMgr::Draw(EnumObjectLayer, bool).
 * _ZN12CMvObjectMgr4DrawE15EnumObjectLayerb
 *
 * The CMvLayer::Draw fix above stopped terrain from being force-blanked by a
 * null tile-attribute buffer, but user report after that fix (game_042.log)
 * is unchanged: terrain, player and NPCs still don't render anywhere, only
 * background + touch UI show. CMvObjectMgr::Draw is the function that
 * actually iterates and draws every player/NPC object -- verified via
 * arm-vita-eabi-objdump at 0x9b654 that for a given layer it reads a
 * registered-object count from `self + layer*0x10 + 0x6c` and an object
 * pointer array from `self + (layer+6)*0x10 + 8`, calling CMvObject::Draw()
 * on each non-null entry. This is a PRISTINE, unhooked function (no patch
 * touches it) -- so if nothing draws, either (a) the count is 0 for every
 * layer, meaning no player/NPC object was ever registered for rendering at
 * all (a spawn/creation-side bug, unrelated to the tile-draw fix), or
 * (b) the count is non-zero but CMvObject::Draw's own return/behavior
 * still produces nothing on screen (a lower-level sprite-draw issue).
 * Logs (edge-triggered per layer, capped) the registered count and this
 * function's own return value (how many objects it believes it drew) so a
 * fresh session log can distinguish these two cases before patching either.
 * Pure passthrough, no behavior change.
 */
static int objmgr_draw_log_count = 0;
#define OBJMGR_DRAW_LOG_MAX 90
static int objmgr_last_count[3] = { -1, -1, -1 };

static int hooked_CMvObjectMgr_Draw(void *self, int layer, int only_visible) {
    int count = -1;
    if (self && layer >= 0 && layer < 3) {
        count = *(int *)((char *)self + layer * 0x10 + 0x6c);
    }
    int drawn = SO_CONTINUE(int, hook_objmgr_draw, self, layer, only_visible);
    if (layer >= 0 && layer < 3 && count != objmgr_last_count[layer] &&
        objmgr_draw_log_count < OBJMGR_DRAW_LOG_MAX) {
        objmgr_last_count[layer] = count;
        objmgr_draw_log_count++;
        game_log("[ObjMgrDraw] layer=%d registered=%d drawn=%d\n", layer, count, drawn);
    }
    return drawn;
}

/*
 * Diagnostic-only passthrough for CMvObject::IsDrawable().
 * _ZN9CMvObject10IsDrawableEv
 *
 * game_043.log with the CMvObjectMgr::Draw diagnostic above showed the real
 * split: cutscene NPC objects (layer 1, e.g. the Princess/Guardian scene)
 * are correctly REGISTERED (registered=2/3) but CMvObject::Draw returns 0
 * for them on every single sample (drawn=0 always) -- they exist but are
 * being told "not drawable" every frame. Meanwhile layer-0 objects during
 * real in-game play always have drawn==registered (CMvObject::Draw returns
 * 1), yet the user still reports nothing renders -- so that path's bug, if
 * any, is further downstream (inside the object's own vtable draw call),
 * not here.
 *
 * CMvObject::Draw only calls IsDrawable() as a real gate for objects whose
 * type byte (self+4) is NOT one of {6,7,9,10,11,12} (those always draw
 * unconditionally) -- confirmed via arm-vita-eabi-objdump (0xa38b6-0xa38be
 * sign-extends self+4 and compares against that set). IsDrawable() itself
 * (0xa3870, verified via objdump, not just Ghidra pseudo-C -- the Ghidra
 * dump was cross-checked and matches here) returns false immediately if
 * self+24 (signed short) == -1, or if self+29 (byte) == 0; otherwise it
 * reads screen bounds from the CMvGraphics singleton (confirmed via GOT/
 * relocation cross-reference against the known ms_pSingleton table at
 * 0x15f2ec = CGsSingleton<CMvGraphics>::ms_pSingleton, not guessed) and
 * finally calls CMvObject::IsInScreen(x,y) -> CGsGraphics::CheckRectInScreen.
 * Logs (once per distinct failing object pointer, capped) the raw
 * self+24/self+29/self+31 values whenever this returns 0, to localize which
 * of these gates is rejecting the cutscene actors before touching any of
 * them. Pure passthrough, no behavior change.
 *
 * ROUND 2 (game_044.log): first log with this hook showed a real 3-way
 * split -- self=...024c (type=13, byteAt29=0) is rejected by the CHEAP
 * early gate (a legitimate "not ready yet" state, probably harmless), but
 * self=...1bac (type=0, byteAt29=1, byteAt31=31) and self=...441c (type=5,
 * byteAt29=1, byteAt31=0) both PASS both early gates and still get
 * rejected -- proving the CMvGraphics/camera on-screen check itself is
 * where real NPC objects die. Per the disassembly (0xa3884-0xa3898):
 * `self+31 == 0` sends the object straight into `IsInScreen(0,0)` --
 * checking the object's screen rect against world ORIGIN instead of the
 * real camera scroll offset -- which will read as "off-screen" for any
 * object whose screen position isn't near (0,0). Added the extra fields
 * below (self+0x2a/0x2c = the object's own already-computed screen X/Y,
 * written by UpdateScreenPosFromWorldPos before this check; and the
 * CMvGraphics/camera chain fields IsDrawable itself reads) to see exactly
 * how far off (0,0) the rejected objects' screen position is, and whether
 * self+31==0 or a false CMvGraphics+8!=2 is what's forcing the (0,0)
 * fallback path for each one. Read-only, no behavior change.
 */
static int is_drawable_log_count = 0;
#define IS_DRAWABLE_LOG_MAX 60
static void *is_drawable_seen[16];
static int is_drawable_seen_n = 0;

static int hooked_CMvObject_IsDrawable(void *self) {
    int result = SO_CONTINUE(int, hook_is_drawable, self);
    if (!self || result != 0 || is_drawable_log_count >= IS_DRAWABLE_LOG_MAX) {
        return result;
    }
    for (int i = 0; i < is_drawable_seen_n; i++) {
        if (is_drawable_seen[i] == self) return result;
    }
    if (is_drawable_seen_n < 16) {
        is_drawable_seen[is_drawable_seen_n++] = self;
    }
    int16_t shortAt24 = *(int16_t *)((char *)self + 24);
    uint8_t byteAt29 = *(uint8_t *)((char *)self + 29);
    uint8_t byteAt31 = *(uint8_t *)((char *)self + 31);
    int8_t type = *(int8_t *)((char *)self + 4);
    int16_t screenX = *(int16_t *)((char *)self + 0x2a);
    int16_t screenY = *(int16_t *)((char *)self + 0x2c);

    int graphicsMode = 0, camFlag = 0, camX = 0, camY = 0;
    void **pGraphicsSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI11CMvGraphicsE13ms_pSingletonE");
    if (pGraphicsSingleton && *pGraphicsSingleton) {
        char *pGraphics = (char *)*pGraphicsSingleton;
        graphicsMode = *(int *)(pGraphics + 8);
        void *pCamera = *(void **)(pGraphics + 12);
        if (pCamera) {
            camFlag = *(uint8_t *)((char *)pCamera + 28);
            camX = *(int *)((char *)pCamera + 20);
            camY = *(int *)((char *)pCamera + 24);
        }
    }

    /* CMvObject::IsInScreen (0xa36c8) reads CGsSingleton<CMvMap>::ms_pSingleton
     * + 0xac/0xae (signed shorts) -- resolved via GOT/relocation trace,
     * confirmed against 0x15f2dc = CGsSingleton<CMvMap>::ms_pSingleton in the
     * nm -D symbol table, not guessed. CORRECTION (round 7): re-tracing
     * IsInScreen -> CGsGraphics::CheckRectInScreen in full showed these two
     * values are NOT the screen bounds themselves -- they're passed as the
     * two additive `int` offset args into CheckRectInScreen(rect, a, b),
     * added to the REAL bounds which live on the (different) CGsGraphics
     * singleton at +0x3c/+0x4c/+0x50. So `mapViewWH == (0,0)` observed live
     * is very likely a harmless "no extra offset" case, not the bug --
     * logged here for completeness/context, but see hooked_CheckRectInScreen
     * below for the actual ground-truth instrumentation of the real
     * pass/fail decision instead of hand-deriving it further. */
    int mapViewW = -1, mapViewH = -1;
    void **pMapSingletonDiag = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (pMapSingletonDiag && *pMapSingletonDiag) {
        char *pMap = (char *)*pMapSingletonDiag;
        mapViewW = *(int16_t *)(pMap + 0xac);
        mapViewH = *(int16_t *)(pMap + 0xae);
    }

    is_drawable_log_count++;
    game_log("[IsDrawable] self=%p type=%d shortAt24=%d byteAt29=%d byteAt31=%d "
             "screenXY=(%d,%d) graphicsMode=%d camFlag=%d camXY=(%d,%d) mapViewWH=(%d,%d) -> NOT drawable\n",
        self, (int)type, (int)shortAt24, (int)byteAt29, (int)byteAt31,
        (int)screenX, (int)screenY, graphicsMode, camFlag, camX, camY, mapViewW, mapViewH);
    return result;
}

/*
 * Diagnostic-only passthrough for CGsGraphics::CheckRectInScreen(TGXRECT, int, int).
 * _ZN11CGsGraphics17CheckRectInScreenE7TGXRECTii
 *
 * This is the actual, final pass/fail comparison behind every IsDrawable()
 * rejection traced so far -- rather than keep hand-deriving its internal
 * algebra from static disassembly (already caught one wrong turn this round:
 * the CMvMap+0xac/0xae fields logged above turned out to be an additive
 * offset passed INTO this call, not the screen bounds themselves), hook the
 * function directly and log its real runtime inputs and output. Confirmed
 * via arm-vita-eabi-objdump (0x6f610) that this reads its actual screen
 * bounds from `this` (the CGsGraphics singleton, 0x15f128 in the nm -D
 * table, confirmed via the same GOT-trace technique) at +0x3c/+0x4c/+0x50 --
 * also confirmed (0x70a38 CGsGraphics::Initialize, 0x70b10
 * CGsGraphics::CGsGraphics ctor, 0xc7f1c inside CMvGraphics::CMvGraphics)
 * that +0x3c is hardcoded to literal 0 by the PRISTINE original ARM
 * constructor chain (not something this port broke) -- so if the real bug
 * is here, it's in +0x4c/+0x50 or in how the rect/offset args compare
 * against them, not a simple "screen size is 0" story. TGXRECT is an 8-byte
 * (4x int16) struct passed by value in r1/r2 per AAPCS, matched here with a
 * plain 4-short struct so the compiler decodes the same registers the
 * pristine caller already set up. Rate-limited to failures only (returns
 * 0), capped. Pure passthrough, no behavior change.
 */
typedef struct { int16_t x0, y0, x1, y1; } TGXRECT8;

static int check_rect_log_count = 0;
#define CHECK_RECT_LOG_MAX 40

static int hooked_CheckRectInScreen(void *self, TGXRECT8 rect, int a, int b) {
    int result = SO_CONTINUE(int, hook_check_rect_in_screen, self, rect, a, b);
    if (result != 0 || !self || check_rect_log_count >= CHECK_RECT_LOG_MAX) {
        return result;
    }
    check_rect_log_count++;
    int32_t bound3c = *(int32_t *)((char *)self + 0x3c);
    int32_t bound4c = *(int32_t *)((char *)self + 0x4c);
    int32_t bound50 = *(int32_t *)((char *)self + 0x50);
    game_log("[CheckRectInScreen] rect=(%d,%d,%d,%d) a=%d b=%d bound3c=%d bound4c=%d bound50=%d -> false\n",
        (int)rect.x0, (int)rect.y0, (int)rect.x1, (int)rect.y1, a, b, bound3c, bound4c, bound50);
    return result;
}

/*
 * Diagnostic hooks for the "Abnormal save file. You need to create a new
 * character." popup reported by the user. Confirmed via
 * decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c (CMvGameState::LoadGameData,
 * ~line 96670) and the exact string (decompressed from
 * assets/data/Eng/StrPopup.zt1: "Abnormal save file. You need to create a new
 * character.") that this popup is NOT a raw file-corruption/decrypt failure --
 * it fires from a completely separate branch than that. LoadGameData's real
 * control flow:
 *   1. CGsEncryptFile::LoadBegin() decrypts the file and checks a 4-byte
 *      stored-size-vs-actual-size header AND a checksum (CulCheckSum). If
 *      EITHER check fails, LoadBegin returns false and LoadGameData returns
 *      silently -- no popup at all is shown for a genuinely corrupt/truncated
 *      file on disk.
 *   2. If LoadBegin succeeds, LoadGameData then compares a value derived from
 *      the just-loaded player data (this+0x38, XORed byte-wise) against a
 *      per-slot summary BYTE cached in the CMvSystemMenu singleton
 *      (ms_pSingleton + slot*0x3c + 0x4c), plus a <= check against another
 *      cached byte (+0x52) vs the loaded player's own field (+0x2eb). ONLY
 *      if this second, completely different comparison fails does it call
 *      CMvSystemMenu::CreateInvalidDataPopup() -- the exact popup reported.
 * These two hooks are pure passthroughs (no behavior change) that log which
 * of the two independent failure modes is actually happening, instead of
 * guessing: a LoadBegin failure means the file itself is stale/corrupt on
 * disk (delete it and start a new save); CreateInvalidDataPopup firing means
 * the file decrypted fine but CMvSystemMenu's cached per-slot summary is out
 * of sync with it (a port-side persistence bug, not a corrupt save).
 */
static int loadbegin_log_count = 0;
#define LOADBEGIN_LOG_MAX 30

static int hooked_CGsEncryptFile_LoadBegin(void *self, const char *filename, int check_phone) {
    int result = SO_CONTINUE(int, hook_encryptfile_loadbegin, self, filename, check_phone);
    if (loadbegin_log_count < LOADBEGIN_LOG_MAX) {
        loadbegin_log_count++;
        game_log("[SaveLoad] CGsEncryptFile::LoadBegin(\"%s\") -> %s\n",
            filename ? filename : "(null)", result ? "OK (size+checksum matched)" : "FAILED (size/checksum mismatch -- file is stale/corrupt on disk)");
    }
    return result;
}

static void hooked_CMvSystemMenu_CreateInvalidDataPopup(void *self) {
    game_log("[SaveLoad] CMvSystemMenu::CreateInvalidDataPopup() fired -- file decrypted OK, "
             "but CMvSystemMenu's cached per-slot summary doesn't match it (THIS is the "
             "'Abnormal save file' popup)\n");
    SO_CONTINUE_VOID(hook_invaliddata_popup, self);
}

/*
 * game_056.log showed BOTH "option.sav" and "Save0.dat" failing
 * CGsEncryptFile::LoadBegin's size/checksum check (not the CreateInvalidDataPopup
 * branch above) -- i.e. LoadBegin's own header-size-vs-real-size or CulCheckSum
 * comparison fails, and LoadGameData/LoadOption just return silently, no popup.
 * Since SaveBegin/WritePtr/SaveEnd/CulCheckSum are pristine, unmodified ARM
 * (unchanged across every round of this port), a file written correctly by this
 * exact code should always re-validate -- so either these specific files are
 * stale leftovers from before some earlier change, or the actual disk write in
 * CGsFile::Save (this + CGsFile::Load) is truncating/corrupting the encrypted
 * blob on our filesystem bridge. These two passthrough hooks log the filename,
 * requested byte count, and real success/failure of the raw file I/O calls that
 * SaveEnd/LoadBegin make underneath the encryption layer, to tell those two
 * causes apart directly instead of guessing.
 */
static int hooked_CGsFile_Save(void *self, void *data, const char *filename, uint32_t size) {
    int ret = SO_CONTINUE(int, hook_gsfile_save, self, data, filename, size);
    game_log("[SaveIO] CGsFile::Save(\"%s\", %u bytes) -> %s\n",
        filename ? filename : "(null)", (unsigned int)size, ret ? "OK" : "FAILED (fopen/fwrite error)");
    return ret;
}

static int hooked_CGsFile_Load(void *self, void *data, const char *filename, uint32_t size) {
    int ret = SO_CONTINUE(int, hook_gsfile_load, self, data, filename, size);
    game_log("[SaveIO] CGsFile::Load(\"%s\", %u bytes) -> %s\n",
        filename ? filename : "(null)", (unsigned int)size, ret ? "OK" : "FAILED (fopen/fread error)");
    return ret;
}

static int hooked_GsFSFileSize(const char *filename, int mode) {
    int size = SO_CONTINUE(int, hook_gsfsfilesize, filename, mode);
    game_log("[SaveIO] GsFSFileSize(\"%s\") -> %d\n", filename ? filename : "null", size);
    return size;
}

static int hooked_MC_fsFileAttribute(const char *filename, void *attr) {
    int ret = SO_CONTINUE(int, hook_mc_fsfileattribute, filename, attr);
    uint32_t size = attr ? *(uint32_t*)((uintptr_t)attr + 8) : 0;
    game_log("[SaveIO] MC_fsFileAttribute(\"%s\") -> %d (size=%u)\n", filename ? filename : "null", ret, size);
    return ret;
}

/*
 * Hook for CMvGameScript::Script_Char_Talk()
 * _ZN13CMvGameScript16Script_Char_TalkEP7sScript
 */
static void *hooked_CMvGameScript_Script_Char_Talk(void *self, void *script) {
    if (!self || !script) return script;

    typedef void *(*GetSelectCharFunc)(void *this, uint32_t uid);
    typedef const char *(*GetStringFunc)(void *this, uint32_t str_type, int str_id);
    typedef void (*OpenSayUIFunc)(void *this, void *pChar, const char *str, int sayBoxType, int faceExp, int textEffect, int b);
    typedef void (*KeyPressedFunc)(void *this, int key);
    typedef void (*SetReleaseKeyFunc)(void *this, int b);

    void *pChar = NULL;
    GetSelectCharFunc GetSelectChar = (GetSelectCharFunc)so_symbol(&so_mod, "_ZN13CMvGameScript13GetSelectCharEj");
    if (GetSelectChar) {
        pChar = GetSelectChar(self, 0xffffffff);
    }

    void **pGameUISingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI9CMvGameUIE13ms_pSingletonE");
    if (!pGameUISingleton || !*pGameUISingleton) return script;
    char *gameUI = (char *)*pGameUISingleton;
    void *sayUI = (void *)(gameUI + 0xd54);

    void *resScript = script;

    int flag_d60 = *(int *)(gameUI + 0xd60);
    int flag_d68 = *(int *)(gameUI + 0xd68);

    if (flag_d60 == 0 && flag_d68 == 0) {
        void **pScriptMgr = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI16CMvGameScriptMgrE13ms_pSingletonE");
        GetStringFunc GetString = (GetStringFunc)so_symbol(&so_mod, "_ZN16CMvGameScriptMgr9GetStringE20EnumScriptStringTypei");

        int script_data = *(int *)((char *)script + 8);
        const char *str = NULL;
        if (pScriptMgr && *pScriptMgr && GetString && script_data) {
            uint32_t str_param = *(uint32_t *)(script_data + 4);
            str = GetString(*pScriptMgr, str_param >> 16, str_param & 0xffff);
        }

        int sayBoxType = script_data ? *(int *)(script_data + 0x10) : 0;
        int textEffect = script_data ? *(int *)(script_data + 0x1c) : 0;

        if (str) {
            int faceExp = 0;
            if (!pChar || *(int16_t *)((char *)pChar + 6) == 0) {
                faceExp = *(int16_t *)((char *)self + 0x10);
                if (faceExp == -1) {
                    *(int16_t *)((char *)self + 0x10) = 0;
                    faceExp = 0;
                }
            } else {
                faceExp = *(int16_t *)((char *)self + 0x10);
            }

            OpenSayUIFunc OpenSayUI = (OpenSayUIFunc)so_symbol(&so_mod, "_ZN8CMvSayUI9OpenSayUIEP12CMvCharacterPKc14EnumSayBoxType18EnumFaceExpression18EnumTextEffectTypeb");
            if (OpenSayUI) {
                OpenSayUI(sayUI, pChar, str, sayBoxType, faceExp, textEffect, 0);
            }
        }
    } else {
        void **pInputKey = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI11CGsInputKeyE13ms_pSingletonE");
        if (pInputKey && *pInputKey) {
            char *inputKey = (char *)*pInputKey;
            if (*(int *)(inputKey + 0x14) == 1) {
                int key = *(int *)(inputKey + 0xc);
                KeyPressedFunc KeyPressed = (KeyPressedFunc)so_symbol(&so_mod, "_ZN8CMvSayUI10KeyPressedE6GsVKey");
                if (KeyPressed) KeyPressed(sayUI, key);

                if (key != 0xb) {
                    SetReleaseKeyFunc SetReleaseKey = (SetReleaseKeyFunc)so_symbol(&so_mod, "_ZN11CGsInputKey13SetReleaseKeyEb");
                    if (SetReleaseKey) SetReleaseKey(inputKey, 1);
                }

                if (*(int *)(gameUI + 0xd60) == 0 && *(int *)(gameUI + 0xd68) == 0) {
                    resScript = *(void **)((char *)self + 0x7c);
                    if (!resScript) {
                        resScript = *(void **)((char *)script + 0x10);
                    }
                }
            }
        }
    }

    return resScript;
}

/*
 * Hook for CMvGameScript::Script_SetTargetObject()
 * _ZN13CMvGameScript22Script_SetTargetObjectEP7sScript
 */
static void *hooked_CMvGameScript_Script_SetTargetObject(void *self, void *script) {
    if (!self || !script) return script;

    typedef void *(*GetSelectCharFunc)(void *this, uint32_t uid);
    typedef uint32_t (*SelectCharIndex2UIDFunc)(void *this, int idx);
    typedef void *(*SearchObjectFunc)(void *this, uint32_t uid);

    int *script_data = *(int **)((char *)script + 8);
    int target_idx = script_data ? script_data[1] : 0;

    GetSelectCharFunc GetSelectChar = (GetSelectCharFunc)so_symbol(&so_mod, "_ZN13CMvGameScript13GetSelectCharEj");
    void *pChar = GetSelectChar ? GetSelectChar(self, 0xffffffff) : NULL;

    SelectCharIndex2UIDFunc SelectCharIndex2UID = (SelectCharIndex2UIDFunc)so_symbol(&so_mod, "_ZN13CMvGameScript20SelectCharIndex2UIDEi");
    uint32_t uid = SelectCharIndex2UID ? SelectCharIndex2UID(self, target_idx) : 0;

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (pObjMgrSingleton && *pObjMgrSingleton) {
        SearchObjectFunc SearchObject = (SearchObjectFunc)so_symbol(&so_mod, "_ZN12CMvObjectMgr12SearchObjectEj");
        void *pTarget = SearchObject ? SearchObject(*pObjMgrSingleton, uid) : NULL;
        if (pTarget && pChar) {
            *(void **)((char *)pChar + 0x54) = pTarget;
        }
    }

    void *resScript = *(void **)((char *)self + 0x7c);
    if (!resScript) {
        resScript = *(void **)((char *)script + 0x10);
    }
    return resScript;
}

/*
 * Hook for FUN_000b31ec / CMvObject::GetTileAttribute(void *obj, int tx, int ty)
 * Located at text_base + 0x000a31ec
 */
static uint16_t hooked_CMvObject_GetTileAttr(void *obj, int tx, int ty) {
    if (tx == -1 && obj) {
        tx = *(uint8_t *)((char *)obj + 0x26);
    }
    if (ty == -1 && obj) {
        ty = *(uint8_t *)((char *)obj + 0x27);
    }

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return 1;
    char *map = (char *)*pMapSingleton;

    void *layers = *(void **)(map + 0xf0);
    if (!layers) return 1;
    
    uint8_t *tileData = get_main_tile_data(map);
    int map_h = *(int *)(map + 0xe0);

    if (!tileData) {
        if (ty >= 14) return 1;
        return 0;
    }

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);

    if (layer_count < 1 || tx < 0 || tx >= map_w || ty < 0 || ty >= map_h) {
        return 1;
    }

    int tile_idx = (ty * map_w + tx) * 3;
    uint8_t f1 = tileData[tile_idx + 1];
    uint8_t f2 = tileData[tile_idx + 2];
    return (uint16_t)f1 | ((uint16_t)f2 << 8);
}
static uint16_t hooked_GetTileAttr_Obj(void *obj) {
    if (!obj) return 1;
    uint8_t tx = *(uint8_t *)((char *)obj + 0x26);
    uint8_t ty = *(uint8_t *)((char *)obj + 0x27);

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return 1;
    char *map = (char *)*pMapSingleton;

    void *layers = *(void **)(map + 0xf0);
    if (!layers) return 1;
    
    uint8_t *tileData = get_main_tile_data(map);
    int map_h = *(int *)(map + 0xe0);

    if (!tileData) {
        if (ty >= 14) return 1;
        return 0;
    }

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);

    if (layer_count < 1 || tx >= map_w || ty >= map_h) return 1;

    int tile_idx = (ty * map_w + tx) * 3;
    uint8_t f1 = tileData[tile_idx + 1];
    uint8_t f2 = tileData[tile_idx + 2];
    return (uint16_t)f1 | ((uint16_t)f2 << 8);
}
/*
 * Hook for CMvObject::AddOccupyAttrib(int tx, int ty, uint16_t attrib)
 * _ZN9CMvObject15AddOccupyAttribEiit
 */
static void hooked_CMvObject_AddOccupyAttrib(void *self, int tx, int ty, uint16_t attrib) {
    if (!self) return;
    if (*(uint8_t *)((char *)self + 28) == 0) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count > 0 && tx >= 0 && tx < map_w && ty >= 0 && ty < map_h) {
        void *layers = *(void **)(map + 0xf0);
        if (layers) {
    uint8_t *tileData = get_main_tile_data(map);
            if (tileData) {
                int tile_idx = (ty * map_w + tx) * 3;
                uint16_t cur = (uint16_t)tileData[tile_idx + 1] | ((uint16_t)tileData[tile_idx + 2] << 8);
                cur |= attrib;
                tileData[tile_idx + 1] = (uint8_t)cur;
                tileData[tile_idx + 2] = (uint8_t)(cur >> 8);
            }
        }
    }

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (pObjMgrSingleton && *pObjMgrSingleton) {
        void (*SetOccupyObject)(void *, void *, int, int, int) = (void *)so_symbol(&so_mod, "_ZN12CMvObjectMgr15SetOccupyObjectEP9CMvObjectiib");
        if (SetOccupyObject) {
            SetOccupyObject(*pObjMgrSingleton, self, tx, ty, 0);
        }
    }
}

/*
 * Hook for CMvObject::DelOccupyAttrib(int tx, int ty, uint16_t attrib)
 * _ZN9CMvObject15DelOccupyAttribEiit
 */
static void hooked_CMvObject_DelOccupyAttrib(void *self, int tx, int ty, uint16_t attrib) {
    if (!self) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count > 0 && tx >= 0 && tx < map_w && ty >= 0 && ty < map_h) {
        void *layers = *(void **)(map + 0xf0);
        if (layers) {
    uint8_t *tileData = get_main_tile_data(map);
            if (tileData) {
                int tile_idx = (ty * map_w + tx) * 3;
                uint16_t cur = (uint16_t)tileData[tile_idx + 1] | ((uint16_t)tileData[tile_idx + 2] << 8);
                cur &= ~attrib;
                tileData[tile_idx + 1] = (uint8_t)cur;
                tileData[tile_idx + 2] = (uint8_t)(cur >> 8);
            }
        }
    }

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (pObjMgrSingleton && *pObjMgrSingleton) {
        void (*SetOccupyObject)(void *, void *, int, int, int) = (void *)so_symbol(&so_mod, "_ZN12CMvObjectMgr15SetOccupyObjectEP9CMvObjectiib");
        if (SetOccupyObject) {
            SetOccupyObject(*pObjMgrSingleton, self, tx, ty, 1);
        }
    }
}

/*
 * Hook for CMvMapObject::DelOccupyAttribSize(uint16_t attrib)
 * _ZN12CMvMapObject19DelOccupyAttribSizeEt
 */
static void hooked_CMvMapObject_DelOccupyAttribSize(void *self, uint16_t attrib) {
    if (!self) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);
    if (layer_count < 1 || map_w <= 0 || map_h <= 0) return;

    void *layers = *(void **)(map + 0xf0);
    if (!layers) return;
    uint8_t *tileData = get_main_tile_data(map);
    if (!tileData) return;

    uint8_t cx = *(uint8_t *)((char *)self + 0x26);
    uint8_t cy = *(uint8_t *)((char *)self + 0x27);

    int (*GetWidth)(void *) = *(int (**)(void *))(*(int *)self + 0x9c);
    int (*GetHeight)(void *) = *(int (**)(void *))(*(int *)self + 0xa0);
    int w = GetWidth ? GetWidth(self) : 0;
    int h = GetHeight ? GetHeight(self) : 0;

    for (int y = 0; y < h; y++) {
        int ty = (int)cy + y;
        if (ty >= 0 && ty < map_h) {
            for (int x = 0; x < w; x++) {
                int tx = (int)cx + x;
                if (tx >= 0 && tx < map_w) {
                    int idx = (ty * map_w + tx) * 3;
                    uint16_t cur = (uint16_t)tileData[idx + 1] | ((uint16_t)tileData[idx + 2] << 8);
                    cur &= ~attrib;
                    tileData[idx + 1] = (uint8_t)cur;
                    tileData[idx + 2] = (uint8_t)(cur >> 8);
                }
            }
        }
    }
}

/*
 * Hook for CMvPlayer::SetHonorProgress(EnumHonorType type, int progress)
 * _ZN9CMvPlayer16SetHonorProgressE13EnumHonorTypei
 */
static void hooked_CMvPlayer_SetHonorProgress(void *self, int honor_type, int progress) {
    if (!self) return;

    void **pXlsMgr = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI9CMvXlsMgrE13ms_pSingletonE");
    if (pXlsMgr && *pXlsMgr) {
        void *(*GetTbl)(void *, int) = (void *)so_symbol(&so_mod, "_ZN9CMvXlsMgr6GetTblEi");
        if (GetTbl) GetTbl(*pXlsMgr, 0x11);
    }

    if (progress == 0) {
        int16_t cur = *(int16_t *)((char *)self + (honor_type + 0x1f0) * 2 + 4);
        if (cur != -1) {
            *(int16_t *)((char *)self + (honor_type + 0x1f0) * 2 + 4) = cur + 1;
        }
    } else {
        int idx = honor_type + 0x1f0;
        int16_t cur = *(int16_t *)((char *)self + idx * 2 + 4);
        if ((int)cur < progress) {
            *(int16_t *)((char *)self + idx * 2 + 4) = (int16_t)progress;
        }
    }
}

/*
 * Hook for CMvMap::Draw()
 * _ZN6CMvMap4DrawEv
 */
/*
 * Diagnostic (edge-triggered, no cap): the real CMvMap::Draw (verified via
 * arm-vita-eabi-objdump, not just the Ghidra pseudo-C) gates its ENTIRE body
 * -- both the cutscene CMvScrollBG::DrawH path and the normal tile-layer +
 * CMvObjectMgr::Draw path that renders terrain/player/NPCs -- on self+0xc8
 * (aka "pTileImages" in these hooks) being non-null. That field is written
 * only by CMvMap::LoadTileImage(int), via MvLoadPzx("map/%03d.pzx", id).
 * User report: terrain, player and NPCs all missing (only background+touch
 * UI show) after a map transition, camera-follow and text still working.
 * This logs every time the draw state changes for the current map (not
 * every frame) so a single captured session shows: which map ids reach
 * NORMAL (tiles+objects) rendering, which stay BLOCKED forever (pTileImages
 * never got populated for that map -- see the LoadTileImage hook below for
 * why), and whether SCROLLBG cutscene mode ever hands off to NORMAL mode at
 * all during a map transition.
 */
static int mapdraw_state_log_count = 0;
#define MAPDRAW_STATE_LOG_MAX 200

static void hooked_CMvMap_Draw(void *self) {
    if (!self) return;
    void *layers = *(void **)((char *)self + 0xf0);
    if (!layers) return;

    void *pTileImagesDiag = *(void **)((char *)self + 200);
    void *scrollBGDiag = *(void **)((char *)self + 0xf4);
    static int last_logged_mapdraw_state = -1;
    int mapdraw_state = !pTileImagesDiag ? 0 : (scrollBGDiag ? 1 : 2);
    if (mapdraw_state != last_logged_mapdraw_state && mapdraw_state_log_count < MAPDRAW_STATE_LOG_MAX) {
        last_logged_mapdraw_state = mapdraw_state;
        mapdraw_state_log_count++;
        game_log("[MapDraw] state -> %s (map_id=%d pTileImages=%p scrollBG=%p)\n",
            mapdraw_state == 0 ? "BLOCKED(pTileImages NULL)" :
            mapdraw_state == 1 ? "SCROLLBG(cutscene)" : "NORMAL(tiles+objects)",
            (int)*(uint8_t *)((char *)self + 4), pTileImagesDiag, scrollBGDiag);
    }

    if (*(int *)((char *)self + 200) == 0) return;

    SO_CONTINUE_VOID(hook_map_draw, self);
}

/*
 * Hook for CMvMap::IsSaveMap()
 * _ZN6CMvMap9IsSaveMapEv
 */
static int hooked_CMvMap_IsSaveMap(void *self) {
    if (!self) return 0;
    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (!pObjMgrSingleton || !*pObjMgrSingleton) return 0;
    void *pPlayer = *(void **)((char *)*pObjMgrSingleton + 8);
    if (!pPlayer) return 0;

    uint8_t px = *(uint8_t *)((char *)pPlayer + 0x26);
    uint8_t py = *(uint8_t *)((char *)pPlayer + 0x27);
    int map_w = *(int *)((char *)self + 0xdc);
    int map_h = *(int *)((char *)self + 0xe0);
    int layer_count = *(int *)((char *)self + 0xd8);

    if (layer_count < 1 || px >= map_w || py >= map_h) return 0;

    void *layers = *(void **)((char *)self + 0xf0);
    if (!layers) return 0;
    uint8_t *tileData = get_main_tile_data(self);
    if (!tileData) return 0;

    int tile_idx = (py * map_w + px) * 3;
    uint16_t flag = (uint16_t)tileData[tile_idx + 1] | ((uint16_t)tileData[tile_idx + 2] << 8);
    /* Original: -1 < (int)((uint)flag << 0x1f) checks bit 0 of flag (not the
     * sign bit / bit 15) — it isolates flag's LSB into bit 31 before the
     * signed comparison. A bit-15 check here silently breaks save-point
     * detection on real map data. */
    if (!(flag & 1) || (flag & 0x30)) {
        uint8_t map_id = *(uint8_t *)((char *)self + 4);
        void **pXlsMgr = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI9CMvXlsMgrE13ms_pSingletonE");
        void *(*GetTbl)(void *, int) = (void *)so_symbol(&so_mod, "_ZN9CMvXlsMgr6GetTblEi");
        int (*GetVal)(void *, int, int) = (void *)so_symbol(&so_mod, "_ZN10GVXLLoader6GetValEii");
        int (*GetMapType)(void *, int) = (void *)so_symbol(&so_mod, "_ZN6CMvMap10GetMapTypeEi");

        if (pXlsMgr && *pXlsMgr && GetTbl && GetVal) {
            void *tbl = GetTbl(*pXlsMgr, 10);
            int val1 = GetVal(tbl, 1, (int)map_id);
            if (val1 != 3 && val1 != 6 && (map_id < 0x6e || map_id > 0x7b)) {
                if (GetMapType && GetMapType(self, 0) != 2) {
                    void **pGameUISingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI9CMvGameUIE13ms_pSingletonE");
                    if (pGameUISingleton && *pGameUISingleton) {
                        return (*(uint8_t *)((char *)*pGameUISingleton + 0x4f8)) ^ 1;
                    }
                }
            }
        }
    }
    
    return 0;
}

/*
 * Hooks for Movement & Skills to prevent tileData null dereference
 */
/*
 * Diagnostic-only: rate-limited logging for the "player is stuck, no input
 * moves them, no crash" report. These gates (is_map_tiledata_ready() and the
 * various tile-attribute/occupy checks) were all verified by hand against
 * the original pseudo-C and, separately, real disassembly for the two
 * hardcoded-address hooks — no logic bug was found in any of them. If the
 * lockup is still real, one of these two counters will show it in the next
 * captured log: either OnMove is being gated to a no-op every time (map
 * "not ready" during live gameplay, which would be new/unexpected), or
 * OnMove reaches the real engine call every time (meaning the block is
 * happening deeper, inside the unmodified native movement code itself, e.g.
 * CheckRide's ride/lock state) and this rules the gate out entirely.
 */
static int move_gate_log_count = 0;
#define MOVE_GATE_LOG_MAX 50


static so_hook hook_char_canmove;
static int hooked_CMvCharacter_CanMove(void *self, int param_1, int param_2, int param_3, int param_4, int param_5, int param_6, int param_7) {
    if (!self || (uintptr_t)self < 0x10000) return 0;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return 0; // Abort if layers is completely null
    int ret = SO_CONTINUE(int, hook_char_canmove, self, param_1, param_2, param_3, param_4, param_5, param_6, param_7);
    restore_dummy_tile_data(dummy);
    return ret;
}

static so_hook hook_char_onmove_1;
static int hooked_CMvCharacter_OnMove_1(void *self, int param_1) {
    if (!self || (uintptr_t)self < 0x10000) return 0;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return 0; // Abort if layers is completely null
    int ret = SO_CONTINUE(int, hook_char_onmove_1, self, param_1);
    restore_dummy_tile_data(dummy);
    return ret;
}

static so_hook hook_char_onmove_5;
static int hooked_CMvCharacter_OnMove_5(void *self, int dir1, int dir2, int p3, int p4, int p5) {
    if (!self || (uintptr_t)self < 0x10000) return 0;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return 0; // Abort if layers is completely null
    int ret = SO_CONTINUE(int, hook_char_onmove_5, self, dir1, dir2, p3, p4, p5);
    restore_dummy_tile_data(dummy);
    return ret;
}

static so_hook hook_set_screen_skill;
static void hooked_CMvCharacter_SetScreenSkillUsePos(void *self, int *p1, void *p2, int p3) {
    if (!self || (uintptr_t)self < 0x10000 || !p1 || !p2) return;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return; // Abort if layers is completely null
    SO_CONTINUE_VOID(hook_set_screen_skill, self, p1, p2, p3);
    restore_dummy_tile_data(dummy);
}

static so_hook hook_wizard_attack;
static void hooked_CMvPCWizard_DoAttack(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return; // Abort if layers is completely null
    SO_CONTINUE_VOID(hook_wizard_attack, self);
    restore_dummy_tile_data(dummy);
}

static so_hook hook_rogue_skill;
static void hooked_CMvPCRogue_DoSkill(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return; // Abort if layers is completely null
    SO_CONTINUE_VOID(hook_rogue_skill, self);
    restore_dummy_tile_data(dummy);
}

/*
 * Diagnostic: game_038.log showed [MapDraw] briefly reaching NORMAL(tiles+
 * objects) with a valid pTileImages (refuting the earlier LoadTileImage
 * theory), then bouncing straight back to SCROLLBG(map_id=2) tied to
 * script/call/002.zt1 + script/map/002_001.zt1 -- re-read hundreds of times
 * (output_eng.zt1 alone: 400+ reads) -- with touch.reportNum staying 0 the
 * whole session. Per the Ghidra decompile, CMvMapObject::OnEvent's case
 * 0x13 (script/call/NNN.zt1 trigger, self+0x54==0x13, self+0x58==iVar9>0)
 * calls CMvGameScriptMgr::CreateCallScript() completely unconditionally --
 * no "already fired"/one-shot flag visible in that case at all. If the
 * player is stuck standing on this trigger's tile (matches the still-open
 * "character stuck, no movement" lead), OnEvent would refire the same
 * call-script every time it's invoked, which would explain the repeating
 * dialogue reads, the repeating sound the user described, and the
 * NORMAL/SCROLLBG flicker together as ONE bug instead of three. Logs the
 * dispatch fields (verified via real disassembly: self+4 signed-byte gate,
 * self+0x54 switch selector, self+0x58 iVar9 arg) plus the player's tile
 * (px,py) so the next log shows directly whether the same (case, iVar9,
 * px, py) repeats call after call (stuck) or actually varies (working as
 * designed, just a busy scripted area).
 */
static int mapobj_onevent_log_count = 0;
#define MAPOBJ_ONEVENT_LOG_MAX 60

static so_hook hook_map_obj_event;
static void hooked_CMvMapObject_OnEvent(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;

    if (mapobj_onevent_log_count < MAPOBJ_ONEVENT_LOG_MAX) {
        int8_t gate = *(int8_t *)((char *)self + 4);
        if (gate == 8) {
            int event_type = *(int *)((char *)self + 0x54);
            int arg = *(int *)((char *)self + 0x58);
            int px = -1, py = -1;
            void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
            if (pObjMgrSingleton && *pObjMgrSingleton) {
                void *pPlayer = *(void **)((char *)*pObjMgrSingleton + 8);
                if (pPlayer && (uintptr_t)pPlayer >= 0x10000) {
                    px = *(uint8_t *)((char *)pPlayer + 0x26);
                    py = *(uint8_t *)((char *)pPlayer + 0x27);
                }
            }
            mapobj_onevent_log_count++;
            game_log("[MapObjEvent] case=0x%x arg=%d player_tile=(%d,%d)\n", event_type, arg, px, py);
        }
    }

    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return; // Abort if layers is completely null
    SO_CONTINUE_VOID(hook_map_obj_event, self);
    restore_dummy_tile_data(dummy);
}

static so_hook hook_ride_rotate_rail;
static void hooked_CMvMapObjectRideRotate_DoRailSetPos(void *self, int param_1) {
    if (!self || (uintptr_t)self < 0x10000) return;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return; // Abort if layers is completely null
    SO_CONTINUE_VOID(hook_ride_rotate_rail, self, param_1);
    restore_dummy_tile_data(dummy);
}

static so_hook hook_projectile_move;
static void hooked_CMvProjectile_OnMove(void *self) {
    if (!self || (uintptr_t)self < 0x10000) return;
    uint8_t *dummy = swap_dummy_tile_data();
    if (dummy == (uint8_t *)-1) return; // Abort if layers is completely null
    SO_CONTINUE_VOID(hook_projectile_move, self);
    restore_dummy_tile_data(dummy);
}

void so_patch(void) {
    MC_grpCreateOffScreenFrameBuffer = (void *)so_symbol(&so_mod, "MC_grpCreateOffScreenFrameBuffer");
    MC_grpGetPixelFromRGB = (void *)so_symbol(&so_mod, "MC_grpGetPixelFromRGB");

    uintptr_t pzx_subframe = (uintptr_t)so_symbol(&so_mod, "_Z13GsPZxSubFrameP11CGxPZxFramei");
    if (pzx_subframe) {
        hook_thumb_func(pzx_subframe, (uintptr_t)&hooked_GsPZxSubFrame);
        game_log("[Patch] Hooked GsPZxSubFrame (0x%08X)\n", (unsigned int)pzx_subframe);
    }

    uintptr_t draw_damage = (uintptr_t)so_symbol(&so_mod, "_ZN11CMvGraphics10DrawDamageEiiiiii10enumDrawOPi");
    if (draw_damage) {
        hook_draw_damage = hook_thumb(draw_damage | 1, (uintptr_t)&hooked_CMvGraphics_DrawDamage);
        game_log("[Patch] Hooked CMvGraphics::DrawDamage (0x%08X)\n", (unsigned int)draw_damage);
    }

    uintptr_t draw_numbers = (uintptr_t)so_symbol(&so_mod, "_ZN11CMvGraphics11DrawNumbersEiiiPKcijbb");
    if (draw_numbers) {
        hook_draw_numbers = hook_thumb(draw_numbers | 1, (uintptr_t)&hooked_CMvGraphics_DrawNumbers);
        game_log("[Patch] Hooked CMvGraphics::DrawNumbers (0x%08X)\n", (unsigned int)draw_numbers);
    }

    uintptr_t num_draw = (uintptr_t)so_symbol(&so_mod, "_ZN15CMvNumberObject4DrawEv");
    if (num_draw) {
        hook_num_draw = hook_thumb(num_draw | 1, (uintptr_t)&hooked_CMvNumberObject_Draw);
        game_log("[Patch] Hooked CMvNumberObject::Draw (0x%08X)\n", (unsigned int)num_draw);
    }

    uintptr_t draw_damage_obj = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvObjectMgr16DrawDamageObjectEv");
    if (draw_damage_obj) {
        hook_draw_damage_obj = hook_thumb(draw_damage_obj | 1, (uintptr_t)&hooked_CMvObjectMgr_DrawDamageObject);
        game_log("[Patch] Hooked CMvObjectMgr::DrawDamageObject (0x%08X)\n", (unsigned int)draw_damage_obj);
    }

    uintptr_t draw_npc_head = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvObjectMgr15DrawNpcHeadInfoEv");
    if (draw_npc_head) {
        hook_draw_npc_head = hook_thumb(draw_npc_head | 1, (uintptr_t)&hooked_CMvObjectMgr_DrawNpcHeadInfo);
        game_log("[Patch] Hooked CMvObjectMgr::DrawNpcHeadInfo (0x%08X)\n", (unsigned int)draw_npc_head);
    }

    uintptr_t create_minimap = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap13CreateMiniMapEv");
    if (create_minimap) {
        hook_create_minimap = hook_thumb(create_minimap | 1, (uintptr_t)&hooked_CreateMiniMap);
        game_log("[Patch] Hooked CMvMap::CreateMiniMap (0x%08X)\n", (unsigned int)create_minimap);
    }

    uintptr_t load_tile_image = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap13LoadTileImageEi");
    if (load_tile_image) {
        hook_load_tile_image = hook_thumb(load_tile_image | 1, (uintptr_t)&hooked_CMvMap_LoadTileImage);
        game_log("[Patch] Hooked CMvMap::LoadTileImage (0x%08X)\n", (unsigned int)load_tile_image);
    }

    uintptr_t draw_minimap = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap11DrawMiniMapEv");
    if (draw_minimap) {
        hook_draw_minimap = hook_thumb(draw_minimap | 1, (uintptr_t)&hooked_CMvMap_DrawMiniMap);
        game_log("[Patch] Hooked CMvMap::DrawMiniMap (0x%08X)\n", (unsigned int)draw_minimap);
    }

    uintptr_t map_draw = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap4DrawEv");
    if (map_draw) {
        hook_map_draw = hook_thumb(map_draw | 1, (uintptr_t)&hooked_CMvMap_Draw);
        game_log("[Patch] Hooked CMvMap::Draw (0x%08X)\n", (unsigned int)map_draw);
    }

    uintptr_t scrollbg_drawh = (uintptr_t)so_symbol(&so_mod, "_ZN11CMvScrollBG5DrawHEii");
    if (scrollbg_drawh) {
        hook_scrollbg_drawh = hook_thumb(scrollbg_drawh | 1, (uintptr_t)&hooked_CMvScrollBG_DrawH);
        game_log("[Patch] Hooked CMvScrollBG::DrawH (0x%08X)\n", (unsigned int)scrollbg_drawh);
    }

    uintptr_t is_save_map = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap9IsSaveMapEv");
    if (is_save_map) {
        hook_thumb_func(is_save_map, (uintptr_t)&hooked_CMvMap_IsSaveMap);
        game_log("[Patch] Hooked CMvMap::IsSaveMap (0x%08X)\n", (unsigned int)is_save_map);
    }

    uintptr_t layer_draw = (uintptr_t)so_symbol(&so_mod, "_ZN8CMvLayer4DrawEiiP12tagTILEPOINTb10enumDrawOPi");
    if (layer_draw) {
        hook_layer_draw = hook_thumb(layer_draw | 1, (uintptr_t)&hooked_CMvLayer_Draw);
        game_log("[Patch] Hooked CMvLayer::Draw (0x%08X)\n", (unsigned int)layer_draw);
    }

    uintptr_t objmgr_draw = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvObjectMgr4DrawE15EnumObjectLayerb");
    if (objmgr_draw) {
        hook_objmgr_draw = hook_thumb(objmgr_draw | 1, (uintptr_t)&hooked_CMvObjectMgr_Draw);
        game_log("[Patch] Hooked CMvObjectMgr::Draw(EnumObjectLayer,bool) (0x%08X)\n", (unsigned int)objmgr_draw);
    }

    uintptr_t is_drawable = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvObject10IsDrawableEv");
    if (is_drawable) {
        hook_is_drawable = hook_thumb(is_drawable | 1, (uintptr_t)&hooked_CMvObject_IsDrawable);
        game_log("[Patch] Hooked CMvObject::IsDrawable (0x%08X)\n", (unsigned int)is_drawable);
    }

    uintptr_t check_rect = (uintptr_t)so_symbol(&so_mod, "_ZN11CGsGraphics17CheckRectInScreenE7TGXRECTii");
    if (check_rect) {
        hook_check_rect_in_screen = hook_thumb(check_rect | 1, (uintptr_t)&hooked_CheckRectInScreen);
        game_log("[Patch] Hooked CGsGraphics::CheckRectInScreen (0x%08X)\n", (unsigned int)check_rect);
    }

    uintptr_t encryptfile_loadbegin = (uintptr_t)so_symbol(&so_mod, "_ZN14CGsEncryptFile9LoadBeginEPKcb");
    if (encryptfile_loadbegin) {
        hook_encryptfile_loadbegin = hook_thumb(encryptfile_loadbegin | 1, (uintptr_t)&hooked_CGsEncryptFile_LoadBegin);
        game_log("[Patch] Hooked CGsEncryptFile::LoadBegin (0x%08X)\n", (unsigned int)encryptfile_loadbegin);
    }

    uintptr_t invaliddata_popup = (uintptr_t)so_symbol(&so_mod, "_ZN13CMvSystemMenu22CreateInvalidDataPopupEv");
    if (invaliddata_popup) {
        hook_invaliddata_popup = hook_thumb(invaliddata_popup | 1, (uintptr_t)&hooked_CMvSystemMenu_CreateInvalidDataPopup);
        game_log("[Patch] Hooked CMvSystemMenu::CreateInvalidDataPopup (0x%08X)\n", (unsigned int)invaliddata_popup);
    }

    /*
     * game_056.log confirmed (via the passthrough hook above) that
     * CGsEncryptFile::LoadBegin fails for BOTH option.sav and Save0.dat with
     * no popup, i.e. inside LoadBegin's own validation, not the
     * CreateInvalidDataPopup path. Verified against the real .so via
     * objdump (not Ghidra pseudo-C) at text_base + 0x6eef0
     * (_ZN14CGsEncryptFile9LoadBeginEPKcb): after decrypting the file it
     * runs three checks that only make sense against the original Android
     * device and can never pass on the port:
     *   1. 0x6ef66: a decrypted 4-byte "stored size" vs "real file size" cmp+beq.
     *   2. 0x6efb4: `mov r3, r9` (r9 = the `check_phone` bool argument) feeding
     *      a `cmp r3,#0 / beq` that, when taken, skips a strcmp() between the
     *      phone number baked into the save header and GsGetPhoneNum() (our
     *      fake, but consistent, JNI phone number stub -- irrelevant, this
     *      check has no reason to ever run on a Vita save).
     *   3. 0x6effa: CulCheckSum(decrypted buffer) vs the checksum stored in
     *      the header, cmp+beq.
     * Same category of bug Advena-Vita hit in this same engine family
     * (CGsEncryptFile::LoadBegin) -- fixed there by bypassing the checksum
     * and phone-ID checks outright. Mirrored here: each `beq` is turned into
     * an unconditional branch to its own "check passed" target (the encoded
     * offset happens to reuse the original low byte, so only the high byte
     * changes 0xd0 -> 0xe0), and the phone-check feeder is neutered by
     * forcing r3 = 0 before the `cmp r3,#0`.
     */
    uint16_t loadbegin_header_ok = 0xe010;
    kuKernelCpuUnrestrictedMemcpy((void *)(so_mod.text_base + 0x6ef66), &loadbegin_header_ok, sizeof(loadbegin_header_ok));
    uint16_t loadbegin_no_phone_check = 0x2300;
    kuKernelCpuUnrestrictedMemcpy((void *)(so_mod.text_base + 0x6efb4), &loadbegin_no_phone_check, sizeof(loadbegin_no_phone_check));
    uint16_t loadbegin_checksum_ok = 0xe007;
    kuKernelCpuUnrestrictedMemcpy((void *)(so_mod.text_base + 0x6effa), &loadbegin_checksum_ok, sizeof(loadbegin_checksum_ok));
    game_log("[Patch] Patched CGsEncryptFile::LoadBegin to bypass header-size/phone/checksum save validation (0x6ef66, 0x6efb4, 0x6effa)\n");

    uintptr_t gsfile_save = (uintptr_t)so_symbol(&so_mod, "_ZN7CGsFile4SaveEPvPKcj");
    if (gsfile_save) {
        hook_gsfile_save = hook_thumb(gsfile_save | 1, (uintptr_t)&hooked_CGsFile_Save);
        game_log("[Patch] Hooked CGsFile::Save (0x%08X)\n", (unsigned int)gsfile_save);
    }

    uintptr_t gsfile_load = (uintptr_t)so_symbol(&so_mod, "_ZN7CGsFile4LoadEPvPKcj");
    if (gsfile_load) {
        hook_gsfile_load = hook_thumb(gsfile_load | 1, (uintptr_t)&hooked_CGsFile_Load);
        game_log("[Patch] Hooked CGsFile::Load (0x%08X)\n", (unsigned int)gsfile_load);
    }
    
    uintptr_t gsfsfilesize = (uintptr_t)so_symbol(&so_mod, "_Z12GsFSFileSizePKci");
    if (gsfsfilesize) {
        hook_gsfsfilesize = hook_thumb(gsfsfilesize | 1, (uintptr_t)&hooked_GsFSFileSize);
        game_log("[Patch] Hooked GsFSFileSize (0x%08X)\n", (unsigned int)gsfsfilesize);
    }

    uintptr_t mc_fsfileattr = (uintptr_t)so_symbol(&so_mod, "MC_fsFileAttribute");
    if (mc_fsfileattr) {
        hook_mc_fsfileattribute = hook_thumb(mc_fsfileattr | 1, (uintptr_t)&hooked_MC_fsFileAttribute);
        game_log("[Patch] Hooked MC_fsFileAttribute (0x%08X)\n", (unsigned int)mc_fsfileattr);
    }

    uintptr_t char_talk = (uintptr_t)so_symbol(&so_mod, "_ZN13CMvGameScript16Script_Char_TalkEP7sScript");
    if (char_talk) {
        hook_thumb_func(char_talk, (uintptr_t)&hooked_CMvGameScript_Script_Char_Talk);
        game_log("[Patch] Hooked CMvGameScript::Script_Char_Talk (0x%08X)\n", (unsigned int)char_talk);
    }

    uintptr_t set_target_obj = (uintptr_t)so_symbol(&so_mod, "_ZN13CMvGameScript22Script_SetTargetObjectEP7sScript");
    if (set_target_obj) {
        hook_thumb_func(set_target_obj, (uintptr_t)&hooked_CMvGameScript_Script_SetTargetObject);
        game_log("[Patch] Hooked CMvGameScript::Script_SetTargetObject (0x%08X)\n", (unsigned int)set_target_obj);
    }

    uintptr_t map_attrib = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvObject12GetMapAttribEii7EnumDiri");
    if (map_attrib) {
        hook_thumb_func(map_attrib, (uintptr_t)&hooked_CMvObject_GetMapAttrib);
        game_log("[Patch] Hooked CMvObject::GetMapAttrib (0x%08X)\n", (unsigned int)map_attrib);
    }

    uintptr_t check_ride = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvPlayer9CheckRideEv");
    if (check_ride) {
        hook_thumb_func(check_ride, (uintptr_t)&hooked_CMvPlayer_CheckRide);
        game_log("[Patch] Hooked CMvPlayer::CheckRide (0x%08X)\n", (unsigned int)check_ride);
    }

    uintptr_t do_fall = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvPlayer10DoFallProcEv");
    if (do_fall) {
        hook_thumb_func(do_fall, (uintptr_t)&hooked_CMvPlayer_DoFallProc);
        game_log("[Patch] Hooked CMvPlayer::DoFallProc (0x%08X)\n", (unsigned int)do_fall);
    }

    uintptr_t update_falling = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvObject13UpdateFallingEv");
    if (update_falling) {
        hook_thumb_func(update_falling, (uintptr_t)&hooked_CMvObject_UpdateFalling);
        game_log("[Patch] Hooked CMvObject::UpdateFalling (0x%08X)\n", (unsigned int)update_falling);
    }

    uintptr_t map_obj_falling = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvMapObject15DoUpdateFallingEv");
    if (map_obj_falling) {
        hook_thumb_func(map_obj_falling, (uintptr_t)&hooked_CMvMapObject_DoUpdateFalling);
        game_log("[Patch] Hooked CMvMapObject::DoUpdateFalling (0x%08X)\n", (unsigned int)map_obj_falling);
    }

    uintptr_t jumping_slp = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvCharacter21DoJumpingSlpDownStairEv");
    if (jumping_slp) {
        hook_thumb_func(jumping_slp, (uintptr_t)&hooked_CMvCharacter_DoJumpingSlpDownStair);
        game_log("[Patch] Hooked CMvCharacter::DoJumpingSlpDownStair (0x%08X)\n", (unsigned int)jumping_slp);
    }

    uintptr_t dropable_pos = (uintptr_t)so_symbol(&so_mod, "_ZN10CMvItemMgr11DropablePosEiiP12tagTILEPOINT15EnumObjectLayer");
    if (dropable_pos) {
        hook_thumb_func(dropable_pos, (uintptr_t)&hooked_CMvItemMgr_DropablePos);
        game_log("[Patch] Hooked CMvItemMgr::DropablePos (0x%08X)\n", (unsigned int)dropable_pos);
    }

    uintptr_t map_obj_tile_attr = so_mod.text_base + 0x000bc128;
    if (map_obj_tile_attr) {
        hook_thumb_func(map_obj_tile_attr, (uintptr_t)&hooked_CMvMapObject_GetTileAttr);
        game_log("[Patch] Hooked CMvMapObject::GetTileAttr (0x%08X)\n", (unsigned int)map_obj_tile_attr);
    }

    uintptr_t tile_attr = so_mod.text_base + 0x000a31ec;
    if (tile_attr) {
        hook_thumb_func(tile_attr, (uintptr_t)&hooked_CMvObject_GetTileAttr);
        game_log("[Patch] Hooked CMvObject::GetTileAttr (0x%08X)\n", (unsigned int)tile_attr);
    }

    uintptr_t tile_attr_obj = so_mod.text_base + 0x0009b9f8;
    if (tile_attr_obj) {
        hook_thumb_func(tile_attr_obj, (uintptr_t)&hooked_GetTileAttr_Obj);
        game_log("[Patch] Hooked CMvObjectMgr::GetTileAttr_Obj (0x%08X)\n", (unsigned int)tile_attr_obj);
    }

    uintptr_t add_occupy = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvObject15AddOccupyAttribEiit");
    if (add_occupy) {
        hook_thumb_func(add_occupy, (uintptr_t)&hooked_CMvObject_AddOccupyAttrib);
        game_log("[Patch] Hooked CMvObject::AddOccupyAttrib (0x%08X)\n", (unsigned int)add_occupy);
    }

    uintptr_t del_occupy = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvObject15DelOccupyAttribEiit");
    if (del_occupy) {
        hook_thumb_func(del_occupy, (uintptr_t)&hooked_CMvObject_DelOccupyAttrib);
        game_log("[Patch] Hooked CMvObject::DelOccupyAttrib (0x%08X)\n", (unsigned int)del_occupy);
    }

    uintptr_t del_occupy_size = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvMapObject19DelOccupyAttribSizeEt");
    if (del_occupy_size) {
        hook_thumb_func(del_occupy_size, (uintptr_t)&hooked_CMvMapObject_DelOccupyAttribSize);
        game_log("[Patch] Hooked CMvMapObject::DelOccupyAttribSize (0x%08X)\n", (unsigned int)del_occupy_size);
    }

    uintptr_t set_honor = (uintptr_t)so_symbol(&so_mod, "_ZN9CMvPlayer16SetHonorProgressE13EnumHonorTypei");
    if (set_honor) {
        hook_thumb_func(set_honor, (uintptr_t)&hooked_CMvPlayer_SetHonorProgress);
        game_log("[Patch] Hooked CMvPlayer::SetHonorProgress (0x%08X)\n", (unsigned int)set_honor);
    }

    
    uintptr_t canmove = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvCharacter7CanMoveE7EnumDiribiiib");
    if (canmove) {
        hook_char_canmove = hook_thumb(canmove | 1, (uintptr_t)&hooked_CMvCharacter_CanMove);
        game_log("[Patch] Hooked CMvCharacter::CanMove (0x%08X)\n", (unsigned int)canmove);
    }

    uintptr_t onmove_1 = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvCharacter6OnMoveEi");
    if (onmove_1) {
        hook_char_onmove_1 = hook_thumb(onmove_1 | 1, (uintptr_t)&hooked_CMvCharacter_OnMove_1);
        game_log("[Patch] Hooked CMvCharacter::OnMove(int) (0x%08X)\n", (unsigned int)onmove_1);
    }

    uintptr_t onmove_5 = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvCharacter6OnMoveE7EnumDirS0_iii");
    if (onmove_5) {
        hook_char_onmove_5 = hook_thumb(onmove_5 | 1, (uintptr_t)&hooked_CMvCharacter_OnMove_5);
        game_log("[Patch] Hooked CMvCharacter::OnMove(5 args) (0x%08X)\n", (unsigned int)onmove_5);
    }

    uintptr_t set_screen_skill = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvCharacter20SetScreenSkillUsePosERiP8TGXPOINTi");
    if (set_screen_skill) {
        hook_set_screen_skill = hook_thumb(set_screen_skill | 1, (uintptr_t)&hooked_CMvCharacter_SetScreenSkillUsePos);
        game_log("[Patch] Hooked CMvCharacter::SetScreenSkillUsePos (0x%08X)\n", (unsigned int)set_screen_skill);
    }

    uintptr_t wizard_attack = (uintptr_t)so_symbol(&so_mod, "_ZN11CMvPCWizard8DoAttackEv");
    if (wizard_attack) {
        hook_wizard_attack = hook_thumb(wizard_attack | 1, (uintptr_t)&hooked_CMvPCWizard_DoAttack);
        game_log("[Patch] Hooked CMvPCWizard::DoAttack (0x%08X)\n", (unsigned int)wizard_attack);
    }

    uintptr_t rogue_skill = (uintptr_t)so_symbol(&so_mod, "_ZN10CMvPCRogue7DoSkillEv");
    if (rogue_skill) {
        hook_rogue_skill = hook_thumb(rogue_skill | 1, (uintptr_t)&hooked_CMvPCRogue_DoSkill);
        game_log("[Patch] Hooked CMvPCRogue::DoSkill (0x%08X)\n", (unsigned int)rogue_skill);
    }

    uintptr_t map_obj_event = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvMapObject7OnEventEv");
    if (map_obj_event) {
        hook_map_obj_event = hook_thumb(map_obj_event | 1, (uintptr_t)&hooked_CMvMapObject_OnEvent);
        game_log("[Patch] Hooked CMvMapObject::OnEvent (0x%08X)\n", (unsigned int)map_obj_event);
    }

    uintptr_t ride_rotate_rail = (uintptr_t)so_symbol(&so_mod, "_ZN22CMvMapObjectRideRotate12DoRailSetPosEi");
    if (ride_rotate_rail) {
        hook_ride_rotate_rail = hook_thumb(ride_rotate_rail | 1, (uintptr_t)&hooked_CMvMapObjectRideRotate_DoRailSetPos);
        game_log("[Patch] Hooked CMvMapObjectRideRotate::DoRailSetPos (0x%08X)\n", (unsigned int)ride_rotate_rail);
    }

    uintptr_t projectile_move = (uintptr_t)so_symbol(&so_mod, "_ZN13CMvProjectile6OnMoveEv");
    if (projectile_move) {
        hook_projectile_move = hook_thumb(projectile_move | 1, (uintptr_t)&hooked_CMvProjectile_OnMove);
        game_log("[Patch] Hooked CMvProjectile::OnMove (0x%08X)\n", (unsigned int)projectile_move);
    }
    // Fix Bug #?: CMvLayerData::PreLoad skips execution because it checks `if (0 < (long)__src)`
    // Pointers on PS Vita are > 0x81000000, which are negative as signed longs.
    // 0x947bc: ble -> beq (0xdd 0x1f -> 0xd0 0x1f)
    uint16_t beq_instr_layer = 0xd01f;
    kuKernelCpuUnrestrictedMemcpy((void *)(so_mod.text_base + 0x947bc), &beq_instr_layer, sizeof(beq_instr_layer));

    // Redirect internal Gcx_MM memory manager and operators new/delete to native malloc/free
    patch_symbol("_Z12Gcx_MM_Allocj", (uintptr_t)&hooked_Gcx_MM_Alloc);
    patch_symbol("_Z13Gcx_MM_Callocj", (uintptr_t)&hooked_Gcx_MM_Calloc);
    patch_symbol("_Z11Gcx_MM_FreePv", (uintptr_t)&hooked_Gcx_MM_Free);
    patch_symbol("_Z14Gcx_MM_ReallocPvj", (uintptr_t)&hooked_Gcx_MM_Realloc);
    patch_symbol("_Znwj", (uintptr_t)&hooked_operator_new);
    patch_symbol("_Znaj", (uintptr_t)&hooked_operator_new_array);
    patch_symbol("_ZdlPv", (uintptr_t)&hooked_operator_delete);
    patch_symbol("_ZdaPv", (uintptr_t)&hooked_operator_delete_array);

}
