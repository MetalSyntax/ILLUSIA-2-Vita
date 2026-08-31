/*
 * java.c
 *
 * Fake JNI environment and method handlers for Gamevil Nexus2 engine (ILLUSIA 2).
 * Handles Dalvik-compatible ArrayObjects, Gamevil Font API (GFA), Save/Load I/O and Audio.
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI.h>

#include <stdio.h>

#include <psp2/apputil.h>
#include <psp2/system_param.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <psp2/io/stat.h>

#include "audio.h"
#include "utils/logger.h"

// UI status reported by engine via OnUIStatusChange
volatile int g_ui_status = -1;

#define SAVE_DIR  "ux0:data/illusia2/saves"
#define SAVE_DIR2 "ux0:data/illusia2/games"

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
 *
 * Backed by an open-addressing pointer hash set rather than a linear array:
 * illusia_is_dalvik_array() is hit on every single JNI array/string call
 * (GetByteArrayElements, GetArrayLength, GetByteArrayRegion, ...), and with
 * readAssets() registering a fresh entry on every call (even cache hits, for
 * correctness — see the readAssets cache below), a long session can push
 * this into the tens of thousands of entries. A linear scan there made every
 * later JNI call progressively slower as the count grew.
 */
static void **g_dalvik_set = NULL;
static int g_dalvik_set_cap = 0;
static int g_dalvik_set_count = 0;

static uint32_t illusia_ptr_hash(void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    v ^= v >> 16;
    v *= 0x85ebca6bu;
    v ^= v >> 13;
    return (uint32_t)v;
}

static void illusia_dalvik_set_insert(void **table, int cap, void *ptr) {
    uint32_t mask = (uint32_t)(cap - 1);
    uint32_t idx = illusia_ptr_hash(ptr) & mask;
    while (table[idx]) {
        idx = (idx + 1) & mask;
    }
    table[idx] = ptr;
}

static void illusia_register_dalvik_array(void *ptr) {
    if (!ptr) return;

    if (g_dalvik_set_cap == 0 || (g_dalvik_set_count + 1) * 4 >= g_dalvik_set_cap * 3) {
        int new_cap = g_dalvik_set_cap ? g_dalvik_set_cap * 2 : 2048;
        void **grown = calloc(new_cap, sizeof(void *));
        if (!grown) {
            game_log("[Java] Warning: Dalvik array set alloc failed (cap %d)\n", g_dalvik_set_cap);
            return;
        }
        for (int i = 0; i < g_dalvik_set_cap; i++) {
            if (g_dalvik_set[i]) illusia_dalvik_set_insert(grown, new_cap, g_dalvik_set[i]);
        }
        free(g_dalvik_set);
        g_dalvik_set = grown;
        g_dalvik_set_cap = new_cap;
    }

    illusia_dalvik_set_insert(g_dalvik_set, g_dalvik_set_cap, ptr);
    g_dalvik_set_count++;
}

static int illusia_is_dalvik_array(void *ptr) {
    if (!ptr || g_dalvik_set_cap == 0) return 0;
    uint32_t mask = (uint32_t)(g_dalvik_set_cap - 1);
    uint32_t idx = illusia_ptr_hash(ptr) & mask;
    while (g_dalvik_set[idx]) {
        if (g_dalvik_set[idx] == ptr) return 1;
        idx = (idx + 1) & mask;
    }
    return 0;
}

/*
 * readAssets() disk-read cache, keyed by resolved path.
 * The engine re-reads the same script/table files (e.g. output_eng.zt1)
 * dozens of times per second during dialogue-heavy scenes — each call used to
 * fopen()+fread() the file from ux0: again, which on Vita storage is slow
 * enough in a tight burst to look like a stall/freeze.
 *
 * This cache stores only the raw bytes. It must NOT hand out the same
 * array_obj pointer twice: our GetByteArrayElements wrapper returns a direct
 * pointer (isCopy=JNI_FALSE) into that buffer, which is a JNI-legal contract
 * for native code to write through — some consumer decoding/decrypting a
 * buffer in place would otherwise permanently corrupt every future "read" of
 * that file for the rest of the process. So every readAssets() call still
 * gets its own fresh malloc + its own g_dalvik_registry entry (matching
 * pre-cache behavior exactly); only the disk I/O is skipped on a hit.
 */
