/* a6_hook.c -- diagnostic hooks inside libguest_asphalt6.so.
 *
 * Hardware 2026-11-17: starting a race with any car but the first one
 * crashed in BaseCarManager::GetPackFile (+0x2d1688): the file system was
 * asked for an EMPTY name (debug.log with trace_files: ".../files/Effects/"
 * and the other search folders, no file name), createAndOpenFile returned
 * NULL and the engine read through it. The name comes from
 * BaseCarManager::GetPackFilename(int) (+0x2d1548), which copies a
 * std::string out of a per-car table (STLport layout: the result's start at
 * +0x14, its end at +0x10; the table at manager+4, 516 bytes per entry, the
 * string's [end, start) at +0x10 / +0x14 of the entry).
 *
 * The hook logs each call (index, the name, the table entry's pointers) so
 * the bad index or the empty entry shows. Installed on the staged image
 * between so_resolve and so_finalize, like the SWP stubs (a6_swp.c):
 *
 *   GetPackFilename+0:  ldr pc, [pc, #-4]      -> a6_hook_GetPackFilename
 *   GetPackFilename+4:  .word hook
 *   trampoline (in .rel.dyn, past the SWP stubs, dead after relocation):
 *     push {r4-r8, lr}                         (the two displaced words; both
 *     add  r2, r2, r2, lsl #7                   are position independent)
 *     ldr  pc, [pc, #-4]
 *     .word GetPackFilename+8
 * MIT.
 */
#include <stdint.h>
#include <string.h>

#include "so_util.h"
#include "util.h"

#include "a6.h"

#define GPF_OFF        0x2d1548u
#define GPF_WORD0      0xe92d41f0u /* push {r4, r5, r6, r7, r8, lr} */
#define GPF_WORD1      0xe0822382u /* add r2, r2, r2, lsl #7 */
#define POOL_SKIP      0x1000u     /* past the SWP stubs (132 bytes) */
#define LDR_PC_MINUS4  0xe51ff004u

typedef void *(*gpf_fn)(void *out, void *mgr, int idx);
static gpf_fn g_gpf_orig;

static int readable(const void *p) {
  uintptr_t a = (uintptr_t)p;
  return a >= 0x1000 && (a & 3) == 0;
}

/* The fix (hardware 2026-11-17, debug.log: "GetPackFilename(manager .., index
 * -1) -> EMPTY", then the crash): RaceCarLoadingThread preloads the cars
 * next to the selected one, taking their numbers from a list at
 * (count + position +- 1) % count -- and a slot there holds -1 ("no car").
 * The engine never checks it. An index below 0 is answered with the last
 * valid car this manager was asked for (its pack exists; the preview of a
 * "no car" slot shows that car instead of crashing). */
static void *g_last_mgr[4];
static int g_last_idx[4];

static int remember_or_fix(void *mgr, int idx) {
  int slot = -1, free_slot = -1;
  for (int i = 0; i < 4; i++) {
    if (g_last_mgr[i] == mgr) {
      slot = i;
      break;
    }
    if (!g_last_mgr[i] && free_slot < 0)
      free_slot = i;
  }
  if (idx >= 0) {
    if (slot < 0)
      slot = free_slot >= 0 ? free_slot : 0;
    g_last_mgr[slot] = mgr;
    g_last_idx[slot] = idx;
    return idx;
  }
  int fixed = slot >= 0 ? g_last_idx[slot] : 0;
  static int told;
  if (told++ < 16)
    debugPrintf("[car] index %d (no car) asked of manager %p: using car %d instead\n", idx, mgr, fixed);
  return fixed;
}

void *a6_hook_GetPackFilename(void *out, void *mgr, int idx) {
  idx = remember_or_fix(mgr, idx);
  void *r = g_gpf_orig(out, mgr, idx);
  const char *s = *(const char *const *)((const uint8_t *)out + 0x14);
  const char *e = *(const char *const *)((const uint8_t *)out + 0x10);
  int empty = !s || s == e || !*s;
  static int logged;
  if (logged < 60 || empty) {
    logged++;
    const uint8_t *table = readable(mgr) ? *(const uint8_t *const *)((const uint8_t *)mgr + 4) : NULL;
    const uint8_t *entry = table ? table + (uint32_t)idx * 516u : NULL;
    debugPrintf("[car] GetPackFilename(manager %p, index %d) -> \"%s\"%s; table %p, entry end %p start %p\n",
                mgr, idx, s && !empty ? s : "", empty ? " EMPTY" : "", table,
                entry ? *(void *const *)(entry + 0x10) : NULL, entry ? *(void *const *)(entry + 0x14) : NULL);
    if (empty)
      log_flush_ring();
  }
  return r;
}

