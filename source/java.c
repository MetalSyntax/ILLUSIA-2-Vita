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

#define SAVE_DIR  "ux0:data/illusia2/games"
#define SAVE_DIR2 "ux0:data/illusia2/saves"

static void ensure_save_dir(void) {
    sceIoMkdir("ux0:data/illusia2", 0777);
    sceIoMkdir(SAVE_DIR, 0777);
    sceIoMkdir(SAVE_DIR2, 0777);
}

// Path resolver for ILLUSIA 2 assets
static int illusia_resolve_asset_path(const char *name, char *out, size_t out_size) {
    if (!name) return 0;

    // Priority 1: Assets folder path ux0:data/illusia2/assets/
    snprintf(out, out_size, "ux0:data/illusia2/assets/%s", name);
    if (access(out, F_OK) == 0) return 1;

    // Priority 2: Direct path under ux0:data/illusia2/
    snprintf(out, out_size, "ux0:data/illusia2/%s", name);
    if (access(out, F_OK) == 0) return 1;

    return 0;
}

/*
 * Dalvik ArrayObject registry for seamless JNI array handling.
 * Grows dynamically: a session can create far more than a few hundred of
 * these (one per readAssets/loadFile call), and silently dropping
 * registration past a fixed cap made later arrays fall through to the real
 * FalsoJNI GetArrayLength/GetByteArrayElements, which misread our
 * malloc(16+size) layout as their own array representation and corrupted
 * memory.
 */
static void **g_dalvik_registry = NULL;
static int g_dalvik_registry_count = 0;
static int g_dalvik_registry_cap = 0;

static void illusia_register_dalvik_array(void *ptr) {
    if (g_dalvik_registry_count >= g_dalvik_registry_cap) {
        int new_cap = g_dalvik_registry_cap ? g_dalvik_registry_cap * 2 : 1024;
        void **grown = realloc(g_dalvik_registry, sizeof(void *) * new_cap);
        if (!grown) {
            game_log("[Java] Warning: Dalvik array registry realloc failed (cap %d)\n", g_dalvik_registry_cap);
            return;
        }
        g_dalvik_registry = grown;
        g_dalvik_registry_cap = new_cap;
    }
    g_dalvik_registry[g_dalvik_registry_count++] = ptr;
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
#define READASSETS_LOG_MAX 4000

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
#define ISASSETEXIST_LOG_MAX 4000

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
        snprintf(path, sizeof(path), "%s/%s", SAVE_DIR2, name);
        f = fopen(path, "rb");
    }
    if (!f) {
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

    JavaDynArray *jda = jda_alloc((jsize) size, FIELD_TYPE_BYTE);
    if (!jda) {
        fclose(f);
        return NULL;
    }

    fread(jda->array, 1, size, f);
    fclose(f);

    game_log("[Java] loadFile: %s (%ld bytes loaded)\n", name, size);
    return (jobject) jda;
}

jint Illusia_isFileExist(jmethodID id, va_list args) {
    jstring filename = va_arg(args, jstring);
    const char *name = (const char *) filename;
    if (!name) return 0;

    char path[256];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, name);
    if (stat(path, &st) == 0) {
        return (jint) st.st_size;
    }

    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR2, name);
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
    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR2, name);
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

static int g_gfa_str_len = 0;

jint Illusia_GFA_GetStringLength(jmethodID id, va_list args) {
    return (jint) g_gfa_str_len;
}

void Illusia_GFA_SetString(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    int len = va_arg(args, int);
    const char *s = (const char *) str;
    if (s) {
        g_gfa_str_len = len > 0 ? len : (int) strlen(s);
    } else {
        g_gfa_str_len = len > 0 ? len : 0;
    }
}

