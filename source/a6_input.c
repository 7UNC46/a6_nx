/* a6_input.c -- controllers and the touch screen -> Asphalt 6.
 *
 * On a phone Asphalt 6 steers by tilting (GLGame.nativeAccelerometer(x,y,z),
 * from the sensor), and everything else is the touch screen: accelerate is
 * automatic, brake / drift and nitro are touch zones, the menus are touch.
 * The few hardware keys (back, menu, d-pad on some phones) come in as
 * Android key codes (GLGame.nativeSetOnKeyDown/Up).
 *
 * Here (see menu_buttons / race_buttons below):
 *   left stick X  -> tilt (the accelerometer the way a phone held in
 *                    landscape reports it), scaled by steer_gain -- races
 *   buttons       -> the game's own gamepad layer (a6_hook.c) plus, for
 *                    brake and nitro, held touches on the HUD buttons
 *   touch screen  -> passed through (handheld)
 *
 * TODO (bring-up): the touch zones below are a guess for a 1280x720 screen;
 *   take them from the game's HUD once it runs (debug.log prints touches),
 *   and check the tilt axis / sign the engine wants. MIT.
 */
#include <math.h>
#include <switch.h>

#include "jni.h"
#include "rt_pad.h"
#include "rt_window.h"
#include "util.h"

#include "a6.h"

/* Android key codes */
#define AK_BACK        4
#define AK_DPAD_UP     19
#define AK_DPAD_DOWN   20
#define AK_DPAD_LEFT   21
#define AK_DPAD_RIGHT  22
#define AK_DPAD_CENTER 23
#define AK_MENU        82

typedef void (*key_fn)(void *env, void *cls, int key);
typedef void (*touch_fn)(void *env, void *obj, int x, int y, int id);
typedef void (*accel_fn)(void *env, void *obj, float x, float y, float z);

static key_fn f_down, f_up;
static touch_fn f_press, f_move, f_release;
static accel_fn f_accel;
static void *g_game_cls, *g_game_obj;
static PadState g_pad;
static int g_w = 1280, g_h = 720;
volatile int g_a6_dump_request;

#define J "Java_com_gameloft_android_HEP_GloftA6HP_GLGame_"

/* Virtual fingers for buttons: ids above any real touch (0..9). */
enum { VF_BRAKE = 10, VF_NITRO = 11 };
/* Zones, as fractions of the screen. GUESSES -- see the TODO above. */
static const float k_brake[2] = {0.10f, 0.85f};
static const float k_nitro[2] = {0.90f, 0.85f};

/* Motion steering (config steering = motion / both): the accelerometer of
 * the console (handheld), the Pro Controller or the left Joy-Con. Gravity
 * along the controller's X axis is how far it is turned like a wheel. */
static HidSixAxisSensorHandle g_sx_handheld, g_sx_full, g_sx_dual[2];
static int g_sx_ok;

static void motion_init(void) {
  if (g_a6_cfg.steer_mode == 0)
    return;
  int ok = 0;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_sx_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(g_sx_handheld)))
    ok |= 1;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_sx_full, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(g_sx_full)))
    ok |= 2;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(g_sx_dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(g_sx_dual[0])))
    ok |= 4;
  g_sx_ok = ok;
  debugPrintf("[input] motion sensors: handheld %s, Pro Controller %s, Joy-Con %s\n", ok & 1 ? "ok" : "no",
              ok & 2 ? "ok" : "no", ok & 4 ? "ok" : "no");
}

/* -1 .. 1 (0: none / level), from the controller in use. */
static float motion_steer(void) {
  u32 style = padGetStyleSet(&g_pad);
  HidSixAxisSensorHandle h;
  if ((style & HidNpadStyleTag_NpadFullKey) && (g_sx_ok & 2))
    h = g_sx_full;
  else if ((style & HidNpadStyleTag_NpadJoyDual) && (g_sx_ok & 4))
    h = g_sx_dual[0];
  else if (g_sx_ok & 1)
    h = g_sx_handheld;
  else
    return 0.0f;
  HidSixAxisSensorState st;
  if (!hidGetSixAxisSensorStates(h, &st, 1))
    return 0.0f;
  float x = st.acceleration.x * g_a6_cfg.motion_gain; /* in g */
  if (g_a6_cfg.motion_invert)
    x = -x;
  static int told;
  if (told < 3 && fabsf(st.acceleration.x) > 0.3f) {
    told++;
    debugPrintf("[input] motion: acceleration %.2f %.2f %.2f g (style 0x%x)\n", (double)st.acceleration.x,
                (double)st.acceleration.y, (double)st.acceleration.z, (unsigned)style);
  }
  return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}