/* The real fix (hardware 2026-11-17, second run: with the index swapped in
 * GetPackFilename the RaceCar was still built for car -1 and crashed in
 * RaceCar::CarCustomizeDecalLayer / CreateDecalTextureEx): skip the preload
 * of a slot whose car is below 0. RaceCarLoadingThread::operator() builds a
 * car at two places, each starting "movw r0, #0x2c04; bl CustomAlloc"
 * (sizeof(RaceCar)) once the slot is known not to be loaded yet:
 *   +0x259c40  the selected car itself, slot in r9, "already loaded" -> +0x259d70
 *   +0x259e34  its neighbours,         slot in r6, "already loaded" -> +0x259f6c
 * The movw becomes a branch to a check: car = _sortedList[slot]; below 0 ->
 * continue as if that slot were loaded (nothing built), else the movw and
 * back. r0 and r3 are free there (the call clobbers them; r3 is reloaded). */
#define RCL_SORTED_LIST 0x8698a0u /* RaceCarLoadingThread::_sortedList (int *) */
#define RCL_MOVW        0xe3020c04u
static volatile uint32_t g_rcl_skipped;
static uint32_t g_rcl_told;

static const struct {
  uint32_t site, skip, reg;
} k_rcl[2] = {
    {0x259c40u, 0x259d70u, 9},
    {0x259e34u, 0x259f6cu, 6},
};

void a6_hooks_report(void) {
  uint32_t n = g_rcl_skipped;
  if (n != g_rcl_told) {
    debugPrintf("[car] preload: %u slot(s) without a car skipped so far\n", (unsigned)n);
    g_rcl_told = n;
  }
}

static void install_rcl(so_module *m, uint32_t pool_off) {
  uint8_t *img = (uint8_t *)m->load_base;
  uintptr_t vbase = (uintptr_t)m->load_virtbase;
  for (int i = 0; i < 2; i++) {
    uint32_t *site = (uint32_t *)(img + k_rcl[i].site);
    if (*site != RCL_MOVW) {
      debugPrintf("[hook] car preload site +0x%x: unexpected code %08x: not patched\n", (unsigned)k_rcl[i].site,
                  (unsigned)*site);
      continue;
    }
    uint32_t off = pool_off + (uint32_t)i * 0x50u;
    uint32_t *t = (uint32_t *)(img + off);
    static const uint32_t code[18] = {
        0xe59f3038u, 0xe5933000u, 0xe3530000u, 0x0a000002u, 0xe7933100u, 0xe3530000u,
        0xba000002u, 0xe3020c04u, 0xe51ff004u, 0,           0xe59f3014u, 0xe5930000u,
        0xe2800001u, 0xe5830000u, 0xe51ff004u, 0,           0,           0,
    };
    memcpy(t, code, sizeof code);
    t[4] |= k_rcl[i].reg;                             /* ldr r3, [r3, rS, lsl #2] */
    t[9] = (uint32_t)(vbase + k_rcl[i].site + 4);     /* back: the bl CustomAlloc */
    t[15] = (uint32_t)(vbase + k_rcl[i].skip);        /* "already loaded" */
    t[16] = (uint32_t)(vbase + RCL_SORTED_LIST);
    t[17] = (uint32_t)(uintptr_t)&g_rcl_skipped;
    int32_t d = ((int32_t)off - (int32_t)(k_rcl[i].site + 8)) >> 2;
    *site = 0xea000000u | ((uint32_t)d & 0x00ffffffu); /* b tramp */
    debugPrintf("[hook] car preload +0x%x: slots without a car are skipped (check at +0x%x)\n",
                (unsigned)k_rcl[i].site, (unsigned)off);
  }
}

/* The cause (hardware 2026-11-17, third run: the third car in the race's
 * car selection crashed in GS_MenuMain::SelectCar -> GetCarString(-1)):
 * GS_MenuMain::SortCars (+0x257eec) fills the menu's car list (menu+0x30,
 * CustomAlloc(4 * GetCarCount()) = 42 ints, NOT initialised) with
 * GetCarIdxFromId(id) for every id of EventManager::GetUnlockList -- which
 * holds 44 ids: two ids that no car has (-1 in the list) and two cars past
 * the end of the 42 slots (the known 8-byte heap overrun, cars 13 and 9).
 * NextCar/PreviousCar walk all 42 slots, so the selection reaches the -1s.
 * After the original runs, the list is rebuilt: its valid entries in order
 * (with the two written past the end, still in our heap guard's tail),
 * duplicates and -1 dropped, every car still missing appended; the selected
 * position follows its car. */
