/*
 * sprigNES — NES emulator firmware for the Hack Club Sprig console
 *
 * Hardware shell around the InfoNES core (vendored from
 * shuichitakano/pico-infones, itself based on InfoNES by Jay Kumogata).
 *
 * Sprig pin map (taken from hackclub/sprig firmware, sprig_hal):
 *   ST7735 160x128 LCD  : SPI0  SCK=GP18 MOSI=GP19 CS=GP20 DC=GP22 RST=GP26 BL=GP17
 *   Buttons (active low): W=GP5 S=GP7 A=GP6 D=GP8  I=GP12 K=GP14 J=GP13 L=GP15
 *   MAX98357A I2S audio : DATA=GP9 BCLK=GP10 LRCLK=GP11
 *   LEDs                : left=GP28 right=GP4
 *
 * NES controller mapping:
 *   W/A/S/D = D-pad,  L = A,  J = B,  I = Start,  K = Select
 *   Hold I+K+J+L together to reset the game.
 *
 * Video: NES 256x240 -> crop 8 lines top/bottom (256x224) -> scale 5/8 x 4/7
 *        -> exactly 160x128, streamed per scanline over SPI DMA. Scaling is
 *        a weighted box filter (dropped pixels/lines are blended into their
 *        neighbors, not discarded).
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/audio_i2s.h"

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Mapper.h"
#include "InfoNES_pAPU.h"

// The .nes ROM is not compiled in. It lives at a fixed flash offset, written
// separately by the flasher tool as extra UF2 blocks. The first 512 KB of
// flash belong to this firmware; everything after is ROM space (~1.5 MB).
#define ROM_FLASH_OFFSET (512u * 1024u)
#define XIP_FLASH_BASE 0x10000000u
static const uint8_t *const nes_rom =
    (const uint8_t *)(XIP_FLASH_BASE + ROM_FLASH_OFFSET);

// ------------------------------------------------------------------
// Sprig hardware constants
// ------------------------------------------------------------------

#define TFT_SPI       spi0
#define PIN_TFT_SCK   18
#define PIN_TFT_MOSI  19
#define PIN_TFT_CS    20
#define PIN_TFT_DC    22
#define PIN_TFT_RST   26
#define PIN_TFT_BL    17

#define PIN_LED_L     28
#define PIN_LED_R     4

// W S A D I K J L (same order as sprig_hal button_pins[])
static const uint8_t BTN_PINS[8] = {5, 7, 6, 8, 12, 14, 13, 15};
#define BTN_W 0
#define BTN_S 1
#define BTN_A 2
#define BTN_D 3
#define BTN_I 4
#define BTN_K 5
#define BTN_J 6
#define BTN_L 7

#define SCREEN_W 160
#define SCREEN_H 128

#define AUDIO_RATE 22050  // matches pAPU_QUALITY=2 set in CMakeLists

// ST7735 commands (subset)
#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_NORON   0x13
#define ST7735_INVOFF  0x20
#define ST7735_DISPON  0x29
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_COLMOD  0x3A
#define ST7735_MADCTL  0x36
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

// MADCTL: MV = landscape raster order, MX/MY = axis flip. This ST7735 panel is
// physically BGR and IGNORES the RGB/BGR select bit (D3) — so the byte order
// can't be fixed in the register; we emit BGR565 pixel data instead (see the
// palette macro below). Upside down on some unit? build -DSPRIG_ROTATE_180=1.
#if SPRIG_ROTATE_180
#define MADCTL_VAL (0x20 | 0x80) /* MV | MY */
#else
#define MADCTL_VAL (0x20 | 0x40) /* MV | MX */
#endif

// Set to 1 to show red/green/blue full-screen bars for ~1.5 s each at boot
// (a channel-order sanity check), then continue to the game. Off for release.
#ifndef SPRIG_COLOR_TEST
#define SPRIG_COLOR_TEST 0
#endif

