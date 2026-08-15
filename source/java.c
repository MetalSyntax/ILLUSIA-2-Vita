/*
 * java.c
 *
 * Fake JNI environment and method handlers for Gamevil Nexus2 engine (ILLUSIA 2).
 * Handles Dalvik-compatible ArrayObjects, Gamevil Font API (GFA), Save/Load I/O and Audio.
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <psp2/io/stat.h>

#include "audio.h"
#include "utils/logger.h"

// UI status reported by engine via OnUIStatusChange
volatile int g_ui_status = -1;

#define SAVE_DIR "ux0:data/illusia2/saves"

static void ensure_save_dir(void) {
    sceIoMkdir("ux0:data/illusia2", 0777);
    sceIoMkdir(SAVE_DIR, 0777);
}

// Path resolver for ILLUSIA 2 assets
static int illusia_resolve_asset_path(const char *name, char *out, size_t out_size) {
    if (!name) return 0;

    // Direct path under ux0:data/illusia2/
    snprintf(out, out_size, "ux0:data/illusia2/%s", name);
    if (access(out, F_OK) == 0) return 1;

    // Assets folder path ux0:data/illusia2/assets/
    snprintf(out, out_size, "ux0:data/illusia2/assets/%s", name);
    if (access(out, F_OK) == 0) return 1;

    return 0;
}

/*
 * Dalvik ArrayObject registry for seamless JNI array handling
 */
#define DALVIK_REGISTRY_MAX 1024
static void *g_dalvik_registry[DALVIK_REGISTRY_MAX];
static int g_dalvik_registry_count = 0;

static void illusia_register_dalvik_array(void *ptr) {
    if (g_dalvik_registry_count < DALVIK_REGISTRY_MAX) {
        g_dalvik_registry[g_dalvik_registry_count++] = ptr;
    } else {
        game_log("[Java] Warning: Dalvik array registry full (%d)\n", DALVIK_REGISTRY_MAX);
    }
}

static int illusia_is_dalvik_array(void *ptr) {
    for (int i = 0; i < g_dalvik_registry_count; i++) {
        if (g_dalvik_registry[i] == ptr) return 1;
    }
    return 0;
}

static jsize (*orig_GetArrayLength)(JNIEnv *, jarray) = NULL;
static jbyte *(*orig_GetByteArrayElements)(JNIEnv *, jbyteArray, jboolean *) = NULL;
static void (*orig_GetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, jbyte *) = NULL;
static void (*orig_ReleaseByteArrayElements)(JNIEnv *, jbyteArray, jbyte *, jint) = NULL;

static jsize Illusia_GetArrayLength_wrapper(JNIEnv *env, jarray array) {
    if (illusia_is_dalvik_array(array)) {
        return (jsize) *(uint32_t *)((char *) array + 8);
    }
    if (orig_GetArrayLength) return orig_GetArrayLength(env, array);
    return 0;
}

static jbyte *Illusia_GetByteArrayElements_wrapper(JNIEnv *env, jbyteArray array, jboolean *isCopy) {
    if (illusia_is_dalvik_array(array)) {
        if (isCopy) *isCopy = JNI_FALSE;
        return (jbyte *)((char *) array + 16);
    }
    if (orig_GetByteArrayElements) return orig_GetByteArrayElements(env, array, isCopy);
    return NULL;
}

static void Illusia_GetByteArrayRegion_wrapper(JNIEnv *env, jbyteArray array, jsize start, jsize len, jbyte *buf) {
    if (illusia_is_dalvik_array(array)) {
        memcpy(buf, (char *) array + 16 + start, len);
        return;
    }
    if (orig_GetByteArrayRegion) orig_GetByteArrayRegion(env, array, start, len, buf);
}

static void Illusia_ReleaseByteArrayElements_wrapper(JNIEnv *env, jbyteArray array, jbyte *elems, jint mode) {
    if (illusia_is_dalvik_array(array)) {
        return;
    }
    if (orig_ReleaseByteArrayElements) orig_ReleaseByteArrayElements(env, array, elems, mode);
}

