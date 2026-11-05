/*
 * Test man hinh TFT 3.2" (ILI9341, 240x320, SPI) tren ESP32 - ESP-IDF
 *
 * TFT CS   -> GPIO27
 * TFT RST  -> GPIO17
 * TFT DC   -> GPIO16
 * TFT MOSI -> GPIO23
 * TFT SCK  -> GPIO18
 * TFT MISO -> GPIO19 (dung chung voi touch)
 * TFT LED  -> 3.3V
 *
 * Touch XPT2046 (dung chung bus SPI voi TFT):
 * T_CLK -> GPIO18, T_DIN -> GPIO23, T_DO -> GPIO19
 * T_CS  -> GPIO25, T_IRQ -> GPIO26
 *
 * Lenh serial (UART0): "relay 4 on/off" (quat), "relay 5 on/off" (khoa)
 */
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_assets.h"

#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_SCK  18
#define PIN_CS   27
#define PIN_DC   16
#define PIN_RST  17
#define PIN_TCS  25 /* T_CS  */
#define PIN_TIRQ 26 /* T_IRQ (active low) */

#define TFT_HOST   SPI3_HOST /* VSPI: GPIO 23/18/19 la chan IOMUX mac dinh */
#define TFT_SPI_HZ (20 * 1000 * 1000)

/* Che do ngang 320x240 */
#define TFT_W 320
#define TFT_H 240

/* MADCTL: 0x28 = ngang, BGR. Neu bi lat/mat guong/sai mau thu: 0x48, 0xE8, 0x88, 0x20... */
#define TFT_MADCTL 0x28

#define CHUNK_LINES 16

/* RGB565 */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     RGB565(255, 0, 0)
#define C_GREEN   RGB565(0, 255, 0)
#define C_BLUE    RGB565(0, 0, 255)
#define C_YELLOW  RGB565(255, 255, 0)
#define C_CYAN    RGB565(0, 255, 255)
#define C_MAGENTA RGB565(255, 0, 255)

static spi_device_handle_t s_spi;
static uint16_t *s_buf; /* buffer DMA: TFT_W * CHUNK_LINES pixel */

/* pre-callback: dat chan DC theo user (0 = lenh, 1 = du lieu) */
static void IRAM_ATTR tft_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_DC, (int)(intptr_t)t->user);
}

static void tft_tx(const void *data, size_t len, int dc)
{
    if (len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
        .user = (void *)(intptr_t)dc,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void tft_cmd(uint8_t cmd)
{
    tft_tx(&cmd, 1, 0);
}

static void tft_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    tft_cmd(cmd);
    tft_tx(data, len, 1);
}

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];

    d[0] = x0 >> 8; d[1] = x0 & 0xFF; d[2] = x1 >> 8; d[3] = x1 & 0xFF;
    tft_cmd_data(0x2A, d, 4);
    d[0] = y0 >> 8; d[1] = y0 & 0xFF; d[2] = y1 >> 8; d[3] = y1 & 0xFF;
    tft_cmd_data(0x2B, d, 4);
    tft_cmd(0x2C); /* ghi RAM */
}

static void tft_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > TFT_W || y + h > TFT_H) {
        return;
    }
    uint16_t c = (color >> 8) | (color << 8); /* big-endian tren day SPI */
    for (int i = 0; i < w * CHUNK_LINES; i++) {
        s_buf[i] = c;
    }
    tft_set_window(x, y, x + w - 1, y + h - 1);
    int remain = h;
    while (remain > 0) {
        int lines = remain > CHUNK_LINES ? CHUNK_LINES : remain;
        tft_tx(s_buf, (size_t)w * lines * 2, 1);
        remain -= lines;
    }
}

static void tft_fill(uint16_t color)
{
    tft_fill_rect(0, 0, TFT_W, TFT_H, color);
}