// ------------------------------------------------------------------
// NES palette in BGR565 — blue in the high bits, red in the low bits — because
// the Sprig panel is BGR and ignores the MADCTL RGB/BGR bit. Base values are
// the standard InfoNES RGB555 palette (0RRRRRGGGGGBBBBB). Bytes are swapped for
// the SPI wire only at scanline-emit time, so blending stays in linear 565.
// ------------------------------------------------------------------

#define G6(v) (((((v) >> 5) & 31) << 1) | ((((v) >> 5) & 31) >> 4))
#define R5(v) ((v) & 31)          /* low bits carry RED for a BGR panel */
#define B5(v) (((v) >> 10) & 31)  /* high bits carry BLUE */
#define BGR565(v) (uint16_t)((B5(v) << 11) | (G6(v) << 5) | R5(v))
#define P(v) BGR565(v)

const WORD __not_in_flash("nespal") NesPalette[64] = {
    P(0x39ce), P(0x1071), P(0x0015), P(0x2013), P(0x440e), P(0x5402), P(0x5000), P(0x3c20),
    P(0x20a0), P(0x0100), P(0x0140), P(0x00e2), P(0x0ceb), P(0x0000), P(0x0000), P(0x0000),
    P(0x5ef7), P(0x01dd), P(0x10fd), P(0x401e), P(0x5c17), P(0x700b), P(0x6ca0), P(0x6521),
    P(0x45c0), P(0x0240), P(0x02a0), P(0x0247), P(0x0211), P(0x0000), P(0x0000), P(0x0000),
    P(0x7fff), P(0x1eff), P(0x2e5f), P(0x223f), P(0x79ff), P(0x7dd6), P(0x7dcc), P(0x7e67),
    P(0x7ae7), P(0x4342), P(0x2769), P(0x2ff3), P(0x03bb), P(0x0000), P(0x0000), P(0x0000),
    P(0x7fff), P(0x579f), P(0x635f), P(0x6b3f), P(0x7f1f), P(0x7f1b), P(0x7ef6), P(0x7f75),
    P(0x7f94), P(0x73f4), P(0x57d7), P(0x5bf9), P(0x4ffe), P(0x0000), P(0x0000), P(0x0000)};

// ------------------------------------------------------------------
// Display
// ------------------------------------------------------------------

static int lcd_dma;  // DMA channel for pixel pushes (audio_i2s uses channel 0)

static WORD line_a[256 + 16];
static WORD line_b[256 + 16];
static uint16_t out_line[SCREEN_W];   // byte-swapped, DMA source
static uint16_t hbuf[SCREEN_W];       // current line after horizontal downscale
static uint16_t vstore[SCREEN_W];     // stored line awaiting vertical blend
static uint8_t hsrc[SCREEN_W];        // output x -> NES x
static uint8_t hmode[SCREEN_W];       // horizontal blend mode per output x
static uint8_t vact[240];             // per NES line: V_* action

// Weighted box filter instead of pixel dropping: every source pixel/line
// contributes to the output, which the 160x128 panel repays in detail.
enum {
    V_SKIP,          // overscan, not displayed
    V_DIRECT,        // emit as-is
    V_STORE,         // keep for blending with the next line
    V_BLEND_EVEN,    // emit 50% stored + 50% current
    V_BLEND_CUR,     // emit 25% stored + 75% current
    V_BLEND_STORED,  // emit 75% stored + 25% current
};

static inline uint16_t avg565(uint16_t a, uint16_t b) {
    return (uint16_t)((((a ^ b) & 0xF7DEu) >> 1) + (a & b));
}

#define DC_DELAY() asm volatile("nop \n nop \n nop")

static inline void dc_low(void)  { DC_DELAY(); gpio_put(PIN_TFT_DC, 0); DC_DELAY(); }
static inline void dc_high(void) { DC_DELAY(); gpio_put(PIN_TFT_DC, 1); DC_DELAY(); }

