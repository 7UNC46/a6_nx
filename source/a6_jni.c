/* a6_jni.c -- the Java side of Asphalt 6, answered in C.
 *
 * The engine has no class names in it: every Java method is looked up on the
 * jclass a native init was called with (GLGame.nativeInit -> GLGame's static
 * methods, GLMediaPlayer.nativeInit -> GLMediaPlayer's...). The names and
 * signatures below are the APK's own (classes.dex, read with tools/dexnat.py).
 * A method missing here falls through to the runtime's default answer and is
 * logged once as "[jni] unhandled ..." in debug.log: that is how the next ones
 * to add show up during bring-up. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "dcr_path.h"
#include "jni.h"
#include "port_config.h"
#include "rt_applet.h"
#include "util.h"

#include "a6.h"

/* GLGame.getSDFolder: the repack's data folder (its DataInstallActivity
 * copies assets/gamedata/ to getExternalFilesDir). The engine appends
 * "/name" ("%s/%s"), so no trailing slash. */
#define A6_SD_FOLDER "/sdcard/Android/data/" PORT_PACKAGE "/files"

/* ------------------------------------------------------------ helpers */
/* Strings handed to the engine are kept alive for good (see mc3_jni.c: the
 * runtime's DeleteLocalRef frees at once, Gameloft code reads after it). */
static jvalue ret_str(const char *s) {
  static struct { char *val; JObj *o; } cache[32];
  static int n;
  static Mutex lock;
  mutexLock(&lock);
  for (int i = 0; i < n; i++)
    if (!strcmp(cache[i].val, s)) {
      mutexUnlock(&lock);
      return jv_l(cache[i].o);
    }
  JObj *o = jni_str(s);
  if (o) {
    o->immortal = 1;
    if (n < 32 && (cache[n].val = strdup(s)))
      cache[n++].o = o;
  }
  mutexUnlock(&lock);
  return jv_l(o);
}

static jvalue ret_bytes(const void *p, int len) {
  JObj *a = jni_array('B', len);
  if (a && len > 0)
    memcpy(a->a.data, p, len);
  return jv_l(a);
}

static jvalue ret_cstr_bytes(const char *s) { return ret_bytes(s, (int)strlen(s)); }

/* An Android path, or one relative to the SD folder, to a host path. */
static const char *res_path(const char *name, char *out, size_t cap) {
  char tmp[DCR_PATH_MAX];
  if (name[0] != '/') {
    snprintf(tmp, sizeof tmp, A6_SD_FOLDER "/%s", name);
    name = tmp;
  }
  return dcr_translate_path(name, out, cap);
}

/* ------------------------------------------------------------ GLGame */
static JNI_H_DECL(h_sd_folder) { return ret_str(A6_SD_FOLDER); }
static JNI_H_DECL(h_version) { return ret_cstr_bytes("1.0.0"); }
static JNI_H_DECL(h_host_name) { return ret_cstr_bytes("nintendo-switch"); }
static JNI_H_DECL(h_imei) { return ret_str("000000000000000"); }
static JNI_H_DECL(h_empty_bytes) { return ret_bytes("", 0); }

static JNI_H_DECL(h_exit) {
  debugPrintf("[a6] GLGame.Exit: leaving\n");
  rt_request_exit();
  a6_sound_shutdown();
  log_flush_ring();
  _exit(0);
  return jv_none();
}

static JNI_H_DECL(h_log_url) {
  debugPrintf("[a6] %s(\"%s\") ignored\n", m->name, a ? jni_utf(a[0].l) : "");
  return jv_none();
}