static void tft_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    spi_bus_config_t bus = {
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_W * CHUNK_LINES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TFT_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = TFT_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 4,
        .pre_cb = tft_pre_cb,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TFT_HOST, &dev, &s_spi));

    s_buf = heap_caps_malloc(TFT_W * CHUNK_LINES * 2, MALLOC_CAP_DMA);
    assert(s_buf);

    /* Reset cung */
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    tft_cmd(0x01); /* Software reset */
    vTaskDelay(pdMS_TO_TICKS(150));

    static const uint8_t pwrb[]   = {0x00, 0xC1, 0x30};
    static const uint8_t pwrseq[] = {0x64, 0x03, 0x12, 0x81};
    static const uint8_t dtca[]   = {0x85, 0x00, 0x78};
    static const uint8_t pwra[]   = {0x39, 0x2C, 0x00, 0x34, 0x02};
    static const uint8_t prc[]    = {0x20};
    static const uint8_t dtcb[]   = {0x00, 0x00};
    static const uint8_t pwr1[]   = {0x23};
    static const uint8_t pwr2[]   = {0x10};
    static const uint8_t vcom1[]  = {0x3E, 0x28};
    static const uint8_t vcom2[]  = {0x86};
    static const uint8_t madctl[] = {TFT_MADCTL};
    static const uint8_t pixfmt[] = {0x55}; /* RGB565 */
    static const uint8_t frmctr[] = {0x00, 0x18};
    static const uint8_t dfunc[]  = {0x08, 0x82, 0x27};
    static const uint8_t gamma3[] = {0x00};
    static const uint8_t gamma[]  = {0x01};
    static const uint8_t gmp[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
    static const uint8_t gmn[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};

    tft_cmd_data(0xCF, pwrb, sizeof(pwrb));
    tft_cmd_data(0xED, pwrseq, sizeof(pwrseq));
    tft_cmd_data(0xE8, dtca, sizeof(dtca));
    tft_cmd_data(0xCB, pwra, sizeof(pwra));
    tft_cmd_data(0xF7, prc, sizeof(prc));
    tft_cmd_data(0xEA, dtcb, sizeof(dtcb));
    tft_cmd_data(0xC0, pwr1, sizeof(pwr1));
    tft_cmd_data(0xC1, pwr2, sizeof(pwr2));
    tft_cmd_data(0xC5, vcom1, sizeof(vcom1));
    tft_cmd_data(0xC7, vcom2, sizeof(vcom2));
    tft_cmd_data(0x36, madctl, sizeof(madctl));
    tft_cmd_data(0x3A, pixfmt, sizeof(pixfmt));
    tft_cmd_data(0xB1, frmctr, sizeof(frmctr));
    tft_cmd_data(0xB6, dfunc, sizeof(dfunc));
    tft_cmd_data(0xF2, gamma3, sizeof(gamma3));
    tft_cmd_data(0x26, gamma, sizeof(gamma));
    tft_cmd_data(0xE0, gmp, sizeof(gmp));
    tft_cmd_data(0xE1, gmn, sizeof(gmn));

    tft_cmd(0x11); /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    tft_cmd(0x29); /* Display on */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ======================= Font 5x7 (bit0 = hang tren) ======================= */
typedef struct {
    char ch;
    uint8_t col[5];
} glyph_t;

static const glyph_t s_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'!', {0x00, 0x00, 0x5F, 0x00, 0x00}}, {'*', {0x14, 0x08, 0x3E, 0x08, 0x14}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *font_get(char c)
{
    c = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == c) {
            return s_font[i].col;
        }
    }
    return s_font[0].col;
}

/* Ve 1 ky tu (o 6s x 8s, co nen bg), scale s */
static void tft_draw_char(int x, int y, char ch, int s, uint16_t fg, uint16_t bg)
{
    int w = 6 * s, h = 8 * s;
    if (x < 0 || y < 0 || x + w > TFT_W || y + h > TFT_H) {
        return;
    }
    const uint8_t *g = font_get(ch);
    uint16_t f = (fg >> 8) | (fg << 8);
    uint16_t b = (bg >> 8) | (bg << 8);
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int col = px / s, row = py / s;
            bool on = col < 5 && row < 7 && ((g[col] >> row) & 1);
            s_buf[py * w + px] = on ? f : b;
        }
    }
    tft_set_window(x, y, x + w - 1, y + h - 1);
    tft_tx(s_buf, (size_t)w * h * 2, 1);
}

/* Ve chuoi can giua theo truc x quanh cx */
static void tft_draw_text_center(int cx, int y, const char *str, int s, uint16_t fg, uint16_t bg)
{
    int n = (int)strlen(str);
    int x = cx - (n * 6 * s) / 2;
    for (int i = 0; i < n; i++) {
        tft_draw_char(x + i * 6 * s, y, str[i], s, fg, bg);
    }
}

