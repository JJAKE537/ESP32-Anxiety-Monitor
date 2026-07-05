#include "app_lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_TOUCH";

#ifndef APP_LVGL_TOUCH_I2C_PORT
#define APP_LVGL_TOUCH_I2C_PORT I2C_NUM_1
#endif

#ifndef APP_LVGL_TOUCH_I2C_SDA
#define APP_LVGL_TOUCH_I2C_SDA GPIO_NUM_7
#endif

#ifndef APP_LVGL_TOUCH_I2C_SCL
#define APP_LVGL_TOUCH_I2C_SCL GPIO_NUM_8
#endif

#ifndef APP_LVGL_TOUCH_INT
#define APP_LVGL_TOUCH_INT GPIO_NUM_NC
#endif

#ifndef APP_LVGL_TOUCH_RST
#define APP_LVGL_TOUCH_RST GPIO_NUM_NC
#endif

#ifndef APP_LVGL_TOUCH_I2C_FREQ_HZ
#define APP_LVGL_TOUCH_I2C_FREQ_HZ 400000
#endif

#ifndef APP_LVGL_TOUCH_I2C_ADDR
#define APP_LVGL_TOUCH_I2C_ADDR 0
#endif

#ifndef APP_LVGL_TOUCH_SWAP_XY
#define APP_LVGL_TOUCH_SWAP_XY 0
#endif
/*
#define APP_LVGL_TOUCH_MIRROR_X 0
#define APP_LVGL_TOUCH_MIRROR_Y 0

#define APP_LVGL_TOUCH_MIRROR_X 1       x/y轴方向 与上边的是反的
#define APP_LVGL_TOUCH_MIRROR_Y 1
*/
#ifndef APP_LVGL_TOUCH_MIRROR_X
#define APP_LVGL_TOUCH_MIRROR_X 1
#endif

#ifndef APP_LVGL_TOUCH_MIRROR_Y
#define APP_LVGL_TOUCH_MIRROR_Y 1
#endif

#define APP_LCD_H_RES 1024
#define APP_LCD_V_RES 600

static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_indev;
static int64_t s_last_touch_log_us;

static uint8_t find_gt911_addr(i2c_master_bus_handle_t bus)
{
    static const uint8_t candidates[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,        // 0x5D
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, // 0x14
    };

    for (int retry = 0; retry < 10; retry++) {
        for (size_t i = 0; i < sizeof(candidates); i++) {
            esp_err_t ret = i2c_master_probe(bus, candidates[i], pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "GT911 probe addr=0x%02x retry=%d: %s",
                     candidates[i], retry, esp_err_to_name(ret));

            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "GT911 found at 0x%02x", candidates[i]);
                return candidates[i];
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "GT911 not found on SDA=%d SCL=%d", APP_LVGL_TOUCH_I2C_SDA, APP_LVGL_TOUCH_I2C_SCL);
    return 0;
}

static void app_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    if (s_touch == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t point[1] = {0};
    uint8_t point_count = 0;

    if (esp_lcd_touch_get_data(s_touch, point, &point_count, 1) == ESP_OK && point_count > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point[0].x;
        data->point.y = point[0].y;

        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_touch_log_us > 250000) {
            s_last_touch_log_us = now_us;
            ESP_LOGI(TAG, "touch x=%d y=%d strength=%u",
                     (int)data->point.x, (int)data->point.y, point[0].strength);
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t app_lvgl_register_touch(lv_display_t *display)
{
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_INVALID_ARG, TAG, "display is null");
    ESP_RETURN_ON_FALSE(APP_LVGL_TOUCH_I2C_SDA != GPIO_NUM_NC, ESP_ERR_INVALID_ARG, TAG, "SDA not configured");
    ESP_RETURN_ON_FALSE(APP_LVGL_TOUCH_I2C_SCL != GPIO_NUM_NC, ESP_ERR_INVALID_ARG, TAG, "SCL not configured");

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = APP_LVGL_TOUCH_I2C_PORT,
        .sda_io_num = APP_LVGL_TOUCH_I2C_SDA,
        .scl_io_num = APP_LVGL_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "create i2c bus");

    uint8_t touch_addr = find_gt911_addr(i2c_bus);
if (touch_addr == 0) {
    return ESP_ERR_NOT_FOUND;
}
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = touch_addr;
    io_cfg.scl_speed_hz = APP_LVGL_TOUCH_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io_handle), TAG, "create touch io");

    static esp_lcd_touch_io_gt911_config_t gt911_cfg;
    gt911_cfg.dev_addr = touch_addr;

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = APP_LCD_H_RES,
        .y_max = APP_LCD_V_RES,
        .rst_gpio_num = APP_LVGL_TOUCH_RST,
        .int_gpio_num = APP_LVGL_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = APP_LVGL_TOUCH_SWAP_XY,
            .mirror_x = APP_LVGL_TOUCH_MIRROR_X,
            .mirror_y = APP_LVGL_TOUCH_MIRROR_Y,
        },
        .driver_data = &gt911_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io_handle, &tp_cfg, &s_touch), TAG, "create gt911");

    s_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_indev != NULL, ESP_ERR_NO_MEM, TAG, "create lvgl indev");

    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_indev, display);
    lv_indev_set_read_cb(s_indev, app_touch_read_cb);

    ESP_LOGI(TAG, "GT911 registered addr=0x%02x SDA=%d SCL=%d INT=%d RST=%d swap=%d mx=%d my=%d",
             touch_addr,
             APP_LVGL_TOUCH_I2C_SDA,
             APP_LVGL_TOUCH_I2C_SCL,
             APP_LVGL_TOUCH_INT,
             APP_LVGL_TOUCH_RST,
             APP_LVGL_TOUCH_SWAP_XY,
             APP_LVGL_TOUCH_MIRROR_X,
             APP_LVGL_TOUCH_MIRROR_Y);
    return ESP_OK;
}