void Illusia_GFA_SetStringFromUnicode(jmethodID id, va_list args) {
    jbyteArray arr = va_arg(args, jbyteArray);
    if (!arr) { g_gfa_str_len = 0; return; }
    if (illusia_is_dalvik_array(arr)) {
        g_gfa_str_len = (*(int *)((char *)arr + 8)) / 2;
    } else {
        JavaDynArray *jda = (JavaDynArray *)arr;
        g_gfa_str_len = jda->len / 2;
    }
}

void Illusia_GFA_SetStringFromKSC5601(jmethodID id, va_list args) {
    jbyteArray arr = va_arg(args, jbyteArray);
    if (!arr) { g_gfa_str_len = 0; return; }
    if (illusia_is_dalvik_array(arr)) {
        g_gfa_str_len = *(int *)((char *)arr + 8);
    } else {
        JavaDynArray *jda = (JavaDynArray *)arr;
        g_gfa_str_len = jda->len;
    }
}

jfloat Illusia_GFA_GetTextWidth(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    int len = va_arg(args, int);
    const char *s = (const char *) str;
    int count = len;
    if (count <= 0 && s) count = (int) strlen(s);
    return (jfloat)(count * g_gfa_text_size * 0.6f);
}

jint Illusia_GFA_GetWordwrapPositionEx(jmethodID id, va_list args) {
    float maxWidth = (float) va_arg(args, double);
    JavaDynArray *ww = (JavaDynArray *) va_arg(args, jobject);
    int32_t *positions = NULL;
    int ww_cap = 0;

    if (ww) {
        if (illusia_is_dalvik_array(ww)) {
            ww_cap = *(int *)((char *)ww + 8);
            positions = (int32_t *)((char *)ww + 16);
        } else {
            positions = (int32_t *) ww->array;
            ww_cap = ww->len;
        }
    }

    int char_w = (int)(g_gfa_text_size * 0.6f);
    if (char_w <= 0) char_w = 8;
    int chars_per_line = (int)(maxWidth / char_w);
    if (chars_per_line <= 0) chars_per_line = 20;

    int sub_len = g_gfa_str_len;
    int ww_count = 0;
    int pos = 0;

    while (sub_len > 0 && chars_per_line < sub_len) {
        sub_len -= chars_per_line;
        pos += chars_per_line;
        if (positions && ww_count < ww_cap) {
            positions[ww_count] = pos;
        }
        ww_count++;
    }
    return (jint) ww_count;
}

