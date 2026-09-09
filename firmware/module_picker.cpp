/*
 * module_picker — implementation. See module_picker.h for the why.
 *
 * Memory: SDMMC1 reads by DMA, and DMA cannot reach DTCMRAM -- which under the
 * bootloader's linker script is where .bss lives. Everything FatFS reads into
 * (the volume window inside FatFSInterface, the file's sector buffer, our
 * chunk buffer) is therefore in SDRAM, which sits on the AXI bus the SDMMC
 * controller can see; libDaisy's disk driver does the cache maintenance. Two
 * consequences to keep straight:
 *
 *   - .sdram_bss is NOLOAD: nothing zeroes it at startup, so every object
 *     here is cleared explicitly before use.
 *   - SDRAM is initialised in hw.Init(), long after static constructors ran,
 *     so no C++ object with a constructor may live there. FatFSInterface has
 *     one; it is placement-constructed into a raw buffer at mount time.
 *
 * The QSPI app slot is written exactly as a DFU upload writes it -- the .bin
 * bytes at 0x90040000, nothing else -- so what the bootloader finds there is
 * indistinguishable from a USB flash.
 */
#include "module_picker.h"
#include "fatfs.h"
#include <new>
#include <stdio.h>
#include <string.h>
#include <strings.h>

using namespace daisy;

namespace
{
constexpr uint32_t APP_SLOT = 0x90040000u;    /* libDaisy core/Makefile QSPI_ADDRESS */
constexpr uint32_t APP_MAX  = 7936u * 1024u;  /* the QSPIFLASH region in the linker scripts */
constexpr uint32_t CHUNK    = 4096u;

/* Where a bootloader image may legitimately point its stack and reset. */
constexpr uint32_t DTCM_LO   = 0x20000000u, DTCM_HI   = 0x20020000u;
constexpr uint32_t AXI_LO    = 0x24000000u, AXI_HI    = 0x24080000u;
constexpr uint32_t QSPI_HI   = 0x90000000u + 8u * 1024u * 1024u;
constexpr uint32_t IFLASH_LO = 0x08000000u, IFLASH_HI = 0x08020000u;

alignas(32) DSY_SDRAM_BSS uint8_t g_fsi_mem[sizeof(FatFSInterface)];
alignas(32) DSY_SDRAM_BSS FIL     g_fil;
alignas(32) DSY_SDRAM_BSS DIR     g_dir;
alignas(32) DSY_SDRAM_BSS FILINFO g_fno;
alignas(32) DSY_SDRAM_BSS uint8_t g_chunk[CHUNK];

SdmmcHandler    g_sd;
FatFSInterface *g_fsi;   /* non-null while mounted */
DaisyPatch     *g_hw;

bool try_mount(SdmmcHandler::BusWidth width, SdmmcHandler::Speed speed)
{
    SdmmcHandler::Config cfg;
    cfg.Defaults();
    cfg.width = width;
    cfg.speed = speed;
    g_sd.Init(cfg);   /* the peripheral is brought up by f_mount, not here */

    memset(g_fsi_mem, 0, sizeof(g_fsi_mem));
    FatFSInterface *fsi = new(g_fsi_mem) FatFSInterface();
    if(fsi->Init(FatFSInterface::Config::MEDIA_SD) != FatFSInterface::OK)
        return false;
    if(f_mount(&fsi->GetSDFileSystem(), fsi->GetSDPath(), 1) != FR_OK)
    {
        f_mount(nullptr, fsi->GetSDPath(), 0);
        fsi->DeInit();
        return false;
    }
    g_fsi = fsi;
    return true;
}

bool mount(void)
{
    if(g_fsi) return true;
    /* 4-bit at full speed is how the Patch is meant to be wired; the
     * fallbacks cover a slow card or a slot with fewer data lines. */
    return try_mount(SdmmcHandler::BusWidth::BITS_4, SdmmcHandler::Speed::FAST)
           || try_mount(SdmmcHandler::BusWidth::BITS_4, SdmmcHandler::Speed::MEDIUM_SLOW)
           || try_mount(SdmmcHandler::BusWidth::BITS_1, SdmmcHandler::Speed::MEDIUM_SLOW);
}

void unmount(void)
{
    if(!g_fsi) return;
    f_mount(nullptr, g_fsi->GetSDPath(), 0);
    g_fsi->DeInit();
    g_fsi = nullptr;
}

bool fail(FIL *f, char *err, size_t err_len, const char *why)
{
    if(f) f_close(f);
    snprintf(err, err_len, "%s", why);
    return false;
}

/* ---- display ------------------------------------------------------------ */

void draw_list(DaisyPatch &hw, const picker::List &list, int sel, const char *status)
{
    char line[32];
    hw.display.Fill(false);

    snprintf(line, sizeof(line), "%-8s %s", "MODULES", status);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* Item 0 is "back"; five rows visible, window follows the cursor. */
    int top = sel - 4;
    if(top < 0) top = 0;
    for(int row = 0; row < 5; row++)
    {
        int item = top + row;
        if(item > list.count) break;
        if(item == 0)
            snprintf(line, sizeof(line), "%c back", sel == 0 ? '>' : ' ');
        else
            snprintf(line, sizeof(line), "%c %-9s %3luk", sel == item ? '>' : ' ',
                     list.name[item - 1],
                     (unsigned long)((list.size[item - 1] + 1023u) / 1024u));
        hw.display.SetCursor(0, 16 + row * 10);
        hw.display.WriteString(line, Font_6x8, true);
    }
    hw.display.Update();
}

void draw_progress(int pct)
{
    if(!g_hw) return;
    DaisyPatch &hw = *g_hw;
    char        line[32];
    hw.display.Fill(false);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString("MODULES", Font_6x8, true);
    snprintf(line, sizeof(line), "%s %3d%%", pct < 50 ? "writing " : "checking", pct);
    hw.display.SetCursor(0, 16);
    hw.display.WriteString(line, Font_6x8, true);
    hw.display.DrawRect(0, 40, 127, 48, true, false);
    if(pct > 0) hw.display.DrawRect(1, 41, (uint8_t)(1 + pct * 125 / 100), 47, true, true);
    hw.display.Update();
}

void wait_release(DaisyPatch &hw)
{
    /* Debounce runs inside ProcessAllControls; keep it fed while waiting. */
    for(;;)
    {
        hw.ProcessAllControls();
        if(!hw.encoder.Pressed()) return;
        System::Delay(1);
    }
}
} // namespace

