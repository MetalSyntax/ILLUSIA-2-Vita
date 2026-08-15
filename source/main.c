/*
 * main.c
 *
 * PS Vita Loader for ILLUSIA 2 (Android ARMv6 / Gamevil Nexus2 Engine).
 * Loads libgameDSO.so via so_util, initializes FalsoJNI and manages the render/input loop.
 */

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <taihen.h>
#include <vitaGL.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/dialog.h"

#define GAME_W 480
#define GAME_H 320
#define SCREEN_W 960
#define SCREEN_H 544

int _newlib_heap_size_user = 128 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

extern so_default_dynlib default_dynlib[];
extern void resolve_imports(so_module* mod);
extern void illusia_install_array_hooks(void);
extern volatile int g_ui_status;

/* Native JNI function pointers */
int  (*Game_JNI_OnLoad)(void *vm, void *reserved);
void (*InitializeJNIGlobalRef)(void);
void (*NativeInitDeviceInfo)(void *env, void *obj, int w, int h);
void (*NativeInitWithBufferSize)(void *env, void *obj, int w, int h);
void (*NativeRender)(void);
void (*NativeResize)(void *env, void *obj, int w, int h);
void (*NativeResumeClet)(void *env, void *obj);
void (*NativePauseClet)(void *env, void *obj);
void (*NativeDestroyClet)(void *env, void *obj);
void (*handleCletEvent)(void *env, void *obj, int type, int p1, int p2, int p3);

/* Input event definitions */
#define MH_KEY_PRESSEVENT       2
#define MH_KEY_RELEASEEVENT     3
#define MH_POINTER_PRESSEVENT   23
#define MH_POINTER_RELEASEEVENT 24
#define MH_POINTER_MOVEEVENT    25

#define HAL_KEY_UP    (-1)
#define HAL_KEY_DOWN  (-2)
#define HAL_KEY_LEFT  (-3)
#define HAL_KEY_RIGHT (-4)
#define HAL_KEY_OK    (-5)
#define HAL_KEY_MAP   (-6)
#define HAL_KEY_SAVE  (-10)
#define HAL_KEY_BACK  (-16)
#define HAL_KEY_SKIP  (35)

typedef struct {
    int type, p1, p2, p3;
} input_event;

static input_event event_queue[32];
static int eq_head = 0, eq_tail = 0;

static void queue_input_event(int type, int p1, int p2, int p3) {
    int next = (eq_tail + 1) % 32;
    if (next != eq_head) {
        event_queue[eq_tail].type = type;
        event_queue[eq_tail].p1 = p1;
        event_queue[eq_tail].p2 = p2;
        event_queue[eq_tail].p3 = p3;
        eq_tail = next;
    }
}

static const struct { unsigned int btn; int hal; } btn_map[] = {
    { SCE_CTRL_UP,       HAL_KEY_UP },
    { SCE_CTRL_DOWN,     HAL_KEY_DOWN },
    { SCE_CTRL_LEFT,     HAL_KEY_LEFT },
    { SCE_CTRL_RIGHT,    HAL_KEY_RIGHT },
    { SCE_CTRL_CROSS,    HAL_KEY_OK },
    { SCE_CTRL_CIRCLE,   HAL_KEY_BACK },
    { SCE_CTRL_TRIANGLE, HAL_KEY_SKIP },
    { SCE_CTRL_SQUARE,   HAL_KEY_MAP },
    { SCE_CTRL_LTRIGGER, HAL_KEY_SAVE },
};
#define BTN_MAP_COUNT (sizeof(btn_map) / sizeof(btn_map[0]))

