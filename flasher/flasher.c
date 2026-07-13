/*
 * sprigNES Flasher — puts any .nes ROM on a Hack Club Sprig.
 *
 * The sprigNES firmware UF2 is embedded in this executable as a resource.
 * Flashing merges it with the chosen ROM (as extra UF2 blocks at flash
 * offset 512 KB) into a single image and copies it to the RPI-RP2
 * bootloader drive. No toolchain, no drivers, no install.
 *
 * Build (MinGW):
 *   windres flasher.rc flasher_res.o
 *   gcc flasher.c flasher_res.o -o SprigNESFlasher.exe -O2 -static -mwindows -lcomdlg32 -lshell32
 *
 * CLI (for scripts; GUI opens when run without arguments):
 *   SprigNESFlasher --merge <rom.nes> <out.uf2>
 *   SprigNESFlasher --flash <rom.nes>
 */

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------- UF2 ---------------- */

#define UF2_MAGIC0 0x0A324655u
#define UF2_MAGIC1 0x9E5D5157u
#define UF2_MAGIC_END 0x0AB16F30u
#define UF2_FLAG_FAMILY 0x00002000u
#define UF2_FAMILY_RP2040 0xE48BFF56u

/* Must match ROM_FLASH_OFFSET in the firmware's src/main.cpp. */
#define ROM_FLASH_ADDR 0x10080000u
#define ROM_MAX_SIZE (1536u * 1024u)

typedef struct {
    uint32_t magic0, magic1, flags, addr, size, blockNo, numBlocks, family;
    uint8_t data[476];
    uint32_t magicEnd;
} Uf2Block;

typedef char uf2_block_must_be_512[(sizeof(Uf2Block) == 512) ? 1 : -1];

/* ---------------- ROM info ---------------- */

typedef struct {
    char path[MAX_PATH];
    uint8_t *data;
    uint32_t len;
    int mapper;
    int prg16k, chr8k;
} RomInfo;

static RomInfo g_rom;

static BOOL load_rom(const char *path, char *err, size_t errlen) {
    free(g_rom.data);
    memset(&g_rom, 0, sizeof(g_rom));

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, errlen, "Cannot open file.");
        return FALSE;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 16 || sz.QuadPart > ROM_MAX_SIZE) {
        CloseHandle(h);
        snprintf(err, errlen,
                 "ROM is %lld KB. It must be 1 KB - %u KB to fit in the "
                 "Sprig's 2 MB flash next to the emulator.",
                 sz.QuadPart / 1024, ROM_MAX_SIZE / 1024);
        return FALSE;
    }
    uint32_t len = (uint32_t)sz.QuadPart;
    uint8_t *buf = malloc(len);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, len, &rd, NULL) && rd == len;
    CloseHandle(h);
    if (!ok) {
        free(buf);
        snprintf(err, errlen, "Read error.");
        return FALSE;
    }
    if (memcmp(buf, "NES\x1a", 4) != 0) {
        free(buf);
        snprintf(err, errlen, "Not a valid iNES ROM (missing NES header).");
        return FALSE;
    }

    g_rom.data = buf;
    g_rom.len = len;
    g_rom.prg16k = buf[4];
    g_rom.chr8k = buf[5];
    g_rom.mapper = (buf[6] >> 4) | (buf[7] & 0xF0);
    strncpy(g_rom.path, path, MAX_PATH - 1);
    return TRUE;
}

/* ---------------- firmware resource + merge ---------------- */

static const uint8_t *firmware_uf2(uint32_t *len) {
    HRSRC r = FindResourceA(NULL, "FIRMWARE", (LPCSTR)RT_RCDATA);
    if (!r)
        return NULL;
    HGLOBAL g = LoadResource(NULL, r);
    *len = SizeofResource(NULL, r);
    return (const uint8_t *)LockResource(g);
}