#define SORT_OFF       0x257eecu
#define SORT_WORD0     0xe92d4070u /* push {r4, r5, r6, lr} */
#define SORT_WORD1     0xe24dd010u /* sub sp, sp, #16 */
#define GAME_GETCARMGR 0x24cca4u
#define BCM_GETCOUNT   0x2d1484u
#define HEAP_TAIL_FILL 0xa6u

typedef void (*sort_fn)(void *menu);
typedef void *(*getmgr_fn)(void);
typedef int (*count_fn)(void *mgr);
static sort_fn g_sort_orig;
static getmgr_fn g_getmgr;
static count_fn g_count;

/* Cars unlocked by stars (hardware 2026-11-18: Audi TT-RS still locked,
 * "40 stars", with 59): the game unlocks them only in
 * EventManager::ProcessEventUnlockable, at the end of a race -- every car in
 * state 0 whose GetCarInfo(car, 57) stars the career total reaches becomes
 * state 2. The same rule, each time the car selection opens. */
#define CAREER_MGR     0x24c7b0u /* Game::GetCareerMgr() */
#define CAREER_STARS   0x2d3900u /* CareerManager::GetStarTotal() */
#define BCM_GETINFO    0x2d1454u /* BaseCarManager::GetCarInfo(car, field) */
#define BCM_GETSTATE   0x2d14c0u
#define BCM_SETSTATE   0x2d14acu
#define CARINFO_STARS  57
typedef void *(*getcareer_fn)(void);
typedef int (*stars_fn)(void *career);
typedef int (*carinfo_fn)(void *mgr, int car, int field);
typedef int (*getstate_fn)(void *mgr, int car);
typedef void (*setstate_fn)(void *mgr, int car, int state);

void a6_unlock_cars_by_stars(void) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  void *career = ((getcareer_fn)(vb + CAREER_MGR))();
  void *mgr = g_getmgr ? g_getmgr() : NULL;
  if (!career || !mgr)
    return;
  int stars = ((stars_fn)(vb + CAREER_STARS))(career);
  int n = g_count(mgr);
  for (int i = 0; i < n && i < 512; i++) {
    if (((getstate_fn)(vb + BCM_GETSTATE))(mgr, i) != 0)
      continue;
    int need = ((carinfo_fn)(vb + BCM_GETINFO))(mgr, i, CARINFO_STARS);
    if (stars >= need) {
      ((setstate_fn)(vb + BCM_SETSTATE))(mgr, i, 2);
      debugPrintf("[car] car %d unlocked: %d stars, it needs %d\n", i, stars, need);
    }
  }
}

void a6_hook_SortCars(void *menu) {
  a6_unlock_cars_by_stars();
  g_sort_orig(menu);
  int n = g_count(g_getmgr());
  int **plist = (int **)((uint8_t *)menu + 0x30);
  int *pos = (int *)((uint8_t *)menu + 0x34);
  int *list = *plist;
  if (!list || n <= 0 || n > 512)
    return;
  int sel = (*pos >= 0 && *pos < n) ? list[*pos] : -1;
  int out[512], have[512];
  memset(have, 0, sizeof have);
  int k = 0, dropped = 0, past = 0;
  for (int i = 0; i < n + 32; i++) {
    int v = list[i];
    if (i >= n) { /* written past the end: only in our guard's tail */
      if (v < 0 || v >= n)
        break;
      past++;
      memset(&list[i], HEAP_TAIL_FILL, sizeof(int)); /* the guard as it was */
    }
    if (v < 0 || v >= n || have[v]) {
      dropped++;
      continue;
    }
    have[v] = 1;
    out[k++] = v;
  }
  int added = 0;
  for (int v = 0; v < n; v++)
    if (!have[v]) {
      out[k++] = v;
      added++;
    }
  memcpy(list, out, (size_t)n * sizeof(int));
  int np = 0;
  for (int i = 0; i < n; i++)
    if (list[i] == sel) {
      np = i;
      break;
    }
  if (dropped || past || added || np != *pos)
    debugPrintf("[car] car list rebuilt: %d cars, %d past the end recovered, %d empty/double dropped, %d added; "
                "selected car %d at position %d (was %d)\n",
                n, past, dropped, added, sel, np, *pos);
  *pos = np;
}

