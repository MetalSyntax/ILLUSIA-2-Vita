#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kubridge.h>
#include <so_util/so_util.h>
#include "utils/logger.h"

extern so_module so_mod;

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
static inline int is_map_tiledata_ready(void) {
    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x80000000) return 0;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);
    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) return 0;

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x80000000) return 0;

    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData || (uintptr_t)tileData < 0x80000000) return 0;

    return 1;
}

/*
 * Hook for GsPZxSubFrame(CGxPZxFrame *pFrame, int subframe_index)
 * _Z13GsPZxSubFrameP11CGxPZxFramei
 */
static int gpzxsf_log_count = 0;
#define GPZXSF_LOG_MAX 50

static void *hooked_GsPZxSubFrame(void *pFrame, int subframe_index) {
    /* Original GsPZxSubFrame has NO range check at all (just pFrame->subframes
     * + index*0x18) — this hook's [0x80000000,0xa0000000) window was added
     * defensively and no caller in the .so was written to expect a NULL
     * back from it. If real sprite/animation frame pointers ever legitimately
     * fall outside this window, every caller silently draws nothing instead
     * of crashing — reported as "static cutscenes / missing graphics", not a
     * crash. Logging (rate-limited) instead of widening the range blind,
     * since the same window is confirmed correct for CMvMap/CMvLayer
     * pointers (rendering works) and may simply be a different case here. */
    if (!pFrame || (uintptr_t)pFrame < 0x80000000 || (uintptr_t)pFrame >= 0xa0000000) {
        if (gpzxsf_log_count < GPZXSF_LOG_MAX) {
            gpzxsf_log_count++;
            game_log("[GsPZxSubFrame] out-of-range pFrame=%p (idx=%d)\n", pFrame, subframe_index);
        }
        return NULL;
    }
    uintptr_t subframes = *(uintptr_t *)((char *)pFrame + 8);
    if (!subframes || subframes < 0x80000000 || subframes >= 0xa0000000) {
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
    if (!pFrame || (uintptr_t)pFrame < 0x80000000 || (uintptr_t)pFrame >= 0xa0000000) return;

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
            if (pSprite && (uintptr_t)pSprite >= 0x80000000 && (uintptr_t)pSprite < 0xa0000000) {
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
    if (font_idx < 0) return 0;

    void **pResourceMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI14CMvResourceMgrE13ms_pSingletonE");
    if (!pResourceMgrSingleton || !*pResourceMgrSingleton) return 0;
    char *resMgr = (char *)*pResourceMgrSingleton;

    void *pCommonRes = *(void **)(resMgr + 4);
    if (!pCommonRes) return 0;

    void *pResInfoArray = *(void **)((char *)pCommonRes + 0x10);
    if (!pResInfoArray) return 0;

    void *pNumberImage = *(void **)((char *)pResInfoArray + 5 * 12 + 4);
    if (!pNumberImage) return 0;

    void *pAni = *(void **)((char *)pNumberImage + 8);
    if (!pAni) return 0;

    void *pFrameMgr = *(void **)((char *)pAni + 12);
    if (!pFrameMgr) return 0;

    int frameCount = *(int *)((char *)pFrameMgr + 12);
    void **ppFrames = *(void ***)((char *)pFrameMgr + 8);
    if (!ppFrames || frameCount <= 0) return 0;

    if (font_idx >= frameCount) {
        font_idx = 0;
    }
    void *pFrame = ppFrames[font_idx];
    if (!pFrame || (uintptr_t)pFrame < 0x80000000 || (uintptr_t)pFrame >= 0xa0000000) return 0;

    int len = strlen(str);
    if (len <= 0) return 0;

    int cur_x = 0;
    int local_38;
    if ((unsigned char)(str[0] - '0') < 10) {
        local_38 = len % 3;
    } else {
        local_38 = (len - 1) % 3;
    }
    if (local_38 == 0) local_38 = 3;

    for (int i = 0; i < len; i++) {
        if (param_7 && local_38 == 0) {
            void *pSub = hooked_GsPZxSubFrame(pFrame, 0xe);
            if (pSub) {
                void *pSprite = *(void **)pSub;
                if (pSprite && (uintptr_t)pSprite >= 0x80000000 && (uintptr_t)pSprite < 0xa0000000) {
                    if (param_8) {
                        void (*sprite_draw)(void *, int, int, int, int, int) = *(void **)(*(int *)pSprite + 0x14);
                        if (sprite_draw) sprite_draw(pSprite, cur_x + x, y, 0xe, param_6, 0);
                    }
                    int (*sprite_width)(void *) = *(int (**)(void *))(*(int *)pSprite + 0x1c);
                    int w = sprite_width ? sprite_width(pSprite) : 0;
                    local_38 = 3;
                    cur_x += param_5 + w;
                }
            }
        }

        unsigned char c = (unsigned char)str[i];
        int sub_idx = -1;
        int delta_y = 0;

        switch (c - 0x20) {
        case 0: sub_idx = 0; break;
        case 3: sub_idx = 0xb; break;
        case 5: sub_idx = 0x12; break;
        case 10: sub_idx = 10; break;
        case 0xb: sub_idx = 0xc; break;
        case 0xc: sub_idx = 0xe; break;
        case 0xd: sub_idx = 0xd; break;
        case 0xe: sub_idx = 0x11; break;
        case 0xf: sub_idx = 0x10; break;
        case 0x1a: sub_idx = 0xf; break;
        default:
            sub_idx = c - '0';
            local_38--;
            break;
        }

        if (sub_idx >= 0) {
            void *pSub = hooked_GsPZxSubFrame(pFrame, sub_idx);
            if (pSub) {
                void *pSprite = *(void **)pSub;
                if (pSprite && (uintptr_t)pSprite >= 0x80000000 && (uintptr_t)pSprite < 0xa0000000) {
                    delta_y = *(int16_t *)((char *)pSub + 6);
                    if (param_8) {
                        void (*sprite_draw)(void *, int, int, int, int, int) = *(void **)(*(int *)pSprite + 0x14);
                        if (sprite_draw) sprite_draw(pSprite, cur_x + x, y + delta_y, 0xe, param_6, 0);
                    }
                    int (*sprite_width)(void *) = *(int (**)(void *))(*(int *)pSprite + 0x1c);
                    int w = sprite_width ? sprite_width(pSprite) : 0;
                    cur_x += param_5 + w;
                }
            }
        }
    }
    return cur_x;
}

/*
 * Hook for CMvNumberObject::Draw()
 * _ZN15CMvNumberObject4DrawEv
 */
static void hooked_CMvNumberObject_Draw(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (*(uint8_t *)((char *)self + 0xc) >= 0x80) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    char *pMap = (char *)*pMapSingleton;

    void **pMvGraphicsSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI11CMvGraphicsE13ms_pSingletonE");
    if (!pMvGraphicsSingleton || !*pMvGraphicsSingleton) return;
    void *pGraphics = *pMvGraphicsSingleton;

    int amount = *(int *)((char *)self + 8);
    int (*GsLogFunc)(int, int) = (int (*)(int, int))so_symbol(&so_mod, "_Z5GsLogii");
    int iVar1 = GsLogFunc ? GsLogFunc(amount, 10) : 1;

    int v1 = (int)*(int8_t *)((char *)self + 0xf) >> 1;
    int iVar3 = 0xf;
    if (v1 <= (int)*(int8_t *)((char *)self + 0xe)) {
        int diff = (int)*(int8_t *)((char *)self + 0xf) - (int)*(int8_t *)((char *)self + 0xe);
        int div = (v1 + 1);
        int v2 = div > 0 ? (diff * 16) / div : 0;
        iVar3 = 0;
        if (v2 > 0) {
            iVar3 = (v2 > 0xf) ? 0xf : v2;
        }
    }

    int font_idx = (int)*(int8_t *)((char *)self + 0x10);
    uint16_t obj_x = *(uint16_t *)((char *)self + 4);
    uint16_t obj_y = *(uint16_t *)((char *)self + 6);
    int16_t map_tx = *(int16_t *)(pMap + 0xac);
    int16_t map_ty = *(int16_t *)(pMap + 0xae);

    int screen_x = (int)(int16_t)(obj_x - ((iVar1 + 1) * 8) - map_tx);
    int screen_y = (int)(int16_t)(obj_y - 8 - map_ty) - 15;

    hooked_CMvGraphics_DrawDamage(pGraphics, font_idx, screen_x, screen_y, amount, 0, -4, 1, iVar3);
}

/*
 * Hook for CMvObjectMgr::DrawDamageObject()
 * _ZN12CMvObjectMgr16DrawDamageObjectEv
 */
static void hooked_CMvObjectMgr_DrawDamageObject(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;

    for (int i = 0; i < 20; i++) {
        void *pNumObj = (char *)self + i * 0x14 + 0x98;
        hooked_CMvNumberObject_Draw(pNumObj);
    }
}

/*
 * Hook for CMvObjectMgr::DrawNpcHeadInfo()
 * _ZN12CMvObjectMgr15DrawNpcHeadInfoEv
 */
static void hooked_CMvObjectMgr_DrawNpcHeadInfo(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;

    typedef void (*DrawHeadInfoFunc)(void *this);
    DrawHeadInfoFunc DrawHeadInfo = (DrawHeadInfoFunc)so_symbol(&so_mod, "_ZN6CMvNPC12DrawHeadInfoEv");
    if (!DrawHeadInfo) return;

    for (int group = 0; group < 2; group++) {
        char *pGroup = (char *)self + 0x78 + group * 16;
        void **ppObjects = *(void ***)pGroup;
        int count = *(int *)(pGroup + 4);
        if (!ppObjects || count <= 0 || (uintptr_t)ppObjects < 0x80000000) continue;

        for (int i = 0; i < count; i++) {
            void *pObj = ppObjects[i];
            if (pObj && (uintptr_t)pObj >= 0x80000000) {
                if (*(int8_t *)((char *)pObj + 4) == 1) {
                    DrawHeadInfo(pObj);
                }
            }
        }
    }
}

/*
 * Hook for CMvObject::GetMapAttrib(int tx, int ty, EnumDir dir, int step)
 * _ZN9CMvObject12GetMapAttribEii7EnumDiri
 */
static uint16_t hooked_CMvObject_GetMapAttrib(void *self, int tx, int ty, int dir, int step) {
    if (tx == -1 && self && (uintptr_t)self >= 0x80000000) {
        tx = *(uint8_t *)((char *)self + 0x26);
    }
    if (ty == -1 && self && (uintptr_t)self >= 0x80000000) {
        ty = *(uint8_t *)((char *)self + 0x27);
    }

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x80000000) return 1;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) return 1;

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x80000000) return 1;

    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData || (uintptr_t)tileData < 0x80000000) return 1;

    int target_x = tx;
    int target_y = ty;

    if (dir != -1) {
        if (dir >= 0 && dir < 8) {
            target_x += (int)dir_deltas[dir][0] * step;
            target_y += (int)dir_deltas[dir][1] * step;
        }
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
    if (!self || (uintptr_t)self < 0x80000000) return 0;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x80000000) return 0;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);
    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) return 0;

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x80000000) return 0;

    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData || (uintptr_t)tileData < 0x80000000) return 0;

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

    if (pRideTarget && (uintptr_t)pRideTarget >= 0x80000000) {
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
    if (!self || (uintptr_t)self < 0x80000000) return 0;

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
    if (!self || (uintptr_t)self < 0x80000000) return 0;

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
    if (!self || (uintptr_t)self < 0x80000000) return;

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
    if (!self || (uintptr_t)self < 0x80000000) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x80000000) return;
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
    if (!pMapSingleton || !*pMapSingleton || (uintptr_t)*pMapSingleton < 0x80000000) return 0;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count <= 0 || map_w <= 0 || map_h <= 0) return 0;

    void *layers = *(void **)(map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x80000000) return 0;

    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData || (uintptr_t)tileData < 0x80000000) return 0;

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
    if (!map || (uintptr_t)map < 0x80000000) return 1;

    int layer_count = *(int *)((char *)map + 0xd8);
    int map_w = *(int *)((char *)map + 0xdc);
    int map_h = *(int *)((char *)map + 0xe0);

    if (layer_count < 1 || tx < 0 || tx >= map_w || ty < 0 || ty >= map_h) {
        return 1;
    }

    void *layers = *(void **)((char *)map + 0xf0);
    if (!layers || (uintptr_t)layers < 0x80000000) return 1;

    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData || (uintptr_t)tileData < 0x80000000) return 1;

    int tile_idx = (ty * map_w + tx) * 3;
    uint8_t f1 = tileData[tile_idx + 1];
    uint8_t f2 = tileData[tile_idx + 2];
    return (uint16_t)f1 | ((uint16_t)f2 << 8);
}

