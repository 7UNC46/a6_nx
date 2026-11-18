#---------------------------------------------------------------------------------
# Asphalt 6: Adrenaline HD -- Nintendo Switch wrapper (32-bit / AArch32, android32)
#
# Ships NO game code and NO game assets: the player's own APK is read at run
# time. Build with ./build.sh (the toolchain container).
#---------------------------------------------------------------------------------
TARGET               := a6_nx
# Pick an id no other android32 port uses (mc3_nx has ...10C3).
PORT_NPDM_PROGRAM_ID := 0x01000000000010A6
PORT_NPDM_VERSION    := 0.8.0
# Glitch + gameswf on the main thread: give it a large stack, as mc3 does.
PORT_NPDM_MAIN_STACK := 0x800000
# The engine reads its .bar archives with many small freads.
PORT_CFLAGS += -DRT_STDIO_READ_BUF=65536
# Guest threads on one core (source/dcr_sched.c): the runtime's 3-core
# scheduler self-test does not apply.
PORT_CFLAGS += -DRT_SCHED_SELFTEST=0

include runtime/runtime.mk

# stb_vorbis (public domain) is compiled inside a6_sound.c.
$(BUILD)/a6_sound.o: CFLAGS += -Wno-unused-value -Wno-unused-but-set-variable -Wno-sign-compare