void raise_clocks() {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

void gl_init_illusia() {
    vglUseTripleBuffering(GL_FALSE);
    vglInitExtended(0, SCREEN_W, SCREEN_H, 6 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
}

int main() {
    raise_clocks();
    game_log("\n=== Starting ILLUSIA 2 Port (PS Vita) ===\n");

    // 1. Cargar modulo de juego
    int res = so_file_load(&so_mod, "ux0:data/illusia2/libgameDSO.so", 0x98000000);
    if (res < 0) {
        game_log("[-] Error critico cargando libgameDSO.so: 0x%08X\n", res);
        fatal_error("Error: could not load ux0:data/illusia2/libgameDSO.so");
        return -1;
    }
    game_log("[+] libgameDSO.so cargado exitosamente en 0x%08X\n", (unsigned int) so_mod.text_base);

    // 2. Relocalizar y resolver imports
    so_relocate(&so_mod);
    game_log("[+] Relocalizacion completada.\n");

    resolve_imports(&so_mod);
    game_log("[+] Imports resueltos con dynlib.\n");

    so_flush_caches(&so_mod);
    so_initialize(&so_mod);
    game_log("[+] Modulo inicializado.\n");

    // 3. Inicializar graficos y FalsoJNI
    gl_init_illusia();
    game_log("[+] vitaGL inicializado a %dx%d.\n", SCREEN_W, SCREEN_H);

    jni_init();
    illusia_install_array_hooks();
    JNIEnv *jniEnv = &jni;
    game_log("[+] FalsoJNI inicializado.\n");

    // 4. Obtener simbolos JNI exportados por libgameDSO.so
    Game_JNI_OnLoad           = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    InitializeJNIGlobalRef    = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef");
    NativeInitWithBufferSize  = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize");
    NativeInitDeviceInfo      = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo");
    NativeRender              = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeRender");
    NativeResize              = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeResize");
    NativeResumeClet          = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeResumeClet");
    NativePauseClet           = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativePauseClet");
    NativeDestroyClet         = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeDestroyClet");
    handleCletEvent           = (void *)so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_handleCletEvent");

    game_log("[+] Symbols: JNI_OnLoad=%p InitJNI=%p InitBuffer=%p InitDevice=%p Render=%p Resize=%p Resume=%p handleClet=%p\n",
             (void*)Game_JNI_OnLoad, (void*)InitializeJNIGlobalRef, (void*)NativeInitWithBufferSize,
             (void*)NativeInitDeviceInfo, (void*)NativeRender, (void*)NativeResize,
             (void*)NativeResumeClet, (void*)handleCletEvent);

    // 5. Secuencia de arranque del motor Gamevil Nexus2
    game_log("[*] Ejecutando JNI_OnLoad...\n");
    if (Game_JNI_OnLoad) Game_JNI_OnLoad(&jvm, NULL);

    game_log("[*] Ejecutando InitializeJNIGlobalRef...\n");
    if (InitializeJNIGlobalRef) InitializeJNIGlobalRef();

    game_log("[*] Ejecutando NativeInitWithBufferSize(%d, %d)...\n", GAME_W, GAME_H);
    if (NativeInitWithBufferSize) NativeInitWithBufferSize(jniEnv, NULL, GAME_W, GAME_H);

    game_log("[*] Ejecutando NativeInitDeviceInfo(%d, %d)...\n", GAME_W, GAME_H);
    if (NativeInitDeviceInfo) NativeInitDeviceInfo(jniEnv, NULL, GAME_W, GAME_H);

    game_log("[*] Ejecutando NativeResize(%d, %d)...\n", SCREEN_W, SCREEN_H);
    if (NativeResize) NativeResize(jniEnv, NULL, SCREEN_W, SCREEN_H);

    game_log("[*] Ejecutando NativeResumeClet...\n");
    if (NativeResumeClet) NativeResumeClet(jniEnv, NULL);

    // 6. Bucle principal
    game_log("[+] Iniciando Bucle Principal de Juego...\n");
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceCtrlData pad;
    SceTouchData touch;
    int last_touch = 0;
    int last_tx = 0, last_ty = 0;
    unsigned int old_buttons = 0;
    int frame = 0;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        if ((frame++ % 300) == 0) {
            game_log("[LOOP] Frame %d alive, touch.reportNum=%d buttons=0x%08X ui_status=%d\n",
                     frame, touch.reportNum, (unsigned int) pad.buttons, g_ui_status);
        }

        // Salida: START + SELECT
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) {
            game_log("[*] Salida solicitada por usuario (START+SELECT).\n");
            break;
        }

        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;

        for (size_t i = 0; i < BTN_MAP_COUNT; i++) {
            if (pressed & btn_map[i].btn)
                queue_input_event(MH_KEY_PRESSEVENT, btn_map[i].hal, 0, 0);
            if (released & btn_map[i].btn)
                queue_input_event(MH_KEY_RELEASEEVENT, btn_map[i].hal, 0, 0);
        }
        old_buttons = pad.buttons;

        // Mapeo táctil al espacio lógico GAME_W x GAME_H (480x320)
        if (touch.reportNum > 0) {
            int x = touch.report[0].x * GAME_W / 1920;
            int y = touch.report[0].y * GAME_H / 1088;

            if (!last_touch) {
                queue_input_event(MH_POINTER_PRESSEVENT, x, y, 0);
                last_touch = 1;
            } else if (x != last_tx || y != last_ty) {
                queue_input_event(MH_POINTER_MOVEEVENT, x, y, 0);
            }
            last_tx = x;
            last_ty = y;
        } else if (last_touch) {
            queue_input_event(MH_POINTER_RELEASEEVENT, last_tx, last_ty, 0);
            last_touch = 0;
        }

        // Procesar cola de eventos antes del render
        if (handleCletEvent && eq_head != eq_tail) {
            input_event *ev = &event_queue[eq_head];
            eq_head = (eq_head + 1) % 32;
            handleCletEvent(jniEnv, NULL, ev->type, ev->p1, ev->p2, ev->p3);
        }

        // Renderizado
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (NativeRender) {
            NativeRender();
        }

        vglSwapBuffers(GL_FALSE);
    }

    if (NativePauseClet) NativePauseClet(jniEnv, NULL);
    if (NativeDestroyClet) NativeDestroyClet(jniEnv, NULL);

    game_log("[*] ILLUSIA 2 finalizado correctamente.\n");
    sceKernelExitProcess(0);
    return 0;
}