typedef struct {
    char *path;
    void *bytes;
    long size;
} AssetCacheEntry;

static AssetCacheEntry *g_asset_cache = NULL;
static int g_asset_cache_count = 0;
static int g_asset_cache_cap = 0;

static AssetCacheEntry *illusia_asset_cache_find(const char *path) {
    for (int i = 0; i < g_asset_cache_count; i++) {
        if (strcmp(g_asset_cache[i].path, path) == 0) return &g_asset_cache[i];
    }
    return NULL;
}

/* Takes ownership of `owned_bytes` (caller must not free/reuse it after). */
static void illusia_asset_cache_add(const char *path, void *owned_bytes, long size) {
    if (g_asset_cache_count >= g_asset_cache_cap) {
        int new_cap = g_asset_cache_cap ? g_asset_cache_cap * 2 : 256;
        AssetCacheEntry *grown = realloc(g_asset_cache, sizeof(AssetCacheEntry) * new_cap);
        if (!grown) { free(owned_bytes); return; }
        g_asset_cache = grown;
        g_asset_cache_cap = new_cap;
    }
    g_asset_cache[g_asset_cache_count].path = strdup(path);
    g_asset_cache[g_asset_cache_count].bytes = owned_bytes;
    g_asset_cache[g_asset_cache_count].size = size;
    g_asset_cache_count++;
}

static void *illusia_get_array_payload(void *arr, int *out_len, int elem_size) {
    if (!arr) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (illusia_is_dalvik_array(arr)) {
        if (out_len) *out_len = (int)*(uint32_t *)((char *)arr + 8);
        return (void *)((char *)arr + 16);
    }
    JavaDynArray *jda = jda_find(arr);
    if (jda && jda->array) {
        if (out_len) *out_len = jda->len;
        return jda->array;
    }
    if (out_len) *out_len = 0;
    return arr;
}

static jsize (*orig_GetArrayLength)(JNIEnv *, jarray) = NULL;
static jbyte *(*orig_GetByteArrayElements)(JNIEnv *, jbyteArray, jboolean *) = NULL;
static void (*orig_GetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, jbyte *) = NULL;
static void (*orig_ReleaseByteArrayElements)(JNIEnv *, jbyteArray, jbyte *, jint) = NULL;
static const char *(*orig_GetStringUTFChars)(JNIEnv *, jstring, jboolean *) = NULL;
static void (*orig_ReleaseStringUTFChars)(JNIEnv *, jstring, char *) = NULL;
static jsize (*orig_GetStringUTFLength)(JNIEnv *, jstring) = NULL;
static jsize (*orig_GetStringLength)(JNIEnv *, jstring) = NULL;
static jfloat *(*orig_GetFloatArrayElements)(JNIEnv *, jfloatArray, jboolean *) = NULL;
static void (*orig_GetFloatArrayRegion)(JNIEnv *, jfloatArray, jsize, jsize, jfloat *) = NULL;
static jint *(*orig_GetIntArrayElements)(JNIEnv *, jintArray, jboolean *) = NULL;
static void (*orig_GetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, jint *) = NULL;
static jshort *(*orig_GetShortArrayElements)(JNIEnv *, jshortArray, jboolean *) = NULL;
static void (*orig_GetShortArrayRegion)(JNIEnv *, jshortArray, jsize, jsize, jshort *) = NULL;
static jdouble *(*orig_GetDoubleArrayElements)(JNIEnv *, jdoubleArray, jboolean *) = NULL;

static jsize Illusia_GetArrayLength_wrapper(JNIEnv *env, jarray array) {
    if (!array) return 0;
    if (illusia_is_dalvik_array(array)) {
        return (jsize) *(uint32_t *)((char *) array + 8);
    }
    JavaDynArray *jda = jda_find((void *)array);
    if (jda) return (jsize)jda->len;
    if (orig_GetArrayLength) {
        jsize len = orig_GetArrayLength(env, array);
        if (len > 0) return len;
    }
    return (jsize) strlen((const char *) array);
}

