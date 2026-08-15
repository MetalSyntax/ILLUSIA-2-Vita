/*
 * java.c
 *
 * Fake JNI environment and method handlers for Gamevil Nexus2 engine (ILLUSIA 2).
 * Handles Dalvik-compatible ArrayObjects for readAssets and provides JNI callbacks.
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "utils/logger.h"

// UI status reported by engine via OnUIStatusChange
volatile int g_ui_status = -1;

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

void Illusia_OnUIStatusChange(jmethodID id, va_list args) {
    int status = va_arg(args, int);
    game_log("[Java] OnUIStatusChange: %d\n", status);
    g_ui_status = status;
}

void Illusia_OnSoundPlay(jmethodID id, va_list args) {
    int snd_id = va_arg(args, int);
    int vol = va_arg(args, int);
    int is_loop = va_arg(args, int);
    game_log("[Java] OnSoundPlay: id=%d vol=%d loop=%d\n", snd_id, vol, is_loop);
}

void Illusia_OnStopSound(jmethodID id, va_list args) {
    game_log("[Java] OnStopSound\n");
}

void Illusia_VoidNoop(jmethodID id, va_list args) {
}

jobject Illusia_StringDummy(jmethodID id, va_list args) {
    return (jobject) "01012345678";
}

jint Illusia_IntZero(jmethodID id, va_list args) {
    return 0;
}

jint Illusia_IntOne(jmethodID id, va_list args) {
    return 1;
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
};

MethodsBoolean methodsBoolean[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { 2,  Illusia_isAssetExist },
    { 4,  Illusia_getGLOptionLinear },
    { 24, Illusia_IntOne },
    { 25, Illusia_IntZero },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { 1,  Illusia_readAssets },
    { 3,  Illusia_readAssets },
    { 6,  Illusia_getPhoneModel },
    { 7,  Illusia_getAbsolueFilePath },
    { 15, Illusia_StringDummy },
    { 16, Illusia_StringDummy },
    { 17, Illusia_StringDummy },
    { 18, Illusia_StringDummy },
    { 19, Illusia_StringDummy },
    { 20, Illusia_StringDummy },
    { 21, Illusia_StringDummy },
    { 22, Illusia_StringDummy },
    { 23, Illusia_StringDummy },
    { 27, Illusia_readAssets },
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