static void lcd_cmd(uint8_t c) {
    dc_low();
    spi_write_blocking(TFT_SPI, &c, 1);
    dc_high();
}

static void lcd_data(const uint8_t *d, size_t n) {
    spi_write_blocking(TFT_SPI, d, n);
}

static void lcd_data1(uint8_t d) { lcd_data(&d, 1); }

static void lcd_wait_idle(void) {
    dma_channel_wait_for_finish_blocking(lcd_dma);
    while (spi_is_busy(TFT_SPI)) tight_loop_contents();
}

// Open the full-screen window and leave the controller in RAMWR mode.
// Called once per frame; every DMA'd scanline then appends in order.
static void lcd_begin_frame(void) {
    lcd_wait_idle();
    static const uint8_t caset[4] = {0, 0, 0, SCREEN_W - 1};
    static const uint8_t raset[4] = {0, 0, 0, SCREEN_H - 1};
    lcd_cmd(ST7735_CASET); lcd_data(caset, 4);
    lcd_cmd(ST7735_RASET); lcd_data(raset, 4);
    lcd_cmd(ST7735_RAMWR);
}

static void lcd_fill(uint16_t color) {
    const uint8_t px[2] = {(uint8_t)(color >> 8), (uint8_t)color};
    lcd_begin_frame();
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i)
        spi_write_blocking(TFT_SPI, px, 2);
}

// Full-screen RED, GREEN, BLUE for ~1.5 s each (BGR565-encoded, matching the
// game's pixel path). If the panel is wired as we assume you'll see red, then
// green, then blue, in that order. Seeing blue-green-red means R/B is reversed.
static void color_test(void) {
    printf("color test: you should see RED, then GREEN, then BLUE\n");
    lcd_fill(0x001F); sleep_ms(1500);  // BGR565: red is the low 5 bits
    lcd_fill(0x07E0); sleep_ms(1500);  // green is the middle 6 bits
    lcd_fill(0xF800); sleep_ms(1500);  // BGR565: blue is the high 5 bits
    lcd_fill(0x0000);
}