static jbyte *Illusia_GetByteArrayElements_wrapper(JNIEnv *env, jbyteArray array, jboolean *isCopy) {
    if (!array) return NULL;
    if (isCopy) *isCopy = JNI_FALSE;
    if (illusia_is_dalvik_array(array)) {
        return (jbyte *)((char *) array + 16);
    }
    JavaDynArray *jda = jda_find((void *)array);
    if (jda && jda->array) return (jbyte *)jda->array;
    if (orig_GetByteArrayElements) {
        jbyte *res = orig_GetByteArrayElements(env, array, isCopy);
        if (res) return res;
    }
    return (jbyte *) array;
}

static void Illusia_GetByteArrayRegion_wrapper(JNIEnv *env, jbyteArray array, jsize start, jsize len, jbyte *buf) {
    if (!array || !buf || len <= 0) return;
    int arr_len = 0;
    void *payload = illusia_get_array_payload(array, &arr_len, 1);
    if (payload) {
        memcpy(buf, (char *)payload + start, len);
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

static const char *Illusia_GetStringUTFChars_wrapper(JNIEnv *env, jstring string, jboolean *isCopy) {
    if (!string) return NULL;
    const char *s = (const char *)string;
    if (illusia_is_dalvik_array(string)) {
        s = (const char *)((char *)string + 16);
    }
    if (isCopy) *isCopy = JNI_TRUE;
    return strdup(s);
}

static void Illusia_ReleaseStringUTFChars_wrapper(JNIEnv *env, jstring string, char *chars) {
    if (chars) free(chars);
}

static jsize Illusia_GetStringUTFLength_wrapper(JNIEnv *env, jstring string) {
    if (!string) return 0;
    if (illusia_is_dalvik_array(string)) {
        return (jsize)*(uint32_t *)((char *)string + 8);
    }
    return (jsize)strlen((const char *)string);
}

static jsize Illusia_GetStringLength_wrapper(JNIEnv *env, jstring string) {
    return Illusia_GetStringUTFLength_wrapper(env, string);
}

static jfloat *Illusia_GetFloatArrayElements_wrapper(JNIEnv *env, jfloatArray array, jboolean *isCopy) {
    if (!array) return NULL;
    if (isCopy) *isCopy = JNI_FALSE;
    if (illusia_is_dalvik_array(array)) {
        return (jfloat *)((char *)array + 16);
    }
    JavaDynArray *jda = jda_find((void *)array);
    if (jda && jda->array) return (jfloat *)jda->array;
    if (orig_GetFloatArrayElements) {
        jfloat *res = orig_GetFloatArrayElements(env, array, isCopy);
        if (res) return res;
    }
    return (jfloat *)array;
}

static void Illusia_GetFloatArrayRegion_wrapper(JNIEnv *env, jfloatArray array, jsize start, jsize len, jfloat *buf) {
    if (!array || !buf || len <= 0) return;
    int arr_len = 0;
    void *payload = illusia_get_array_payload(array, &arr_len, sizeof(jfloat));
    if (payload) {
        memcpy(buf, (jfloat *)payload + start, len * sizeof(jfloat));
        return;
    }
    if (orig_GetFloatArrayRegion) orig_GetFloatArrayRegion(env, array, start, len, buf);
}

static jint *Illusia_GetIntArrayElements_wrapper(JNIEnv *env, jintArray array, jboolean *isCopy) {
    if (!array) return NULL;
    if (isCopy) *isCopy = JNI_FALSE;
    if (illusia_is_dalvik_array(array)) {
        return (jint *)((char *)array + 16);
    }
    JavaDynArray *jda = jda_find((void *)array);
    if (jda && jda->array) return (jint *)jda->array;
    if (orig_GetIntArrayElements) {
        jint *res = orig_GetIntArrayElements(env, array, isCopy);
        if (res) return res;
    }
    return (jint *)array;
}

static void Illusia_GetIntArrayRegion_wrapper(JNIEnv *env, jintArray array, jsize start, jsize len, jint *buf) {
    if (!array || !buf || len <= 0) return;
    int arr_len = 0;
    void *payload = illusia_get_array_payload(array, &arr_len, sizeof(jint));
    if (payload) {
        memcpy(buf, (jint *)payload + start, len * sizeof(jint));
        return;
    }
    if (orig_GetIntArrayRegion) orig_GetIntArrayRegion(env, array, start, len, buf);
}

static jshort *Illusia_GetShortArrayElements_wrapper(JNIEnv *env, jshortArray array, jboolean *isCopy) {
    if (!array) return NULL;
    if (isCopy) *isCopy = JNI_FALSE;
    if (illusia_is_dalvik_array(array)) {
        return (jshort *)((char *)array + 16);
    }
    JavaDynArray *jda = jda_find((void *)array);
    if (jda && jda->array) return (jshort *)jda->array;
    if (orig_GetShortArrayElements) {
        jshort *res = orig_GetShortArrayElements(env, array, isCopy);
        if (res) return res;
    }
    return (jshort *)array;
}

static void Illusia_GetShortArrayRegion_wrapper(JNIEnv *env, jshortArray array, jsize start, jsize len, jshort *buf) {
    if (!array || !buf || len <= 0) return;
    int arr_len = 0;
    void *payload = illusia_get_array_payload(array, &arr_len, sizeof(jshort));
    if (payload) {
        memcpy(buf, (jshort *)payload + start, len * sizeof(jshort));
        return;
    }
    if (orig_GetShortArrayRegion) orig_GetShortArrayRegion(env, array, start, len, buf);
}

static jdouble *Illusia_GetDoubleArrayElements_wrapper(JNIEnv *env, jdoubleArray array, jboolean *isCopy) {
    if (!array) return NULL;
    if (isCopy) *isCopy = JNI_FALSE;
    if (illusia_is_dalvik_array(array)) {
        return (jdouble *)((char *)array + 16);
    }
    JavaDynArray *jda = jda_find((void *)array);
    if (jda && jda->array) return (jdouble *)jda->array;
    if (orig_GetDoubleArrayElements) {
        jdouble *res = orig_GetDoubleArrayElements(env, array, isCopy);
        if (res) return res;
    }
    return (jdouble *)array;
}

void illusia_install_array_hooks(void) {
    struct JNINativeInterface *funcs = (struct JNINativeInterface *)(uintptr_t) jni;
    orig_GetArrayLength = funcs->GetArrayLength;
    orig_GetByteArrayElements = funcs->GetByteArrayElements;
    orig_GetByteArrayRegion = funcs->GetByteArrayRegion;
    orig_ReleaseByteArrayElements = funcs->ReleaseByteArrayElements;
    orig_GetStringUTFChars = funcs->GetStringUTFChars;
    orig_ReleaseStringUTFChars = funcs->ReleaseStringUTFChars;
    orig_GetStringUTFLength = funcs->GetStringUTFLength;
    orig_GetStringLength = funcs->GetStringLength;
    orig_GetFloatArrayElements = funcs->GetFloatArrayElements;
    orig_GetFloatArrayRegion = funcs->GetFloatArrayRegion;
    orig_GetIntArrayElements = funcs->GetIntArrayElements;
    orig_GetIntArrayRegion = funcs->GetIntArrayRegion;
    orig_GetShortArrayElements = funcs->GetShortArrayElements;
    orig_GetShortArrayRegion = funcs->GetShortArrayRegion;
    orig_GetDoubleArrayElements = funcs->GetDoubleArrayElements;

    funcs->GetArrayLength = Illusia_GetArrayLength_wrapper;
    funcs->GetByteArrayElements = Illusia_GetByteArrayElements_wrapper;
    funcs->GetByteArrayRegion = Illusia_GetByteArrayRegion_wrapper;
    funcs->ReleaseByteArrayElements = Illusia_ReleaseByteArrayElements_wrapper;
    funcs->GetStringUTFChars = Illusia_GetStringUTFChars_wrapper;
    funcs->ReleaseStringUTFChars = Illusia_ReleaseStringUTFChars_wrapper;
    funcs->GetStringUTFLength = Illusia_GetStringUTFLength_wrapper;
    funcs->GetStringLength = Illusia_GetStringLength_wrapper;
    funcs->GetFloatArrayElements = Illusia_GetFloatArrayElements_wrapper;
    funcs->GetFloatArrayRegion = Illusia_GetFloatArrayRegion_wrapper;
    funcs->GetIntArrayElements = Illusia_GetIntArrayElements_wrapper;
    funcs->GetIntArrayRegion = Illusia_GetIntArrayRegion_wrapper;
    funcs->GetShortArrayElements = Illusia_GetShortArrayElements_wrapper;
    funcs->GetShortArrayRegion = Illusia_GetShortArrayRegion_wrapper;
    funcs->GetDoubleArrayElements = Illusia_GetDoubleArrayElements_wrapper;

    game_log("[Java] FalsoJNI array and string wrappers installed.\n");
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

    long size;
    const void *src;

    AssetCacheEntry *cached = illusia_asset_cache_find(path);
    if (cached) {
        size = cached->size;
        src = cached->bytes;
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) {
            game_log("[Java] readAssets: failed to open %s\n", path);
            return NULL;
        }

        struct stat st;
        size = -1;
        if (fstat(fileno(f), &st) == 0) {
            size = st.st_size;
        }

        if (size < 0 || size > 64 * 1024 * 1024) {
            game_log("[Java] readAssets: invalid size %ld for %s\n", size, path);
            fclose(f);
            return NULL;
        }

        void *disk_bytes = malloc(size);
        if (!disk_bytes) {
            fclose(f);
            return NULL;
        }
        fread(disk_bytes, 1, size, f);
        fclose(f);

        illusia_asset_cache_add(path, disk_bytes, size); /* takes ownership of disk_bytes */
        src = disk_bytes;
    }

    void *array_obj = malloc(16 + size);
    if (!array_obj) return NULL;

    memset(array_obj, 0, 16);
    *(uint32_t *)((char *)array_obj + 8) = (uint32_t)size;
    memcpy((char *)array_obj + 16, src, size);

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

static jobject illusia_new_byte_array_str(const char *s) {
    if (!s) return NULL;
    int len = (int) strlen(s);
    void *array_obj = malloc(16 + len + 1);
    if (!array_obj) return NULL;
    memset(array_obj, 0, 16);
    *(uint32_t *)((char *)array_obj + 8) = (uint32_t)len;
    memcpy((char *)array_obj + 16, s, len);
    ((char *)array_obj + 16)[len] = '\0';
    illusia_register_dalvik_array(array_obj);
    return (jobject) array_obj;
}

jobject Illusia_getPhoneModel(jmethodID id, va_list args) {
    return illusia_new_byte_array_str("PlayStation Vita");
}

jobject Illusia_getAbsolueFilePath(jmethodID id, va_list args) {
    /* The engine expects a jstring, which FalsoJNI treats as a raw char* */
    ensure_save_dir();
    return (jobject)SAVE_DIR;
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

#include "font8x8.h"

static int g_gfa_str_len = 0;
static char g_gfa_current_string[2048] = {0};

jint Illusia_GFA_GetStringLength(jmethodID id, va_list args) {
    return (jint) g_gfa_str_len;
}

static void gfa_set_raw_bytes(const uint8_t *raw, int len) {
    if (!raw || len <= 0) {
        g_gfa_str_len = 0;
        g_gfa_current_string[0] = '\0';
        return;
    }

    int is_utf16_le = (len >= 2 && raw[1] == 0 && raw[0] != 0);
    int is_utf16_be = (len >= 2 && raw[0] == 0 && raw[1] != 0);

    int out_idx = 0;
    if (is_utf16_le) {
        for (int i = 0; i + 1 < len && out_idx < (int)sizeof(g_gfa_current_string) - 1; i += 2) {
            uint16_t ch = raw[i] | (raw[i + 1] << 8);
            if (ch == 0) break;
            g_gfa_current_string[out_idx++] = ((ch >= 0x20 && ch <= 0x7E) || ch == '\n' || ch == '\r') ? (char)ch : ' ';
        }
    } else if (is_utf16_be) {
        for (int i = 0; i + 1 < len && out_idx < (int)sizeof(g_gfa_current_string) - 1; i += 2) {
            uint16_t ch = (raw[i] << 8) | raw[i + 1];
            if (ch == 0) break;
            g_gfa_current_string[out_idx++] = ((ch >= 0x20 && ch <= 0x7E) || ch == '\n' || ch == '\r') ? (char)ch : ' ';
        }
    } else {
        // Raw 8-bit ASCII / UTF-8
        for (int i = 0; i < len && out_idx < (int)sizeof(g_gfa_current_string) - 1; i++) {
            uint8_t ch = raw[i];
            if (ch == 0) break;
            g_gfa_current_string[out_idx++] = ((ch >= 0x20 && ch <= 0x7E) || ch == '\n' || ch == '\r') ? (char)ch : ' ';
        }
    }
    g_gfa_current_string[out_idx] = '\0';
    g_gfa_str_len = out_idx;
}

void Illusia_GFA_SetString(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    int len = va_arg(args, int);
    const char *s = (const char *) str;
    if (s) {
        if (illusia_is_dalvik_array(str)) {
            s = (const char *)((char *)str + 16);
        }
        strncpy(g_gfa_current_string, s, sizeof(g_gfa_current_string) - 1);
        g_gfa_current_string[sizeof(g_gfa_current_string) - 1] = '\0';
        g_gfa_str_len = len > 0 ? len : (int) strlen(s);
    } else {
        g_gfa_current_string[0] = '\0';
        g_gfa_str_len = 0;
    }
}

void Illusia_GFA_SetStringFromUnicode(jmethodID id, va_list args) {
    jbyteArray arr = va_arg(args, jbyteArray);
    if (!arr) { g_gfa_str_len = 0; g_gfa_current_string[0] = '\0'; return; }
    int len = 0;
    uint8_t *raw = (uint8_t *)illusia_get_array_payload(arr, &len, 1);
    gfa_set_raw_bytes(raw, len);
}

void Illusia_GFA_SetStringFromKSC5601(jmethodID id, va_list args) {
    jbyteArray arr = va_arg(args, jbyteArray);
    if (!arr) { g_gfa_str_len = 0; g_gfa_current_string[0] = '\0'; return; }
    int len = 0;
    uint8_t *raw = (uint8_t *)illusia_get_array_payload(arr, &len, 1);
    gfa_set_raw_bytes(raw, len);
}

static void gfa_rasterize_text(float start_x, float start_y, float max_w) {
    if (!g_gfa_pixels32_jda || !g_gfa_pixels16_jda) return;

    int buf_w = g_gfa_width > 0 ? g_gfa_width : 480;
    int buf_h = g_gfa_height > 0 ? g_gfa_height : 320;
    int total_pixels = buf_w * buf_h;

    uint32_t *p32 = (uint32_t *)g_gfa_pixels32_jda->array;
    uint16_t *p16 = (uint16_t *)g_gfa_pixels16_jda->array;

    // Clear buffer
    memset(p32, 0, total_pixels * sizeof(uint32_t));
    memset(p16, 0, total_pixels * sizeof(uint16_t));

    int scale = 1;
    if (g_gfa_text_size >= 18.0f) scale = 2;
    else if (g_gfa_text_size >= 14.0f) scale = 2;
    else scale = 1;

    uint32_t col32 = (uint32_t)g_gfa_color;
    if ((col32 & 0xFF000000) == 0) {
        col32 |= 0xFF000000;
    }
    uint8_t a = (col32 >> 24) & 0xFF;
    uint8_t r = (col32 >> 16) & 0xFF;
    uint8_t g = (col32 >> 8) & 0xFF;
    uint8_t b = col32 & 0xFF;
    uint16_t col16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    int cur_x = (int)start_x;
    int cur_y = (int)start_y;
    int max_x = cur_x;
    int line_h = 8 * scale + 2;

    const char *p = g_gfa_current_string;
    while (*p) {
        if (*p == '\n') {
            cur_x = (int)start_x;
            cur_y += line_h;
            p++;
            continue;
        }
        if (*p == '\r') {
            p++;
            continue;
        }

        uint8_t ch = (uint8_t)*p;
        const uint8_t *glyph = (ch >= 0x20 && ch <= 0x7E) ? g_font8x8[ch - 0x20] : g_font8x8[0];

        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if ((bits >> (7 - col)) & 1) {
                    for (int dy = 0; dy < scale; dy++) {
                        int py = cur_y + row * scale + dy;
                        if (py < 0 || py >= buf_h) continue;
                        for (int dx = 0; dx < scale; dx++) {
                            int px = cur_x + col * scale + dx;
                            if (px < 0 || px >= buf_w) continue;
                            int idx = py * buf_w + px;
                            p32[idx] = col32;
                            p16[idx] = col16;
                        }
                    }
                }
            }
        }

        cur_x += 8 * scale;
        if (cur_x > max_x) max_x = cur_x;

        if (max_w > 0 && cur_x + 8 * scale > (int)(start_x + max_w)) {
            cur_x = (int)start_x;
            cur_y += line_h;
        }

        p++;
    }

    if (!g_gfa_measure_rect_jda) {
        g_gfa_measure_rect_jda = jda_alloc(4, FIELD_TYPE_FLOAT);
    }
    float *f = (float *)g_gfa_measure_rect_jda->array;
    f[0] = start_x;
    f[1] = start_y;
    f[2] = (float)(max_x - (int)start_x);
    f[3] = (float)(cur_y + line_h - (int)start_y);
}

jfloat Illusia_GFA_GetTextWidth(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    int len = va_arg(args, int);
    const char *s = (const char *) str;
    int count = len;
    if (count <= 0 && s) count = (int) strlen(s);
    if (count <= 0) count = g_gfa_str_len;
    int scale = (g_gfa_text_size >= 14.0f) ? 2 : 1;
    return (jfloat)(count * 8 * scale);
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

    int scale = (g_gfa_text_size >= 14.0f) ? 2 : 1;
    int char_w = 8 * scale;
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

    int scale = (g_gfa_text_size >= 14.0f) ? 2 : 1;
    int char_w = 8 * scale;
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
    gfa_rasterize_text(0.0f, 0.0f, 0.0f);
    return (jobject) g_gfa_measure_rect_jda;
}

jobject Illusia_GFA_DrawText(jmethodID id, va_list args) {
    float x = (float) va_arg(args, double);
    float y = (float) va_arg(args, double);
    int align = va_arg(args, int);
    float max_w = (float) va_arg(args, double);
    (void) align;
    gfa_rasterize_text(x, y, max_w);
    return (jobject) g_gfa_measure_rect_jda;
}

jobject Illusia_GFA_MeasureText(jmethodID id, va_list args) {
    float x = (float) va_arg(args, double);
    float y = (float) va_arg(args, double);
    int align = va_arg(args, int);
    float max_w = (float) va_arg(args, double);
    (void) align;
    gfa_rasterize_text(x, y, max_w);
    return (jobject) g_gfa_measure_rect_jda;
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
    return (jobject)"1.0.3";
}

jobject Illusia_getPlayerName(jmethodID id, va_list args) {
    return (jobject)"Hero";
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
    return illusia_new_byte_array_str("GESTURE_NONE");
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
    { 144, "trackPageViewDispatch", METHOD_TYPE_VOID },
    { 145, "getDeviceModel", METHOD_TYPE_OBJECT },
    { 146, "getOldPhoneNumber", METHOD_TYPE_OBJECT },
    { 147, "isK", METHOD_TYPE_INT },
    { 148, "isNexusOne", METHOD_TYPE_INT },
    { 149, "isUserAcceptC2dm", METHOD_TYPE_INT },
    { 150, "setUserAcceptC2dm", METHOD_TYPE_VOID },
    { 151, "saveCurrenUIStatus", METHOD_TYPE_VOID },
    { 152, "setFrameSpeed", METHOD_TYPE_VOID },
    { 153, "setLocalPushNotification", METHOD_TYPE_VOID },
    { 154, "setTimeeOut", METHOD_TYPE_VOID },
    { 155, "NexusSetBlockText", METHOD_TYPE_VOID },
    { 156, "NexusSetEngInput", METHOD_TYPE_VOID },
    { 157, "NexusSensorBottomOrientation", METHOD_TYPE_VOID },
    { 158, "NexusSensorInitOrientationType", METHOD_TYPE_VOID },
    { 159, "NexusSensorLeftOrientation", METHOD_TYPE_VOID },
    { 160, "NexusSensorRightOrientation", METHOD_TYPE_VOID },
    { 161, "NexusSensorTopOrientation", METHOD_TYPE_VOID },
    { 162, "NexusTouchSetCheckGestureBuilder", METHOD_TYPE_VOID },
    { 163, "NexusTouchSetUseCustomeGesture", METHOD_TYPE_VOID },
    { 164, "GFA_GetStringLengthEx", METHOD_TYPE_INT },
    { 165, "GFA_GetTextWidths", METHOD_TYPE_FLOAT },
    { 166, "GFA_InitString", METHOD_TYPE_VOID },
    { 167, "GFA_SetStringEx", METHOD_TYPE_VOID },
    { 168, "getNumberFromKeyboard", METHOD_TYPE_OBJECT },
    { 169, "getTextFromKeyboard", METHOD_TYPE_OBJECT },
    { 22,  "GetLanguage", METHOD_TYPE_VOID },
    { 170, "NetErrorDialog", METHOD_TYPE_VOID },
    { 170, "NetErrorDialogException", METHOD_TYPE_VOID },
    { 171, "getLocaleID", METHOD_TYPE_INT },
    { 172, "setPurchaseChange", METHOD_TYPE_VOID },
    { 173, "OnEvent", METHOD_TYPE_VOID },
    { 174, "OnAsyncTimerSet", METHOD_TYPE_VOID },
    { 175, "GetFileFromHttp", METHOD_TYPE_VOID },
    { 176, "isGamevilLiveLogined", METHOD_TYPE_INT },
    { 177, "GFA_SetAntiAlias", METHOD_TYPE_VOID },
    { 178, "postGPoint", METHOD_TYPE_VOID },
    { 179, "postLanguage", METHOD_TYPE_VOID },
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
    { 71,  Illusia_GFA_GetTextSize },
    { 97,  Illusia_GFA_GetTextWidth },
    { 165, Illusia_GFA_GetTextWidth },
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
    { 147, Illusia_IntZero }, // isK
    { 148, Illusia_IntZero }, // isNexusOne
    { 149, Illusia_IntZero }, // isUserAcceptC2dm
    { 164, Illusia_GFA_GetStringLength },
    { 171, Illusia_IntZero }, // getLocaleID
    { 176, Illusia_IntZero }, // isGamevilLiveLogined
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
    { 145, Illusia_getPhoneModel },
    { 146, Illusia_getPhoneNumber },
    { 168, (void *) Illusia_IntZero },
    { 169, (void *) Illusia_IntZero },
};
MethodsShort methodsShort[] = {};

extern void (*handleCletEvent)(void *env, void *obj, int type, int p1, int p2, int p3);

void Illusia_GetLanguage(jmethodID id, va_list args) {
    int system_language = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &system_language);
    
    int game_lang = 1; // English
    switch (system_language) {
        case SCE_SYSTEM_PARAM_LANG_KOREAN:
            game_lang = 0;
            break;
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:
            game_lang = 2;
            break;
        default:
            game_lang = 1;
            break;
    }
    
    l_debug("[Java] GetLanguage requested! Responding with lang_id = %d", game_lang);
    if (handleCletEvent) {
        handleCletEvent((void *)1, NULL, 5001, game_lang, 0, 0); // 5001 is GET_LANGUAGE in Nexus2
    }
}