/* ======================= Touch XPT2046 ======================= */
static spi_device_handle_t s_touch;

/* Cam ung duoc hieu chinh 2 diem moi lan khoi dong (touch_calibrate) */
#define TOUCH_Z_MIN   50 /* nguong ap luc de coi la dang cham (thap = nhay hon) */
#define TOUCH_USE_IRQ 0   /* 1: dung chan T_IRQ de loc nhanh (chi bat khi T_IRQ xuong thap dung khi cham) */
#define TOUCH_DEBUG   0   /* 1: in gia tri tho ra serial moi 500ms de chan doan day noi */

typedef struct {
    bool swap;
    float ox, kx; /* x = (u - ux1) * kx + ox */
    float oy, ky;
    int ux1, vy1;
} touch_cal_t;

static touch_cal_t s_cal;

static void touch_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_TIRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TCS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TFT_HOST, &dev, &s_touch));
}

static inline bool touch_pressed(void)
{
    return gpio_get_level(PIN_TIRQ) == 0;
}

static int touch_read_ch(uint8_t cmd)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 24,
        .tx_data = {cmd, 0, 0},
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_touch, &t));
    return (((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0x0FFF;
}

static int median5(int *a)
{
    for (int i = 1; i < 5; i++) {
        int v = a[i], j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
    return a[2];
}

/* Doc toa do tho; phat hien cham bang ap luc (Z) thay vi chan T_IRQ */
static bool touch_read_raw(int *rx, int *ry)
{
    if (TOUCH_USE_IRQ && !touch_pressed()) { /* T_IRQ muc cao = khong cham */
        return false;
    }
    touch_read_ch(0xB1); /* bo mau dau */
    int z1 = touch_read_ch(0xB1);
    int z2 = touch_read_ch(0xC1);
    if (z1 + 4095 - z2 < TOUCH_Z_MIN) {
        return false;
    }
    int xs[5], ys[5];
    touch_read_ch(0xD0); /* bo mau dau */
    for (int i = 0; i < 5; i++) {
        xs[i] = touch_read_ch(0xD0);
        ys[i] = touch_read_ch(0x90);
    }
    *rx = median5(xs);
    *ry = median5(ys);
    return *rx > 50 && *rx < 4050 && *ry > 50 && *ry < 4050;
}

#if TOUCH_DEBUG
static void touch_debug_print(void)
{
    touch_read_ch(0xB1);
    int z1 = touch_read_ch(0xB1);
    int z2 = touch_read_ch(0xC1);
    int x = touch_read_ch(0xD0);
    x = touch_read_ch(0xD0);
    int y = touch_read_ch(0x90);
    y = touch_read_ch(0x90);
    //printf("TOUCH irq=%d z1=%d z2=%d x=%d y=%d\n", gpio_get_level(PIN_TIRQ), z1, z2, x, y);
}
#endif

/* Toa do man hinh (pixel); false neu khong cham */
static bool touch_get(int *x, int *y)
{
    int rx, ry;
    if (!touch_read_raw(&rx, &ry)) {
        return false;
    }
    int u = s_cal.swap ? ry : rx;
    int v = s_cal.swap ? rx : ry;
    float fx = (u - s_cal.ux1) * s_cal.kx + s_cal.ox;
    float fy = (v - s_cal.vy1) * s_cal.ky + s_cal.oy;
    *x = fx < 0 ? 0 : (fx > TFT_W - 1 ? TFT_W - 1 : (int)fx);
    *y = fy < 0 ? 0 : (fy > TFT_H - 1 ? TFT_H - 1 : (int)fy);
    return true;
}

static void touch_wait_raw(int *rx, int *ry)
{
    for (;;) {
        while (!touch_read_raw(rx, ry)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(150)); /* cho tin hieu on dinh */
        if (touch_read_raw(rx, ry)) {
            break;
        }
    }
    int a, b;
    while (touch_read_raw(&a, &b)) { /* cho nha tay */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void draw_cross(int x, int y, uint16_t c)
{
    tft_fill_rect(x - 10, y - 1, 21, 3, c);
    tft_fill_rect(x - 1, y - 10, 3, 21, c);
}

/* Hieu chinh 2 diem: goc tren-trai va duoi-phai, chay moi lan khoi dong */
static void touch_calibrate(void)
{
    const int X1 = 20, Y1 = 20, X2 = TFT_W - 20, Y2 = TFT_H - 20;
    int r1x, r1y, r2x, r2y;

    tft_fill(C_BLACK);
    tft_draw_text_center(TFT_W / 2, 100, "TAP THE CROSS", 2, C_WHITE, C_BLACK);
    draw_cross(X1, Y1, C_WHITE);
    touch_wait_raw(&r1x, &r1y);

    tft_fill(C_BLACK);
    tft_draw_text_center(TFT_W / 2, 100, "TAP THE CROSS", 2, C_WHITE, C_BLACK);
    draw_cross(X2, Y2, C_WHITE);
    touch_wait_raw(&r2x, &r2y);

    s_cal.swap = abs(r2x - r1x) < abs(r2y - r1y);
    int u1 = s_cal.swap ? r1y : r1x, u2 = s_cal.swap ? r2y : r2x;
    int v1 = s_cal.swap ? r1x : r1y, v2 = s_cal.swap ? r2x : r2y;
    if (u2 == u1) u2 = u1 + 1;
    if (v2 == v1) v2 = v1 + 1;
    s_cal.ux1 = u1;
    s_cal.vy1 = v1;
    s_cal.ox = X1;
    s_cal.oy = Y1;
    s_cal.kx = (float)(X2 - X1) / (u2 - u1);
    s_cal.ky = (float)(Y2 - Y1) / (v2 - v1);
}

/* ======================= UI + dieu khien ======================= */
#define LOCK_OPEN_MS 10000
#define PASSWORD     "1234"
#define PASS_MAX        8
#define ERR_SHOW_MS     700
#define IDLE_TIMEOUT_MS 20000

typedef enum { ST_IDLE, ST_PASSWORD, ST_MAIN, ST_OPEN } app_state_t;

typedef struct {
    int x, y, w, h;
} rect_t;

/* Anh RGB565 big-endian nam trong flash: xem src/ui_images.c (tao boi tools/make_assets.py) */

static const rect_t KEY_RECT[12] = UI_KEY_RECTS; /* 1..9, CLEAR, 0, DELETE */
static const char KEY_CH[12]     = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 0, '0', 0};
static const rect_t BTN_LOGIN    = UI_LOGIN_RECT;
static const rect_t BTN_FAN      = UI_FAN_RECT;
static const rect_t BTN_LOCK     = UI_UNLOCK_RECT;
static const rect_t BTN_LOGOUT   = UI_LOGOUT_RECT;
static const rect_t BTN_EXIT     = UI_EXIT_RECT;
static const rect_t DOT_BAND     = UI_DOT_BAND;
static const rect_t FAN2_RECT    = UI_FAN2_RECT;
static const rect_t CD_RECT      = UI_CD_RECT;

static app_state_t s_state = ST_PASSWORD;
static bool s_fan_on;
static TickType_t s_lock_deadline;
static int s_lock_secs_shown;
static char s_pass[PASS_MAX + 1];
static int s_pass_len;
static bool s_pass_err;
static TickType_t s_err_deadline;
static TickType_t s_idle_deadline;

/* Gui lenh qua serial (UART0 / cong USB) */
static void relay_send(int n, bool on)
{
    printf("relay %d %s\n", n, on ? "on" : "off");
    fflush(stdout);
}

static bool in_rect(const rect_t *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* ---------- Day anh tu flash ra man hinh ---------- */
/* src: con tro toi pixel (0,0) cua vung, stride: so byte moi hang trong nguon.
 * Flash khong DMA duoc nen copy tung khoi vao s_buf roi gui. */
static void tft_blit(const uint8_t *src, int stride, int x, int y, int w, int h)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > TFT_W || y + h > TFT_H) {
        return;
    }
    int per = (TFT_W * CHUNK_LINES) / w;
    uint8_t *buf = (uint8_t *)s_buf;
    tft_set_window(x, y, x + w - 1, y + h - 1);
    for (int j = 0; j < h; j += per) {
        int rows = h - j > per ? per : h - j;
        for (int k = 0; k < rows; k++) {
            memcpy(buf + (size_t)k * w * 2, src + (size_t)(j + k) * stride, (size_t)w * 2);
        }
        tft_tx(buf, (size_t)w * rows * 2, 1);
    }
}

static void blit_page(const uint8_t *img)
{
    tft_blit(img, TFT_W * 2, 0, 0, TFT_W, TFT_H);
}

static void blit_page_region(const uint8_t *img, const rect_t *r)
{
    tft_blit(img + ((size_t)r->y * TFT_W + r->x) * 2, TFT_W * 2, r->x, r->y, r->w, r->h);
}

static void blit_sprite(const uint8_t *spr, const rect_t *r)
{
    tft_blit(spr, r->w * 2, r->x, r->y, r->w, r->h);
}

/* ---------- Page 1: nhap PIN ---------- */
/* Ve cac cham PIN (khu rang cua) len vung nen cua anh page1 */
static void pass_draw_dots(void)
{
    const rect_t *b = &DOT_BAND;
    uint8_t *buf = (uint8_t *)s_buf;
    for (int j = 0; j < b->h; j++) {
        memcpy(buf + (size_t)j * b->w * 2, img_page1 + ((size_t)(b->y + j) * TFT_W + b->x) * 2, (size_t)b->w * 2);
    }

    const int fr = s_pass_err ? 255 : 216, fg = s_pass_err ? 80 : 238, fb = s_pass_err ? 80 : 252;
    for (int i = 0; i < s_pass_len; i++) {
        float cx = UI_DOT_CX + (i - (s_pass_len - 1) / 2.0f) * UI_DOT_PITCH;
        float cy = UI_DOT_CY;
        for (int py = (int)(cy - UI_DOT_R - 1); py <= (int)(cy + UI_DOT_R + 1); py++) {
            for (int px = (int)(cx - UI_DOT_R - 1); px <= (int)(cx + UI_DOT_R + 1); px++) {
                if (px < b->x || px >= b->x + b->w || py < b->y || py >= b->y + b->h) {
                    continue;
                }
                float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
                float a = UI_DOT_R + 0.5f - sqrtf(dx * dx + dy * dy);
                if (a <= 0) {
                    continue;
                }
                if (a > 1) {
                    a = 1;
                }
                uint8_t *p = buf + ((size_t)(py - b->y) * b->w + (px - b->x)) * 2;
                uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
                int r = ((v >> 11) & 31) << 3, g = ((v >> 5) & 63) << 2, bl = (v & 31) << 3;
                r += (int)((fr - r) * a);
                g += (int)((fg - g) * a);
                bl += (int)((fb - bl) * a);
                v = RGB565(r, g, bl);
                p[0] = (uint8_t)(v >> 8);
                p[1] = (uint8_t)(v & 0xFF);
            }
        }
    }
    tft_set_window(b->x, b->y, b->x + b->w - 1, b->y + b->h - 1);
    tft_tx(buf, (size_t)b->w * b->h * 2, 1);
}

static void pass_draw_screen(void)
{
    blit_page(img_page1);
    pass_draw_dots();
}

static void pass_reset(void)
{
    s_pass_len = 0;
    s_pass[0] = 0;
    s_pass_err = false;
}

static void ui_draw_all(void);

static void pass_handle_tap(int x, int y)
{
    if (in_rect(&BTN_LOGIN, x, y)) {
        if (strcmp(s_pass, PASSWORD) == 0) {
            pass_reset();
            s_state = ST_MAIN;
            ui_draw_all();
        } else {
            s_pass_err = true;
            s_err_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ERR_SHOW_MS);
            pass_draw_dots();
        }
        return;
    }
    for (int i = 0; i < 12; i++) {
        if (!in_rect(&KEY_RECT[i], x, y)) {
            continue;
        }
        if (s_pass_err) {
            pass_reset();
        }
        if (i == 9) {
            pass_reset(); /* CLEAR */
        } else if (i == 11) {
            if (s_pass_len > 0) { /* DELETE */
                s_pass[--s_pass_len] = 0;
            }
        } else if (s_pass_len < PASS_MAX) {
            s_pass[s_pass_len++] = KEY_CH[i];
            s_pass[s_pass_len] = 0;
        }
        pass_draw_dots();
        return;
    }
}

/* ---------- Page 2: dieu khien ---------- */
static void render_fan(void)
{
    if (s_fan_on) {
        blit_page_region(img_page2, &FAN2_RECT);
    } else {
        blit_sprite(img_fan2_off, &FAN2_RECT);
    }
}

static void ui_draw_all(void)
{
    blit_page(img_page2);
    if (!s_fan_on) {
        blit_sprite(img_fan2_off, &FAN2_RECT);
    }
}

/* ---------- Page 3: mo cua 10 giay ---------- */
static void open_draw_secs(void)
{
    int k = s_lock_secs_shown;
    if (k < 0) {
        k = 0;
    } else if (k > 10) {
        k = 10;
    }
    blit_sprite(img_cd + (size_t)k * CD_RECT.w * CD_RECT.h * 2, &CD_RECT);
}

static void open_draw_screen(void)
{
    blit_page(img_page3); /* popup lon, kem vong dem nguoc k = 10 */
    open_draw_secs();
}

static void main_handle_tap(int x, int y)
{
    if (in_rect(&BTN_FAN, x, y)) {
        s_fan_on = !s_fan_on;
        relay_send(4, s_fan_on);
        render_fan();
    } else if (in_rect(&BTN_LOCK, x, y)) {
        relay_send(5, true);
        s_lock_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LOCK_OPEN_MS);
        s_lock_secs_shown = LOCK_OPEN_MS / 1000;
        s_state = ST_OPEN;
        open_draw_screen();
    } else if (in_rect(&BTN_LOGOUT, x, y)) {
        pass_reset();
        s_state = ST_PASSWORD;
        pass_draw_screen();
    }
}

/* ---------- Page 0: man hinh cho (screensaver) ---------- */
static void idle_draw_screen(void)
{
    blit_page(img_page0);
}

static void idle_handle_tap(void)
{
    pass_reset();
    s_state = ST_PASSWORD;
    pass_draw_screen();
}

static void open_handle_tap(int x, int y)
{
    if (in_rect(&BTN_EXIT, x, y)) {
        relay_send(5, false);
        s_state = ST_MAIN;
        s_idle_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_TIMEOUT_MS);
        ui_draw_all();
    }
}