// Init sequence taken from the stock Sprig firmware (sprig_hal ST7735_TFT.h),
// with MADCTL switched to landscape raster order.
static void lcd_init(void) {
    gpio_init(PIN_TFT_BL);
    gpio_set_dir(PIN_TFT_BL, GPIO_OUT);
    gpio_put(PIN_TFT_BL, 1);

    spi_init(TFT_SPI, 31500000);
    gpio_set_function(PIN_TFT_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_TFT_MOSI, GPIO_FUNC_SPI);

    gpio_init(PIN_TFT_CS);
    gpio_set_dir(PIN_TFT_CS, GPIO_OUT);
    gpio_put(PIN_TFT_CS, 0);  // keep selected forever; the bus is ours alone

    gpio_init(PIN_TFT_DC);
    gpio_set_dir(PIN_TFT_DC, GPIO_OUT);
    gpio_put(PIN_TFT_DC, 0);

    gpio_init(PIN_TFT_RST);
    gpio_set_dir(PIN_TFT_RST, GPIO_OUT);
    gpio_put(PIN_TFT_RST, 1); sleep_ms(10);
    gpio_put(PIN_TFT_RST, 0); sleep_ms(10);
    gpio_put(PIN_TFT_RST, 1); sleep_ms(10);

    lcd_cmd(ST7735_SWRESET); sleep_ms(150);
    lcd_cmd(ST7735_SLPOUT);  sleep_ms(500);

    lcd_cmd(ST7735_FRMCTR1); lcd_data1(0x01); lcd_data1(0x2C); lcd_data1(0x2D);
    lcd_cmd(ST7735_FRMCTR2); lcd_data1(0x01); lcd_data1(0x2C); lcd_data1(0x2D);
    lcd_cmd(ST7735_FRMCTR3);
    lcd_data1(0x01); lcd_data1(0x2C); lcd_data1(0x2D);
    lcd_data1(0x01); lcd_data1(0x2C); lcd_data1(0x2D);
    lcd_cmd(ST7735_INVCTR);  lcd_data1(0x07);
    lcd_cmd(ST7735_PWCTR1);  lcd_data1(0xA2); lcd_data1(0x02); lcd_data1(0x84);
    lcd_cmd(ST7735_PWCTR2);  lcd_data1(0xC5);
    lcd_cmd(ST7735_PWCTR3);  lcd_data1(0x0A); lcd_data1(0x00);
    lcd_cmd(ST7735_PWCTR4);  lcd_data1(0x8A); lcd_data1(0x2A);
    lcd_cmd(ST7735_PWCTR5);  lcd_data1(0x8A); lcd_data1(0xEE);
    lcd_cmd(ST7735_VMCTR1);  lcd_data1(0x0E);
    lcd_cmd(ST7735_INVOFF);
    lcd_cmd(ST7735_MADCTL);  lcd_data1(MADCTL_VAL);
    lcd_cmd(ST7735_COLMOD);  lcd_data1(0x05);

    lcd_cmd(ST7735_GMCTRP1);
    lcd_data1(0x02); lcd_data1(0x1C); lcd_data1(0x07); lcd_data1(0x12);
    lcd_data1(0x37); lcd_data1(0x32); lcd_data1(0x29); lcd_data1(0x2D);
    lcd_data1(0x29); lcd_data1(0x25); lcd_data1(0x2B); lcd_data1(0x39);
    lcd_data1(0x00); lcd_data1(0x01); lcd_data1(0x03); lcd_data1(0x10);
    lcd_cmd(ST7735_GMCTRN1);
    lcd_data1(0x03); lcd_data1(0x1D); lcd_data1(0x07); lcd_data1(0x06);
    lcd_data1(0x2E); lcd_data1(0x2C); lcd_data1(0x29); lcd_data1(0x2D);
    lcd_data1(0x2E); lcd_data1(0x2E); lcd_data1(0x37); lcd_data1(0x3F);
    lcd_data1(0x00); lcd_data1(0x00); lcd_data1(0x02); lcd_data1(0x10);
    lcd_cmd(ST7735_NORON);   sleep_ms(10);
    lcd_cmd(ST7735_DISPON);  sleep_ms(100);

    // DMA channel 0 is hard-assigned to audio_i2s (same as stock firmware);
    // claim channel 1 for the display so the two can never collide.
    lcd_dma = 1;
    dma_channel_claim(lcd_dma);
    dma_channel_config c = dma_channel_get_default_config(lcd_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(TFT_SPI, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(lcd_dma, &c, &spi_get_hw(TFT_SPI)->dr, NULL, 0, false);

    // Scaling tables. Horizontal: 256 -> 160 (5/8), sampling at x*8/5 with
    // the fractional position deciding the blend weights. Vertical: crop NES
    // overscan lines 0-7 and 232-239, then 224 -> 128 (4/7); each group of 7
    // source lines becomes 4 output rows (1 direct + 3 weighted pairs).
    static const uint8_t hm[5] = {0 /*frac .0*/, 1 /*.6*/, 2 /*.2*/, 3 /*.8*/, 1 /*.4*/};
    for (int x = 0; x < SCREEN_W; ++x) {
        hsrc[x] = (uint8_t)((x * 8) / 5);
        hmode[x] = hm[x % 5];
    }
    for (int l = 0; l < 240; ++l)
        vact[l] = V_SKIP;
    static const uint8_t va[7] = {V_DIRECT, V_STORE, V_BLEND_CUR, V_STORE,
                                  V_BLEND_EVEN, V_STORE, V_BLEND_STORED};
    for (int l = 8; l < 232; ++l)
        vact[l] = va[(l - 8) % 7];
}

// ------------------------------------------------------------------
// Audio: InfoNES APU -> mono ring buffer -> pico-extras I2S
// ------------------------------------------------------------------

#define ARING_BITS 12
#define ARING_LEN (1u << ARING_BITS)
#define ARING_MASK (ARING_LEN - 1)

static int16_t aring[ARING_LEN];
static volatile uint32_t a_head, a_tail;
static struct audio_buffer_pool *apool;

static void audio_init(void) {
    static audio_format_t fmt;
    fmt.sample_freq = AUDIO_RATE;
    fmt.format = AUDIO_BUFFER_FORMAT_PCM_S16;
    fmt.channel_count = 1;

    static struct audio_buffer_format pfmt;
    pfmt.format = &fmt;
    pfmt.sample_stride = 2;

    apool = audio_new_producer_pool(&pfmt, 3, 576);

    struct audio_i2s_config cfg;
    cfg.data_pin = PICO_AUDIO_I2S_DATA_PIN;
    cfg.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
    cfg.dma_channel = 0;
    cfg.pio_sm = 0;

    if (!audio_i2s_setup(&fmt, &cfg))
        panic("audio_i2s_setup failed");
    audio_i2s_connect(apool);
    audio_i2s_set_enabled(true);
}

static void __not_in_flash_func(pump_audio)(void) {
    while (a_head != a_tail) {
        audio_buffer_t *b = take_audio_buffer(apool, false);
        if (!b)
            return;
        int16_t *out = (int16_t *)b->buffer->bytes;
        uint32_t avail = a_head - a_tail;
        uint32_t n = b->max_sample_count;
        if (n > avail) n = avail;
        for (uint32_t i = 0; i < n; ++i)
            out[i] = aring[(a_tail + i) & ARING_MASK];
        a_tail += n;
        b->sample_count = n;
        give_audio_buffer(apool, b);
    }
}

int __not_in_flash_func(InfoNES_GetSoundBufferSize)() {
    return (int)(ARING_LEN - (a_head - a_tail));
}

void __not_in_flash_func(InfoNES_SoundOutput)(int samples, BYTE *wave1, BYTE *wave2,
                                              BYTE *wave3, BYTE *wave4, BYTE *wave5) {
    static int32_t dc;
    int space = (int)(ARING_LEN - (a_head - a_tail));
    if (samples > space)
        samples = space;
    while (samples-- > 0) {
        // Channel weights from pico-infones; max is 255*128 = 32640.
        int32_t m = *wave1++ * 4 + *wave2++ * 4 + *wave3++ * 5 +
                    *wave4++ * 51 + *wave5++ * 64;
        // One-pole high-pass to strip the DC offset (MAX98357A is DC-coupled).
        dc += (m - dc) >> 9;
        int32_t s = m - dc;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        aring[a_head & ARING_MASK] = (int16_t)s;
        ++a_head;
    }
}

void InfoNES_SoundInit(void) {}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate) {
    printf("APU: %d samples/sync @ %d Hz\n", samples_per_sync, sample_rate);
    if (sample_rate != AUDIO_RATE)
        printf("WARNING: APU rate %d != I2S rate %d\n", sample_rate, AUDIO_RATE);
    return 0;
}

void InfoNES_SoundClose(void) {}

// ------------------------------------------------------------------
// Video callbacks
// ------------------------------------------------------------------

void __not_in_flash_func(InfoNES_PreDrawLine)(int line) {
    InfoNES_SetLineBuffer((line & 1) ? line_b : line_a, 256);
}

void __not_in_flash_func(InfoNES_PostDrawLine)(int line) {
    if ((unsigned)line >= 240)
        return;
    uint8_t act = vact[line];
    if (act == V_SKIP)
        return;
    const WORD *src = (line & 1) ? line_b : line_a;

    // Horizontal 256 -> 160 with weighted blending.
    for (int x = 0; x < SCREEN_W; ++x) {
        uint16_t a = src[hsrc[x]];
        uint16_t v;
        switch (hmode[x]) {
        default:
            v = a;
            break;
        case 1:
            v = avg565(a, src[hsrc[x] + 1]);
            break;
        case 2: {
            uint16_t b = src[hsrc[x] + 1];
            v = avg565(a, avg565(a, b));
            break;
        }
        case 3: {
            uint16_t b = src[hsrc[x] + 1];
            v = avg565(b, avg565(a, b));
            break;
        }
        }
        hbuf[x] = v;
    }

    if (act == V_STORE) {
        memcpy(vstore, hbuf, sizeof(vstore));
        return;
    }

    // Vertical blend (where needed), byte-swap for the wire, then DMA out.
    dma_channel_wait_for_finish_blocking(lcd_dma);
    switch (act) {
    case V_DIRECT:
        for (int x = 0; x < SCREEN_W; ++x)
            out_line[x] = __builtin_bswap16(hbuf[x]);
        break;
    case V_BLEND_EVEN:
        for (int x = 0; x < SCREEN_W; ++x)
            out_line[x] = __builtin_bswap16(avg565(vstore[x], hbuf[x]));
        break;
    case V_BLEND_CUR:
        for (int x = 0; x < SCREEN_W; ++x) {
            uint16_t m = avg565(vstore[x], hbuf[x]);
            out_line[x] = __builtin_bswap16(avg565(hbuf[x], m));
        }
        break;
    case V_BLEND_STORED:
        for (int x = 0; x < SCREEN_W; ++x) {
            uint16_t m = avg565(vstore[x], hbuf[x]);
            out_line[x] = __builtin_bswap16(avg565(vstore[x], m));
        }
        break;
    }
    dma_channel_set_trans_count(lcd_dma, SCREEN_W * 2, false);
    dma_channel_set_read_addr(lcd_dma, out_line, true);
}

void __not_in_flash_func(InfoNES_LoadFrame)() {
    // Reopen the window for the next frame's scanline stream.
    lcd_begin_frame();

    pump_audio();

    // Heartbeat on USB serial (~every 10 s) so a connected PC can see that
    // emulation is alive.
    static uint32_t frames;
    if ((++frames % 600) == 0)
        printf("sprigNES: running, frame %u\n", (unsigned)frames);

    // Pace to ~60 fps.
    static absolute_time_t next;
    if (is_nil_time(next))
        next = get_absolute_time();
    next = delayed_by_us(next, 16667);
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(next, now) > 33000) {
        next = now;  // fell way behind, resync instead of fast-forwarding
    } else {
        while (absolute_time_diff_us(get_absolute_time(), next) > 0) {
            pump_audio();
            tight_loop_contents();
        }
    }
}