void illusia_install_array_hooks(void) {
    struct JNINativeInterface *funcs = (struct JNINativeInterface *)(uintptr_t) jni;
    orig_GetArrayLength = funcs->GetArrayLength;
    orig_GetByteArrayElements = funcs->GetByteArrayElements;
    orig_GetByteArrayRegion = funcs->GetByteArrayRegion;
    orig_ReleaseByteArrayElements = funcs->ReleaseByteArrayElements;
    funcs->GetArrayLength = Illusia_GetArrayLength_wrapper;
    funcs->GetByteArrayElements = Illusia_GetByteArrayElements_wrapper;
    funcs->GetByteArrayRegion = Illusia_GetByteArrayRegion_wrapper;
    funcs->ReleaseByteArrayElements = Illusia_ReleaseByteArrayElements_wrapper;
    game_log("[Java] FalsoJNI array wrappers installed.\n");
}

/*
 * Method Implementations
 */
static int readassets_log_count = 0;
#define READASSETS_LOG_MAX 30

jobject Illusia_readAssets(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    const char *name = (const char *) filename;

    if (!name) return NULL;

    char path[256];
    if (!illusia_resolve_asset_path(name, path, sizeof(path))) {
        if (readassets_log_count < READASSETS_LOG_MAX)
            game_log("[Java] readAssets: not found: %s\n", name);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        game_log("[Java] readAssets: failed to open %s\n", path);
        return NULL;
    }

    struct stat st;
    long size = -1;
    if (fstat(fileno(f), &st) == 0) {
        size = st.st_size;
    }

    if (size < 0 || size > 64 * 1024 * 1024) {
        game_log("[Java] readAssets: invalid size %ld for %s\n", size, path);
        fclose(f);
        return NULL;
    }

    void *array_obj = malloc(16 + size);
    if (!array_obj) {
        fclose(f);
        return NULL;
    }

    memset(array_obj, 0, 16);
    *(uint32_t *)((char *)array_obj + 8) = (uint32_t)size;

    fread((char *) array_obj + 16, 1, size, f);
    fclose(f);

    illusia_register_dalvik_array(array_obj);

    if (readassets_log_count < READASSETS_LOG_MAX) {
        game_log("[Java] readAssets: %s (%ld bytes)\n", path, size);
        readassets_log_count++;
    }
    return (jobject) array_obj;
}

static int isassetexist_log_count = 0;
#define ISASSETEXIST_LOG_MAX 30

jint Illusia_isAssetExist(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    const char *name = (const char *) filename;
    if (!name) return 0;

    char path[256];
    if (illusia_resolve_asset_path(name, path, sizeof(path))) {
        struct stat st;
        if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            if (isassetexist_log_count < ISASSETEXIST_LOG_MAX) {
                game_log("[Java] isAssetExist: %s -> %ld (%s)\n", name, (long)st.st_size, path);
                isassetexist_log_count++;
            }
            return (jint)st.st_size;
        }
    }

    if (isassetexist_log_count < ISASSETEXIST_LOG_MAX) {
        game_log("[Java] isAssetExist: %s -> 0 (not found)\n", name);
        isassetexist_log_count++;
    }
    return 0;
}

jint Illusia_getGLOptionLinear(jmethodID id, va_list args) {
    return 1;
}

void Illusia_SetSpeed(jmethodID id, va_list args) {
    int speed = va_arg(args, int);
    game_log("[Java] SetSpeed: %d\n", speed);
}

jobject Illusia_getPhoneModel(jmethodID id, va_list args) {
    return (jobject) "PlayStation Vita";
}

jobject Illusia_getAbsolueFilePath(jmethodID id, va_list args) {
    return (jobject) "ux0:data/illusia2/";
}

static jobject illusia_new_byte_array_str(const char *s) {
    int len = (int) strlen(s);
    JavaDynArray *jda = jda_alloc(len, FIELD_TYPE_BYTE);
    if (!jda) return NULL;
    memcpy(jda->array, s, len);
    return (jobject) jda;
}

jobject Illusia_getPhoneNumber(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("01012345678");
}

jobject Illusia_getSimSerialNumber(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("8982000012345678901");
}

jobject Illusia_getMacAddress(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("00:00:00:00:00:00");
}

jobject Illusia_getDeviceID(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("000000000000000");
}

jobject Illusia_getAndroidID(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("000000000000000");
}

jobject Illusia_getOsVersion(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("4.4.4");
}

jobject Illusia_getDeviceType(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("AD Default");
}

