/* a6_sound.c -- the game's Java GLMediaPlayer, in C.
 *
 * On a phone GLMediaPlayer plays OGG files (registerSoundFile: an id and a
 * path) with SoundPool / MediaPlayer: short sounds with a volume, loop and
 * pitch (the engine sounds' rpm: setPitch), music streamed. Here:
 *   - a sound is decoded to s16 once, on its first play, and kept;
 *   - music (and any sound over A6_STREAM_BYTES on disk) is streamed by its
 *     own stb_vorbis decoder in the mixer;
 *   - vox's AudioTrack stream (if the engine uses it) goes through a ring;
 *   - all of it is mixed on the runtime's audout pump (rt_audout.h), at the
 *     output rate, with linear resampling (which also does the pitch).
 * MIT; stb_vorbis is public domain (thirdparty/).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_path.h"
#include "rt_audout.h"
#include "util.h"

#include "a6.h"

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#include "thirdparty/stb_vorbis.c"

#define MAX_SOUNDS       1024
#define MAX_VOICES       32
#define A6_STREAM_BYTES  (512 * 1024) /* larger files stream instead of decoding */
#define RING_FRAMES      (48000 / 4)  /* the AudioTrack ring: 250 ms */

typedef struct {
  char *path;        /* host path */
  int16_t *pcm;      /* decoded, interleaved; NULL until first use */
  int frames, chans, rate;
  int stream;        /* 1: never decoded whole */
  int failed;
} Sound;

typedef struct {
  int used, gen, id, paused, loop, music;
  float vol, pitch;
  /* sample voices: position in frames, 32.32 fixed point */
  uint64_t pos;
  /* streamed voices */
  stb_vorbis *vf;
  int16_t buf[2 * 2048];
  int buf_frames, buf_chans, rate;
  double spos;       /* position in buf, fractional */
} Voice;

static Sound g_snd[MAX_SOUNDS];
static Voice g_v[MAX_VOICES];
static Mutex g_lock;
static float g_master = 1.0f;
static unsigned g_out_rate = 48000;
static int g_ready;

static int16_t g_ring[RING_FRAMES * 2];
static volatile unsigned g_ring_w, g_ring_r; /* in frames, free running */
static double g_ring_step = 1.0, g_ring_pos;

/* ------------------------------------------------------------ table */
void a6_sound_register(int id, const char *android_path, int flags) {
  if (id < 0 || id >= MAX_SOUNDS || !android_path)
    return;
  char host[DCR_PATH_MAX];
  dcr_translate_path(android_path, host, sizeof host);
  mutexLock(&g_lock);
  Sound *s = &g_snd[id];
  free(s->path);
  free(s->pcm);
  memset(s, 0, sizeof *s);
  s->path = strdup(host);
  FILE *f = fopen(host, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    s->stream = ftell(f) > A6_STREAM_BYTES;
    fclose(f);
  } else {
    s->failed = 1;
    debugPrintf("[snd] %d: %s missing\n", id, host);
  }
  mutexUnlock(&g_lock);
  (void)flags;
}

/* Decodes a sound whole (outside the lock: the mixer keeps going). */
static int ensure_decoded(int id) {
  Sound *s = &g_snd[id];
  if (s->pcm || s->stream)
    return 0;
  if (s->failed || !s->path)
    return -1;
  int ch = 0, rate = 0;
  short *out = NULL;
  int n = stb_vorbis_decode_filename(s->path, &ch, &rate, &out);
  if (n <= 0 || !out) {
    s->failed = 1;
    debugPrintf("[snd] %d: cannot decode %s\n", id, s->path);
    return -1;
  }
  s->frames = n;
  s->chans = ch;
  s->rate = rate;
  s->pcm = out;
  return 0;
}

/* ------------------------------------------------------------ voices */
static int handle_of(int i) { return (g_v[i].gen << 8) | i; }
static Voice *voice_of(int h) {
  int i = h & 0xff;
  if (h < 0 || i >= MAX_VOICES || !g_v[i].used || g_v[i].gen != (h >> 8))
    return NULL;
  return &g_v[i];
}

static void voice_free(Voice *v) {
  if (v->vf)
    stb_vorbis_close(v->vf);
  v->vf = NULL;
  v->used = 0;
}