// ------------------------------------------------------------------
// Input
// ------------------------------------------------------------------

void __not_in_flash_func(InfoNES_PadState)(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem) {
    enum {
        NES_A = 1 << 0, NES_B = 1 << 1, NES_SELECT = 1 << 2, NES_START = 1 << 3,
        NES_UP = 1 << 4, NES_DOWN = 1 << 5, NES_LEFT = 1 << 6, NES_RIGHT = 1 << 7,
    };

    uint32_t g = gpio_get_all();
    #define DOWN(i) (!(g & (1u << BTN_PINS[i])))

    DWORD v = (DOWN(BTN_W) ? NES_UP : 0) |
              (DOWN(BTN_S) ? NES_DOWN : 0) |
              (DOWN(BTN_A) ? NES_LEFT : 0) |
              (DOWN(BTN_D) ? NES_RIGHT : 0) |
              (DOWN(BTN_L) ? NES_A : 0) |
              (DOWN(BTN_J) ? NES_B : 0) |
              (DOWN(BTN_I) ? NES_START : 0) |
              (DOWN(BTN_K) ? NES_SELECT : 0);

    // All four right-hand buttons held = reset the game.
    bool reset = DOWN(BTN_I) && DOWN(BTN_J) && DOWN(BTN_K) && DOWN(BTN_L);
    #undef DOWN

    *pdwPad1 = v;
    *pdwPad2 = 0;
    *pdwSystem = reset ? PAD_SYS_QUIT : 0;
}

