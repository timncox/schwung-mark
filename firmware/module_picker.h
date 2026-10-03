/*
 * module_picker — switch firmware from the front panel.
 *
 * The Daisy bootloader boots whatever .bin sits in its QSPI app slot, and
 * libDaisy can write QSPI and reset into the bootloader. Put the two together
 * and the SD card becomes a module library: the .bin files in /modules on the card, a
 * list on the OLED, pick one, and a few seconds later it is running. The card
 * never leaves the slot, USB never comes out, nothing behind the panel is
 * touched.
 *
 * Requirements, shared by all three Patch ports:
 *   - the Daisy bootloader in internal flash (once: make program-boot from
 *     ST ROM DFU)
 *   - every app built APP_TYPE = BOOT_SRAM (or BOOT_QSPI), so it runs under
 *     the bootloader and its .bin is what the picker copies
 *   - USE_FATFS = 1 in the Makefile, and module_picker.cpp in CPP_SOURCES
 *   - a FAT32 card with a /modules folder. Keep .bin files OUT of the root:
 *     the bootloader itself flashes the first root .bin it finds at every
 *     power-up, which is a different (and cruder) mechanism.
 *
 * Shared by copy across the belt/smack/mark firmware dirs, like patch_alloc.
 */
#pragma once
#include "daisy_patch.h"
#include <stddef.h>
#include <stdint.h>

namespace picker
{
constexpr int MAX_MODULES = 8;
constexpr int NAME_LEN    = 12;   /* "smack" fits; 8.3-safe names are the convention */

struct List
{
    int      count;
    char     name[MAX_MODULES][NAME_LEN];   /* without ".bin" */
    uint32_t size[MAX_MODULES];
};

/* Mount the card and list the .bin files in /modules. false = no card, or no folder. */
bool scan(List &out);

/* Copy /modules/<name>.bin into the bootloader's app slot, verify it, and
 * reset into the bootloader, which runs it. Returns only on failure, with a
 * short reason in err. progress is called with 0..100 as it goes. */
bool load(daisy::DaisyPatch &hw, const char *name, void (*progress)(int pct),
          char *err, size_t err_len);

/* The modal screen: turn to choose, press to load, "back" to return. Owns
 * the encoder and display until it returns; audio keeps running underneath.
 * Waits for the encoder to be released before starting and before returning,
 * so the gesture that opened it never leaks back to the caller. Never returns
 * if a module was loaded. */
void run(daisy::DaisyPatch &hw);
}