/* Merge firmware UF2 + ROM into one UF2 image. Caller frees.
 *
 * The RP2040 boot ROM only accepts UF2s that form one CONTIGUOUS image:
 * each block's target address must equal base + blockNo*256, or the block
 * is silently ignored. So the gap between the end of the firmware and the
 * fixed ROM offset is filled with 0xFF padding blocks. */
static uint8_t *build_merged(const uint8_t *rom, uint32_t romlen,
                             uint32_t *outlen, char *err, size_t errlen) {
    uint32_t fwlen = 0;
    const uint8_t *fw = firmware_uf2(&fwlen);
    if (!fw || fwlen == 0 || fwlen % 512 != 0) {
        snprintf(err, errlen, "Embedded firmware resource is missing or corrupt.");
        return NULL;
    }

    uint32_t nfw = fwlen / 512;
    uint32_t base = 0;
    for (uint32_t i = 0; i < nfw; ++i) {
        const Uf2Block *b = (const Uf2Block *)(fw + (size_t)i * 512);
        if (b->magic0 != UF2_MAGIC0 || b->magic1 != UF2_MAGIC1 ||
            b->magicEnd != UF2_MAGIC_END || b->size != 256) {
            snprintf(err, errlen, "Embedded firmware UF2 is corrupt (block %u).", i);
            return NULL;
        }
        if (i == 0)
            base = b->addr;
        if (b->addr != base + i * 256) {
            snprintf(err, errlen,
                     "Embedded firmware UF2 is not contiguous (block %u).", i);
            return NULL;
        }
        if (b->addr + b->size > ROM_FLASH_ADDR) {
            snprintf(err, errlen,
                     "Firmware overlaps the ROM area - rebuild with a larger "
                     "ROM_FLASH_OFFSET.");
            return NULL;
        }
    }

    uint32_t fw_end = base + nfw * 256;
    uint32_t npad = (ROM_FLASH_ADDR - fw_end) / 256;
    uint32_t nrom = (romlen + 255) / 256;
    uint32_t total = nfw + npad + nrom;

    Uf2Block *out = calloc(total, sizeof(Uf2Block));

    /* firmware, renumbered */
    for (uint32_t i = 0; i < nfw; ++i) {
        memcpy(&out[i], fw + (size_t)i * 512, 512);
        out[i].blockNo = i;
        out[i].numBlocks = total;
    }

    /* 0xFF padding + ROM, one contiguous run of addresses */
    for (uint32_t j = 0; j < npad + nrom; ++j) {
        Uf2Block *b = &out[nfw + j];
        b->magic0 = UF2_MAGIC0;
        b->magic1 = UF2_MAGIC1;
        b->flags = UF2_FLAG_FAMILY;
        b->addr = fw_end + j * 256;
        b->size = 256;
        b->blockNo = nfw + j;
        b->numBlocks = total;
        b->family = UF2_FAMILY_RP2040;
        memset(b->data, 0xFF, 256);
        if (j >= npad) {
            uint32_t off = (j - npad) * 256;
            uint32_t n = (romlen - off > 256) ? 256 : romlen - off;
            memcpy(b->data, rom + off, n);
        }
        b->magicEnd = UF2_MAGIC_END;
    }

    *outlen = total * 512;
    return (uint8_t *)out;
}

/* ---------------- drive + flash ---------------- */

static char find_rp2_drive(void) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        char root[8];
        snprintf(root, sizeof(root), "%c:\\", 'A' + i);
        if (GetDriveTypeA(root) != DRIVE_REMOVABLE)
            continue;
        char label[MAX_PATH + 1] = {0};
        if (GetVolumeInformationA(root, label, sizeof(label), NULL, NULL, NULL,
                                  NULL, 0) &&
            strcmp(label, "RPI-RP2") == 0)
            return (char)('A' + i);
    }
    return 0;
}