/*
 * Hook for CMvMap::CreateMiniMap()
 * _ZN6CMvMap13CreateMiniMapEv
 */
static void hooked_CreateMiniMap(void *self) {
    if (!self) return;
    int w = *(int *)((char *)self + 0xdc);
    int h = *(int *)((char *)self + 0xe0);
    if (w <= 0 || h <= 0) return;

    if (!MC_grpCreateOffScreenFrameBuffer || !MC_grpGetPixelFromRGB) return;

    void *fb = MC_grpCreateOffScreenFrameBuffer(w, h);
    *(void **)((char *)self + 0xd0) = fb;
    if (!fb) return;

    uint16_t *pixels = *(uint16_t **)((char *)fb + 16);
    if (!pixels) return;

    uint16_t gray = MC_grpGetPixelFromRGB(128, 128, 128);
    uint16_t blue = MC_grpGetPixelFromRGB(48, 48, 223);

    int layer_count = *(int *)((char *)self + 0xd8);
    void *layers = *(void **)((char *)self + 0xf0);
    uint8_t *tileData = NULL;
    if (layers && layer_count > 0) {
        tileData = *(uint8_t **)((char *)layers + 12);
    }

    for (int y = 0; y < h; y++) {
        uint16_t *row = pixels + (y * w);
        for (int x = 0; x < w; x++) {
            uint16_t pixel = blue;
            if (tileData && y < h && x < w) {
                int tile_idx = y * w + x;
                uint8_t *tile = tileData + tile_idx * 3;
                uint16_t flag = (uint16_t)tile[1] | ((uint16_t)tile[2] << 8);
                if (!(flag & 0x100) && !(flag & 0x0f) && !(flag & 0x30) && !(flag & 0x40)) {
                    pixel = gray;
                }
            }
            row[x] = pixel;
        }
    }
}