void a6_input_init(void) {
  f_down = (key_fn)so_try_find_addr_rx(&g_a6_mod, J "nativeSetOnKeyDown");
  f_up = (key_fn)so_try_find_addr_rx(&g_a6_mod, J "nativeSetOnKeyUp");
  f_press = (touch_fn)so_try_find_addr_rx(&g_a6_mod, J "nativeTouchPressed");
  f_move = (touch_fn)so_try_find_addr_rx(&g_a6_mod, J "nativeTouchMoved");
  f_release = (touch_fn)so_try_find_addr_rx(&g_a6_mod, J "nativeTouchReleased");
  f_accel = (accel_fn)so_try_find_addr_rx(&g_a6_mod, J "nativeAccelerometer");
  g_game_cls = jni_class(A6_CLS_GAME)->obj;
  g_game_obj = jni_singleton(A6_CLS_GAME);
  dcr_window_size(&g_w, &g_h);
  rt_pad_setup(1, 1);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  motion_init();
  debugPrintf("[input] keys %s, touch %s, accel %s\n", f_down && f_up ? "ok" : "MISSING",
              f_press && f_move && f_release ? "ok" : "MISSING", f_accel ? "ok" : "MISSING");
}

static void key(int k, int down) {
  key_fn fn = down ? f_down : f_up;
  if (fn)
    fn(g_jni_env, g_game_cls, k);
}

static void vfinger(int id, const float *zone, int was, int now) {
  int x = (int)(zone[0] * g_w), y = (int)(zone[1] * g_h);
  if (now && !was && f_press)
    f_press(g_jni_env, g_game_obj, x, y, id);
  else if (!now && was && f_release)
    f_release(g_jni_env, g_game_obj, x, y, id);
}

static void poll_touch(void) {
  static HidTouchScreenState old;
  HidTouchScreenState st;
  if (!hidGetTouchScreenStates(&st, 1))
    return;
  if (!g_a6_cfg.touch)
    st.count = 0;
  /* The panel is 1280x720 whatever the rendering size. */
  float sx = g_w / 1280.0f, sy = g_h / 720.0f;
  for (int i = 0; i < st.count; i++) {
    int id = st.touches[i].finger_id, x = (int)(st.touches[i].x * sx), y = (int)(st.touches[i].y * sy), seen = 0;
    for (int j = 0; j < old.count; j++)
      seen |= old.touches[j].finger_id == (u32)id;
    touch_fn fn = seen ? f_move : f_press;
    static int told;
    if (!seen && told < 40) {
      told++;
      debugPrintf("[touch] finger %d down at %d,%d (%.2f, %.2f of the screen)%s\n", id, x, y, (double)x / g_w,
                  (double)y / g_h, a6_in_race() ? " in a race" : "");
    }
    if (fn)
      fn(g_jni_env, g_game_obj, x, y, id);
  }
  for (int j = 0; j < old.count; j++) {
    int gone = 1;
    for (int i = 0; i < st.count; i++)
      gone &= st.touches[i].finger_id != old.touches[j].finger_id;
    if (gone && f_release)
      f_release(g_jni_env, g_game_obj, (int)(old.touches[j].x * sx), (int)(old.touches[j].y * sy),
                (int)old.touches[j].finger_id);
  }
  old = st;
}

/* Gamepad bits of the game (a6_hook.c). */
#define GP_UP      0x1u
#define GP_DOWN    0x2u
#define GP_LEFT    0x4u
#define GP_RIGHT   0x8u
#define GP_CAMERA  0x40u
#define GP_NITRO   0x200u
#define GP_CONFIRM 0x8000u

/* Controls (user's layout, hardware 2026-11-18):
 *   menus: d-pad = move, A = confirm, B / - = back, + = menu
 *   race:  left stick = steer, R / ZR = gas, L / ZL / B = brake,
 *          A / X = nitro, Y = camera, + = pause, - = back
 * Buttons are only sent to the side they belong to: entering or leaving a
 * race releases whatever was held. */
static unsigned g_gp_held;  /* gamepad bits we hold down */
static int g_touch_brake, g_touch_nitro;

static void gp_set(unsigned want) {
  unsigned on = want & ~g_gp_held, off = g_gp_held & ~want;
  static int told;
  if ((on || off) && told < 40) {
    told++;
    debugPrintf("[input] gamepad bits: down 0x%x, up 0x%x\n", on, off);
  }
  if (off)
    a6_game_pad(0, off);
  if (on)
    a6_game_pad(1, on);
  g_gp_held = want;
}

