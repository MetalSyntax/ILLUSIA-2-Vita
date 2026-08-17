/*
 * audio.c
 *
 * Audio engine for ILLUSIA 2 PS Vita port.
 * Mixed output via SceAudioOut and Tremor (libvorbisidec).
 */

#include "audio.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tremor/ivorbisfile.h>
#include "utils/logger.h"

#define AUDIO_RATE 44100
#define AUDIO_GRAIN 512

#define VOICE_BGM  0
#define VOICE_SFX0 1
#define NUM_VOICES 5

typedef struct {
    OggVorbis_File vf;
    int active;
    int loop;
    int channels;
    long rate;
    float gain;
    float pos_frac;
    int16_t prev_l, prev_r;
    int have_prev;
    int16_t stage[256];
    int stage_frames, stage_pos;
} voice_t;

static voice_t voices[NUM_VOICES];
static SceUID audio_mutex = -1;
static SceUID audio_thread_id = -1;
static int audio_port = -1;
static volatile int audio_running = 0;

static int audio_resolve_path(int snd_id, char *out, size_t out_size) {
    snprintf(out, out_size, "ux0:data/illusia2/assets/sound/s%03d.ogg", snd_id);
    if (access(out, F_OK) == 0) return 1;

    snprintf(out, out_size, "ux0:data/illusia2/sound/s%03d.ogg", snd_id);
    if (access(out, F_OK) == 0) return 1;

    snprintf(out, out_size, "ux0:data/illusia2/assets/res/raw/s%03d.ogg", snd_id);
    if (access(out, F_OK) == 0) return 1;

    snprintf(out, out_size, "ux0:data/illusia2/assets/raw/s%03d.ogg", snd_id);
    if (access(out, F_OK) == 0) return 1;

    snprintf(out, out_size, "ux0:data/illusia2/assets/s%03d.ogg", snd_id);
    if (access(out, F_OK) == 0) return 1;

    return 0;
}

static void voice_close(voice_t *v) {
    if (v->active) {
        ov_clear(&v->vf);
        v->active = 0;
    }
}

static int voice_next_src_frame(voice_t *v, int16_t *l, int16_t *r) {
    while (v->stage_pos >= v->stage_frames) {
        int bs;
        long got = ov_read(&v->vf, (char *) v->stage, sizeof(v->stage), &bs);
        if (got <= 0) {
            if (v->loop && got == 0 && ov_pcm_seek(&v->vf, 0) == 0)
                continue;
            voice_close(v);
            return 0;
        }
        v->stage_frames = (int) got / (v->channels * 2);
        v->stage_pos = 0;
    }

    *l = v->stage[v->stage_pos * v->channels];
    *r = v->stage[v->stage_pos * v->channels + (v->channels > 1 ? 1 : 0)];
    v->stage_pos++;
    return 1;
}

static int voice_decode(voice_t *v, int16_t *out, int frames) {
    int done = 0;

    if (v->rate == AUDIO_RATE) {
        int16_t l, r;
        while (done < frames && v->active) {
            if (!voice_next_src_frame(v, &l, &r)) break;
            out[done * 2]     = (int16_t)(l * v->gain);
            out[done * 2 + 1] = (int16_t)(r * v->gain);
            done++;
        }
        return done;
    }

    float step = (float) v->rate / (float) AUDIO_RATE;
    while (done < frames && v->active) {
        if (!v->have_prev) {
            if (!voice_next_src_frame(v, &v->prev_l, &v->prev_r)) break;
            v->have_prev = 1;
            v->pos_frac = 0.0f;
        }
        int16_t nl = v->prev_l, nr = v->prev_r;
        while (v->pos_frac >= 1.0f) {
            if (!voice_next_src_frame(v, &nl, &nr)) { v->have_prev = 0; break; }
            v->prev_l = nl; v->prev_r = nr;
            v->pos_frac -= 1.0f;
        }
        if (!v->active || !v->have_prev) break;

        out[done * 2]     = (int16_t)(v->prev_l * v->gain);
        out[done * 2 + 1] = (int16_t)(v->prev_r * v->gain);
        done++;
        v->pos_frac += step;
    }
    return done;
}