static BOOL write_uf2(char drive, const uint8_t *img, uint32_t len, char *err,
                      size_t errlen) {
    char path[32];
    snprintf(path, sizeof(path), "%c:\\sprignes.uf2", drive);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, errlen, "Cannot write to drive %c: (error %lu).", drive,
                 GetLastError());
        return FALSE;
    }
    DWORD wr = 0;
    BOOL ok = WriteFile(h, img, len, &wr, NULL) && wr == len;
    /* The board reboots the moment the last block lands, so cleanup calls
       after a complete write may fail. That's success, not an error. */
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok)
        snprintf(err, errlen, "Write failed at %lu/%u bytes.", wr, len);
    return ok;
}

/* ---------------- GUI ---------------- */

#define IDC_BTN_ROM 101
#define IDC_BTN_FLASH 102
#define IDC_LBL_ROM 103
#define IDC_LBL_DRIVE 104
#define IDT_DRIVESCAN 1

static HWND g_hBtnRom, g_hBtnFlash, g_hLblRom, g_hLblDrive;

static void update_flash_button(void) {
    EnableWindow(g_hBtnFlash, g_rom.data != NULL && find_rp2_drive() != 0);
}

static void set_rom_label(void) {
    char txt[512];
    const char *base = strrchr(g_rom.path, '\\');
    base = base ? base + 1 : g_rom.path;
    snprintf(txt, sizeof(txt), "%s\r\n%u KB, mapper %d, PRG %d KB, CHR %d KB",
             base, g_rom.len / 1024, g_rom.mapper, g_rom.prg16k * 16,
             g_rom.chr8k * 8);
    SetWindowTextA(g_hLblRom, txt);
}

static void pick_rom(HWND hwnd) {
    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "NES ROMs (*.nes)\0*.nes\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = "Choose a .nes ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&ofn))
        return;
    char err[256];
    if (load_rom(file, err, sizeof(err))) {
        set_rom_label();
    } else {
        SetWindowTextA(g_hLblRom, "No ROM selected.");
        MessageBoxA(hwnd, err, "Invalid ROM", MB_ICONWARNING);
    }
    update_flash_button();
}

