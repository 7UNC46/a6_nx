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

void a6_hook_SortCars(void *menu) {
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

void a6_hud_speed_settext(void *fx, void *ch, const char *txt, int b) {
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
  return 0;
}