static void race_buttons(u64 now, u64 down, u64 up) {
  const u64 gas = HidNpadButton_R | HidNpadButton_ZR;
  const u64 brake = HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_B;
  const u64 nitro = HidNpadButton_A | HidNpadButton_X;
  const u64 sticks = HidNpadButton_StickL | HidNpadButton_StickR;
  unsigned want = 0;
  if (now & gas)
    want |= GP_UP;
  if (now & brake)
    want |= GP_DOWN;
  if (now & nitro)
    want |= GP_NITRO;
  /* Easter egg: L3 + R3 together step the developers' free cameras (the
   * game's debug key; the step happens when they are let go) */
  if ((now & sticks) == sticks)
    want |= GP_CAMERA;
  gp_set(want);
  /* Y: the race's own camera button (3 views) */
  if (down & HidNpadButton_Y)
    a6_race_next_camera();
  /* brake and nitro also as held fingers on the HUD's buttons */
  int b = (now & brake) != 0, n = (now & nitro) != 0;
  vfinger(VF_BRAKE, k_brake, g_touch_brake, b);
  vfinger(VF_NITRO, k_nitro, g_touch_nitro, n);
  g_touch_brake = b;
  g_touch_nitro = n;
  /* + pauses (Android BACK in a race opens the pause menu), - is BACK too */
  const u64 back = HidNpadButton_Plus | HidNpadButton_Minus;
  if (down & back)
    key(AK_BACK, 1);
  if (up & back)
    key(AK_BACK, 0);
}

static void menu_buttons(u64 now, u64 down, u64 up, int paused) {
  unsigned want = 0;
  if (now & HidNpadButton_Up)
    want |= GP_UP;
  if (now & HidNpadButton_Down)
    want |= GP_DOWN;
  if (now & HidNpadButton_Left)
    want |= GP_LEFT;
  if (now & HidNpadButton_Right)
    want |= GP_RIGHT;
  if (now & HidNpadButton_A)
    want |= GP_CONFIRM;
  gp_set(want);
  static const struct { u64 btn; int key; } map[] = {
      {HidNpadButton_B, AK_BACK}, {HidNpadButton_Minus, AK_BACK}, {HidNpadButton_Plus, AK_MENU}};
  if (paused) { /* the pause menu: B, - and + all close it */
    const u64 back = HidNpadButton_B | HidNpadButton_Minus | HidNpadButton_Plus;
    if (down & back)
      key(AK_BACK, 1);
    if (up & back)
      key(AK_BACK, 0);
    return;
  }
  for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
    if (down & map[i].btn)
      key(map[i].key, 1);
    if (up & map[i].btn)
      key(map[i].key, 0);
  }
}

void a6_input_poll(void) {
  float sticks[4] = {0};
  static u64 old;
  static int was_race = -1;
  padUpdate(&g_pad); /* rt_pad_read only reads the state padUpdate took */
  u64 now = rt_pad_read(&g_pad, sticks);
  int race = a6_in_race();
  if (race && a6_race_paused())
    race = 2; /* the pause menu: menu buttons */
  if (race != was_race) {
    /* release everything held on the other side; buttons still held count
     * as new presses only once let go and pressed again */
    gp_set(0);
    vfinger(VF_BRAKE, k_brake, g_touch_brake, 0);
    vfinger(VF_NITRO, k_nitro, g_touch_nitro, 0);
    g_touch_brake = g_touch_nitro = 0;
    key(AK_BACK, 0); /* a + that paused is let go in the pause menu */
    key(AK_MENU, 0);
    if (was_race >= 0)
      debugPrintf("[input] %s\n", race == 1 ? "race: stick steers, R/ZR gas, L/ZL/B brake, A/X nitro, Y camera, + pause"
                                   : race == 2 ? "pause menu: d-pad moves, A confirms, B / + resume"
                                               : "menus: d-pad moves, A confirms, B back");
    was_race = race;
    old = now; /* no edges across the switch */
  }
  u64 down = now & ~old, up = old & ~now;
  if (race == 1)
    race_buttons(now, down, up);
  else
    menu_buttons(now, down, up, race == 2);

  /* steering: tilt. A landscape phone reports gravity on Y when tilted
   * left/right; 9.81 is a full tilt. Sign and axis to check on hardware. */
  if (f_accel) {
    float x = sticks[0];
    if (fabsf(x) < 0.12f)
      x = 0.0f;
    x *= g_a6_cfg.steer_gain;
    if (g_a6_cfg.steer_mode == 1)
      x = motion_steer();
    else if (g_a6_cfg.steer_mode == 2) {
      float m = motion_steer();
      x = fabsf(x) > fabsf(m) ? x : m;
    }
    f_accel(g_jni_env, g_game_obj, 0.0f, 9.81f * x, 9.81f);
  }

  /* L + R + Minus held 2 s: ask for a thread dump (a6_main.c). */
  extern int g_a6_dump_combo;
  static uint64_t combo_since;
  static int combo_fired;
  const u64 combo = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus;
  if (g_a6_dump_combo && (now & combo) == combo) {
    uint64_t t = armTicksToNs(armGetSystemTick());
    if (!combo_since)
      combo_since = t;
    else if (!combo_fired && t - combo_since > 2000000000ull) {
      g_a6_dump_request = 1;
      combo_fired = 1; /* once per hold */
    }
  } else {
    combo_since = 0;
    combo_fired = 0;
  }

  poll_touch();
  old = now;
}
