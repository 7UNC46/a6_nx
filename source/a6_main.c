/* a6_main.c -- Asphalt 6 on android32: from the player's APK to the game's
 * frame loop.
 *
 * port_load: the runtime has found the APK (by its package); the setup
 *   unpacks PORT_ABI_DIR/libguest_asphalt6.so next to it (once) and the game
 *   data out of the assets (a6_setup.c). Then the library is mapped,
 *   relocated, bound to the runtime's bionic shims, its 3 SWPs rewritten
 *   (ARMv8's AArch32 has no SWP) and sealed.
 *
 * port_run: what the Java side does on a phone (classes.dex of the APK):
 *     System.loadLibrary          -> JNI_OnLoad
 *     GLResLoader.init            -> GLResLoader.nativeInit()        static
 *     Device.init                 -> GLUtils.Device.nativeInit()     static
 *     GLMediaPlayer.init          -> GLMediaPlayer.nativeInit()      static
 *     GLGame.onCreate             -> GLGame.nativeInit()             static
 *     GameRenderer (GL thread):
 *       onSurfaceCreated          -> nativeGetJNIEnv() (instance), nativeInit(III) static
 *       onSurfaceChanged          -> nativeResize(II)                static
 *       onDrawFrame               -> nativeRender()                  static
 *     GameGLSurfaceView.onPause/onResume -> nativePause()/nativeResume() (instance)
 *   Here everything runs on the main thread, which is the GL thread too.
 *
 * nativeInit(width, height, language): see a6_language() (a6_config.c).
 * Sound is vox on OpenSL ES (port_config.h). MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_boost.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "jni.h"
#include "port_config.h"
#include "rt_applet.h"
#include "rt_boot.h"
#include "rt_cfg.h"
#include "rt_window.h"
#include "so_util.h"
#include "util.h"

#include "a6.h"

so_module g_a6_mod;

/* opensles.c (RT_OPENSLES 1) */
void dcr_audio_pause(int paused);
void dcr_audio_close(void);
int g_a6_swapped;

typedef int (*jni_onload_fn)(void *vm, void *reserved);
typedef void (*jni_void_fn)(void *env, void *cls_or_obj);
typedef void (*jni_iii_fn)(void *env, void *cls, int a, int b, int c);
typedef void (*jni_ii_fn)(void *env, void *cls, int a, int b);

static jni_void_fn f_render, f_pause, f_resume, f_pause_sounds;
static void *g_rend_cls, *g_view_obj;

/* ---------------------------------------------------------------- setup */
static const char *const k_libs[] = {A6_LIB};

/* With the APK open: the game data out of assets/gamedata/ (returns at once
 * when it is there and the APK has not changed). */
static const RtSetupStep k_steps[] = {
    {"game data", RT_STEP_WITH_ZIP, 100, 1000, a6_setup_data},
};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = 1,
    .apk_requirement = "Asphalt 6: Adrenaline HD (" PORT_PACKAGE ") with " A6_LIB,
    .libs_p0 = 0,
    .libs_p1 = 100,
    .steps = k_steps,
    .nsteps = 1,
};

const char *port_apk_help(void) {
  return "Copy the APK of your own Asphalt 6: Adrenaline HD to /switch/" PORT_NAME "/\n"
         "(any name). It must contain " PORT_ABI_DIR A6_LIB " and the game data\n"
         "in " A6_ASSET_DIR ".";
}

int port_load(const char *apk) {
  dcr_setup_from_apk(apk);

  unsigned long crc = 0, size = 0;
  if (A6_LIB_CRC32 && rt_setup_stamp_get(A6_LIB, &crc, &size) && (crc != A6_LIB_CRC32 || size != A6_LIB_SIZE))
    fatal_error("This APK's %s is not the build this port was made for\n"
                "(CRC %08lx, %lu bytes; expected %08lx, %lu bytes).\n\n%s",
                A6_LIB, crc, size, A6_LIB_CRC32, A6_LIB_SIZE, port_apk_help());

  char path[DCR_PATH_MAX];
  rt_root_path(path, sizeof path, A6_LIB);
  if (so_load(&g_a6_mod, path, NULL, PORT_SO_REGION_BYTES) != 0)
    fatal_error("Could not load %s.", path);
  so_relocate(&g_a6_mod);
  so_resolve(&g_a6_mod, NULL, 0, 1);
  so_fix_kuser_helpers(&g_a6_mod);
  if (a6_swp_fix(&g_a6_mod) != 0)
    fatal_error("Could not rewrite %s's SWP instructions (see debug.log).", A6_LIB);
  a6_hooks_install(&g_a6_mod); /* diagnostics only: the game runs without them */
  so_finalize(&g_a6_mod);
  debugPrintf("[a6] %s mapped at %p\n", A6_LIB, g_a6_mod.load_virtbase);
  return 0;
}

/* ---------------------------------------------------------------- focus */
void port_focus_lost(void) {
  if (f_pause_sounds && g_view_obj)
    f_pause_sounds(g_jni_env, g_view_obj);
  if (f_pause && g_view_obj)
    f_pause(g_jni_env, g_view_obj);
  dcr_audio_pause(1);
}

void port_focus_gained(void) {
  dcr_audio_pause(0);
  if (f_resume && g_view_obj)
    f_resume(g_jni_env, g_view_obj);
}