namespace picker
{
bool scan(List &out)
{
    out.count = 0;
    if(!mount()) return false;

    char path[24];
    snprintf(path, sizeof(path), "%smodules", g_fsi->GetSDPath());
    memset(&g_dir, 0, sizeof(g_dir));
    if(f_opendir(&g_dir, path) != FR_OK) return false;

    for(;;)
    {
        memset(&g_fno, 0, sizeof(g_fno));
        if(f_readdir(&g_dir, &g_fno) != FR_OK || g_fno.fname[0] == '\0') break;
        if(g_fno.fattrib & (AM_DIR | AM_HID)) continue;
        /* macOS writes an AppleDouble "._name.bin" next to every file it
         * copies to FAT. Those end in .bin too; the vector-table check would
         * refuse them, but they should not be on the list at all. */
        if(g_fno.fname[0] == '.') continue;
        const char *fn = g_fno.fname;
        size_t      L  = strlen(fn);
        if(L < 5 || strcasecmp(fn + L - 4, ".bin") != 0) continue;
        if(L - 4 > (size_t)NAME_LEN - 1) continue;   /* name too long to reopen: skip */
        if(out.count >= MAX_MODULES) break;
        memcpy(out.name[out.count], fn, L - 4);
        out.name[out.count][L - 4] = '\0';
        out.size[out.count]        = (uint32_t)g_fno.fsize;
        out.count++;
    }
    f_closedir(&g_dir);

    /* Alphabetical, so the list is stable across cards and rescans. */
    for(int i = 1; i < out.count; i++)
        for(int j = i; j > 0 && strcasecmp(out.name[j - 1], out.name[j]) > 0; j--)
        {
            char     tn[NAME_LEN];
            uint32_t ts;
            memcpy(tn, out.name[j], NAME_LEN);
            ts = out.size[j];
            memcpy(out.name[j], out.name[j - 1], NAME_LEN);
            out.size[j] = out.size[j - 1];
            memcpy(out.name[j - 1], tn, NAME_LEN);
            out.size[j - 1] = ts;
        }
    return true;
}

bool load(DaisyPatch &hw, const char *name, void (*progress)(int pct), char *err, size_t err_len)
{
    if(!mount()) return fail(nullptr, err, err_len, "no card");

    char path[48];
    snprintf(path, sizeof(path), "%smodules/%s.bin", g_fsi->GetSDPath(), name);
    memset(&g_fil, 0, sizeof(g_fil));
    if(f_open(&g_fil, path, FA_READ) != FR_OK) return fail(nullptr, err, err_len, "open failed");

    uint32_t size = (uint32_t)f_size(&g_fil);
    if(size < 512u || size > APP_MAX) return fail(&g_fil, err, err_len, "bad size");

    /* Vector-table sanity, the same test the bootloader applies before it
     * jumps: stack in RAM, reset vector in SRAM or QSPI. An image built for
     * internal flash (BOOT_NONE) is refused here instead of poisoning the
     * slot -- it would leave the bootloader with nothing to boot. */
    UINT n = 0;
    if(f_read(&g_fil, g_chunk, 8, &n) != FR_OK || n != 8)
        return fail(&g_fil, err, err_len, "read failed");
    uint32_t sp, pc;
    memcpy(&sp, g_chunk, 4);
    memcpy(&pc, g_chunk + 4, 4);
    if(pc >= IFLASH_LO && pc < IFLASH_HI) return fail(&g_fil, err, err_len, "flash build");
    bool sp_ok = (sp >= DTCM_LO && sp <= DTCM_HI) || (sp >= AXI_LO && sp <= AXI_HI);
    bool pc_ok = (pc >= AXI_LO && pc < AXI_HI) || (pc >= APP_SLOT && pc < QSPI_HI);
    if(!sp_ok || !pc_ok) return fail(&g_fil, err, err_len, "not bootable");
    f_lseek(&g_fil, 0);

    QSPIHandle &q = hw.seed.qspi;
    if(q.Erase(APP_SLOT, APP_SLOT + size) != QSPIHandle::Result::OK)
        return fail(&g_fil, err, err_len, "erase failed");

    uint32_t off = 0;
    while(off < size)
    {
        if(f_read(&g_fil, g_chunk, CHUNK, &n) != FR_OK || n == 0)
            return fail(&g_fil, err, err_len, "read failed");
        if(q.Write(APP_SLOT + off, n, g_chunk) != QSPIHandle::Result::OK)
            return fail(&g_fil, err, err_len, "write failed");
        off += n;
        if(progress) progress((int)((uint64_t)off * 50u / size));
    }

    /* Verify against the memory-mapped view; QSPI is back in that mode after
     * Write(). The data cache may hold stale lines for the region, so each
     * chunk is invalidated before it is compared. */
    f_lseek(&g_fil, 0);
    off = 0;
    while(off < size)
    {
        if(f_read(&g_fil, g_chunk, CHUNK, &n) != FR_OK || n == 0)
            return fail(&g_fil, err, err_len, "read failed");
        const uint8_t *mm = (const uint8_t *)q.GetData(APP_SLOT + off);
        SCB_InvalidateDCache_by_Addr((uint32_t *)mm, (int32_t)((n + 31u) & ~31u));
        if(memcmp(mm, g_chunk, n) != 0) return fail(&g_fil, err, err_len, "verify failed");
        off += n;
        if(progress) progress(50 + (int)((uint64_t)off * 50u / size));
    }
    f_close(&g_fil);
    unmount();

    /* Hand over. Skip the bootloader's DFU grace period: the image is in the
     * slot, boot it. This returns only from an internal-flash (BOOT_NONE)
     * build, where there is no bootloader to return to. */
    System::ResetToBootloader(System::BootloaderMode::DAISY_SKIP_TIMEOUT);
    return fail(nullptr, err, err_len, "no bootloader");
}

void run(DaisyPatch &hw)
{
    g_hw = &hw;
    wait_release(hw);

    List list;
    bool have = scan(list);
    char status[16];
    snprintf(status, sizeof(status), "%s",
             have ? (list.count ? "" : "empty") : (g_fsi ? "no folder" : "no card"));

    int      sel       = 0;
    uint32_t last_draw = 0;

    for(;;)
    {
        hw.ProcessAllControls();

        int inc = hw.encoder.Increment();
        if(inc)
        {
            sel += inc;
            if(sel < 0) sel = 0;
            if(sel > list.count) sel = list.count;
        }

        if(hw.encoder.RisingEdge())
        {
            if(sel == 0) break;
            char err[16];
            if(!load(hw, list.name[sel - 1], draw_progress, err, sizeof(err)))
            {
                snprintf(status, sizeof(status), "%s", err);
                wait_release(hw);
                have = scan(list);
                if(sel > list.count) sel = list.count;
            }
        }

        uint32_t now = System::GetNow();
        if(now - last_draw >= 50u)
        {
            draw_list(hw, list, sel, status);
            last_draw = now;
        }
    }

    unmount();
    wait_release(hw);
}
} // namespace picker