MethodsVoid methodsVoid[] = {
    { 5,   Illusia_SetSpeed },
    { 8,   Illusia_OnUIStatusChange },
    { 9,   Illusia_OnSoundPlay },
    { 10,  Illusia_OnStopSound },
    { 11,  Illusia_VoidNoop },
    { 12,  Illusia_VoidNoop },
    { 13,  Illusia_VoidNoop },
    { 14,  Illusia_VoidNoop },
    { 22,  Illusia_GetLanguage }, // GetLanguage
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
    { 144, Illusia_VoidNoop },
    { 150, Illusia_VoidNoop },
    { 151, Illusia_VoidNoop },
    { 152, Illusia_VoidNoop },
    { 153, Illusia_VoidNoop },
    { 154, Illusia_VoidNoop },
    { 155, Illusia_VoidNoop },
    { 156, Illusia_VoidNoop },
    { 157, Illusia_VoidNoop },
    { 158, Illusia_VoidNoop },
    { 159, Illusia_VoidNoop },
    { 160, Illusia_VoidNoop },
    { 161, Illusia_VoidNoop },
    { 162, Illusia_VoidNoop },
    { 163, Illusia_VoidNoop },
    { 166, Illusia_VoidNoop },
    { 167, Illusia_GFA_SetString },
    { 170, Illusia_VoidNoop },
    { 172, Illusia_VoidNoop },
    { 173, Illusia_VoidNoop },
    { 174, Illusia_VoidNoop },
    { 175, Illusia_VoidNoop },
    { 177, Illusia_VoidNoop },
    { 178, Illusia_VoidNoop },
    { 179, Illusia_VoidNoop },
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
