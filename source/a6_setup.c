/* a6_setup.c -- the game data out of the APK, once.
 *
 * The repack carries Asphalt 6's data (the .bar archives, the texture zips,
 * the OGG sounds...) as APK assets under assets/gamedata/, listed in
 * assets/gamedata.lst; on a phone its DataInstallActivity copies them to the
 * app's external files folder on the first start. Here the same files go to
 * <game folder>/external/files/, which the runtime's path translation makes
 * /sdcard/Android/data/<pkg>/files/ for the game (GLGame.getSDFolder).
 *
 * Done again only when the APK changes (its size is the stamp).
 * TODO: compare with the repack's DataInstallActivity (classes2.dex,
 * com/a6port/DataInstallActivity) if it renames or skips files. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "port_config.h"
#include "rt_boot.h"
#include "util.h"

#include "a6.h"

#define STAMP_KEY "gamedata"

void a6_setup_data(RtSetupCtx *ctx) {
  long apk_size = rt_file_size(ctx->apk_path);
  unsigned long stamp = 0, count = 0;
  if (rt_setup_stamp_get(STAMP_KEY, &stamp, &count) && stamp == (unsigned long)apk_size && count > 0)
    return; /* done, and the APK has not changed */

  mz_zip_archive *zip = ctx->apk;
  const size_t plen = strlen(A6_ASSET_DIR);
  unsigned n = mz_zip_reader_get_num_files(zip);
  uint64_t total = 0, done = 0;
  for (unsigned i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(zip, i, &st) && !st.m_is_directory && !strncmp(st.m_filename, A6_ASSET_DIR, plen))
      total += st.m_uncomp_size;
  }
  if (!total)
    fatal_error("This APK has no game data in %s.\n\n%s", A6_ASSET_DIR, port_apk_help());
  debugPrintf("[a6] unpacking the game data: %llu MB\n", (unsigned long long)(total >> 20));

  unsigned made = 0;
  for (unsigned i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st) || st.m_is_directory || strncmp(st.m_filename, A6_ASSET_DIR, plen))
      continue;
    const char *name = st.m_filename + plen;
    if (!*name || strstr(name, ".."))
      continue;
    char rel[DCR_PATH_MAX], dst[DCR_PATH_MAX];
    snprintf(rel, sizeof rel, A6_DATA_DIR "/%s", name);
    rt_root_path(dst, sizeof dst, rel);
    int p0 = ctx->p0 + (int)((uint64_t)(ctx->p1 - ctx->p0) * done / total);
    done += st.m_uncomp_size;
    int p1 = ctx->p0 + (int)((uint64_t)(ctx->p1 - ctx->p0) * done / total);
    if (rt_setup_extract_entry(zip, (int)i, dst, "Unpacking the game data", p0, p1) != 0)
      fatal_error("Could not unpack %s (SD card full?).", name);
    made++;
  }
  rt_setup_stamp_set(STAMP_KEY, (unsigned long)apk_size, made);
  debugPrintf("[a6] %u files unpacked to %s/\n", made, A6_DATA_DIR);
}