static void open_tick(void)
{
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_lock_deadline) >= 0) {
        relay_send(5, false);
        s_state = ST_MAIN;
        s_idle_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_TIMEOUT_MS);
        ui_draw_all();
        return;
    }
    int remain_ms = (int)((s_lock_deadline - now) * portTICK_PERIOD_MS);
    int secs = (remain_ms + 999) / 1000;
    if (secs != s_lock_secs_shown) {
        s_lock_secs_shown = secs;
        open_draw_secs();
    }
}

static void pass_tick(void)
{
    if (s_pass_err && (int32_t)(xTaskGetTickCount() - s_err_deadline) >= 0) {
        pass_reset();
        pass_draw_dots();
    }
}

void app_main(void)
{
    tft_init();
    touch_init();
    touch_calibrate();
    esp_log_level_set("*", ESP_LOG_NONE); /* tranh log lan vao lenh serial */
    s_idle_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_TIMEOUT_MS);
    pass_draw_screen();

    bool touching = false;
    int released_cnt = 0;
    int dbg_cnt = 0;

    while (1) {
#if TOUCH_DEBUG
        if (++dbg_cnt >= 100) { /* ~500ms */
            dbg_cnt = 0;
            touch_debug_print();
        }
#else
        (void)dbg_cnt;
#endif
        int x, y;
        if (touch_get(&x, &y)) {
            released_cnt = 0;
            s_idle_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_TIMEOUT_MS);
            if (!touching) {
                touching = true;
                if (s_state == ST_IDLE) {
                    idle_handle_tap();
                } else if (s_state == ST_PASSWORD) {
                    pass_handle_tap(x, y);
                } else if (s_state == ST_MAIN) {
                    main_handle_tap(x, y);
                } else if (s_state == ST_OPEN) {
                    open_handle_tap(x, y);
                }
            }
        } else if (touching && ++released_cnt >= 6) { /* chong doi nut ~30ms */
            touching = false;
        }

        if (s_state == ST_OPEN) {
            open_tick();
        } else if (s_state == ST_PASSWORD) {
            pass_tick();
        }

        /* 20 giay khong cham man hinh -> quay ve man hinh cho Page 0 (ST_IDLE) */
        if (s_state != ST_IDLE && s_state != ST_OPEN) {
            if ((int32_t)(xTaskGetTickCount() - s_idle_deadline) >= 0) {
                if (s_state == ST_PASSWORD) {
                    pass_reset();
                }
                s_state = ST_IDLE;
                idle_draw_screen();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
