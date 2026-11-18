/* port_config.h -- Asphalt 6: Adrenaline HD for Switch (android32).
 *
 * com.gameloft.android.HEP.GloftA6HP. The engine is Gameloft's 32-bit
 * Glitch build (ARMv7-A, VFPv2, softfp, SONAME libjni.so, GLES 2:
 * "#define GLITCH_OPENGLES_2" in its shaders). The APK this port targets is
 * the repack for 64-bit-only phones: the original 32-bit library is kept as
 * libguest_asphalt6.so next to 64-bit host shims (libasphalt6.so,
 * libguestcore.so...) that this port does NOT use -- the Switch runs the
 * 32-bit library directly as an AArch32 program.
 * MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#define PORT_TITLE   "Asphalt 6: Adrenaline"
#define PORT_NAME    "a6_nx"
#define PORT_PACKAGE "com.gameloft.android.HEP.GloftA6HP"
#define PORT_VERSION "0.8.0"
/* First line of debug.log: name, title and version */
#define PORT_BANNER  PORT_NAME " v" PORT_VERSION ": " PORT_TITLE

/* Where the 32-bit library sits in the repack's APK. The repack only has
 * the arm64-v8a folder (the 32-bit library is a "guest" there).
 * TODO: check with `unzip -l your.apk | grep libguest_asphalt6` and change
 * this if it lies elsewhere (an original 32-bit APK: "lib/armeabi/"). */
#define PORT_ABI_DIR "lib/arm64-v8a/"

#define A6_LIB        "libguest_asphalt6.so"
/* The build the hard-coded facts of this port were read from (3 SWP sites,
 * the JNI signatures in a6_jni.c). md5 0c29933d2a02409edba5966cfacb381b.
 * Fill the CRC-32 / size in once the setup has stamped it (debug.log) to
 * refuse other builds; 0 = accept any. */
#define A6_LIB_CRC32  0x00000000UL
#define A6_LIB_SIZE   8811740UL

/* The game data the repack carries as APK assets ("gamedata.lst" + the
 * files under assets/gamedata/), copied by its DataInstallActivity to the
 * app's external files folder. a6_setup.c does the same, once. */
#define A6_ASSET_DIR  "assets/gamedata/"
#define A6_DATA_DIR   "external/files"   /* = /sdcard/Android/data/<pkg>/files */

/* Sound: Gameloft's vox engine mixes everything itself and plays it through
 * OpenSL ES (vox::DriverAndroid::_InitOSL; JNI_OnLoad tells it API level 9).
 * The APK's Java GLMediaPlayer is an empty stub (registerSoundFile does
 * nothing, playSound returns -1), so the runtime's OpenSL ES on audout is
 * the whole sound path, as in a8retry_nx. */
#define RT_OPENSLES 1

#endif /* PORT_CONFIG_H */