static int voice_start(int id, float vol, int loop, float pitch, int music) {
  if (id < 0 || id >= MAX_SOUNDS || !g_ready)
    return -1;
  Sound *s = &g_snd[id];
  if (!music && ensure_decoded(id) != 0)
    return -1;
  stb_vorbis *vf = NULL;
  if (music || s->stream) {
    if (!s->path)
      return -1;
    int err = 0;
    vf = stb_vorbis_open_filename(s->path, &err, NULL);
    if (!vf) {
      debugPrintf("[snd] %d: cannot open %s (%d)\n", id, s->path, err);
      return -1;
    }
  }
  mutexLock(&g_lock);
  int i, best = -1;
  for (i = 0; i < MAX_VOICES; i++)
    if (!g_v[i].used) {
      best = i;
      break;
    }
  if (best < 0) { /* all busy: take the first non-looping sound */
    for (i = 0; i < MAX_VOICES; i++)
      if (!g_v[i].loop && !g_v[i].music) {
        voice_free(&g_v[i]);
        best = i;
        break;
      }
  }
  int h = -1;
  if (best >= 0) {
    Voice *v = &g_v[best];
    int gen = (v->gen + 1) & 0x7fffff;
    memset(v, 0, sizeof *v);
    v->gen = gen;
    v->used = 1;
    v->id = id;
    v->vol = vol;
    v->loop = loop;
    v->pitch = pitch > 0.0f ? pitch : 1.0f;
    v->music = music;
    v->vf = vf;
    if (vf) {
      stb_vorbis_info inf = stb_vorbis_get_info(vf);
      v->buf_chans = inf.channels;
      v->rate = inf.sample_rate;
    }
    h = handle_of(best);
  } else if (vf) {
    stb_vorbis_close(vf);
  }
  mutexUnlock(&g_lock);
  return h;
}

int a6_sound_play(int id, float vol, int loop, float pitch) {
  return voice_start(id, vol, loop, pitch, 0);
}

/* Music by its sound id: one voice per id. */
static int music_voice(int id) {
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_v[i].used && g_v[i].music && (id < 0 || g_v[i].id == id))
      return i;
  return -1;
}

int a6_music_load(int id) {
  return id >= 0 && id < MAX_SOUNDS && g_snd[id].path && !g_snd[id].failed ? 0 : -1;
}

int a6_music_play(int id, float vol, int loop) {
  a6_music_stop(id);
  return voice_start(id, vol, loop, 1.0f, 1);
}

void a6_music_pause(int id, int paused) {
  mutexLock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_v[i].used && g_v[i].music && (id < 0 || g_v[i].id == id))
      g_v[i].paused = paused;
  mutexUnlock(&g_lock);
}

void a6_music_stop(int id) {
  mutexLock(&g_lock);
  int i;
  while ((i = music_voice(id)) >= 0)
    voice_free(&g_v[i]);
  mutexUnlock(&g_lock);
}

void a6_music_volume(int id, float vol) {
  mutexLock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_v[i].used && g_v[i].music && (id < 0 || g_v[i].id == id))
      g_v[i].vol = vol;
  mutexUnlock(&g_lock);
}

int a6_music_playing(int id) {
  mutexLock(&g_lock);
  int i = music_voice(id);
  int on = i >= 0 && !g_v[i].paused;
  mutexUnlock(&g_lock);
  return on;
}

void a6_voice_pitch(int h, float pitch) {
  mutexLock(&g_lock);
  Voice *v = voice_of(h);
  if (v && pitch > 0.0f)
    v->pitch = pitch;
  mutexUnlock(&g_lock);
}

void a6_voice_stop(int h) {
  mutexLock(&g_lock);
  Voice *v = voice_of(h);
  if (v)
    voice_free(v);
  mutexUnlock(&g_lock);
}

void a6_sound_stop_all(void) {
  mutexLock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_v[i].used && !g_v[i].music)
      voice_free(&g_v[i]);
  mutexUnlock(&g_lock);
}

void a6_sound_master(float vol) { g_master = vol < 0 ? 0 : vol > 1 ? 1 : vol; }
float a6_sound_get_master(void) { return g_master; }

/* ------------------------------------------------------------ AudioTrack */
void a6_stream_write(const int16_t *pcm, int frames, int chans, int rate) {
  if (!g_ready)
    return; /* no mixer (sound is OpenSL ES): drop, never block the engine */
  g_ring_step = (double)rate / (double)g_out_rate;
  for (int i = 0; i < frames; i++) {
    while (g_ring_w - g_ring_r >= RING_FRAMES)
      svcSleepThread(2000000); /* full: wait, as AudioTrack.write blocks */
    unsigned w = g_ring_w % RING_FRAMES;
    g_ring[2 * w] = pcm[i * chans];
    g_ring[2 * w + 1] = pcm[i * chans + (chans > 1)];
    g_ring_w++;
  }
}