static void install_sort(so_module *m, uint32_t tramp_off) {
  uint8_t *img = (uint8_t *)m->load_base;
  uintptr_t vbase = (uintptr_t)m->load_virtbase;
  uint32_t *site = (uint32_t *)(img + SORT_OFF);
  if (site[0] != SORT_WORD0 || site[1] != SORT_WORD1) {
    debugPrintf("[hook] SortCars: unexpected code (%08x %08x): not hooked\n", (unsigned)site[0], (unsigned)site[1]);
    return;
  }
  uint32_t *t = (uint32_t *)(img + tramp_off);
  t[0] = SORT_WORD0;
  t[1] = SORT_WORD1;
  t[2] = LDR_PC_MINUS4;
  t[3] = (uint32_t)(vbase + SORT_OFF + 8);
  site[0] = LDR_PC_MINUS4;
  site[1] = (uint32_t)(uintptr_t)&a6_hook_SortCars;
  g_getmgr = (getmgr_fn)(vbase + GAME_GETCARMGR);
  g_count = (count_fn)(vbase + BCM_GETCOUNT);
  g_sort_orig = (sort_fn)(vbase + tramp_off);
  debugPrintf("[hook] SortCars hooked: the car list is checked (trampoline at +0x%x)\n", (unsigned)tramp_off);
}

/* ---- the game's gamepad input (controls, hardware 2026-11-18) ----------
 * Asphalt 6 has a gamepad layer (the dev / PC build's keyboard): Game::
 * s_pInstance + 192 is a GamePadManager; GamePadEvt(mgr, down, bits, pad)
 * queues bits, ProcessGamePads (each frame) turns them into pressed / held /
 * released. Bits (from Application::onEvent's key table and their users):
 *   1 up / accelerate   2 down / brake   4 left   8 right
 *   0x40 camera (GP_RaceNormal, on release)   0x200 nitro (held)
 *   0x8000 confirm in the menus (Enter)
 * The Android build never feeds it (notifyKeyPressed only knows BACK/MENU);
 * a6_input.c does. */
#define GAME_S_INSTANCE  0x868688u
#define GPM_EVT          0x2ceea0u
#define GAME_CURSTATE    0x24d89cu
#define VT_GS_RACE       (0x82c6a0u + 8u)

typedef void (*gpevt_fn)(void *mgr, int down, int bits, int pad);
typedef void *(*curstate_fn)(void);

void a6_game_pad(int down, unsigned bits) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  uint8_t *game = *(uint8_t **)(vb + GAME_S_INSTANCE);
  if (!game || !bits)
    return;
  ((gpevt_fn)(vb + GPM_EVT))(game + 192, down, (int)bits, 0);
}

int a6_in_race(void) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  if (!*(void **)(vb + GAME_S_INSTANCE))
    return 0;
  void *st = ((curstate_fn)(vb + GAME_CURSTATE))();
  return st && *(uintptr_t *)st == vb + VT_GS_RACE;
}

/* Paused: GP_RaceNormal hands the d-pad / confirm bits to the pause menu
 * when the race state's +0x2c is set (its ExecuteGamePad, +0x39c750). */
int a6_race_paused(void) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  if (!*(void **)(vb + GAME_S_INSTANCE))
    return 0;
  uint8_t *st = ((curstate_fn)(vb + GAME_CURSTATE))();
  return st && *(uintptr_t *)st == vb + VT_GS_RACE && *(uint32_t *)(st + 0x2c) != 0;
}

/* The garage (GS_MenuMain's gameplay part, +0x18, is a GP_Garage): it
 * moves its camera on the gamepad's direction bits (GP_Garage::ExecuteGamePad:
 * press 1..8 -> Camera::SetMoveFactor, release -> stop). */
#define VT_GP_GARAGE (0x82c890u + 8u)
int a6_in_garage(void) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  if (!*(void **)(vb + GAME_S_INSTANCE))
    return 0;
  uint8_t *st = ((curstate_fn)(vb + GAME_CURSTATE))();
  if (!st)
    return 0;
  uintptr_t *gp = *(uintptr_t **)(st + 0x18);
  return gp && *gp == vb + VT_GP_GARAGE;
}