jint Illusia_GFA_GetWordwrapPosition(jmethodID id, va_list args) {
    int line_idx = va_arg(args, int);
    float maxWidth = (float) va_arg(args, double);

    if (g_gfa_str_len <= 0) return 0;

    int char_w = (int)(g_gfa_text_size * 0.6f);
    if (char_w <= 0) char_w = 8;
    int chars_per_line = (int)(maxWidth / char_w);
    if (chars_per_line <= 0) chars_per_line = 20;

    if (line_idx < 0) {
        int lines = (g_gfa_str_len + chars_per_line - 1) / chars_per_line;
        return (jint) lines;
    }

    int pos = (line_idx + 1) * chars_per_line;
    if (pos > g_gfa_str_len) pos = g_gfa_str_len;
    return (jint) pos;
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

jfloat Illusia_GFA_GetTextSize(jmethodID id, va_list args) {
    return (jfloat) g_gfa_text_size;
}

jlong Illusia_getTotalMemory(jmethodID id, va_list args) {
    return 256L * 1024 * 1024;
}

jlong Illusia_getFreeMemory(jmethodID id, va_list args) {
    return 128L * 1024 * 1024;
}

jobject Illusia_getVersion(jmethodID id, va_list args) {
    return (jobject) "1.0.3";
}

jobject Illusia_getPlayerName(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("Hero");
}

jobject Illusia_getPlayerNumberByte(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("0");
}

jobject Illusia_getGamevilLiveID(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("");
}

jobject Illusia_getGamevilLivePW(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("");
}

jobject Illusia_getInetHostAddress(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("0.0.0.0");
}

jobject Illusia_getGestureName(jmethodID id, va_list args) {
    return (jobject) "GESTURE_NONE";
}

/*
 * Method mappings
 */
NameToMethodID nameToMethodId[] = {
    { 1,   "readAssets", METHOD_TYPE_OBJECT },
    { 2,   "isAssetExist", METHOD_TYPE_INT },
    { 3,   "readAssete", METHOD_TYPE_OBJECT },
    { 4,   "getGLOptionLinear", METHOD_TYPE_INT },
    { 5,   "SetSpeed", METHOD_TYPE_VOID },
    { 6,   "getPhoneModel", METHOD_TYPE_OBJECT },
    { 7,   "getAbsolueFilePath", METHOD_TYPE_OBJECT },
    { 8,   "OnUIStatusChange", METHOD_TYPE_VOID },
    { 9,   "OnSoundPlay", METHOD_TYPE_VOID },
    { 10,  "OnStopSound", METHOD_TYPE_VOID },
    { 11,  "hideLoadingDialog", METHOD_TYPE_VOID },
    { 12,  "OnShowSaveButton", METHOD_TYPE_VOID },
    { 13,  "OnVibrate", METHOD_TYPE_VOID },
    { 14,  "OnEvent", METHOD_TYPE_VOID },
    { 15,  "getPhoneNumber", METHOD_TYPE_OBJECT },
    { 16,  "getMacAddress", METHOD_TYPE_OBJECT },
    { 17,  "getDeviceID", METHOD_TYPE_OBJECT },
    { 18,  "getAndroidID", METHOD_TYPE_OBJECT },
    { 19,  "getOsVersion", METHOD_TYPE_OBJECT },
    { 20,  "getDeviceType", METHOD_TYPE_OBJECT },
    { 21,  "getSimSerialNumber", METHOD_TYPE_OBJECT },
    { 22,  "getLanguage", METHOD_TYPE_OBJECT },
    { 23,  "getCarrierName", METHOD_TYPE_OBJECT },
    { 24,  "getLocaleID", METHOD_TYPE_INT },
    { 25,  "isGamevilLiveLogined", METHOD_TYPE_INT },
    { 26,  "TrackEventDispatch", METHOD_TYPE_VOID },
    { 27,  "loadFile", METHOD_TYPE_OBJECT },
    { 28,  "saveFile", METHOD_TYPE_INT },
    { 29,  "deleteFile", METHOD_TYPE_INT },
    { 30,  "isFileExist", METHOD_TYPE_INT },
    { 31,  "loadFileFromStorage", METHOD_TYPE_OBJECT },
    // GFA (Gamevil Font API)
    { 32,  "GFA_IsInitialized", METHOD_TYPE_BOOLEAN },
    { 33,  "GFA_Init", METHOD_TYPE_BOOLEAN },
    { 34,  "GFA_SetTextSize", METHOD_TYPE_VOID },
    { 35,  "GFA_CreateFont", METHOD_TYPE_INT },
    { 36,  "GFA_SetColor", METHOD_TYPE_VOID },
    { 37,  "GFA_SetFont", METHOD_TYPE_INT },
    { 38,  "GFA_CharWidth", METHOD_TYPE_INT },
    { 39,  "GFA_CharHeight", METHOD_TYPE_INT },
    { 40,  "GFA_GetAscent", METHOD_TYPE_INT },
    { 41,  "GFA_GetDescent", METHOD_TYPE_INT },
    { 42,  "GFA_GetCurrentFont", METHOD_TYPE_INT },
    { 43,  "GFA_GetColor", METHOD_TYPE_INT },
    { 44,  "GFA_GetStringLength", METHOD_TYPE_INT },
    { 45,  "GFA_SetString", METHOD_TYPE_VOID },
    { 46,  "GFA_SetStringFromUnicode", METHOD_TYPE_VOID },
    { 47,  "GFA_ReleaseFont", METHOD_TYPE_VOID },
    { 48,  "GFA_Release", METHOD_TYPE_VOID },
    { 49,  "GFA_DrawFont", METHOD_TYPE_OBJECT },
    { 50,  "GFA_DrawText", METHOD_TYPE_OBJECT },
    { 51,  "GFA_MeasureText", METHOD_TYPE_OBJECT },
    { 52,  "GFA_GetPixels32", METHOD_TYPE_OBJECT },
    { 53,  "GFA_GetPixels16", METHOD_TYPE_OBJECT },
    { 54,  "hideTitleComponent", METHOD_TYPE_VOID },
    { 55,  "hidePurchaseComponent", METHOD_TYPE_VOID },
    { 56,  "hideMenuComponent", METHOD_TYPE_VOID },
    { 57,  "hideMenuItemComponent", METHOD_TYPE_VOID },
    { 58,  "getGPoint", METHOD_TYPE_VOID },
    { 59,  "isNetAvailable", METHOD_TYPE_INT },
    { 60,  "hashCode", METHOD_TYPE_INT },
    { 61,  "getCompany", METHOD_TYPE_INT },
    { 62,  "getGameState", METHOD_TYPE_INT },
    { 63,  "getPlayerName", METHOD_TYPE_OBJECT },
    { 64,  "getPlayerNameByte", METHOD_TYPE_OBJECT },
    { 65,  "getPlayerNumberByte", METHOD_TYPE_OBJECT },
    { 66,  "clearPlayerName", METHOD_TYPE_VOID },
    { 67,  "clearNumber", METHOD_TYPE_VOID },
    { 68,  "updateDialogue", METHOD_TYPE_VOID },
    { 69,  "requestIAP", METHOD_TYPE_VOID },
    { 70,  "OnAsyncTimerSet", METHOD_TYPE_VOID },
    { 71,  "GFA_GetTextSize", METHOD_TYPE_FLOAT },
    { 72,  "getTotalMemory", METHOD_TYPE_LONG },
    { 73,  "getMaxMemory", METHOD_TYPE_LONG },
    { 74,  "getFreeMemory", METHOD_TYPE_LONG },
    { 75,  "getUsedMemory", METHOD_TYPE_LONG },
    { 76,  "getVersion", METHOD_TYPE_OBJECT },
    { 77,  "getInetHostAddress", METHOD_TYPE_OBJECT },
    { 78,  "getGamevilLiveID", METHOD_TYPE_OBJECT },
    { 79,  "getGamevilLivePW", METHOD_TYPE_OBJECT },
    { 80,  "reqestGamevilLiveLogin", METHOD_TYPE_VOID },
    { 81,  "showTermsButton", METHOD_TYPE_VOID },
    { 82,  "hideTermsButton", METHOD_TYPE_VOID },
    { 83,  "saveGLOptionLinear", METHOD_TYPE_VOID },
    { 84,  "showBuyShopButton", METHOD_TYPE_VOID },
    { 85,  "hideBuyShopButton", METHOD_TYPE_VOID },
    { 86,  "showBuy", METHOD_TYPE_VOID },
    { 87,  "closeBuy", METHOD_TYPE_VOID },
    { 88,  "setPurchaseChange", METHOD_TYPE_VOID },
    { 89,  "showReplyMoveComponent", METHOD_TYPE_VOID },
    { 90,  "hideReplyMoveComponent", METHOD_TYPE_VOID },
    { 91,  "showAwardGamevilCPIDialog", METHOD_TYPE_VOID },
    { 92,  "getAwardGamevilCPIDialog", METHOD_TYPE_VOID },
    { 93,  "finishApp", METHOD_TYPE_VOID },
    { 94,  "stopAndroidSound", METHOD_TYPE_VOID },
    { 95,  "GFA_GetWordwrapPositionEx", METHOD_TYPE_INT },
    { 96,  "GFA_GetWordwrapPosition", METHOD_TYPE_INT },
    { 97,  "GFA_GetTextWidth", METHOD_TYPE_FLOAT },
    { 98,  "GFA_SetAntiAlias", METHOD_TYPE_VOID },
    { 99,  "GFA_SetLocale", METHOD_TYPE_VOID },
    { 100, "GFA_SetTextAlign", METHOD_TYPE_VOID },
    { 101, "GFA_SetStringFromKSC5601", METHOD_TYPE_VOID },
    { 102, "NexusSetMaxLengthTextInput", METHOD_TYPE_VOID },
    { 103, "NexusSetMaxLengthNumberInput", METHOD_TYPE_VOID },
    { 104, "NexusSetMaxLengthNumInput", METHOD_TYPE_VOID },
    { 105, "setMaxLengthNumInput", METHOD_TYPE_VOID },
    { 106, "NexusSetText", METHOD_TYPE_VOID },
    { 107, "setTapjoyReady", METHOD_TYPE_VOID },
    { 108, "openPurchasePopup", METHOD_TYPE_VOID },
    { 109, "openStoreWithProductId", METHOD_TYPE_VOID },
    { 110, "openUrl", METHOD_TYPE_VOID },
    { 111, "startPurchase", METHOD_TYPE_VOID },
    { 112, "requestOfferwall", METHOD_TYPE_VOID },
    { 113, "requestGamevilGift", METHOD_TYPE_VOID },
    { 114, "isOfferwallEnabled", METHOD_TYPE_INT },
    { 115, "getGamevilGiftString", METHOD_TYPE_OBJECT },
    { 116, "resumeSound", METHOD_TYPE_VOID },
    { 117, "vibrateAndroid", METHOD_TYPE_VOID },
    { 118, "showPurchaseComponent", METHOD_TYPE_VOID },
    { 119, "showShopButton", METHOD_TYPE_VOID },
    { 120, "hideShopButton", METHOD_TYPE_VOID },
    { 121, "showLoadingDialog", METHOD_TYPE_VOID },
    { 122, "HideLoadingDialog", METHOD_TYPE_VOID },
    { 123, "NexusTouchGestureEnable", METHOD_TYPE_VOID },
    { 124, "NexusTouchGestureName", METHOD_TYPE_OBJECT },
    { 125, "NexusTouchInitGestureName", METHOD_TYPE_VOID },
    { 126, "NexusTouchSetCheckGesture", METHOD_TYPE_VOID },
    { 127, "NexusTouchSetShowGestureLine", METHOD_TYPE_VOID },
    { 128, "NexusTouchSimpleFlingEnable", METHOD_TYPE_VOID },
    { 129, "NexusSensorEnable", METHOD_TYPE_VOID },
    { 130, "NexusSensorOrientationType", METHOD_TYPE_VOID },
    { 131, "GetNexusSensorXAngle", METHOD_TYPE_INT },
    { 132, "GetNexusSensorXInc", METHOD_TYPE_INT },
    { 133, "GetNexusSensorYAngle", METHOD_TYPE_INT },
    { 134, "GetNexusSensorYInc", METHOD_TYPE_INT },
    { 135, "IsNexusSensorBottomAcceleration", METHOD_TYPE_BOOLEAN },
    { 136, "IsNexusSensorBottomOrientation", METHOD_TYPE_BOOLEAN },
    { 137, "IsNexusSensorLeftAcceleration", METHOD_TYPE_BOOLEAN },
    { 138, "IsNexusSensorLeftOrientation", METHOD_TYPE_BOOLEAN },
    { 139, "IsNexusSensorRightAcceleration", METHOD_TYPE_BOOLEAN },
    { 140, "IsNexusSensorRightOrientation", METHOD_TYPE_BOOLEAN },
    { 141, "IsNexusSensorShakeDevice", METHOD_TYPE_BOOLEAN },
    { 142, "IsNexusSensorTopAcceleration", METHOD_TYPE_BOOLEAN },
    { 143, "IsNexusSensorTopOrientation", METHOD_TYPE_BOOLEAN },
};

MethodsBoolean methodsBoolean[] = {
    { 32,  Illusia_GFA_IsInitialized },
    { 33,  Illusia_GFA_Init },
    { 135, (void *) Illusia_IntZero },
    { 136, (void *) Illusia_IntZero },
    { 137, (void *) Illusia_IntZero },
    { 138, (void *) Illusia_IntZero },
    { 139, (void *) Illusia_IntZero },
    { 140, (void *) Illusia_IntZero },
    { 141, (void *) Illusia_IntZero },
    { 142, (void *) Illusia_IntZero },
    { 143, (void *) Illusia_IntZero },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
    { 71, Illusia_GFA_GetTextSize },
    { 97, Illusia_GFA_GetTextWidth },
};
MethodsInt methodsInt[] = {
    { 2,   Illusia_isAssetExist },
    { 4,   Illusia_getGLOptionLinear },
    { 24,  Illusia_IntOne },
    { 25,  Illusia_IntZero },
    { 28,  Illusia_saveFile },
    { 29,  Illusia_deleteFile },
    { 30,  Illusia_isFileExist },
    { 35,  Illusia_GFA_CreateFont },
    { 37,  Illusia_GFA_SetFont },
    { 38,  Illusia_GFA_CharWidth },
    { 39,  Illusia_GFA_CharHeight },
    { 40,  Illusia_GFA_GetAscent },
    { 41,  Illusia_GFA_GetDescent },
    { 42,  Illusia_GFA_GetCurrentFont },
    { 43,  Illusia_GFA_GetColor },
    { 44,  Illusia_GFA_GetStringLength },
    { 59,  Illusia_IntZero },
    { 60,  Illusia_IntZero }, // hashCode -> 0
    { 61,  Illusia_IntZero }, // getCompany -> 0
    { 62,  Illusia_IntZero }, // getGameState -> 0
    { 95,  Illusia_GFA_GetWordwrapPositionEx },
    { 96,  Illusia_GFA_GetWordwrapPosition },
    { 114, Illusia_IntZero }, // isOfferwallEnabled -> 0
    { 131, Illusia_IntZero }, // GetNexusSensorXAngle
    { 132, Illusia_IntZero }, // GetNexusSensorXInc
    { 133, Illusia_IntZero }, // GetNexusSensorYAngle
    { 134, Illusia_IntZero }, // GetNexusSensorYInc
};
MethodsLong methodsLong[] = {
    { 72, Illusia_getTotalMemory },
    { 73, Illusia_getTotalMemory },
    { 74, Illusia_getFreeMemory },
    { 75, Illusia_getFreeMemory },
};
MethodsObject methodsObject[] = {
    { 1,   Illusia_readAssets },
    { 3,   Illusia_readAssets },
    { 6,   Illusia_getPhoneModel },
    { 7,   Illusia_getAbsolueFilePath },
    { 15,  Illusia_getPhoneNumber },
    { 16,  Illusia_getMacAddress },
    { 17,  Illusia_getDeviceID },
    { 18,  Illusia_getAndroidID },
    { 19,  Illusia_getOsVersion },
    { 20,  Illusia_getDeviceType },
    { 21,  Illusia_getSimSerialNumber },
    { 22,  Illusia_getLanguage },
    { 23,  Illusia_getCarrierName },
    { 27,  Illusia_loadFile },
    { 31,  Illusia_loadFile },
    { 49,  Illusia_GFA_DrawFont },
    { 50,  Illusia_GFA_DrawText },
    { 51,  Illusia_GFA_MeasureText },
    { 52,  Illusia_GFA_GetPixels32 },
    { 53,  Illusia_GFA_GetPixels16 },
    { 63,  Illusia_getPlayerName },
    { 64,  Illusia_getPlayerName },
    { 65,  Illusia_getPlayerNumberByte },
    { 76,  Illusia_getVersion },
    { 77,  Illusia_getInetHostAddress },
    { 78,  Illusia_getGamevilLiveID },
    { 79,  Illusia_getGamevilLivePW },
    { 115, (void *) Illusia_IntZero }, // getGamevilGiftString -> NULL
    { 124, Illusia_getGestureName },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 5,   Illusia_SetSpeed },
    { 8,   Illusia_OnUIStatusChange },
    { 9,   Illusia_OnSoundPlay },
    { 10,  Illusia_OnStopSound },
    { 11,  Illusia_VoidNoop },
    { 12,  Illusia_VoidNoop },
    { 13,  Illusia_VoidNoop },
    { 14,  Illusia_VoidNoop },
    { 26,  Illusia_VoidNoop },
    { 34,  Illusia_GFA_SetTextSize },
    { 36,  Illusia_GFA_SetColor },
    { 45,  Illusia_GFA_SetString },
    { 46,  Illusia_GFA_SetStringFromUnicode },
    { 47,  Illusia_GFA_ReleaseFont },
    { 48,  Illusia_GFA_Release },
    { 54,  Illusia_VoidNoop },
    { 55,  Illusia_VoidNoop },
    { 56,  Illusia_VoidNoop },
    { 57,  Illusia_VoidNoop },
    { 58,  Illusia_VoidNoop },
    { 66,  Illusia_VoidNoop },
    { 67,  Illusia_VoidNoop },
    { 68,  Illusia_VoidNoop },
    { 69,  Illusia_VoidNoop },
    { 70,  Illusia_VoidNoop },
    { 80,  Illusia_VoidNoop },
    { 81,  Illusia_VoidNoop },
    { 82,  Illusia_VoidNoop },
    { 83,  Illusia_VoidNoop },
    { 84,  Illusia_VoidNoop },
    { 85,  Illusia_VoidNoop },
    { 86,  Illusia_VoidNoop },
    { 87,  Illusia_VoidNoop },
    { 88,  Illusia_VoidNoop },
    { 89,  Illusia_VoidNoop },
    { 90,  Illusia_VoidNoop },
    { 91,  Illusia_VoidNoop },
    { 92,  Illusia_VoidNoop },
    { 93,  Illusia_VoidNoop },
    { 94,  Illusia_OnStopSound },
    { 98,  Illusia_VoidNoop },
    { 99,  Illusia_VoidNoop },
    { 100, Illusia_VoidNoop },
    { 101, Illusia_GFA_SetStringFromKSC5601 },
    { 102, Illusia_VoidNoop },
    { 103, Illusia_VoidNoop },
    { 104, Illusia_VoidNoop },
    { 105, Illusia_VoidNoop },
    { 106, Illusia_VoidNoop },
    { 107, Illusia_VoidNoop },
    { 108, Illusia_VoidNoop },
    { 109, Illusia_VoidNoop },
    { 110, Illusia_VoidNoop },
    { 111, Illusia_VoidNoop },
    { 112, Illusia_VoidNoop },
    { 113, Illusia_VoidNoop },
    { 116, Illusia_VoidNoop },
    { 117, Illusia_VoidNoop },
    { 118, Illusia_VoidNoop },
    { 119, Illusia_VoidNoop },
    { 120, Illusia_VoidNoop },
    { 121, Illusia_VoidNoop },
    { 122, Illusia_VoidNoop },
    { 123, Illusia_VoidNoop },
    { 125, Illusia_VoidNoop },
    { 126, Illusia_VoidNoop },
    { 127, Illusia_VoidNoop },
    { 128, Illusia_VoidNoop },
    { 129, Illusia_VoidNoop },
    { 130, Illusia_VoidNoop },
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