jobject Illusia_getCarrierName(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("SKT");
}

jobject Illusia_getLanguage(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("en");
}

void Illusia_OnUIStatusChange(jmethodID id, va_list args) {
    int status = va_arg(args, int);
    game_log("[Java] OnUIStatusChange: %d\n", status);
    g_ui_status = status;
}

void Illusia_OnSoundPlay(jmethodID id, va_list args) {
    int snd_id = va_arg(args, int);
    int vol = va_arg(args, int);
    int is_loop = va_arg(args, int);
    audio_play(snd_id, vol, is_loop);
}

void Illusia_OnStopSound(jmethodID id, va_list args) {
    audio_stop_all();
}

void Illusia_VoidNoop(jmethodID id, va_list args) {
}

jint Illusia_IntZero(jmethodID id, va_list args) {
    return 0;
}

jint Illusia_IntOne(jmethodID id, va_list args) {
    return 1;
}

/*
 * Save / Load I/O Implementation (Direct file storage under ux0:data/illusia2/saves/)
 */
jint Illusia_saveFile(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    jbyteArray data = va_arg(args, jbyteArray);
    const char *name = (const char *) filename;

    if (!name || !data) return 0;

    ensure_save_dir();
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, name);

    jsize len = 0;
    jbyte *bytes = NULL;
    if (illusia_is_dalvik_array(data)) {
        len = (jsize) *(uint32_t *)((char *) data + 8);
        bytes = (jbyte *)((char *) data + 16);
    } else {
        JavaDynArray *jda = (JavaDynArray *) data;
        len = jda->len;
        bytes = (jbyte *) jda->array;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        game_log("[Java] saveFile failed to open %s\n", path);
        return 0;
    }

    size_t written = fwrite(bytes, 1, len, f);
    fclose(f);

    game_log("[Java] saveFile: %s (%d bytes saved)\n", name, (int)written);
    return (jint) written;
}

jobject Illusia_loadFile(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    const char *name = (const char *) filename;
    if (!name) return NULL;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        // Fallback check in data root
        snprintf(path, sizeof(path), "ux0:data/illusia2/%s", name);
        f = fopen(path, "rb");
        if (!f) return NULL;
    }

    struct stat st;
    long size = -1;
    if (fstat(fileno(f), &st) == 0) {
        size = st.st_size;
    }

    if (size < 0 || size > 64 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    void *array_obj = malloc(16 + size);
    if (!array_obj) {
        fclose(f);
        return NULL;
    }

    memset(array_obj, 0, 16);
    *(uint32_t *)((char *)array_obj + 8) = (uint32_t)size;
    fread((char *) array_obj + 16, 1, size, f);
    fclose(f);

    illusia_register_dalvik_array(array_obj);
    game_log("[Java] loadFile: %s (%ld bytes loaded)\n", name, size);
    return (jobject) array_obj;
}

jint Illusia_isFileExist(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    const char *name = (const char *) filename;
    if (!name) return 0;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, name);
    struct stat st;
    if (stat(path, &st) == 0) {
        return (jint) st.st_size;
    }

    snprintf(path, sizeof(path), "ux0:data/illusia2/%s", name);
    if (stat(path, &st) == 0) {
        return (jint) st.st_size;
    }

    return 0;
}

jint Illusia_deleteFile(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    const char *name = (const char *) filename;
    if (!name) return 0;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, name);
    unlink(path);
    game_log("[Java] deleteFile: %s\n", name);
    return 1;
}

/*
 * Gamevil Font API (GFA)
 */
#define GFA_MAX_FONTS 5
static int g_gfa_initialized = 0;
static float g_gfa_text_size = 12.0f;
static int g_gfa_color = 0xff000000;
static int g_gfa_cur_font = 0;
static char g_gfa_font_family[GFA_MAX_FONTS][128];
static int g_gfa_font_used[GFA_MAX_FONTS] = {0};
static int g_gfa_width = 0, g_gfa_height = 0, g_gfa_bpp = 32;

static float g_gfa_measure_rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static JavaDynArray *g_gfa_measure_rect_jda = NULL;
static JavaDynArray *g_gfa_pixels32_jda = NULL;
static JavaDynArray *g_gfa_pixels16_jda = NULL;