/* The garage's own on-screen sticks (its Flash HUD -> GP_Garage::
 * ExecuteSetGameData): walk = Camera::SetMoveFactor(x, y) (-1..1, up is
 * y -1), look = Camera::SetCursorMove(dx, dy) (ints, x / 320 and y / 480:
 * -1..1). The Switch's sticks drive the same two, without any touch -- a
 * touch near a car is taken as choosing it. */
#define GAME_GETCAMERA  0x24a694u
#define CAM_SETMOVE     0x281af4u
#define CAM_SETCURSOR   0x281b50u
typedef void *(*getcam_fn)(void);
typedef void (*cammove_fn)(void *cam, float x, float y);
typedef void (*camcursor_fn)(void *cam, int x, int y);

void a6_garage_sticks(float wx, float wy, float lx, float ly, int walk_owned) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  void *cam = ((getcam_fn)(vb + GAME_GETCAMERA))();
  if (!cam)
    return;
  static int walking;
  if (!walk_owned) { /* the d-pad's bits walk: leave the factor to them */
    if (wx != 0.0f || wy != 0.0f || walking)
      ((cammove_fn)(vb + CAM_SETMOVE))(cam, wx, -wy);
    walking = wx != 0.0f || wy != 0.0f;
  }
  ((camcursor_fn)(vb + CAM_SETCURSOR))(cam, (int)(lx * 320.0f), (int)(-ly * 480.0f));
}

/* The race's own camera button (the HUD's, GS_Race::StateOnFlashEvent
 * +0x268b78): sMenuData::aGameplayInfoData[+0x28] (GP_RaceNormal) +0x49 = 1;
 * GP_RaceNormal::UpdateCameraMode then steps through the 3 cameras. */
#define GAMEPLAY_INFO 0x87e8dcu
void a6_race_next_camera(void) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  uint8_t *gp = *(uint8_t **)(vb + GAMEPLAY_INFO + 0x28);
  if (gp)
    gp[0x49] = 1;
}

/* ---- the "999 km/h" (diagnostic) ---------------------------------------
 * GS_Race::RaceUpdateHud writes the speed with FormatLength and
 * RenderFX::SetText(fx, hud.container.speed_value.txt, text) at +0x26d820,
 * only when the shown number changes. That call goes through a stub that
 * logs what is written and to which character (NULL: the text field was
 * never found, the SWF's own "999" stays). */
#define HUD_SPEED_CALL  0x26d820u
#define RFX_SETTEXT     0x462eb0u
typedef void (*settext_fn)(void *fx, void *ch, const char *txt, int b);
static settext_fn g_settext;
static void write_car_speed(const char *txt);

/* The "999 km/h" (hud.swf, hardware 2026-11-18): hud.container.cash_value
 * is a copy of the speed text clip (its placeholder "999 km/h") placed over
 * the position counter, for a cash race mode this build never uses -- no
 * code of the game names it, so nothing hides it. Hidden here, on the
 * RenderFX the speed is written to. */
#define RFX_SETVISIBLE 0x4666d4u /* RenderFX::SetVisible(char const*, bool) */
typedef void (*setvisible_fn)(void *fx, const char *path, int visible);

/* The gap to the cars ahead and behind ("4th -0:00.91", hud.mc_car_ahead /
 * hud.mc_car_behind): hud.swf shows it in every race; wanted only in duels
 * (race type 3, Game +0x1d4: Checkpoint::SceneObjDoResult picks the duel
 * rules on it). Hidden on each speed update otherwise. */
#define GAME_RACE_TYPE 0x1d4
#define RACE_DUEL      3
static void hide_gaps(void *fx) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  uint8_t *game = *(uint8_t **)(vb + GAME_S_INSTANCE);
  if (!game || *(int *)(game + GAME_RACE_TYPE) == RACE_DUEL)
    return;
  ((setvisible_fn)(vb + RFX_SETVISIBLE))(fx, "hud.mc_car_ahead", 0);
  ((setvisible_fn)(vb + RFX_SETVISIBLE))(fx, "hud.mc_car_behind", 0);
}

static void hide_cash_value(void *fx) {
  static void *done_fx;
  static int calls;
  hide_gaps(fx);
  if (fx == done_fx && (++calls & 63)) /* again now and then: the HUD may reset it */
    return;
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  ((setvisible_fn)(vb + RFX_SETVISIBLE))(fx, "hud.container.cash_value", 0);
  if (fx != done_fx)
    debugPrintf("[hud] hud.container.cash_value (the \"999 km/h\") hidden\n");
  done_fx = fx;
}

