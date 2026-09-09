# Mark — Daisy Patch firmware build.
#
#   make -C firmware              build mark_patch.bin
#   make -C firmware program-dfu  flash over USB
#
# Requires libDaisy checked out and built:
#   git clone --recurse-submodules https://github.com/electro-smith/libDaisy \
#       ~/tim-os/daisy-sdk/libDaisy && make -C ~/tim-os/daisy-sdk/libDaisy
#
# DaisySP is deliberately NOT used — mark_core is self-contained DSP.

TARGET = mark_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# The engine is one source shared with Move; the panel is selected entirely
# by these five defines. See mark_core.h for what each one means.
#
#   MARK_TRACKS=4     one track per audio jack pair
#   MARK_CH=1         mono track storage — each track is fed by ONE mono jack,
#                     so stereo storage would hold the same signal twice and
#                     double the only allocation that matters. 28.8 MB rather
#                     than 57.6 MB for 4 tracks + undo at 60 s.
#   MARK_SR=48000     libDaisy's SaiHandle offers no 44.1 kHz
#   MARK_HOSTED_FX=0  no dlopen/dirent on bare metal; tracks use built-in FX
#   MARK_SESSIONS=0   no filesystem yet. The Patch HAS an SD card, so this is
#                     the obvious next feature via libDaisy's FatFS — which is
#                     why it is a flag and not a deletion.
MARK_DEFS = -DMARK_TRACKS=4 -DMARK_CH=1 -DMARK_SR=48000 \
            -DMARK_HOSTED_FX=0 -DMARK_SESSIONS=0

C_DEFS   += $(MARK_DEFS)
CPP_DEFS += $(MARK_DEFS)

# Mark is the largest of the three Patch ports (mark_core is ~2700 lines
# against belt_core's 796), so BOOT_NONE is not a given. Build it and read
# --print-memory-usage: if FLASH exceeds 131072 B, switch to BOOT_SRAM and
# follow smack-versio's FLASHING.md.
APP_TYPE ?= BOOT_NONE

CPP_SOURCES = mark_patch.cpp

# mark_core_patch.c #includes vendor/mark_core.c with calloc/free redirected
# to the SDRAM pool. Never list vendor/mark_core.c here as well — that would
# define every engine symbol twice.
#
# clock_adapter.c is smack-versio's gate-to-MIDI-clock adapter, copied
# verbatim (it has no libDaisy dependency). It gives Gate In 1 to the engine's
# existing 24 ppqn clock path instead of porting the timing model to analog.
C_SOURCES = patch_alloc.c mark_core_patch.c clock_adapter.c

OPT = -Os

# NOTE: no `-u _printf_float`. newlib-nano silently prints nothing for %f
# without it. This firmware formats only %d, %u and %s. If you add a %f to
# the display code you must add the flag or the field renders empty.

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