static int audio_thread(SceSize args, void *argp) {
    static int16_t mix[AUDIO_GRAIN * 2];
    static int16_t buf[AUDIO_GRAIN * 2];

    while (audio_running) {
        memset(mix, 0, sizeof(mix));

        sceKernelLockMutex(audio_mutex, 1, NULL);
        for (int vi = 0; vi < NUM_VOICES; vi++) {
            voice_t *v = &voices[vi];
            if (!v->active) continue;
            int got = voice_decode(v, buf, AUDIO_GRAIN);
            for (int i = 0; i < got * 2; i++) {
                int s = mix[i] + buf[i];
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                mix[i] = (int16_t) s;
            }
        }
        sceKernelUnlockMutex(audio_mutex, 1);

        sceAudioOutOutput(audio_port, mix);
    }
    return 0;
}

void audio_init(void) {
    audio_mutex = sceKernelCreateMutex("illusia_audio_mtx", 0, 0, NULL);
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, AUDIO_RATE, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    memset(voices, 0, sizeof(voices));

    audio_running = 1;
    audio_thread_id = sceKernelCreateThread("illusia_audio_th", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(audio_thread_id, 0, NULL);

    game_log("[Audio] Sistema de audio inicializado.\n");
}

static int current_bgm_id = -1;

void audio_play(int snd_id, int vol, int is_loop) {
    int is_bgm = (is_loop || snd_id >= 100);

    // If BGM is already playing the exact same track, just update volume smoothly
    if (is_bgm && snd_id == current_bgm_id && voices[VOICE_BGM].active) {
        sceKernelLockMutex(audio_mutex, 1, NULL);
        voices[VOICE_BGM].gain = (vol <= 0) ? 1.0f : ((float) vol / 80.0f);
        if (voices[VOICE_BGM].gain > 1.0f) voices[VOICE_BGM].gain = 1.0f;
        sceKernelUnlockMutex(audio_mutex, 1);
        return;
    }

    char path[256];
    if (!audio_resolve_path(snd_id, path, sizeof(path))) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return;

    OggVorbis_File vf;
    if (ov_open(f, &vf, NULL, 0) < 0) {
        fclose(f);
        return;
    }

    vorbis_info *vi = ov_info(&vf, -1);
    if (!vi || (vi->channels != 1 && vi->channels != 2)) {
        ov_clear(&vf);
        return;
    }

    int target_slot;
    if (is_bgm) {
        target_slot = VOICE_BGM;
        current_bgm_id = snd_id;
    } else {
        static int sfx_rr = 0;
        target_slot = VOICE_SFX0 + (sfx_rr++ % (NUM_VOICES - VOICE_SFX0));
    }

    sceKernelLockMutex(audio_mutex, 1, NULL);
    voice_t *v = &voices[target_slot];
    voice_close(v);

    v->vf = vf;
    v->active = 1;
    v->loop = is_bgm;
    v->channels = vi->channels;
    v->rate = vi->rate;
    v->gain = (vol <= 0) ? 1.0f : ((float) vol / 80.0f);
    if (v->gain > 1.0f) v->gain = 1.0f;
    v->pos_frac = 0.0f;
    v->have_prev = 0;
    v->stage_frames = 0;
    v->stage_pos = 0;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_stop_all(void) {
    if (audio_mutex < 0) return;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    current_bgm_id = -1;
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_close(&voices[i]);
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_shutdown(void) {
    if (!audio_running) return;
    audio_running = 0;
    sceKernelWaitThreadEnd(audio_thread_id, NULL, NULL);
    sceKernelDeleteThread(audio_thread_id);
    audio_stop_all();
    if (audio_port >= 0) sceAudioOutReleasePort(audio_port);
    if (audio_mutex >= 0) sceKernelDeleteMutex(audio_mutex);
}