/* ------------------------------------------------------------ GLResLoader */
static JNI_H_DECL(h_res_full) {
  char p[DCR_PATH_MAX];
  const char *name = a ? jni_utf(a[0].l) : "";
  FILE *f = fopen(res_path(name, p, sizeof p), "rb");
  if (!f) {
    debugPrintf("[res] getResourceFull(\"%s\"): not found (%s)\n", name, p);
    return jv_l(NULL);
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  JObj *arr = jni_array('B', (jsize)len);
  if (arr && len > 0 && fread(arr->a.data, 1, (size_t)len, f) != (size_t)len)
    debugPrintf("[res] short read on %s\n", p);
  fclose(f);
  return jv_l(arr);
}

static JNI_H_DECL(h_res_bytes) {
  char p[DCR_PATH_MAX];
  const char *name = a ? jni_utf(a[0].l) : "";
  int off = a ? a[1].i : 0, len = a ? a[2].i : 0;
  FILE *f = fopen(res_path(name, p, sizeof p), "rb");
  if (!f || len <= 0) {
    if (f)
      fclose(f);
    return jv_l(NULL);
  }
  JObj *arr = jni_array('B', len);
  fseek(f, off, SEEK_SET);
  if (arr)
    fread(arr->a.data, 1, (size_t)len, f);
  fclose(f);
  return jv_l(arr);
}

static JNI_H_DECL(h_res_length) {
  char p[DCR_PATH_MAX];
  const char *name = a ? jni_utf(a[0].l) : "";
  FILE *f = fopen(res_path(name, p, sizeof p), "rb");
  if (!f)
    return jv_i(-1);
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fclose(f);
  return jv_i((jint)len);
}

/* ------------------------------------------------------------ GameRenderer */
static JNI_H_DECL(h_swap) {
  g_a6_swapped = 1;
  a6_gl_swap();
  return jv_none();
}

/* TODO: the on-screen keyboard (player name). swkbd from the 32-bit side,
 * then the text in getKeyboardText and onKeyboardFinish called. */
static char g_kbd_text[64] = "Player";
static JNI_H_DECL(h_set_keyboard) {
  debugPrintf("[a6] setKeyboard(%d, \"%s\", %d): no keyboard yet\n", a ? a[0].i : 0,
              a ? jni_utf(a[1].l) : "", a ? a[2].i : 0);
  return jv_none();
}
static JNI_H_DECL(h_keyboard_text) { return ret_cstr_bytes(g_kbd_text); }

/* ------------------------------------------------------------ GLMediaPlayer */
static JNI_H_DECL(h_register_sound) {
  a6_sound_register(a[0].i, jni_utf(a[1].l), a[2].i);
  return jv_none();
}
/* playSound(id, volume, loop, pitch) -> a voice handle */
static JNI_H_DECL(h_play_sound) { return jv_i(a6_sound_play(a[0].i, a[1].f, a[2].i, a[3].f)); }
static JNI_H_DECL(h_load_music) { a6_music_load(a[0].i); return jv_none(); }
static JNI_H_DECL(h_music_loaded) { return jv_i(a6_music_load(a[0].i) == 0); }
/* playMusic(id, volume, loop) */
static JNI_H_DECL(h_play_music) { return jv_i(a6_music_play(a[0].i, a[1].f, a[2].i)); }
static JNI_H_DECL(h_pause_music) { a6_music_pause(a[0].i, 1); return jv_none(); }
static JNI_H_DECL(h_resume_music) { a6_music_pause(a[0].i, 0); a6_music_volume(a[0].i, a[1].f); return jv_none(); }
static JNI_H_DECL(h_stop_music) { a6_music_stop(a[0].i); return jv_none(); }
static JNI_H_DECL(h_music_vol1) { a6_music_volume(a[0].i, a[1].f); return jv_none(); }
static JNI_H_DECL(h_music_vol) { a6_music_volume(a[1].i, a[0].f); return jv_none(); } /* (FI) */
static JNI_H_DECL(h_media_playing) { return jv_z(a6_music_playing(a[0].i)); }
static JNI_H_DECL(h_pause_all_music) { a6_music_pause(-1, 1); return jv_none(); }
static JNI_H_DECL(h_resume_all_music) { a6_music_pause(-1, 0); return jv_none(); }
static JNI_H_DECL(h_stop_all_music) { a6_music_stop(-1); return jv_none(); }
static JNI_H_DECL(h_stop_all_sounds) { a6_sound_stop_all(); return jv_none(); }
/* setPitch(id, voice, pitch): the engine sounds' rpm */
static JNI_H_DECL(h_set_pitch) { a6_voice_pitch(a[1].i, a[2].f); return jv_none(); }
static JNI_H_DECL(h_set_master) { a6_sound_master(a[0].f); return jv_none(); }
static JNI_H_DECL(h_get_master) { return jv_f(a6_sound_get_master()); }
static JNI_H_DECL(h_one_f) { return jv_f(1.0f); }
/* Emitters (id, voice): the same voices, by handle. */
static JNI_H_DECL(h_emitter_pitch) { a6_voice_pitch(a[1].i, a[2].f); return jv_none(); }
static JNI_H_DECL(h_emitter_stop) { a6_voice_stop(a[1].i); return jv_none(); }
static JNI_H_DECL(h_players) { return jv_i(16); }

/* ------------------------------------------------------------ AudioTrack
 * vox::DriverAndroid's stream (the engine's own mixer, if it uses it). */
static JNI_H_DECL(h_at_init) {
  if (self) {
    self->v[0] = a ? a[1].i : 44100;
    self->v[1] = a && a[2].i == 4 ? 1 : 2; /* CHANNEL_OUT_MONO 4 / STEREO 12 */
  }
  debugPrintf("[a6] AudioTrack(%d Hz, %d ch)\n", self ? (int)self->v[0] : 0, self ? (int)self->v[1] : 0);
  return jv_none();
}
static JNI_H_DECL(h_at_write) {
  JObj *arr = a ? a[0].l : NULL;
  jint off = a ? a[1].i : 0, len = a ? a[2].i : 0;
  if (!arr || arr->kind != JK_ARRAY || off < 0 || len <= 0 || off + len > arr->a.len)
    return jv_i(0);
  int rate = self ? (int)self->v[0] : 44100, ch = self && self->v[1] ? (int)self->v[1] : 2;
  const int16_t *s = arr->a.elem == 'S' ? (const int16_t *)arr->a.data + off
                                        : (const int16_t *)((const uint8_t *)arr->a.data + off);
  int frames = arr->a.elem == 'S' ? len / ch : len / 2 / ch;
  a6_stream_write(s, frames, ch, rate);
  return jv_i(len);
}
static JNI_H_DECL(h_at_minbuf) { return jv_i(8192); }
static JNI_H_DECL(h_44k) { return jv_i(44100); }

/* ------------------------------------------------------------ the tables */
#define G  A6_CLS_GAME
#define MP A6_CLS_MEDIA
#define RL A6_CLS_RES
#define GR A6_CLS_REND
#define DV A6_CLS_DEVICE
#define AT "android/media/AudioTrack"

const JMethodDef jni_method_defs[] = {
    /* GLGame */
    {G, "getSDFolder", NULL, h_sd_folder},
    {G, "getVersion", NULL, h_version},
    {G, "getHostName", NULL, h_host_name},
    {G, "getIMEI", NULL, h_imei},
    {G, "getWifiIP", NULL, jni_h_zero},
    {G, "IsWifiEnabled", NULL, jni_h_zero},
    {G, "IsInternetAvaliable", NULL, jni_h_zero},
    {G, "isExternalMusicActive", NULL, jni_h_zero},
    {G, "IsDemo", NULL, jni_h_zero},
    {G, "IsDoubleOption", NULL, jni_h_zero},
    {G, "GetDoubleOptionText1", NULL, h_empty_bytes},
    {G, "GetDoubleOptionText2", NULL, h_empty_bytes},
    {G, "GetDoubleOptionText3", NULL, h_empty_bytes},
    {G, "Exit", NULL, h_exit},
    {G, "sendAppToBackground", NULL, jni_h_void},
    {G, "OpenBrowser", NULL, h_log_url},
    {G, "OpenGLive", NULL, jni_h_void},
    {G, "OpenIGP", NULL, jni_h_void},
    {G, "LaunchBilling", NULL, jni_h_void},
    {G, "LaunchCustomerSupport", NULL, jni_h_void},
    {G, "OpenCustomerSupport", NULL, jni_h_void},
    {G, "NotifyTrophy", NULL, jni_h_void},
    {G, "initialize_Trophy", NULL, jni_h_void},
    {G, "launchGetGames", NULL, jni_h_void},

    /* GLUtils.Device */
    {DV, "getHostName", NULL, h_host_name},
    {DV, "getIMEI", NULL, h_imei},
    {DV, "getDeviceId", NULL, h_imei},
    {DV, "IsWifiEnable", NULL, jni_h_false},
    {DV, "IsConnectionReady", NULL, jni_h_false},

    /* GLResLoader */
    {RL, "getResourceFull", "(Ljava/lang/String;)[B", h_res_full},
    {RL, "getResourceBytes", NULL, h_res_bytes},
    {RL, "getResourceLength", NULL, h_res_length},

    /* GameRenderer */
    {GR, "swapEGLBuffers", NULL, h_swap},
    {GR, "setKeyboard", NULL, h_set_keyboard},
    {GR, "getKeyboardText", NULL, h_keyboard_text},
    {GR, "isKeyboardVisible", NULL, jni_h_zero},

    /* GLMediaPlayer */
    {MP, "registerSoundFile", NULL, h_register_sound},
    {MP, "loadMusic", NULL, h_load_music},
    {MP, "isMusicLoaded", NULL, h_music_loaded},
    {MP, "unloadMusic", NULL, h_stop_music},
    {MP, "playMusic", NULL, h_play_music},
    {MP, "playSound", NULL, h_play_sound},
    {MP, "pauseMusic", NULL, h_pause_music},
    {MP, "resumeMusic", NULL, h_resume_music},
    {MP, "stopMusic", NULL, h_stop_music},
    {MP, "resetMusic", NULL, h_stop_music},
    {MP, "setVolumeOneMusic", NULL, h_music_vol1},
    {MP, "setVolumeMusic", NULL, h_music_vol},
    {MP, "getVolumeMusic", NULL, h_one_f},
    {MP, "isMediaPlaying", NULL, h_media_playing},
    {MP, "pauseAllMusic", NULL, h_pause_all_music},
    {MP, "resumeAllMusic", NULL, h_resume_all_music},
    {MP, "stopAllMusic", NULL, h_stop_all_music},
    {MP, "stopAllSounds", NULL, h_stop_all_sounds},
    {MP, "setPitch", NULL, h_set_pitch},
    {MP, "setMasterVolume", NULL, h_set_master},
    {MP, "getMasterVolume", NULL, h_get_master},
    {MP, "setEmitterPitch", NULL, h_emitter_pitch},
    {MP, "stopEmitter", NULL, h_emitter_stop},
    {MP, "getNumberSimultaneousPlayers", NULL, h_players},
    {MP, "isFinishBackground", NULL, jni_h_true},
    {MP, "isRecoveringAudio", NULL, jni_h_zero},
    {MP, "isEmitterPlaying", NULL, jni_h_false},
    {MP, NULL, NULL, jni_h_void}, /* the rest (pools, emitters' volume, movies...) */

    /* android.media.AudioTrack */
    {AT, "<init>", NULL, h_at_init},
    {AT, "getMinBufferSize", NULL, h_at_minbuf},
    {AT, "getNativeOutputSampleRate", NULL, h_44k},
    {AT, "write", NULL, h_at_write},
    {AT, "play", NULL, jni_h_void},
    {AT, "pause", NULL, jni_h_void},
    {AT, "stop", NULL, jni_h_void},
    {AT, "flush", NULL, jni_h_void},
    {AT, "release", NULL, jni_h_void},
    {AT, "getPlayState", NULL, jni_h_zero},

    {NULL, NULL, NULL, NULL},
};

const JFieldDef jni_field_defs[] = {
    {"android/os/Build$VERSION", "SDK_INT", NULL, 10, NULL}, /* the game is from 2.3 days */
    {NULL, NULL, NULL, 0, NULL},
};

const char *const jni_class_supers[][2] = {{NULL, NULL}};
const char *const jni_missing_classes[] = {NULL};
