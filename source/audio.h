/*
 * audio.h
 *
 * Audio engine for ILLUSIA 2 PS Vita port.
 * Uses Tremor (libvorbisidec) and SceAudioOut with a dedicated mixing thread.
 */

#ifndef ILLUSIA_AUDIO_H
#define ILLUSIA_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_play(int snd_id, int vol, int is_loop);
void audio_stop_all(void);
void audio_set_bgm_volume(float vol);
void audio_set_sfx_volume(float vol);
void audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // ILLUSIA_AUDIO_H
