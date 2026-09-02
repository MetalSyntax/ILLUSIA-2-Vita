/*
 * audio.c
 *
 * Audio engine for ILLUSIA 2 PS Vita port.
 * Mixed output via SceAudioOut and Tremor (libvorbisidec).
 */

#include "audio.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tremor/ivorbisfile.h>
#include "utils/logger.h"

#define AUDIO_RATE 44100
#define AUDIO_GRAIN 512

/* Real engine (NexusSound.java) keeps the "main track" (id==0 or id>9) split across TWO
 * independent MediaPlayer instances chosen purely by the `loop` flag -- mBgmPlayer for
 * loop==true, mPlayer for loop==false -- so a one-shot cue (e.g. a scene-transition sting)
 * never stops the currently playing ambient/BGM loop. VOICE_BGM/VOICE_MAIN_ONESHOT mirror
 * that split; sharing a single slot for both (as this file used to) made every one-shot
 * main-track cue kill and replace the ambient loop, confirmed in game_071.log: id=10
 * (loop=0) repeatedly interrupting id=72/79/80/81/82 (loop=1) ambient cues. */
#define VOICE_BGM          0
#define VOICE_MAIN_ONESHOT 1
#define VOICE_SFX0         2
#define NUM_VOICES         6

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

/* CGsSound::Play(soundId, ...) never sends `soundId` over the OnSoundPlay JNI bridge --
 * it calls CGsSound::PlaySound -> GetSoundIDListIdx(soundId), which linearly searches an
 * 84-entry internal array (CGsSound::OnCreate(this, 0x54, &table), out_ghidra.c:98682) and
 * sends the POSITION of soundId within that array instead (MC_androidPlaySound(idx, ...),
 * out_ghidra.c:19838). On real Android this is invisible because that 84-entry table is
 * exactly the game's res/raw/sNNN.ogg files in ascending numeric order, and aapt assigns
 * R.raw.s000..R.raw.sNNN the same way (alphabetical/ascending, no gaps for missing
 * numbers) -- so `R.raw.s000 + index` (SkeletonUIControllerView.java:215) always lands on
 * the right file by construction. Confirmed empirically via game_074.log's paired
 * `[Sound] CGsSound::Play id=X` / `[Java] OnSoundPlay: id=Y` diagnostic lines (real id ->
 * index): 100->72, 107->79, 109->81, 110->82, 13->12, 11->10, 15->14, 7->6, 0->0 -- every
 * single one matches "index = position of id among the 84 sNNN.ogg files that actually
 * exist, sorted ascending" exactly, and res/raw contains exactly 84 such files (0x54,
 * matching the registration count exactly). audio_play/Illusia_OnSoundPlay used to treat
 * this index as if it were the literal file number, which only coincidentally worked for
 * ids 1-9 (no gaps in that range) and silently played the WRONG file for every id at or
 * past the first gap (id 4) -- explaining both "menu has no music" (title BGM's real id is
 * high enough to have drifted several slots from its index) and "in-game loops the wrong
 * effect instead of ambient". */
#define MAX_SOUND_IDS 128
static int g_sound_id_table[MAX_SOUND_IDS];
static int g_sound_id_count = 0;

static int cmp_int_asc(const void *a, const void *b) {
    return (*(const int *) a) - (*(const int *) b);
}

static void build_sound_id_table(void) {
    g_sound_id_count = 0;
    DIR *d = opendir("ux0:data/illusia2/res/raw");
    if (!d) {
        game_log("[Audio] Warning: could not open res/raw to build sound-id table\n");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_sound_id_count < MAX_SOUND_IDS) {
        int id;
        if (sscanf(ent->d_name, "s%3d.ogg", &id) == 1) {
            g_sound_id_table[g_sound_id_count++] = id;
        }
    }
    closedir(d);
    qsort(g_sound_id_table, g_sound_id_count, sizeof(int), cmp_int_asc);
    game_log("[Audio] Built sound-id translation table: %d entries\n", g_sound_id_count);
}

/* Translates an OnSoundPlay "id" (really an index into the registered-sound-id list) into
 * the real sNNN.ogg file number. Returns -1 if out of range (no matching real file). */
static int translate_sound_index(int index) {
    if (index < 0 || index >= g_sound_id_count) return -1;
    return g_sound_id_table[index];
}

