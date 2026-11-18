/* a6_config.c -- config.ini for Asphalt 6 (the runtime's INI engine, rt_cfg.c,
 * reads and writes it; this is the table). MIT. */
#include <switch.h>

#include "rt_cfg.h"
#include "util.h"

#include "a6.h"

A6Config g_a6_cfg = {.fps60 = 0, .steer_gain = 0.6f, .touch = 1, .swap_ab = 0, .motion_gain = 1.5f, .promo_cars = 1, .garage_look = 0.3f};

int g_a6_cooperative = 0;
int g_a6_dump_combo = 1;
int g_a6_trace_files = 0;
static int g_boost = 1;
static int g_lang_choice = 0; /* index into k_langs */

/* The engine's language numbers are its .bar files' order (libguest_asphalt6.so:
 * ID UK FR DE SP IT JP BR SC KR RU PT PL) and GLGame's table (classes.dex,
 * GLGame.<clinit>): 0 "ID" = the bare string IDs, 1 English, 2 French... */
#define A6_LANGS "auto,en,fr,de,es,it,ja,pt-br,zh,ko,ru,pt,pl"
static int g_fps_choice = 1; /* index into "60,30": the game was made for 30 */

static const CfgOpt k_opts[] = {
    CFG_ROW_RESOLUTION("auto",
                       "Rendering resolution: auto (1080 if docked when the game starts, 720 in\n"
                       "# handheld), 1080 or 720."),
    {"display", "frame_rate", "30",
     "60 or 30 frames a second. The game was made for 30 on 2010 phones.",
     CFG_CHOICE, "60,30", &g_fps_choice, 0, 0, 0, 0},
    {"controls", "steer_gain", "0.6",
     "How far the left stick tilts the 'phone' (0.2 - 1.5).",
     CFG_FLOAT, NULL, &g_a6_cfg.steer_gain, 0.2f, 1.5f, 0, 0},
    {"controls", "steering", "stick",
     "stick: the left stick steers. motion: tilt the Switch (handheld), the Pro\n"
     "# Controller or the Joy-Con grip like a steering wheel. both: either.",
     CFG_CHOICE, "stick,motion,both", &g_a6_cfg.steer_mode, 0, 0, 0, 0},
    {"controls", "motion_gain", "1.5",
     "Motion steering: how strongly a tilt steers (0.5 - 4).",
     CFG_FLOAT, NULL, &g_a6_cfg.motion_gain, 0.5f, 4.0f, 0, 0},
    {"controls", "motion_invert", "false",
     "Motion steering goes the wrong way: set this to true.",
     CFG_BOOL, NULL, &g_a6_cfg.motion_invert, 0, 0, 0, 0},
    {"controls", "garage_look_speed", "0.3",
     "Garage: how fast the right stick looks around (0.05 - 1).",
     CFG_FLOAT, NULL, &g_a6_cfg.garage_look, 0.05f, 1.0f, 0, 0},
    {"controls", "touch_screen", "true",
     "The touch screen works in handheld.",
     CFG_BOOL, NULL, &g_a6_cfg.touch, 0, 0, 0, 0},
    {"game", "language", "auto",
     "Game language: auto (the Switch's), en, fr, de, es, it, ja, pt-br, zh, ko,\n"
     "# ru, pt or pl.",
     CFG_CHOICE, A6_LANGS, &g_lang_choice, 0, 0, 0, 0},
    {"game", "promo_cars", "true",
     "Audi TT-RS and Ferrari California: the game keeps them locked for an online\n"
     "# promotion that no longer exists. true: they unlock by stars like the others.",
     CFG_BOOL, NULL, &g_a6_cfg.promo_cars, 0, 0, 0, 0},
    CFG_ROW_BOOST("CPU at 1785 MHz while the game loads (frames over 50 ms), normal otherwise.",
                  &g_boost),
    CFG_ROW_GL_SELFTEST(NULL),
    {"debug", "cooperative", "false",
     "false: the game's threads are time-sliced every 10 ms (the car selection\n"
     "# needs it). true: they take turns only when one waits (hangs there).",
     CFG_BOOL, NULL, &g_a6_cooperative, 0, 0, 0, 0},
    {"debug", "trace_files", "false",
     "Write every file the game opens to debug.log, with the result (slow;\n"
     "# for bug reports: which file the car selection cannot open).",
     CFG_BOOL, NULL, &g_a6_trace_files, 0, 0, 0, 0},
    {"debug", "hang_dump", "true",
     "Hold L + R + Minus for 2 s: the game pauses 12 s and debug.log gets\n"
     "# where every thread is (for bug reports of hangs).",
     CFG_BOOL, NULL, &g_a6_dump_combo, 0, 0, 0, 0},
    CFG_ROW_BOOT_LOG("Show the start-up log on screen.", NULL),
    CFG_ROW_LOG_JNI("Write every Java call the game makes to debug.log (slow; for bring-up).",
                    NULL),
};

static void apply(void) {
  g_a6_cfg.fps60 = g_fps_choice == 0;
  debugPrintf("[config] resolution %s, %d fps, steer %.2f, touch %s, boost %s\n",
              rt_config_get("display", "resolution"), g_a6_cfg.fps60 ? 60 : 30,
              (double)g_a6_cfg.steer_gain, g_a6_cfg.touch ? "on" : "off", g_boost ? "on" : "off");
  static const char *const k_steer[] = {"left stick", "motion", "stick and motion"};
  debugPrintf("[config] steering: %s (motion gain %.1f%s)\n", k_steer[g_a6_cfg.steer_mode % 3],
              (double)g_a6_cfg.motion_gain, g_a6_cfg.motion_invert ? ", inverted" : "");
}

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .version = 1,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }

/* The game's number for the chosen language; auto: the console's. */
int a6_language(void) {
  if (g_lang_choice > 0)
    return g_lang_choice; /* the choices are in the engine's order */
  int lang = 1;           /* English when the console's is not in the game */
  u64 code = 0;
  SetLanguage sl;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl))) {
      switch (sl) {
      case SetLanguage_FR: case SetLanguage_FRCA: lang = 2; break;
      case SetLanguage_DE: lang = 3; break;
      case SetLanguage_ES: case SetLanguage_ES419: lang = 4; break;
      case SetLanguage_IT: lang = 5; break;
      case SetLanguage_JA: lang = 6; break;
      case SetLanguage_PTBR: lang = 7; break;
      case SetLanguage_ZHCN: case SetLanguage_ZHHANS: lang = 8; break;
      case SetLanguage_KO: lang = 9; break;
      case SetLanguage_RU: lang = 10; break;
      case SetLanguage_PT: lang = 11; break;
      default: lang = 1; break;
      }
    }
    setExit();
  }
  debugPrintf("[config] language %d (console's)\n", lang);
  return lang;
}

/* dcr_path.c / bionic_io.c / bionic_stdio.c: which paths to log ([io] fopen
 * -> its FILE* or 0x0, [io] open -> fd, [io] stat). The runtime's default:
 * the APK only. */
#include <string.h>
int dcr_path_traced(const char *p) {
  if (!p)
    return 0;
  return g_a6_trace_files || strstr(p, ".apk") != NULL;
}