static void do_flash(HWND hwnd) {
    char err[256];
    char drive = find_rp2_drive();
    if (!drive || !g_rom.data)
        return;
    EnableWindow(g_hBtnFlash, FALSE);
    SetWindowTextA(g_hLblDrive, "Flashing...");
    UpdateWindow(hwnd);

    uint32_t len = 0;
    uint8_t *img = build_merged(g_rom.data, g_rom.len, &len, err, sizeof(err));
    BOOL ok = img && write_uf2(drive, img, len, err, sizeof(err));
    free(img);

    if (ok) {
        MessageBoxA(hwnd,
                    "Done! Your Sprig is rebooting into the game right now.\n\n"
                    "Controls: WASD = D-pad, L = A, J = B, I = Start, "
                    "K = Select.\n\nTo flash a different game, hold BOOTSEL "
                    "while plugging the Sprig back in.",
                    "sprigNES", MB_ICONINFORMATION);
    } else {
        MessageBoxA(hwnd, err, "Flash failed", MB_ICONERROR);
    }
    update_flash_button();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_ROM)
            pick_rom(hwnd);
        else if (LOWORD(wp) == IDC_BTN_FLASH)
            do_flash(hwnd);
        return 0;
    case WM_DROPFILES: {
        char file[MAX_PATH];
        if (DragQueryFileA((HDROP)wp, 0, file, sizeof(file))) {
            char err[256];
            if (load_rom(file, err, sizeof(err)))
                set_rom_label();
            else
                MessageBoxA(hwnd, err, "Invalid ROM", MB_ICONWARNING);
            update_flash_button();
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_DRIVESCAN) {
            char d = find_rp2_drive();
            char txt[128];
            if (d)
                snprintf(txt, sizeof(txt), "Sprig detected on drive %c:", d);
            else
                snprintf(txt, sizeof(txt),
                         "No Sprig found. Hold BOOTSEL while plugging in USB.");
            SetWindowTextA(g_hLblDrive, txt);
            update_flash_button();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int run_gui(HINSTANCE hInst) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "sprignesFlasher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        WS_EX_ACCEPTFILES, wc.lpszClassName, "sprigNES Flasher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 320, NULL, NULL, hInst, NULL);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT title = CreateFontA(28, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    HWND h;
    h = CreateWindowA("STATIC", "sprigNES Flasher", WS_CHILD | WS_VISIBLE | SS_CENTER,
                      20, 18, 424, 34, hwnd, NULL, hInst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)title, TRUE);

    h = CreateWindowA("STATIC",
                      "Turn your Hack Club Sprig into a NES. Pick a ROM, plug "
                      "in the Sprig with BOOTSEL held, hit Flash.",
                      WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 56, 424, 34, hwnd,
                      NULL, hInst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);

    g_hBtnRom = CreateWindowA("BUTTON", "Choose .nes ROM...",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 100, 140,
                              30, hwnd, (HMENU)IDC_BTN_ROM, hInst, NULL);
    SendMessageA(g_hBtnRom, WM_SETFONT, (WPARAM)font, TRUE);

    g_hLblRom = CreateWindowA(
        "STATIC", "No ROM selected. (You can also drag && drop one here.)",
        WS_CHILD | WS_VISIBLE, 176, 100, 268, 44, hwnd, (HMENU)IDC_LBL_ROM,
        hInst, NULL);
    SendMessageA(g_hLblRom, WM_SETFONT, (WPARAM)font, TRUE);

    g_hLblDrive = CreateWindowA("STATIC", "Looking for a Sprig...",
                                WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 165, 424,
                                20, hwnd, (HMENU)IDC_LBL_DRIVE, hInst, NULL);
    SendMessageA(g_hLblDrive, WM_SETFONT, (WPARAM)font, TRUE);

    g_hBtnFlash = CreateWindowA("BUTTON", "Flash to Sprig",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                                140, 195, 184, 40, hwnd, (HMENU)IDC_BTN_FLASH,
                                hInst, NULL);
    SendMessageA(g_hBtnFlash, WM_SETFONT, (WPARAM)font, TRUE);

    h = CreateWindowA("STATIC",
                      "Restore stock Sprig firmware anytime: BOOTSEL mode + "
                      "upload from sprig.hackclub.com",
                      WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 248, 424, 20, hwnd,
                      NULL, hInst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);

    SetTimer(hwnd, IDT_DRIVESCAN, 800, NULL);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}

/* ---------------- CLI ---------------- */

static int run_cli(int argc, char **argv) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    printf("\n");

    char err[256];
    if (argc >= 4 && strcmp(argv[1], "--merge") == 0) {
        if (!load_rom(argv[2], err, sizeof(err))) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        uint32_t len = 0;
        uint8_t *img = build_merged(g_rom.data, g_rom.len, &len, err, sizeof(err));
        if (!img) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f || fwrite(img, 1, len, f) != len) {
            fprintf(stderr, "error: cannot write %s\n", argv[3]);
            return 1;
        }
        fclose(f);
        printf("wrote %s (%u bytes, %u blocks)\n", argv[3], len, len / 512);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--flash") == 0) {
        if (!load_rom(argv[2], err, sizeof(err))) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        char drive = find_rp2_drive();
        if (!drive) {
            fprintf(stderr, "error: no RPI-RP2 drive found (hold BOOTSEL while plugging in)\n");
            return 1;
        }
        uint32_t len = 0;
        uint8_t *img = build_merged(g_rom.data, g_rom.len, &len, err, sizeof(err));
        BOOL ok = img && write_uf2(drive, img, len, err, sizeof(err));
        free(img);
        if (!ok) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        printf("flashed %u bytes to drive %c:\n", len, drive);
        return 0;
    }
    fprintf(stderr,
            "usage: SprigNESFlasher [--merge rom.nes out.uf2 | --flash rom.nes]\n"
            "       (run without arguments for the GUI)\n");
    return 2;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdline, int show) {
    (void)hPrev; (void)cmdline; (void)show;
    if (__argc > 1)
        return run_cli(__argc, __argv);
    return run_gui(hInst);
}