jboolean Illusia_GFA_IsInitialized(jmethodID id, va_list args) {
    return g_gfa_initialized ? JNI_TRUE : JNI_FALSE;
}

jboolean Illusia_GFA_Init(jmethodID id, va_list args) {
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    int bpp = va_arg(args, int);

    g_gfa_width = w;
    g_gfa_height = h;
    g_gfa_bpp = bpp;
    g_gfa_initialized = 1;

    if (!g_gfa_measure_rect_jda) {
        g_gfa_measure_rect_jda = jda_alloc(4, FIELD_TYPE_FLOAT);
    }
    if (!g_gfa_pixels32_jda) {
        g_gfa_pixels32_jda = jda_alloc(w * h, FIELD_TYPE_INT);
    }
    if (!g_gfa_pixels16_jda) {
        g_gfa_pixels16_jda = jda_alloc(w * h, FIELD_TYPE_SHORT);
    }

    game_log("[Java] GFA_Init: w=%d h=%d bpp=%d\n", w, h, bpp);
    return JNI_TRUE;
}

void Illusia_GFA_SetTextSize(jmethodID id, va_list args) {
    float sz = (float) va_arg(args, double);
    g_gfa_text_size = sz;
}

jint Illusia_GFA_CreateFont(jmethodID id, va_list args) {
    jstring family_str = va_arg(args, jstring);
    int style = va_arg(args, int);
    const char *family = (const char *) family_str;

    game_log("[Java] GFA_CreateFont: %s (style %d)\n", family ? family : "(default)", style);

    for (int i = 0; i < GFA_MAX_FONTS; i++) {
        if (g_gfa_font_used[i] && family && strcmp(g_gfa_font_family[i], family) == 0) {
            return (jint) i;
        }
    }
    for (int i = 0; i < GFA_MAX_FONTS; i++) {
        if (!g_gfa_font_used[i]) {
            g_gfa_font_used[i] = 1;
            if (family) strncpy(g_gfa_font_family[i], family, 127);
            else g_gfa_font_family[i][0] = '\0';
            return (jint) i;
        }
    }
    return 0;
}

void Illusia_GFA_SetColor(jmethodID id, va_list args) {
    int color = va_arg(args, int);
    g_gfa_color = color;
}

jint Illusia_GFA_SetFont(jmethodID id, va_list args) {
    int idx = va_arg(args, int);
    if (idx >= 0 && idx < GFA_MAX_FONTS) {
        g_gfa_cur_font = idx;
        return (jint) idx;
    }
    return 0;
}

jint Illusia_GFA_CharWidth(jmethodID id, va_list args) {
    return (jint)(g_gfa_text_size * 0.6f);
}

jint Illusia_GFA_CharHeight(jmethodID id, va_list args) {
    return (jint) g_gfa_text_size;
}

jint Illusia_GFA_GetAscent(jmethodID id, va_list args) {
    return (jint)(g_gfa_text_size * 0.8f);
}

jint Illusia_GFA_GetDescent(jmethodID id, va_list args) {
    return (jint)(g_gfa_text_size * 0.2f);
}

jint Illusia_GFA_GetCurrentFont(jmethodID id, va_list args) {
    return (jint) g_gfa_cur_font;
}

jint Illusia_GFA_GetColor(jmethodID id, va_list args) {
    return (jint) g_gfa_color;
}

jint Illusia_GFA_GetStringLength(jmethodID id, va_list args) {
    return 0;
}

void Illusia_GFA_SetString(jmethodID id, va_list args) {
}

void Illusia_GFA_SetStringFromUnicode(jmethodID id, va_list args) {
}

void Illusia_GFA_ReleaseFont(jmethodID id, va_list args) {
    int idx = va_arg(args, int);
    if (idx >= 0 && idx < GFA_MAX_FONTS) {
        g_gfa_font_used[idx] = 0;
    }
}

void Illusia_GFA_Release(jmethodID id, va_list args) {
    g_gfa_initialized = 0;
}

jobject Illusia_GFA_DrawFont(jmethodID id, va_list args) {
    if (!g_gfa_measure_rect_jda) {
        g_gfa_measure_rect_jda = jda_alloc(4, FIELD_TYPE_FLOAT);
    }
    float *f = (float *) g_gfa_measure_rect_jda->array;
    f[0] = 0.0f; f[1] = 0.0f; f[2] = g_gfa_text_size; f[3] = g_gfa_text_size;
    return (jobject) g_gfa_measure_rect_jda;
}