void a6_hud_speed_settext(void *fx, void *ch, const char *txt, int b) {
  if (fx)
    hide_cash_value(fx);
  static int n;
  static void *last_ch = (void *)1;
  if (n < 12 || ch != last_ch) {
    debugPrintf("[hud] speed text \"%s\" -> character %p (RenderFX %p)\n", txt ? txt : "(null)", ch, fx);
    n++;
    last_ch = ch;
  }
  g_settext(fx, ch, txt, b);
  write_car_speed(txt);
}

/* hud_carSpeed (FlashDB label 16, "999" until written): GS_Race only
 * SetText()s hud.container.speed_value.txt; the label the HUD's ActionScript
 * shows stays at FlashDBResetHud's -1 (the "999 km/h" over the position).
 * Written here with the same number, each time the speed text changes. */
#define GAME_GETFLASHDB 0x24ae6cu
#define FDB_WRITE_DBL   0x2d98a0u /* BaseFlashDataBase::WriteToDB<double>(label, double) */
#define LABEL_CARSPEED  16
typedef void *(*getfdb_fn)(void);
typedef void (*wrdbl_fn)(void *db, int label, double v);

static void write_car_speed(const char *txt) {
  if (!txt)
    return;
  int v = 0, any = 0;
  for (const char *p = txt; *p >= '0' && *p <= '9'; p++, any = 1)
    v = v * 10 + (*p - '0');
  if (!any)
    return;
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  uint8_t *fdb = ((getfdb_fn)(vb + GAME_GETFLASHDB))();
  if (fdb)
    ((wrdbl_fn)(vb + FDB_WRITE_DBL))(fdb + 4, LABEL_CARSPEED, (double)v);
}

static void install_hud(so_module *m, uint32_t stub_off) {
  uint8_t *img = (uint8_t *)m->load_base;
  uintptr_t vbase = (uintptr_t)m->load_virtbase;
  uint32_t *site = (uint32_t *)(img + HUD_SPEED_CALL);
  int32_t want = ((int32_t)RFX_SETTEXT - (int32_t)(HUD_SPEED_CALL + 8)) >> 2;
  if (*site != (0xeb000000u | ((uint32_t)want & 0xffffffu))) {
    debugPrintf("[hook] HUD speed call: unexpected code %08x: not hooked\n", (unsigned)*site);
    return;
  }
  uint32_t *t = (uint32_t *)(img + stub_off);
  t[0] = LDR_PC_MINUS4;
  t[1] = (uint32_t)(uintptr_t)&a6_hud_speed_settext;
  g_settext = (settext_fn)(vbase + RFX_SETTEXT);
  int32_t d = ((int32_t)stub_off - (int32_t)(HUD_SPEED_CALL + 8)) >> 2;
  *site = 0xeb000000u | ((uint32_t)d & 0xffffffu); /* bl stub (lr = back) */
  debugPrintf("[hook] HUD speed text is logged (stub at +0x%x)\n", (unsigned)stub_off);
}

/* NativeIsGamepadOpen (the menus' ActionScript asks it; Xperia Play's slide)
 * answers false: "mov r1, #0" -> "mov r1, #1". */
#define IS_GP_OPEN_MOV 0x253cb4u
/* Game::OnGamePadAction(action, pad): logged (does the game get our bits?) */
#define ONGPA_OFF   0x2502ccu
#define ONGPA_WORD0 0xe92d4070u /* push {r4, r5, r6, lr} */
#define ONGPA_WORD1 0xe1a05001u /* mov r5, r1 */
typedef void (*ongpa_fn)(void *game, int action, int pad);
static ongpa_fn g_ongpa_orig;

void a6_hook_OnGamePadAction(void *game, int action, int pad) {
  static int n;
  if (n < 40) {
    n++;
    const uint32_t *w = (const uint32_t *)((uint8_t *)game + 192 + 4);
    debugPrintf("[pad] game gets action %d (pad %d): pressed 0x%x held 0x%x released 0x%x\n", action, pad,
                (unsigned)w[0], (unsigned)w[1], (unsigned)w[2]);
  }
  g_ongpa_orig(game, action, pad);
}