// ------------------------------------------------------------------
// ROM handling
// ------------------------------------------------------------------

static bool parse_rom(const uint8_t *f) {
    memcpy(&NesHeader, f, sizeof(NesHeader));
    if (memcmp(NesHeader.byID, "NES\x1a", 4) != 0)
        return false;
    f += sizeof(NesHeader);

    memset(SRAM, 0, SRAM_SIZE);

    if (NesHeader.byInfo1 & 4) {  // trainer
        memcpy(&SRAM[0x1000], f, 512);
        f += 512;
    }

    ROM = (BYTE *)f;  // PRG runs straight out of QSPI flash (XIP)
    f += (uint32_t)NesHeader.byRomSize * 0x4000;

    if (NesHeader.byVRomSize > 0)
        VROM = (BYTE *)f;

    return true;
}

// Shown when there is no valid ROM at ROM_FLASH_OFFSET. Paints three vertical
// bands — encoded as RED | GREEN | BLUE left-to-right — and reports the flash
// contents over USB serial once a second. (The band order doubles as a display
// channel-order test: if you see blue on the LEFT, the panel wants R/B swapped.)
static void rom_error_screen(void) {
    lcd_begin_frame();
    for (int y = 0; y < SCREEN_H; ++y) {
        for (int x = 0; x < SCREEN_W; ++x) {
            uint16_t c = (x < 53) ? 0x001F : (x < 107) ? 0x07E0 : 0xF800; /* BGR565 R|G|B */
            const uint8_t px[2] = {(uint8_t)(c >> 8), (uint8_t)c};
            spi_write_blocking(TFT_SPI, px, 2);
        }
    }
    gpio_init(PIN_LED_L);
    gpio_set_dir(PIN_LED_L, GPIO_OUT);
    while (true) {
        printf("sprigNES: no valid ROM at %p. flash bytes: %02x %02x %02x %02x "
               "(expected 4e 45 53 1a) - flash a game with SprigNESFlasher\n",
               (const void *)nes_rom, nes_rom[0], nes_rom[1], nes_rom[2],
               nes_rom[3]);
        gpio_xor_mask(1u << PIN_LED_L);
        sleep_ms(1000);
    }
}