/*
 * Hook for CMvMap::DrawMiniMap()
 * _ZN6CMvMap11DrawMiniMapEv
 */
static so_hook hook_draw_minimap;
static void hooked_CMvMap_DrawMiniMap(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (!is_map_tiledata_ready()) return;

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    if (!pObjMgrSingleton || !*pObjMgrSingleton || (uintptr_t)*pObjMgrSingleton < 0x80000000) return;
    void *pPlayer = *(void **)((char *)*pObjMgrSingleton + 8);
    if (!pPlayer || (uintptr_t)pPlayer < 0x80000000) return;

    SO_CONTINUE_VOID(hook_draw_minimap, self);
}

/*
 * Hook for CMvLayer::Draw()
 * _ZN8CMvLayer4DrawEiiP12tagTILEPOINTb10enumDrawOPi
 */
static void hooked_CMvLayer_Draw(void *self, int screen_x, int screen_y, void *tile_pt,
                                 int arg4, int draw_op, int arg6) {
    if (!self) return;
    uint8_t *pTileData = *(uint8_t **)((char *)self + 12);
    if (!pTileData) return;
    if (!tile_pt) return;

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return;
    void *pMap = *pMapSingleton;

    int map_w = *(int *)((char *)pMap + 0xdc);
    int map_h = *(int *)((char *)pMap + 0xe0);
    if (map_w <= 0 || map_h <= 0) return;

    void *pTileImages = *(void **)((char *)pMap + 200);
    if (!pTileImages) return;

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    void *pObjMgr = pObjMgrSingleton ? *pObjMgrSingleton : NULL;
    void *pPlayer = pObjMgr ? *(void **)((char *)pObjMgr + 8) : NULL;

    uint8_t player_x = pPlayer ? *(uint8_t *)((char *)pPlayer + 0x26) : 0;
    char player_state = pPlayer ? *(char *)((char *)pPlayer + 0x20) : 0;
    uint32_t player_y = pPlayer ? (uint32_t)*(uint8_t *)((char *)pPlayer + 0x27) : 0;

    if (screen_y >= 320) return;

    uint8_t tile_start_y = ((uint8_t *)tile_pt)[1];
    uint32_t cur_tile_y = tile_start_y;
    int cur_y = screen_y;

    do {
        if ((cur_y + 15 >= 0) && ((int)cur_tile_y < map_h) && (screen_x < 480)) {
            uint8_t tile_start_x = ((uint8_t *)tile_pt)[0];
            int cur_x = screen_x;
            int tile_x_offset = 0;

            do {
                int tile_map_x = tile_x_offset + tile_start_x;
                if ((cur_x + 15 >= 0) && (tile_map_x < map_w)) {
                    int tile_idx = (cur_tile_y * map_w + tile_map_x) * 3;
                    uint8_t tile_id = pTileData[tile_idx];
                    if (tile_id != 0) {
                        int tile_sprite_idx = tile_id - 1;
                        int local_54 = 0;

                        if (player_state == 12) {
                            /* Original checks rows {player_y, player_y+1, player_y-1}, i.e. the
                             * row above the player too — not player_y+1 twice. */
                            if ((player_y == cur_tile_y || cur_tile_y == player_y + 1 || (player_y > 0 && cur_tile_y == player_y - 1)) &&
                                ((tile_start_x - player_x + tile_x_offset) < 6 && (tile_start_x - player_x + tile_x_offset) > -6)) {
                                int diff = (tile_start_x - player_x + tile_x_offset);
                                if (diff < 0) diff = -diff;
                                local_54 = (5 - diff) * 2;

                                void *layer0_data = *(void **)((char *)pMap + 0xf0);
                                uint8_t *layer0_tiles = layer0_data ? *(uint8_t **)((char *)layer0_data + 12) : NULL;
                                if (layer0_tiles && *(int *)((char *)pMap + 0xd8) > 0 && player_x < map_w && player_y < map_h) {
                                    uint8_t f = layer0_tiles[(player_y * map_w + player_x) * 3 + 1];
                                    if ((f & 0x10) || (f & 0x20)) {
                                        void *pImagesStruct = *(void **)((char *)pTileImages + 8);
                                        if (pImagesStruct && tile_sprite_idx < *(int *)((char *)pImagesStruct + 12)) {
                                            void **sprite_array = *(void ***)((char *)pImagesStruct + 8);
                                            void *sprite = sprite_array ? sprite_array[tile_sprite_idx] : NULL;
                                            if (sprite) {
                                                void (*sprite_draw)(void *, int, int, int, int, int) = *(void **)(*(int *)sprite + 0x14);
                                                if (sprite_draw) sprite_draw(sprite, cur_x, cur_y, draw_op, arg6, 0);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        void *pImagesStruct = *(void **)((char *)pTileImages + 8);
                        if (pImagesStruct && tile_sprite_idx < *(int *)((char *)pImagesStruct + 12)) {
                            void **sprite_array = *(void ***)((char *)pImagesStruct + 8);
                            void *sprite = sprite_array ? sprite_array[tile_sprite_idx] : NULL;
                            if (sprite) {
                                void (*sprite_draw)(void *, int, int, int, int, int) = *(void **)(*(int *)sprite + 0x14);
                                if (sprite_draw) sprite_draw(sprite, cur_x, cur_y + local_54, draw_op, arg6, 0);
                            }
                        }
                    }
                }
                cur_x += 16;
                tile_x_offset++;
            } while (cur_x < 480);
        }
        cur_y += 16;
        cur_tile_y++;
    } while (cur_y < 320);
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

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count < 1 || tx < 0 || tx >= map_w || ty < 0 || ty >= map_h) {
        return 1;
    }

    void *layers = *(void **)(map + 0xf0);
    if (!layers) return 1;
    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData) return 1;

    int tile_idx = (ty * map_w + tx) * 3;
    uint8_t f1 = tileData[tile_idx + 1];
    uint8_t f2 = tileData[tile_idx + 2];
    return (uint16_t)f1 | ((uint16_t)f2 << 8);
}

/*
 * Hook for FUN_0009b9f8 / CMvObjectMgr GetTileAttr for Object (during DoMapChange)
 * Located at text_base + 0x0009b9f8
 */
static uint16_t hooked_GetTileAttr_Obj(void *obj) {
    if (!obj) return 1;
    uint8_t tx = *(uint8_t *)((char *)obj + 0x26);
    uint8_t ty = *(uint8_t *)((char *)obj + 0x27);

    void **pMapSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI6CMvMapE13ms_pSingletonE");
    if (!pMapSingleton || !*pMapSingleton) return 1;
    char *map = (char *)*pMapSingleton;

    int layer_count = *(int *)(map + 0xd8);
    int map_w = *(int *)(map + 0xdc);
    int map_h = *(int *)(map + 0xe0);

    if (layer_count < 1 || tx >= map_w || ty >= map_h) return 1;

    void *layers = *(void **)(map + 0xf0);
    if (!layers) return 1;
    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
    if (!tileData) return 1;

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
            uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
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
            uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
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
    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
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
static void hooked_CMvMap_Draw(void *self) {
    if (!self) return;
    if (*(int *)((char *)self + 200) == 0) return;

    int is_dark = *(uint8_t *)((char *)self + 0x360);

    typedef void (*ClearScreenFunc)(void *this, uint32_t col);
    typedef void *(*GetTblFunc)(void *this, int id);
    typedef int (*GetValFunc)(void *this, int col, int row);
    typedef void (*DrawColorGradationRectFunc)(void *this, int x, int y, int w, int h, int dir, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2);
    typedef void (*DrawHFunc)(void *this, int x, int y);
    typedef void (*DrawCloudFunc)(void *this);
    typedef void (*DrawWeatherFunc)(int type);
    typedef void (*PrepareLightFunc)(void *this, int val);
    typedef int (*GetShakeOffFunc)(void *this);
    typedef void (*ParticleDrawFunc)(void *this, int layer, int b);
    typedef void (*PaletteFunc)(void *this, void *bitmap);
    typedef void (*ObjMgrDrawFunc)(void *this, int layer, int b);
    typedef void (*DrawMapChangeZoneEffectFunc)(void *this);
    typedef void (*DrawBackupScreenFunc)(void *this, int arg1, int arg2);
    typedef int (*GetSkillLVFunc)(void *this, int skill);
    typedef int (*GetUpAndDownCountFunc)(int v, int max, int step);
    typedef void (*DrawScreenMosaicFunc)(void *this, int val, int mode);
    typedef void (*PlayerAreaFunc)(void *this, int val);

    void **pGraphicsSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI11CGsGraphicsE13ms_pSingletonE");
    void *pGraphics = pGraphicsSingleton ? *pGraphicsSingleton : NULL;

    void *scrollBG = *(void **)((char *)self + 0xf4);
    if (!scrollBG) {
        if (*(int *)((char *)self + 0xe4) == 7) {
            ClearScreenFunc ClearScreen = (ClearScreenFunc)so_symbol(&so_mod, "_ZN11CGsGraphics11ClearScreenEj");
            if (ClearScreen && pGraphics) ClearScreen(pGraphics, 0);
        } else {
            uint8_t map_id = *(uint8_t *)((char *)self + 4);
            void **pXlsMgr = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI9CMvXlsMgrE13ms_pSingletonE");
            GetTblFunc GetTbl = (GetTblFunc)so_symbol(&so_mod, "_ZN9CMvXlsMgr6GetTblEi");
            GetValFunc GetVal = (GetValFunc)so_symbol(&so_mod, "_ZN10GVXLLoader6GetValEii");
            if (pXlsMgr && *pXlsMgr && GetTbl && GetVal) {
                void *tbl = GetTbl(*pXlsMgr, 10);
                int val = GetVal(tbl, 2, (int)map_id);
                int idx = val * 3;
                if (idx >= 0 && idx + 2 < 0x18) {
                    uint8_t *dat0 = (uint8_t *)((char *)so_mod.text_base + 0x13f900);
                    uint8_t *dat1 = (uint8_t *)((char *)so_mod.text_base + 0x13f918);
                    DrawColorGradationRectFunc DrawGrad = (DrawColorGradationRectFunc)so_symbol(&so_mod, "_ZN11CGsGraphics22DrawColorGradationRectEiiiiNS_7EnumDirEhhhhhh");
                    if (DrawGrad && pGraphics) {
                        DrawGrad(pGraphics, 0, 0, 480, 320, 1, dat0[idx], dat0[idx+1], dat0[idx+2], dat1[idx], dat1[idx+1], dat1[idx+2]);
                    }
                }
            }
        }
    } else {
        int map_h = *(int *)((char *)self + 0xe0);
        int16_t off_y = *(int16_t *)((char *)self + 0xae);
        ClearScreenFunc ClearScreen = (ClearScreenFunc)so_symbol(&so_mod, "_ZN11CGsGraphics11ClearScreenEj");
        if (ClearScreen && pGraphics) ClearScreen(pGraphics, 0);

        GetShakeOffFunc GetShakeOffX = (GetShakeOffFunc)so_symbol(&so_mod, "_ZN6CMvMap12GetShakeOffXEv");
        GetShakeOffFunc GetShakeOffY = (GetShakeOffFunc)so_symbol(&so_mod, "_ZN6CMvMap12GetShakeOffYEv");
        int sx = GetShakeOffX ? GetShakeOffX(self) : 0;
        int sy = GetShakeOffY ? GetShakeOffY(self) : 0;
        DrawHFunc DrawH = (DrawHFunc)so_symbol(&so_mod, "_ZN11CMvScrollBG5DrawHEii");
        if (DrawH) {
            DrawH(scrollBG, sx, sy + (((map_h * 16 - 320) - off_y) >> 2));
        }
    }

    void **pMvGraphicsSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI11CMvGraphicsE13ms_pSingletonE");
    void *pMvGraphics = pMvGraphicsSingleton ? *pMvGraphicsSingleton : NULL;

    if (!is_dark) {
        DrawCloudFunc DrawCloud = (DrawCloudFunc)so_symbol(&so_mod, "_ZN6CMvMap9DrawCloudEv");
        if (DrawCloud) DrawCloud(self);
    }

    if (pMvGraphics && *(int *)((char *)pMvGraphics + 8) == 2) {
        DrawWeatherFunc DrawBottomWeather = (DrawWeatherFunc)so_symbol(&so_mod, "_Z23DrawBottomWeatherEffect15EnumWeatherType");
        if (DrawBottomWeather) DrawBottomWeather(*(int *)((char *)self + 0xec));
    }

    PrepareLightFunc PrepareLight = (PrepareLightFunc)so_symbol(&so_mod, "_ZN11CMvGraphics12PrepareLightEi");
    if (PrepareLight && pMvGraphics) PrepareLight(pMvGraphics, *(int *)((char *)self + 0xe8));

    uint8_t tpx = *(uint8_t *)((char *)self + 0x304);
    int16_t tx_off = *(int16_t *)((char *)self + 0xac);
    uint8_t tpy = *(uint8_t *)((char *)self + 0x305);
    int16_t ty_off = *(int16_t *)((char *)self + 0xae);

    GetShakeOffFunc GetShakeOffX = (GetShakeOffFunc)so_symbol(&so_mod, "_ZN6CMvMap12GetShakeOffXEv");
    GetShakeOffFunc GetShakeOffY = (GetShakeOffFunc)so_symbol(&so_mod, "_ZN6CMvMap12GetShakeOffYEv");
    int shake_x = GetShakeOffX ? GetShakeOffX(self) : 0;
    int shake_y = GetShakeOffY ? GetShakeOffY(self) : 0;

    int render_x = (int)tpx * 16 - (int)tx_off + shake_x;
    int render_y = (int)tpy * 16 - (int)ty_off + shake_y;

    void **pParticleMgr = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI16CGsParticleMgrExE13ms_pSingletonE");
    ParticleDrawFunc ParticleDraw = (ParticleDrawFunc)so_symbol(&so_mod, "_ZN16CGsParticleMgrEx4DrawEib");
    if (ParticleDraw && pParticleMgr && *pParticleMgr) {
        ParticleDraw(*pParticleMgr, 0, 1);
    }

    void *tile_images = *(void **)((char *)self + 200);
    void *img_bitmap = NULL;
    if (tile_images && (uintptr_t)tile_images >= 0x80000000) {
        void *p8 = *(void **)((char *)tile_images + 8);
        if (p8 && (uintptr_t)p8 >= 0x80000000) {
            void *p8_8 = *(void **)((char *)p8 + 8);
            if (p8_8 && (uintptr_t)p8_8 >= 0x80000000) {
                img_bitmap = *(void **)p8_8;
            }
        }
    }

    PaletteFunc PaletteBackup = (PaletteFunc)so_symbol(&so_mod, "_ZN6CMvMap13PaletteBackupEP12CGxPZxBitmap");
    PaletteFunc PaletteFadeOut = (PaletteFunc)so_symbol(&so_mod, "_ZN6CMvMap14PaletteFadeOutEP12CGxPZxBitmap");
    PaletteFunc PaletteRestore = (PaletteFunc)so_symbol(&so_mod, "_ZN6CMvMap14PaletteRestoreEP12CGxPZxBitmap");

    if (PaletteBackup && img_bitmap) PaletteBackup(self, img_bitmap);
    if (PaletteFadeOut && img_bitmap) PaletteFadeOut(self, img_bitmap);

    void *tile_pt = (void *)((char *)self + 0x304);
    void *layers = *(void **)((char *)self + 0xf0);

    void **pObjMgrSingleton = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI12CMvObjectMgrE13ms_pSingletonE");
    void *pObjMgr = pObjMgrSingleton ? *pObjMgrSingleton : NULL;
    ObjMgrDrawFunc ObjMgrDraw = (ObjMgrDrawFunc)so_symbol(&so_mod, "_ZN12CMvObjectMgr4DrawE15EnumObjectLayerb");

    if (layers) {
        hooked_CMvLayer_Draw(layers, render_x, render_y, tile_pt, 0, 0, 0);
    }
    if (ObjMgrDraw && pObjMgr) ObjMgrDraw(pObjMgr, 0, 0);
    if (ParticleDraw && pParticleMgr && *pParticleMgr) ParticleDraw(*pParticleMgr, 1, 1);

    if (layers) {
        hooked_CMvLayer_Draw((char *)layers + 24, render_x, render_y, tile_pt, 0, 0, 0);
    }
    if (ObjMgrDraw && pObjMgr) ObjMgrDraw(pObjMgr, 1, 0);
    if (ParticleDraw && pParticleMgr && *pParticleMgr) ParticleDraw(*pParticleMgr, 2, 1);

    DrawMapChangeZoneEffectFunc DrawMapChangeZoneEffect = (DrawMapChangeZoneEffectFunc)so_symbol(&so_mod, "_ZN6CMvMap23DrawMapChangeZoneEffectEv");
    if (DrawMapChangeZoneEffect) DrawMapChangeZoneEffect(self);

    int under_flag = 0;
    void *pPlayer = pObjMgr ? *(void **)((char *)pObjMgr + 8) : NULL;
    if (pPlayer) {
        uint8_t px = *(uint8_t *)((char *)pPlayer + 0x26);
        uint8_t py = *(uint8_t *)((char *)pPlayer + 0x27);
        int map_w = *(int *)((char *)self + 0xdc);
        int map_h = *(int *)((char *)self + 0xe0);
        int layer_count = *(int *)((char *)self + 0xd8);
        if (layer_count > 0 && px < map_w && py < map_h && layers) {
            uint8_t *layer0_tile = *(uint8_t **)((char *)layers + 12);
            if (layer0_tile) {
                under_flag = layer0_tile[(py * map_w + px) * 3 + 2] & 1;
            }
        }
    }

    int skill_level = -1;
    if (*(uint8_t *)((char *)self + 0x34c) == 0) {
        int screenEffCounter = *(int *)((char *)self + 0x354);
        /* Original: (0 < val) && (-1 < val << 0x1f) — the shift isolates val's
         * LSB, so this only fires while the counter is a positive EVEN value. */
        if (screenEffCounter > 0 && !(screenEffCounter & 1)) {
            void **pEffMgr = (void **)so_symbol(&so_mod, "_ZN12CGsSingletonI15CGsScreenEffMgrE13ms_pSingletonE");
            DrawBackupScreenFunc DrawBackup = (DrawBackupScreenFunc)so_symbol(&so_mod, "_ZN15CGsScreenEffMgr16DrawBackupScreenEii");
            if (DrawBackup && pEffMgr && *pEffMgr) DrawBackup(*pEffMgr, 1, 6);
        }
        if (pObjMgr) {
            void *p10 = *(void **)((char *)pObjMgr + 0x10);
            GetSkillLVFunc GetSkillLV = (GetSkillLVFunc)so_symbol(&so_mod, "_ZN15CMvSpriteObject10GetSkillLVE15EnumSpriteSkill");
            if (GetSkillLV && p10) skill_level = GetSkillLV(p10, 2);
        }
    } else {
        if (pMvGraphics) {
            int counter = *(int *)((char *)pMvGraphics + 0x18) + 1;
            *(int *)((char *)pMvGraphics + 0x18) = counter;
            GetUpAndDownCountFunc GetCount = (GetUpAndDownCountFunc)so_symbol(&so_mod, "_Z17GetUpAndDownCountiii");
            DrawScreenMosaicFunc DrawMosaic = (DrawScreenMosaicFunc)so_symbol(&so_mod, "_ZN11CMvGraphics16DrawScreenMosaicEii");
            int c = GetCount ? GetCount(counter, 6, 4) : 0;
            if (DrawMosaic) DrawMosaic(pMvGraphics, c, 3);
        }
        if (pObjMgr) {
            void *p10 = *(void **)((char *)pObjMgr + 0x10);
            GetSkillLVFunc GetSkillLV = (GetSkillLVFunc)so_symbol(&so_mod, "_ZN15CMvSpriteObject10GetSkillLVE15EnumSpriteSkill");
            if (GetSkillLV && p10) skill_level = GetSkillLV(p10, 2);
        }
    }

    int circle_radius = (skill_level == -1) ? 50 : 65;

    if (under_flag == 0) {
        if (layers) {
            hooked_CMvLayer_Draw((char *)layers + 48, render_x, render_y, tile_pt, 0, 0, 0);
        }
        if (ObjMgrDraw && pObjMgr) ObjMgrDraw(pObjMgr, 2, 0);
    } else {
        PlayerAreaFunc CopyArea = (PlayerAreaFunc)so_symbol(&so_mod, "_ZN9CMvPlayer14CopyScreenAreaEi");
        PlayerAreaFunc DrawCircle = (PlayerAreaFunc)so_symbol(&so_mod, "_ZN9CMvPlayer20DrawCircleScreenAreaEi");
        if (CopyArea && pPlayer) CopyArea(pPlayer, circle_radius);
        if (layers) {
            hooked_CMvLayer_Draw((char *)layers + 48, render_x, render_y, tile_pt, 0, 0, 0);
        }
        if (ObjMgrDraw && pObjMgr) ObjMgrDraw(pObjMgr, 2, 0);
        if (DrawCircle && pPlayer) DrawCircle(pPlayer, circle_radius);
    }

    if (ParticleDraw && pParticleMgr && *pParticleMgr) ParticleDraw(*pParticleMgr, 3, 1);

    if (pObjMgr) {
        hooked_CMvObjectMgr_DrawDamageObject(pObjMgr);
        hooked_CMvObjectMgr_DrawNpcHeadInfo(pObjMgr);
    }

    if (PaletteRestore && img_bitmap) PaletteRestore(self, img_bitmap);

    if (!is_dark) {
        typedef void (*ScreenPaletteScrollFunc)(void *this);
        ScreenPaletteScrollFunc PaletteScroll = (ScreenPaletteScrollFunc)so_symbol(&so_mod, "_ZN6CMvMap19ScreenPaletteScrollEv");
        if (PaletteScroll) PaletteScroll(self);
    }

    if (pMvGraphics && *(int *)((char *)pMvGraphics + 8) == 2) {
        DrawWeatherFunc DrawTopWeather = (DrawWeatherFunc)so_symbol(&so_mod, "_Z20DrawTopWeatherEffect15EnumWeatherType");
        if (DrawTopWeather) DrawTopWeather(*(int *)((char *)self + 0xec));
    }
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
    uint8_t *tileData = *(uint8_t **)((char *)layers + 12);
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

static so_hook hook_char_onmove_1;
static int hooked_CMvCharacter_OnMove_1(void *self, int param_1) {
    if (!self || (uintptr_t)self < 0x80000000) return 0;
    if (!is_map_tiledata_ready()) {
        if (move_gate_log_count < MOVE_GATE_LOG_MAX) {
            move_gate_log_count++;
            game_log("[Move] OnMove_1 blocked: map tiledata not ready (self=%p)\n", self);
        }
        return 0;
    }
    return SO_CONTINUE(int, hook_char_onmove_1, self, param_1);
}

static so_hook hook_char_onmove_5;
static int hooked_CMvCharacter_OnMove_5(void *self, int dir1, int dir2, int p3, int p4, int p5) {
    if (!self || (uintptr_t)self < 0x80000000) return 0;
    if (!is_map_tiledata_ready()) {
        if (move_gate_log_count < MOVE_GATE_LOG_MAX) {
            move_gate_log_count++;
            game_log("[Move] OnMove_5 blocked: map tiledata not ready (self=%p dir1=%d dir2=%d)\n", self, dir1, dir2);
        }
        return 0;
    }
    return SO_CONTINUE(int, hook_char_onmove_5, self, dir1, dir2, p3, p4, p5);
}

static so_hook hook_set_screen_skill;
static void hooked_CMvCharacter_SetScreenSkillUsePos(void *self, int *p1, void *p2, int p3) {
    if (!self || (uintptr_t)self < 0x80000000 || !p1 || !p2) return;
    if (!is_map_tiledata_ready()) return;
    SO_CONTINUE_VOID(hook_set_screen_skill, self, p1, p2, p3);
}

static so_hook hook_wizard_attack;
static void hooked_CMvPCWizard_DoAttack(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (!is_map_tiledata_ready()) return;
    SO_CONTINUE_VOID(hook_wizard_attack, self);
}

static so_hook hook_rogue_skill;
static void hooked_CMvPCRogue_DoSkill(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (!is_map_tiledata_ready()) return;
    SO_CONTINUE_VOID(hook_rogue_skill, self);
}

static so_hook hook_map_obj_event;
static void hooked_CMvMapObject_OnEvent(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (!is_map_tiledata_ready()) return;
    SO_CONTINUE_VOID(hook_map_obj_event, self);
}

static so_hook hook_ride_rotate_rail;
static void hooked_CMvMapObjectRideRotate_DoRailSetPos(void *self, int param_1) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (!is_map_tiledata_ready()) return;
    SO_CONTINUE_VOID(hook_ride_rotate_rail, self, param_1);
}

static so_hook hook_projectile_move;
static void hooked_CMvProjectile_OnMove(void *self) {
    if (!self || (uintptr_t)self < 0x80000000) return;
    if (!is_map_tiledata_ready()) return;
    SO_CONTINUE_VOID(hook_projectile_move, self);
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
        hook_thumb_func(draw_damage, (uintptr_t)&hooked_CMvGraphics_DrawDamage);
        game_log("[Patch] Hooked CMvGraphics::DrawDamage (0x%08X)\n", (unsigned int)draw_damage);
    }

    uintptr_t draw_numbers = (uintptr_t)so_symbol(&so_mod, "_ZN11CMvGraphics11DrawNumbersEiiiPKcijbb");
    if (draw_numbers) {
        hook_thumb_func(draw_numbers, (uintptr_t)&hooked_CMvGraphics_DrawNumbers);
        game_log("[Patch] Hooked CMvGraphics::DrawNumbers (0x%08X)\n", (unsigned int)draw_numbers);
    }

    uintptr_t num_draw = (uintptr_t)so_symbol(&so_mod, "_ZN15CMvNumberObject4DrawEv");
    if (num_draw) {
        hook_thumb_func(num_draw, (uintptr_t)&hooked_CMvNumberObject_Draw);
        game_log("[Patch] Hooked CMvNumberObject::Draw (0x%08X)\n", (unsigned int)num_draw);
    }

    uintptr_t draw_damage_obj = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvObjectMgr16DrawDamageObjectEv");
    if (draw_damage_obj) {
        hook_thumb_func(draw_damage_obj, (uintptr_t)&hooked_CMvObjectMgr_DrawDamageObject);
        game_log("[Patch] Hooked CMvObjectMgr::DrawDamageObject (0x%08X)\n", (unsigned int)draw_damage_obj);
    }

    uintptr_t draw_npc_head = (uintptr_t)so_symbol(&so_mod, "_ZN12CMvObjectMgr15DrawNpcHeadInfoEv");
    if (draw_npc_head) {
        hook_thumb_func(draw_npc_head, (uintptr_t)&hooked_CMvObjectMgr_DrawNpcHeadInfo);
        game_log("[Patch] Hooked CMvObjectMgr::DrawNpcHeadInfo (0x%08X)\n", (unsigned int)draw_npc_head);
    }

    uintptr_t create_minimap = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap13CreateMiniMapEv");
    if (create_minimap) {
        hook_thumb_func(create_minimap, (uintptr_t)&hooked_CreateMiniMap);
        game_log("[Patch] Hooked CMvMap::CreateMiniMap (0x%08X)\n", (unsigned int)create_minimap);
    }

    uintptr_t draw_minimap = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap11DrawMiniMapEv");
    if (draw_minimap) {
        hook_draw_minimap = hook_thumb(draw_minimap | 1, (uintptr_t)&hooked_CMvMap_DrawMiniMap);
        game_log("[Patch] Hooked CMvMap::DrawMiniMap (0x%08X)\n", (unsigned int)draw_minimap);
    }

    uintptr_t map_draw = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap4DrawEv");
    if (map_draw) {
        hook_thumb_func(map_draw, (uintptr_t)&hooked_CMvMap_Draw);
        game_log("[Patch] Hooked CMvMap::Draw (0x%08X)\n", (unsigned int)map_draw);
    }

    uintptr_t is_save_map = (uintptr_t)so_symbol(&so_mod, "_ZN6CMvMap9IsSaveMapEv");
    if (is_save_map) {
        hook_thumb_func(is_save_map, (uintptr_t)&hooked_CMvMap_IsSaveMap);
        game_log("[Patch] Hooked CMvMap::IsSaveMap (0x%08X)\n", (unsigned int)is_save_map);
    }

    uintptr_t layer_draw = (uintptr_t)so_symbol(&so_mod, "_ZN8CMvLayer4DrawEiiP12tagTILEPOINTb10enumDrawOPi");
    if (layer_draw) {
        hook_thumb_func(layer_draw, (uintptr_t)&hooked_CMvLayer_Draw);
        game_log("[Patch] Hooked CMvLayer::Draw (0x%08X)\n", (unsigned int)layer_draw);
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
}
