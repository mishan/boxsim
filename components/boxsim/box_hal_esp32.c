/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* box_hal.h on the ESP32-S3-BOX itself, its parts as Espressif's esp-box
   BSP drives them. */
#include "box_hal.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#define STRIP 16                /* rows the SPI sends at most at once */

static box_hal_taps taps;
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t imu, touch;
static esp_lcd_panel_handle_t panel;
static esp_codec_dev_handle_t speaker;
static bool side = false;

void
box_hal_tap (const box_hal_taps *t)
{
    taps = *t;
}

/* ---- the speaker: I2S into the ES8311, the amplifier on GPIO 46 ---------- */

static bool
speakerOpen (int rate, int volume)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = 2, .bclk = 17, .ws = 47, .dout = 15, .din = 16 },
    };

    /* Silence, not the last buffers again, once nothing more is written. */
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK ||
        i2s_channel_init_std_mode(tx, &sc) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK)
        return false;

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0,
                                      .addr = ES8311_CODEC_DEFAULT_ADDR,
                                      .bus_handle = bus };
    es8311_codec_cfg_t es = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = 46, .use_mclk = true,
    };
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT,
                               .codec_if = es8311_codec_new(&es),
                               .data_if = audio_codec_new_i2s_data(&i2s_cfg) };
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2,
                                       .sample_rate = rate };
    esp_codec_dev_handle_t out = esp_codec_dev_new(&dc);

    if (!out || esp_codec_dev_open(out, &fs) != ESP_CODEC_DEV_OK)
        return false;
    esp_codec_dev_set_out_vol(out, volume);
    speaker = out;
    return true;
}

void
box_audio_write (const int16_t *frames, int count)
{
    if (taps.written)
        taps.written(frames, count);
    if (speaker)
        esp_codec_dev_write(speaker, (void *)frames, count * 4);
}

/* ---- the LCD, an ILI9342C on SPI ------------------------------------------ */

static bool
screenOpen (void)
{
    spi_bus_config_t sb = {
        .mosi_io_num = GPIO_NUM_6, .miso_io_num = GPIO_NUM_NC, .sclk_io_num = GPIO_NUM_7,
        .quadwp_io_num = GPIO_NUM_NC, .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BOX_HAL_W * STRIP * 2,
    };
    esp_lcd_panel_io_spi_config_t ic = {
        .dc_gpio_num = GPIO_NUM_4, .cs_gpio_num = GPIO_NUM_5, .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .trans_queue_depth = 2,
    };
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = GPIO_NUM_48, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_io_handle_t io;

    if (spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO) != ESP_OK ||
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ic, &io) != ESP_OK ||
        esp_lcd_new_panel_st7789(io, &pc, &panel) != ESP_OK)
        return false;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_mirror(panel, true, true);
    esp_lcd_panel_disp_on_off(panel, true);

    gpio_config_t bl = { .pin_bit_mask = 1ULL << GPIO_NUM_45, .mode = GPIO_MODE_OUTPUT };

    gpio_config(&bl);
    gpio_set_level(GPIO_NUM_45, 1);
    return true;
}

/* Rows by STRIP, the most the SPI was set up to send at once. */
void
box_lcd_draw (int x0, int y0, int x1, int y1, const uint16_t *pixels)
{
    if (taps.drawn)
        taps.drawn(x0, y0, x1, y1, pixels);
    if (panel)
        esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, pixels);
}

/* ---- the touchscreen, a TT21100 at 0x24, and the red button --------------- */

/* The red button under the screen, as its report last said. */
static bool homeDown = false;

/* Read as it asks to be: the length of its report, then the whole report,
   whatever its length. A report left unread is offered again and again,
   and nothing newer behind it, so every one is read through. Finger
   records are ten bytes each after a seven-byte header; a 14-byte report
   is the buttons'. */