static void install_pad(so_module *m, uint32_t tramp_off) {
  uint8_t *img = (uint8_t *)m->load_base;
  uintptr_t vbase = (uintptr_t)m->load_virtbase;
  uint32_t *mov = (uint32_t *)(img + IS_GP_OPEN_MOV);
  if (*mov == 0xe3a01000u) {
    *mov = 0xe3a01001u;
    debugPrintf("[hook] NativeIsGamepadOpen answers true\n");
  }
  uint32_t *site = (uint32_t *)(img + ONGPA_OFF);
  if (site[0] != ONGPA_WORD0 || site[1] != ONGPA_WORD1) {
    debugPrintf("[hook] OnGamePadAction: unexpected code: not hooked\n");
    return;
  }
  uint32_t *t = (uint32_t *)(img + tramp_off);
  t[0] = ONGPA_WORD0;
  t[1] = ONGPA_WORD1;
  t[2] = LDR_PC_MINUS4;
  t[3] = (uint32_t)(vbase + ONGPA_OFF + 8);
  site[0] = LDR_PC_MINUS4;
  site[1] = (uint32_t)(uintptr_t)&a6_hook_OnGamePadAction;
  g_ongpa_orig = (ongpa_fn)(vbase + tramp_off);
  debugPrintf("[hook] Game::OnGamePadAction logged (trampoline at +0x%x)\n", (unsigned)tramp_off);
}

/* ---- the pause menu's Music Player ---------------------------------------
 * A leftover of the iPhone version (its iPod library): on Android every
 * SoundManager::iPod_* is an empty stub with 30 placeholder songs ("Song
 * 01".."Song 30"), so choosing one plays nothing. Here the player plays the
 * game's own race soundtrack: race music is sound 590 + 0..11 (the 12 tracks
 * SetMusicPlaying picks from at random), so "Song NN" is track NN. */
#define SM_SET_TRACK   0x332ae0u /* SoundManager::SetMusicTrack(int) */
#define SM_SET_PLAYING 0x3327e4u /* SoundManager::SetMusicPlaying(bool) */
#define RACE_MUSIC     590
#define RACE_TRACKS    12
#define IPOD_PLAY      0x32ed64u
#define IPOD_PAUSE     0x32ed68u
#define IPOD_STOP      0x32ed6cu
#define IPOD_COUNT     0x32eda4u /* mov r0, #30 */
#define IPOD_SETSONG   0x32edacu
#define IPOD_STATE     0x32edccu /* mov r0, #0: stopped */
typedef void (*smtrack_fn)(void *sm, int id);
typedef void (*smplay_fn)(void *sm, int on);
static int g_song, g_song_state; /* MPMusicPlaybackState: 0 stopped, 1 playing, 2 paused */

static void song_start(void *sm) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  ((smtrack_fn)(vb + SM_SET_TRACK))(sm, RACE_MUSIC + g_song % RACE_TRACKS);
  if (!((uint8_t *)sm)[0xc]) /* music was off: the player asks for it */
    ((smplay_fn)(vb + SM_SET_PLAYING))(sm, 1);
  g_song_state = 1;
  debugPrintf("[music] player: song %d (sound %d)\n", g_song + 1, RACE_MUSIC + g_song % RACE_TRACKS);
}
void a6_ipod_set_song(void *sm, int i) {
  g_song = i < 0 ? 0 : i;
  song_start(sm);
}
void a6_ipod_play(void *sm) { song_start(sm); }
void a6_ipod_pause(void *sm) {
  uintptr_t vb = (uintptr_t)g_a6_mod.load_virtbase;
  ((smplay_fn)(vb + SM_SET_PLAYING))(sm, 0);
  g_song_state = 2;
}
void a6_ipod_stop(void *sm) { (void)sm; g_song_state = 0; } /* the next SetSong takes over */
int a6_ipod_state(void *sm) { (void)sm; return g_song_state; }

/* GS_MenuMain::OnLoad3DScene, after SortCars, in the full game
 * (Game::mIsDemo == 0): SetCarState(18 Ferrari California, 0) and
 * SetCarState(6 Audi TT-RS, 0) -- locked again each time the menu loads,
 * whatever the stars (a promotion's cars). config promo_cars: those two
 * calls become nops, so the stars rule unlocks them like the others. */