/* ---------------------------------------------------------------- run */
static void *need(const char *sym) {
  void *p = (void *)so_try_find_addr_rx(&g_a6_mod, sym);
  if (!p)
    fatal_error("%s has no %s: not the expected build?", A6_LIB, sym);
  return p;
}

static void *cls(const char *name) { return jni_class(name)->obj; }

#define J "Java_com_gameloft_android_HEP_GloftA6HP_"

void port_run(void) {
  jni_init();
  so_execute_init_array(&g_a6_mod);

  g_rend_cls = cls(A6_CLS_REND);
  g_view_obj = jni_singleton(A6_CLS_VIEW);
  void *rend_obj = jni_singleton(A6_CLS_REND);

  jni_onload_fn onload = (jni_onload_fn)need("JNI_OnLoad");
  debugPrintf("[a6] JNI_OnLoad -> 0x%x\n", onload(g_jni_vm, NULL));

  /* The engine keeps ONE JNIEnv in a global and never uses the env argument
   * of its natives: GameRenderer.nativeGetJNIEnv (instance) only stores it.
   * Every other native (GLResLoader.nativeInit first) reads that global, so it
   * must be set before any of them (crash at GLResLoader_nativeInit+0x1c,
   * a NULL env, when it was called later). */
  ((jni_void_fn)need(J "GameRenderer_nativeGetJNIEnv"))(g_jni_env, rend_obj);
  debugPrintf("[a6] GameRenderer.nativeGetJNIEnv done (the engine's env global)\n");

  ((jni_void_fn)need(J "GLResLoader_nativeInit"))(g_jni_env, cls(A6_CLS_RES));
  debugPrintf("[a6] GLResLoader.nativeInit done\n");
  ((jni_void_fn)need(J "GLUtils_Device_nativeInit"))(g_jni_env, cls(A6_CLS_DEVICE));
  debugPrintf("[a6] Device.nativeInit done\n");
  ((jni_void_fn)need(J "GLMediaPlayer_nativeInit"))(g_jni_env, cls(A6_CLS_MEDIA));
  debugPrintf("[a6] GLMediaPlayer.nativeInit done\n");
  ((jni_void_fn)need(J "GLGame_nativeInit"))(g_jni_env, cls(A6_CLS_GAME));
  debugPrintf("[a6] GLGame.nativeInit done\n");

  if (a6_gl_init() != 0)
    fatal_error("The graphics could not start (EGL). See debug.log.");

  int w = 1280, h = 720;
  dcr_window_size(&w, &h);

  /* GameRenderer.onSurfaceCreated / onSurfaceChanged */
  /* The third int is the language (GLGame.onStart: getLanguage() -> GLGame.i,
   * GameRenderer.onDrawFrame: nativeInit(w, h, GLGame.i)). 0 is "ID.bar",
   * the string IDs themselves ("BTN_career"): never pass 0. */
  int lang = a6_language();
  ((jni_iii_fn)need(J "GameRenderer_nativeInit"))(g_jni_env, g_rend_cls, w, h, lang);
  debugPrintf("[a6] GameRenderer.nativeInit(%d, %d, language %d) done\n", w, h, lang);
  ((jni_ii_fn)need(J "GameRenderer_nativeResize"))(g_jni_env, g_rend_cls, w, h);
  debugPrintf("[a6] nativeResize %dx%d\n", w, h);

  f_render = (jni_void_fn)need(J "GameRenderer_nativeRender");
  f_pause = (jni_void_fn)so_try_find_addr_rx(&g_a6_mod, J "GameGLSurfaceView_nativePause");
  f_resume = (jni_void_fn)so_try_find_addr_rx(&g_a6_mod, J "GameGLSurfaceView_nativeResume");
  f_pause_sounds = (jni_void_fn)so_try_find_addr_rx(&g_a6_mod, J "GameGLSurfaceView_nativePauseSounds");
  a6_input_init();

  debugPrintf("[a6] frame loop\n");
  uint64_t frame = 0;
  while (!rt_exit_requested() && appletMainLoop()) {
    rt_applet_poll();
    if (!rt_focused()) {
      svcSleepThread(50000000LL);
      continue;
    }
    dcr_boost_frame_begin();
    a6_input_poll();
    if (g_a6_dump_request) {
      /* No frame for 12 s: the watchdog (10 s) snapshots every thread and
       * logs where each one is -- the loader that never finishes included. */
      debugPrintf("[a6] hang dump requested (L+R+Minus): pausing 12 s for the watchdog\n");
      log_flush_ring();
      svcSleepThread(12000000000ll);
      g_a6_dump_request = 0;
      debugPrintf("[a6] hang dump done, resuming\n");
      log_flush_ring();
    }
    g_a6_swapped = 0;
    f_render(g_jni_env, g_rend_cls);
    a6_gl_swap(); /* GLSurfaceView swaps after every onDrawFrame */
    if (frame < 3)
      debugPrintf("[a6] frame %llu rendered%s\n", (unsigned long long)frame,
                  g_a6_swapped ? " (the engine swapped too)" : "");
    dcr_boost_frame_end(++frame);
    if (frame % 90 == 0) {
      a6_heap_report("every 90 frames");
      a6_hooks_report();
    }
  }
  rt_applet_stop();
  dcr_audio_close();
  debugPrintf("[a6] exit after %llu frames\n", (unsigned long long)frame);
  log_flush_ring();
}