/* ------------------------------------------------------------ the mix */
static inline int16_t sample_at(const int16_t *p, int chans, int frame, int c) {
  return p[frame * chans + (chans > 1 ? c : 0)];
}

static void mix_sample_voice(Voice *v, int32_t *acc, int frames) {
  Sound *s = &g_snd[v->id];
  double step = (double)s->rate / g_out_rate * v->pitch;
  uint64_t inc = (uint64_t)(step * 4294967296.0);
  int32_t vol = (int32_t)(v->vol * g_master * 256.0f);
  for (int i = 0; i < frames; i++) {
    uint32_t f = (uint32_t)(v->pos >> 32);
    if ((int)f >= s->frames) {
      if (!v->loop) {
        voice_free(v);
        return;
      }
      v->pos -= (uint64_t)s->frames << 32;
      f = (uint32_t)(v->pos >> 32);
    }
    acc[2 * i] += (sample_at(s->pcm, s->chans, f, 0) * vol) >> 8;
    acc[2 * i + 1] += (sample_at(s->pcm, s->chans, f, 1) * vol) >> 8;
    v->pos += inc;
  }
}

static void mix_stream_voice(Voice *v, int32_t *acc, int frames) {
  double step = (double)v->rate / g_out_rate * v->pitch;
  int32_t vol = (int32_t)(v->vol * g_master * 256.0f);
  for (int i = 0; i < frames; i++) {
    int tries = 0;
    while ((int)v->spos >= v->buf_frames) {
      if (++tries > 4) { /* a file that yields nothing, even from its start */
        voice_free(v);
        return;
      }
      v->spos -= v->buf_frames;
      v->buf_frames = stb_vorbis_get_samples_short_interleaved(v->vf, v->buf_chans, v->buf,
                                                               (int)(sizeof v->buf / sizeof v->buf[0]));
      if (v->buf_frames <= 0) {
        if (!v->loop) {
          voice_free(v);
          return;
        }
        stb_vorbis_seek_start(v->vf);
        v->buf_frames = 0;
        v->spos = 0;
      }
    }
    int f = (int)v->spos;
    acc[2 * i] += (sample_at(v->buf, v->buf_chans, f, 0) * vol) >> 8;
    acc[2 * i + 1] += (sample_at(v->buf, v->buf_chans, f, 1) * vol) >> 8;
    v->spos += step;
  }
}

static void mix_ring(int32_t *acc, int frames) {
  for (int i = 0; i < frames && g_ring_r != g_ring_w; i++) {
    unsigned r = g_ring_r % RING_FRAMES;
    acc[2 * i] += g_ring[2 * r];
    acc[2 * i + 1] += g_ring[2 * r + 1];
    g_ring_pos += g_ring_step;
    while (g_ring_pos >= 1.0 && g_ring_r != g_ring_w) {
      g_ring_pos -= 1.0;
      g_ring_r++;
    }
  }
}

static void fill(int16_t *out, int frames, void *ud) {
  (void)ud;
  static int32_t acc[RT_AUDOUT_FRAMES * 2];
  if (frames > RT_AUDOUT_FRAMES)
    frames = RT_AUDOUT_FRAMES;
  memset(acc, 0, sizeof(int32_t) * 2 * frames);
  mutexLock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++) {
    Voice *v = &g_v[i];
    if (!v->used || v->paused)
      continue;
    if (v->vf)
      mix_stream_voice(v, acc, frames);
    else if (g_snd[v->id].pcm)
      mix_sample_voice(v, acc, frames);
  }
  mutexUnlock(&g_lock);
  mix_ring(acc, frames);
  for (int i = 0; i < 2 * frames; i++)
    out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}

/* ------------------------------------------------------------ life */
int a6_sound_init(void) {
  mutexInit(&g_lock);
  if (rt_audout_open() != 0)
    return -1;
  g_out_rate = rt_audout_rate();
  if (rt_audout_pump_start(fill, NULL, 0x2A, -2) != 0)
    return -1;
  g_ready = 1;
  debugPrintf("[snd] mixer at %u Hz, %d voices\n", g_out_rate, MAX_VOICES);
  return 0;
}

void a6_sound_pause(int paused) { rt_audout_pause(paused); }

void a6_sound_shutdown(void) {
  if (!g_ready)
    return;
  g_ready = 0;
  rt_audout_pump_stop();
  rt_audout_close();
}