jobject Illusia_GFA_DrawText(jmethodID id, va_list args) {
    return Illusia_GFA_DrawFont(id, args);
}

jobject Illusia_GFA_MeasureText(jmethodID id, va_list args) {
    return Illusia_GFA_DrawFont(id, args);
}

jobject Illusia_GFA_GetPixels32(jmethodID id, va_list args) {
    return (jobject) g_gfa_pixels32_jda;
}

jobject Illusia_GFA_GetPixels16(jmethodID id, va_list args) {
    return (jobject) g_gfa_pixels16_jda;
}

/*
 * Method mappings
 */
NameToMethodID nameToMethodId[] = {
    { 1,  "readAssets", METHOD_TYPE_OBJECT },
    { 2,  "isAssetExist", METHOD_TYPE_INT },
    { 3,  "readAssete", METHOD_TYPE_OBJECT },
    { 4,  "getGLOptionLinear", METHOD_TYPE_INT },
    { 5,  "SetSpeed", METHOD_TYPE_VOID },
    { 6,  "getPhoneModel", METHOD_TYPE_OBJECT },
    { 7,  "getAbsolueFilePath", METHOD_TYPE_OBJECT },
    { 8,  "OnUIStatusChange", METHOD_TYPE_VOID },
    { 9,  "OnSoundPlay", METHOD_TYPE_VOID },
    { 10, "OnStopSound", METHOD_TYPE_VOID },
    { 11, "hideLoadingDialog", METHOD_TYPE_VOID },
    { 12, "OnShowSaveButton", METHOD_TYPE_VOID },
    { 13, "OnVibrate", METHOD_TYPE_VOID },
    { 14, "OnEvent", METHOD_TYPE_VOID },
    { 15, "getPhoneNumber", METHOD_TYPE_OBJECT },
    { 16, "getMacAddress", METHOD_TYPE_OBJECT },
    { 17, "getDeviceID", METHOD_TYPE_OBJECT },
    { 18, "getAndroidID", METHOD_TYPE_OBJECT },
    { 19, "getOsVersion", METHOD_TYPE_OBJECT },
    { 20, "getDeviceType", METHOD_TYPE_OBJECT },
    { 21, "getSimSerialNumber", METHOD_TYPE_OBJECT },
    { 22, "getLanguage", METHOD_TYPE_OBJECT },
    { 23, "getCarrierName", METHOD_TYPE_OBJECT },
    { 24, "getLocaleID", METHOD_TYPE_INT },
    { 25, "isGamevilLiveLogined", METHOD_TYPE_INT },
    { 26, "TrackEventDispatch", METHOD_TYPE_VOID },
    { 27, "loadFile", METHOD_TYPE_OBJECT },
    { 28, "saveFile", METHOD_TYPE_INT },
    { 29, "deleteFile", METHOD_TYPE_INT },
    { 30, "isFileExist", METHOD_TYPE_INT },
    { 31, "loadFileFromStorage", METHOD_TYPE_OBJECT },
    // GFA (Gamevil Font API)
    { 32, "GFA_IsInitialized", METHOD_TYPE_BOOLEAN },
    { 33, "GFA_Init", METHOD_TYPE_BOOLEAN },
    { 34, "GFA_SetTextSize", METHOD_TYPE_VOID },
    { 35, "GFA_CreateFont", METHOD_TYPE_INT },
    { 36, "GFA_SetColor", METHOD_TYPE_VOID },
    { 37, "GFA_SetFont", METHOD_TYPE_INT },
    { 38, "GFA_CharWidth", METHOD_TYPE_INT },
    { 39, "GFA_CharHeight", METHOD_TYPE_INT },
    { 40, "GFA_GetAscent", METHOD_TYPE_INT },
    { 41, "GFA_GetDescent", METHOD_TYPE_INT },
    { 42, "GFA_GetCurrentFont", METHOD_TYPE_INT },
    { 43, "GFA_GetColor", METHOD_TYPE_INT },
    { 44, "GFA_GetStringLength", METHOD_TYPE_INT },
    { 45, "GFA_SetString", METHOD_TYPE_VOID },
    { 46, "GFA_SetStringFromUnicode", METHOD_TYPE_VOID },
    { 47, "GFA_ReleaseFont", METHOD_TYPE_VOID },
    { 48, "GFA_Release", METHOD_TYPE_VOID },
    { 49, "GFA_DrawFont", METHOD_TYPE_OBJECT },
    { 50, "GFA_DrawText", METHOD_TYPE_OBJECT },
    { 51, "GFA_MeasureText", METHOD_TYPE_OBJECT },
    { 52, "GFA_GetPixels32", METHOD_TYPE_OBJECT },
    { 53, "GFA_GetPixels16", METHOD_TYPE_OBJECT },
    { 54, "hideTitleComponent", METHOD_TYPE_VOID },
    { 55, "hidePurchaseComponent", METHOD_TYPE_VOID },
    { 56, "hideMenuComponent", METHOD_TYPE_VOID },
    { 57, "hideMenuItemComponent", METHOD_TYPE_VOID },
    { 58, "getGPoint", METHOD_TYPE_VOID },
    { 59, "isNetAvailable", METHOD_TYPE_INT },
};