static int audio_resolve_path(int snd_id, char *out, size_t out_size) {
    /* Real APK layout: these sounds are Android `res/raw` resources (SoundPool.load()/
     * MediaPlayer.create() take a resource id, both backed by res/raw/sNNN.ogg on real
     * Android -- decompiled/apk_jadx/sources/.../NexusSound.java + SkeletonUIControllerView's
     * `R.raw.s000 + i`), not `assets/`. Check the real path first. */
    snprintf(out, out_size, "ux0:data/illusia2/res/raw/s%03d.ogg", snd_id);
    if (access(out, F_OK) == 0) return 1;

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

/* Additive PCM mixing with a hard int16 clamp distorts audibly (harsh digital clipping)
 * as soon as more than ~2 voices sum near full scale at once -- easily reached here with
 * up to NUM_VOICES simultaneous voices (BGM + one-shot + SFX round-robin). Soften only the
 * overshoot above a knee close to full scale with a saturating curve; anything below the
 * knee (the normal, non-overlapping case) passes through byte-for-byte unchanged. */
#define MIX_CLIP_KNEE 26000

static int16_t mix_soft_clip(int32_t s, int *clipped) {
    if (s > MIX_CLIP_KNEE) {
        float over = (float) (s - MIX_CLIP_KNEE) / (float) (32767 - MIX_CLIP_KNEE);
        s = MIX_CLIP_KNEE + (int32_t) ((32767 - MIX_CLIP_KNEE) * (1.0f - 1.0f / (1.0f + over)));
        if (clipped) *clipped = 1;
    } else if (s < -MIX_CLIP_KNEE) {
        float over = (float) (-MIX_CLIP_KNEE - s) / (float) (32768 - MIX_CLIP_KNEE);
        s = -MIX_CLIP_KNEE - (int32_t) ((32768 - MIX_CLIP_KNEE) * (1.0f - 1.0f / (1.0f + over)));
        if (clipped) *clipped = 1;
    }
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t) s;
}

static int audio_thread(SceSize args, void *argp) {
    static int16_t mix[AUDIO_GRAIN * 2];
    static int16_t buf[AUDIO_GRAIN * 2];
    static int clip_log_count = 0;

    while (audio_running) {
        memset(mix, 0, sizeof(mix));
        int active_voices = 0;
        int grain_clipped = 0;

        sceKernelLockMutex(audio_mutex, 1, NULL);
        for (int vi = 0; vi < NUM_VOICES; vi++) {
            voice_t *v = &voices[vi];
            if (!v->active) continue;
            active_voices++;
            int got = voice_decode(v, buf, AUDIO_GRAIN);
            for (int i = 0; i < got * 2; i++) {
                int s = mix[i] + buf[i];
                mix[i] = mix_soft_clip(s, &grain_clipped);
            }
        }
        sceKernelUnlockMutex(audio_mutex, 1);

        if (grain_clipped && clip_log_count < 40) {
            clip_log_count++;
            game_log("[Audio] Mix overshoot softened (active_voices=%d)\n", active_voices);
        }

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

    build_sound_id_table();

    game_log("[Audio] Sistema de audio inicializado.\n");
}

static int current_bgm_id = -1;

void audio_play(int snd_id, int vol, int is_loop) {
    /* Real engine sends OnSoundPlay(id, vol=0, isLoop=true) as a dedicated
     * "stop the BGM" signal, not a request to play anything -- confirmed in
     * decompiled/apk_jadx/sources/.../SkeletonUIControllerView.java:
     *   if (i2 == 0 && z) { NexusSound.stopBGMSound(); return; }
     * Without this, the old code fell through and started a new "BGM" voice
     * at gain=1.0 (vol<=0 defaults to full volume), i.e. the stop signal
     * played something loudly instead of silencing it. */
    if (vol == 0 && is_loop) {
        sceKernelLockMutex(audio_mutex, 1, NULL);
        voice_close(&voices[VOICE_BGM]);
        current_bgm_id = -1;
        sceKernelUnlockMutex(audio_mutex, 1);
        return;
    }

    /* `snd_id` as received here is actually an index into the game's internal
     * registered-sound-id list, not a real sNNN.ogg file number -- see the long
     * comment on translate_sound_index()/build_sound_id_table() above. Translate it
     * before doing anything id-based (routing, path resolution, same-track checks). */
    int real_id = translate_sound_index(snd_id);
    if (real_id < 0) return;
    snd_id = real_id;

    /* Routing mirrors the real engine's split (same file): ids 1-9 always go
     * through NexusSound.playSFXSound() (short one-shot SFX); id 0 or any id
     * > 9 go through NexusSound.playSound(id, isLoop). Looping is controlled
     * ENTIRELY by the isLoop flag the engine passes -- never by the id
     * itself. The previous `is_loop || snd_id >= 100` condition invented an
     * id-based looping rule with no basis in the real game: any one-shot
     * (isLoop=false) sound with id >= 100 got routed to the single BGM voice
     * AND had its loop flag forced to 1, so it played forever instead of
     * once -- a non-ambient sound stuck looping. */
    int is_main_track = (snd_id == 0 || snd_id > 9);

    // If the looping BGM is already playing the exact same track, just update volume smoothly.
    // Only applies to the loop case: a one-shot main-track cue always restarts (matches
    // mPlayer always being re-created in NexusSound.playSound() for the non-loop branch).
    if (is_main_track && is_loop && snd_id == current_bgm_id && voices[VOICE_BGM].active) {
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
    if (is_main_track) {
        if (is_loop) {
            target_slot = VOICE_BGM;
            current_bgm_id = snd_id;
        } else {
            // One-shot main-track cue: dedicated slot, does not touch the ambient/BGM voice.
            target_slot = VOICE_MAIN_ONESHOT;
        }
    } else {
        static int sfx_rr = 0;
        target_slot = VOICE_SFX0 + (sfx_rr++ % (NUM_VOICES - VOICE_SFX0));
    }

    sceKernelLockMutex(audio_mutex, 1, NULL);
    voice_t *v = &voices[target_slot];
    voice_close(v);

    v->vf = vf;
    v->active = 1;
    v->loop = is_loop;
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