int InfoNES_Menu() {
    if (!parse_rom(nes_rom))
        rom_error_screen();  // never returns
    if (InfoNES_Reset() < 0) {
        printf("InfoNES_Reset failed\n");
        return -1;
    }
    return 0;
}

int InfoNES_ReadRom(const char *) { return -1; }  // unused, ROM is embedded

void InfoNES_ReleaseRom() {
    ROM = NULL;
    VROM = NULL;
}

void InfoNES_MessageBox(const char *pszMsg, ...) {
    va_list args;
    va_start(args, pszMsg);
    vprintf(pszMsg, args);
    va_end(args);
    printf("\n");
}

void InfoNES_DebugPrint(const char *pszMsg) { printf("%s\n", pszMsg); }

// ------------------------------------------------------------------

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(252000, true);

    stdio_init_all();
    printf("sprigNES starting, rom at %p\n", (const void *)nes_rom);

    for (int i = 0; i < 8; ++i) {
        gpio_init(BTN_PINS[i]);
        gpio_set_dir(BTN_PINS[i], GPIO_IN);
        gpio_pull_up(BTN_PINS[i]);
    }

    lcd_init();
    lcd_fill(P(0x0000));  // black
#if SPRIG_COLOR_TEST
    color_test();
#endif
    audio_init();

    lcd_begin_frame();  // window must be open before the first scanline lands
    InfoNES_Main();

    return 0;
}