MethodsBoolean methodsBoolean[] = {
    { 32, Illusia_GFA_IsInitialized },
    { 33, Illusia_GFA_Init },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { 2,  Illusia_isAssetExist },
    { 4,  Illusia_getGLOptionLinear },
    { 24, Illusia_IntOne },
    { 25, Illusia_IntZero },
    { 28, Illusia_saveFile },
    { 29, Illusia_deleteFile },
    { 30, Illusia_isFileExist },
    { 35, Illusia_GFA_CreateFont },
    { 37, Illusia_GFA_SetFont },
    { 38, Illusia_GFA_CharWidth },
    { 39, Illusia_GFA_CharHeight },
    { 40, Illusia_GFA_GetAscent },
    { 41, Illusia_GFA_GetDescent },
    { 42, Illusia_GFA_GetCurrentFont },
    { 43, Illusia_GFA_GetColor },
    { 44, Illusia_GFA_GetStringLength },
    { 59, Illusia_IntZero },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { 1,  Illusia_readAssets },
    { 3,  Illusia_readAssets },
    { 6,  Illusia_getPhoneModel },
    { 7,  Illusia_getAbsolueFilePath },
    { 15, Illusia_getPhoneNumber },
    { 16, Illusia_getMacAddress },
    { 17, Illusia_getDeviceID },
    { 18, Illusia_getAndroidID },
    { 19, Illusia_getOsVersion },
    { 20, Illusia_getDeviceType },
    { 21, Illusia_getSimSerialNumber },
    { 22, Illusia_getLanguage },
    { 23, Illusia_getCarrierName },
    { 27, Illusia_loadFile },
    { 31, Illusia_loadFile },
    { 49, Illusia_GFA_DrawFont },
    { 50, Illusia_GFA_DrawText },
    { 51, Illusia_GFA_MeasureText },
    { 52, Illusia_GFA_GetPixels32 },
    { 53, Illusia_GFA_GetPixels16 },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 5,  Illusia_SetSpeed },
    { 8,  Illusia_OnUIStatusChange },
    { 9,  Illusia_OnSoundPlay },
    { 10, Illusia_OnStopSound },
    { 11, Illusia_VoidNoop },
    { 12, Illusia_VoidNoop },
    { 13, Illusia_VoidNoop },
    { 14, Illusia_VoidNoop },
    { 26, Illusia_VoidNoop },
    { 34, Illusia_GFA_SetTextSize },
    { 36, Illusia_GFA_SetColor },
    { 45, Illusia_GFA_SetString },
    { 46, Illusia_GFA_SetStringFromUnicode },
    { 47, Illusia_GFA_ReleaseFont },
    { 48, Illusia_GFA_Release },
    { 54, Illusia_VoidNoop },
    { 55, Illusia_VoidNoop },
    { 56, Illusia_VoidNoop },
    { 57, Illusia_VoidNoop },
    { 58, Illusia_VoidNoop },
};

/*
 * JNI Fields
 */
NameToFieldID nameToFieldId[] = {};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {};
FieldsObject fieldsObject[] = {};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
