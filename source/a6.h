/* a6.h -- what the port's files share. MIT. */
#ifndef A6_H
#define A6_H
#include <stdint.h>

#include "so_util.h"

extern so_module g_a6_mod;

/* The Java classes the engine is handed (as jclass / jobject). */
#define A6_JPKG        "com/gameloft/android/HEP/GloftA6HP/"
#define A6_CLS_GAME    A6_JPKG "GLGame"
#define A6_CLS_REND    A6_JPKG "GameRenderer"
#define A6_CLS_VIEW    A6_JPKG "GameGLSurfaceView"
#define A6_CLS_MEDIA   A6_JPKG "GLMediaPlayer"
#define A6_CLS_RES     A6_JPKG "GLResLoader"
#define A6_CLS_DEVICE  A6_JPKG "GLUtils/Device"

/* a6_setup.c: the game data out of the APK's assets. */
#include "dcr_setup.h"
void a6_setup_data(RtSetupCtx *ctx);

/* a6_swp.c: SWP -> LDREX/STREX stubs, between so_resolve and so_finalize. */
int a6_swp_fix(so_module *m);
/* a6_hook.c: diagnostic hooks (GetPackFilename), after a6_swp_fix. */
int a6_hooks_install(so_module *m);

/* a6_gl.c: the EGL window context (GLES 2), made before the first GL call. */
int a6_gl_init(void);
void a6_gl_swap(void);
void a6_gl_set_interval(int interval);
/* 1 when the engine swapped through GameRenderer.swapEGLBuffers this frame. */
extern int g_a6_swapped;

/* a6_sound.c: GLMediaPlayer (OGG voices) + AudioTrack stream -> audout. */
int a6_sound_init(void);
void a6_sound_shutdown(void);
void a6_sound_pause(int paused);
void a6_sound_register(int id, const char *android_path, int flags);
int a6_sound_play(int id, float vol, int loop, float pitch);       /* -> voice */
int a6_music_load(int id);
int a6_music_play(int id, float vol, int loop);
void a6_music_pause(int id, int paused);
void a6_music_stop(int id);
void a6_music_volume(int id, float vol);
int a6_music_playing(int id);
void a6_voice_pitch(int voice, float pitch);
void a6_voice_stop(int voice);
void a6_sound_stop_all(void);
void a6_sound_master(float vol);
float a6_sound_get_master(void);
/* vox's AudioTrack: interleaved s16 at `rate`, blocks while the ring is full. */
void a6_stream_write(const int16_t *pcm, int frames, int chans, int rate);

/* a6_input.c: controllers + touch -> the engine. */
void a6_input_init(void);
void a6_input_poll(void);
/* L+R+Minus held: a6_main.c stops presenting so the watchdog dumps threads. */
extern volatile int g_a6_dump_request;

/* a6_config.c: config.ini. */
typedef struct {
  int fps60;         /* 1: 60 fps, 0: 30 */
  float steer_gain;  /* left stick -> accelerometer tilt */
  int touch;         /* touch screen passed to the game */
  int swap_ab;       /* Nintendo vs Xbox layout in menus */
  int steer_mode;    /* 0 left stick, 1 motion (tilt the console / controller), 2 both */
  float motion_gain; /* motion: how many "phone tilts" one console tilt is */
  int motion_invert; /* motion: steer the other way */
  float garage_look; /* garage: right stick look speed */
  int promo_cars;    /* Audi TT-RS and Ferrari California unlock by stars too */
} A6Config;
extern A6Config g_a6_cfg;
/* The engine's language number (1 English, 3 German...): config or console. */
int a6_language(void);
/* a6_heap.c: one [mem] line (the game vs the whole heap). */
void a6_heap_report(const char *why);
void a6_hooks_report(void);
void a6_game_pad(int down, unsigned bits);
int a6_in_race(void);
int a6_race_paused(void);
int a6_in_garage(void);
void a6_garage_sticks(float wx, float wy, float lx, float ly, int walk_owned);
void a6_race_next_camera(void);
void a6_cursor_set(float x, float y, int shown, int pressed, int crosshair);
void a6_unlock_cars_by_stars(void);

#endif /* A6_H */