#define RELOCK_CALIFORNIA 0x25e1a4u
#define RELOCK_TTRS       0x25e1b4u
static void install_promo(so_module *m) {
  if (!g_a6_cfg.promo_cars)
    return;
  uint8_t *img = (uint8_t *)m->load_base;
  const uint32_t at[2] = {RELOCK_CALIFORNIA, RELOCK_TTRS}, want[2] = {0xeb01ccc0u, 0xeb01ccbcu};
  for (int i = 0; i < 2; i++)
    if (*(uint32_t *)(img + at[i]) != want[i]) {
      debugPrintf("[hook] promo cars: unexpected code at +0x%x: not patched\n", (unsigned)at[i]);
      return;
    }
  for (int i = 0; i < 2; i++)
    *(uint32_t *)(img + at[i]) = 0xe320f000u; /* nop */
  debugPrintf("[hook] promo cars: Audi TT-RS and Ferrari California unlock by stars\n");
}

static void install_music(so_module *m, uint32_t pool_off) {
  uint8_t *img = (uint8_t *)m->load_base;
  static const struct { uint32_t off, word; void *fn; } k[] = {
      {IPOD_PLAY, 0xe12fff1eu, (void *)a6_ipod_play},       {IPOD_PAUSE, 0xe12fff1eu, (void *)a6_ipod_pause},
      {IPOD_STOP, 0xe12fff1eu, (void *)a6_ipod_stop},       {IPOD_SETSONG, 0xe12fff1eu, (void *)a6_ipod_set_song},
      {IPOD_STATE, 0xe3a00000u, (void *)a6_ipod_state},
  };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (*(uint32_t *)(img + k[i].off) != k[i].word) {
      debugPrintf("[hook] music player: unexpected code at +0x%x: not hooked\n", (unsigned)k[i].off);
      return;
    }
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
    uint32_t stub = pool_off + i * 8;
    uint32_t *t = (uint32_t *)(img + stub);
    t[0] = LDR_PC_MINUS4;
    t[1] = (uint32_t)(uintptr_t)k[i].fn;
    int32_t d = ((int32_t)stub - (int32_t)(k[i].off + 8)) >> 2;
    *(uint32_t *)(img + k[i].off) = 0xea000000u | ((uint32_t)d & 0xffffffu); /* b stub */
  }
  uint32_t *cnt = (uint32_t *)(img + IPOD_COUNT);
  if (*cnt == 0xe3a0001eu)
    *cnt = 0xe3a00000u | RACE_TRACKS; /* mov r0, #12 */
  debugPrintf("[hook] music player: plays the %d race tracks\n", RACE_TRACKS);
}

static const Elf32_Shdr *section(so_module *m, const char *name) {
  for (int i = 0; i < m->elf_hdr->e_shnum; i++)
    if (!strcmp(m->shstrtab + m->sec_hdr[i].sh_name, name))
      return &m->sec_hdr[i];
  return NULL;
}

int a6_hooks_install(so_module *m) {
  if (m->state != SO_STAGED)
    return -1;
  const Elf32_Shdr *rel = section(m, ".rel.dyn");
  if (!rel || rel->sh_size < POOL_SKIP + 0x200)
    return -1;
  uint8_t *img = (uint8_t *)m->load_base;
  uint32_t *site = (uint32_t *)(img + GPF_OFF);
  if (site[0] != GPF_WORD0 || site[1] != GPF_WORD1) {
    debugPrintf("[hook] GetPackFilename: unexpected code (%08x %08x): not hooked\n", (unsigned)site[0],
                (unsigned)site[1]);
    return -1;
  }
  uintptr_t vbase = (uintptr_t)m->load_virtbase;
  uint32_t tramp_off = rel->sh_addr + POOL_SKIP;
  uint32_t *t = (uint32_t *)(img + tramp_off);
  t[0] = GPF_WORD0;
  t[1] = GPF_WORD1;
  t[2] = LDR_PC_MINUS4;
  t[3] = (uint32_t)(vbase + GPF_OFF + 8);
  site[0] = LDR_PC_MINUS4;
  site[1] = (uint32_t)(uintptr_t)&a6_hook_GetPackFilename;
  g_gpf_orig = (gpf_fn)(vbase + tramp_off); /* ARM code: bit 0 clear */
  debugPrintf("[hook] GetPackFilename hooked (trampoline at +0x%x)\n", (unsigned)tramp_off);
  install_rcl(m, tramp_off + 0x20);  /* 2 x 0x50 bytes */
  install_sort(m, tramp_off + 0xd0);
  install_hud(m, tramp_off + 0xf0);   /* 8 bytes */
  install_pad(m, tramp_off + 0x100);  /* 16 bytes */
  install_music(m, tramp_off + 0x110); /* 5 x 8 bytes */
  install_promo(m);
  return 0;
}