int
box_touch_read (box_hal_contact *out, int most)
{
    static uint8_t report[512];
    uint8_t len[2];
    int count = 0;
    const int n = touch && i2c_master_receive(touch, len, 2, 20) == ESP_OK
                ? len[0] | (len[1] << 8) : 0;

    if (n >= 7 && n <= (int)sizeof(report) &&
        i2c_master_receive(touch, report, n, 20) == ESP_OK)
    {
        if (n == 14)
            homeDown = report[5] & 1;
        else
            for (int r = 0; r < (report[5] & 0x1f) && 7 + 10 * (r + 1) <= n; r++)
            {
                const uint8_t *t = report + 7 + 10 * r;

                if ((t[0] & 0x07) != 0 || count >= most)    /* a finger, not a pen */
                    continue;
                out[count].id = t[1] & 0x1f;
                /* Mirrored across the columns, as the screen is drawn. */
                out[count].x = BOX_HAL_W - 1 - (t[2] | (t[3] << 8));
                out[count].y = t[4] | (t[5] << 8);
                count++;
            }
    }
    if (most > 0 && taps.finger && taps.finger(&out[0].x, &out[0].y))
    {
        out[0].id = 31;
        count = 1;
    }
    return count;
}

/* ---- the buttons: the side button is BOOT, low while pressed -------------- */

bool
box_button_down (int button)
{
    const bool down = button == BOX_HAL_SIDE ? side && gpio_get_level(GPIO_NUM_0) == 0
                                             : homeDown;

    return down || (taps.held && taps.held(button));
}

/* ---- the IMU, an ICM-42607 at 0x68 ---------------------------------------- */

bool
box_imu_read (float accel[3], float gyro[3])
{
    uint8_t reg = 0x0b, d[12];

    if (!imu || i2c_master_transmit_receive(imu, &reg, 1, d, sizeof(d), 20) != ESP_OK)
        return false;
    for (int k = 0; k < 3; k++)
    {
        /* At its defaults: 2048 to the g, 16.4 to the degree a second. */
        accel[k] = (int16_t)((d[2 * k] << 8) | d[2 * k + 1]) / 2048.0f;
        gyro[k] = (int16_t)((d[6 + 2 * k] << 8) | d[7 + 2 * k]) / 16.4f;
    }
    if (taps.jolt && taps.jolt())
        for (int k = 0; k < 3; k++)
            accel[k] *= 3;
    return true;
}

/* ---- all of it ------------------------------------------------------------ */

unsigned
box_hal_open (unsigned want, int rate, int volume)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_8, .scl_io_num = GPIO_NUM_18,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t ic = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .scl_speed_hz = 400000 };
    gpio_config_t button = { .pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE };
    unsigned opened = 0;

    if ((want & BOX_HAL_SIDE_BUTTON) && gpio_config(&button) == ESP_OK)
    {
        side = true;
        opened |= BOX_HAL_SIDE_BUTTON;
    }
    if (!bus && i2c_new_master_bus(&bc, &bus) != ESP_OK)
        return opened;
    if ((want & BOX_HAL_SPEAKER) && speakerOpen(rate, volume))
        opened |= BOX_HAL_SPEAKER;
    if (want & BOX_HAL_IMU)
    {
        /* Accelerometer and gyro on, low noise. */
        uint8_t on[2] = { 0x1f, 0x0f };

        ic.device_address = 0x68;
        if (i2c_master_bus_add_device(bus, &ic, &imu) == ESP_OK &&
            i2c_master_transmit(imu, on, 2, 100) == ESP_OK)
            opened |= BOX_HAL_IMU;
    }
    if (want & BOX_HAL_TOUCH)
    {
        ic.device_address = 0x24;
        if (i2c_master_bus_add_device(bus, &ic, &touch) == ESP_OK)
            opened |= BOX_HAL_TOUCH;
    }
    if ((want & BOX_HAL_SCREEN) && screenOpen())
        opened |= BOX_HAL_SCREEN;
    return opened;
}
