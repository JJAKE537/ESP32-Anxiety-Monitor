#include "app_lvgl.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "app_config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

LV_FONT_DECLARE(app_ui_font_16);

#define APP_LCD_H_RES      1024
#define APP_UI_HEADER_H    42
#define APP_UI_NAV_Y       548
#define APP_UI_PAGE_H      (APP_UI_NAV_Y - APP_UI_HEADER_H - 6)

#ifndef APP_SYMBOL_INFO
#define APP_SYMBOL_INFO "\xEF\x81\x9A"
#endif

#ifndef APP_SYMBOL_SHIELD
#define APP_SYMBOL_SHIELD "\xEF\x8F\xAD"
#endif

static lv_style_t s_style_page;
static lv_style_t s_style_card;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_active;
static lv_style_t s_style_btn_danger;
static lv_style_t s_style_muted;
static lv_style_t s_style_cyan;
static lv_style_t s_style_green;
static lv_style_t s_style_red;
static bool s_style_inited;

static lv_obj_t *s_page;

static lv_obj_t *s_history_total_label;
static lv_obj_t *s_mem_used_label;
static lv_obj_t *s_mem_free_label;
static lv_obj_t *s_refresh_time_label;

static lv_obj_t *s_log_switch;
static lv_obj_t *s_log_keep_dropdown;
static lv_obj_t *s_log_used_label;
static lv_obj_t *s_log_status_label;

static lv_obj_t *s_low_slider;
static lv_obj_t *s_mid_slider;
static lv_obj_t *s_high_slider;
static lv_obj_t *s_low_right_label;
static lv_obj_t *s_mid_left_label;
static lv_obj_t *s_mid_right_label;
static lv_obj_t *s_high_left_label;

static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_sleep_dropdown;
static lv_obj_t *s_theme_dropdown;

static lv_timer_t *s_refresh_timer;
static bool s_threshold_updating;

static const lv_font_t *ui_font(void)
{
    return &app_ui_font_16;
}

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static void apply_config_theme_styles(void)
{
    app_lvgl_theme_palette_t p;
    app_lvgl_theme_get_palette(&p);

    lv_style_set_bg_color(&s_style_page, c(p.bg));
    lv_style_set_bg_grad_color(&s_style_page, c(p.bg_grad));
    lv_style_set_text_color(&s_style_page, c(p.text));

    lv_style_set_bg_color(&s_style_card, c(p.card));
    lv_style_set_bg_opa(&s_style_card, p.card_opa);
    lv_style_set_border_color(&s_style_card, c(p.card_border));
    lv_style_set_text_color(&s_style_card, c(p.text));

    lv_style_set_bg_color(&s_style_btn, c(p.nav));
    lv_style_set_bg_opa(&s_style_btn, p.nav_opa);
    lv_style_set_border_color(&s_style_btn, c(p.nav_border));
    lv_style_set_text_color(&s_style_btn, c(p.text));

    lv_style_set_bg_color(&s_style_btn_active, c(p.nav_active));
    lv_style_set_bg_grad_color(&s_style_btn_active, c(p.accent));
    lv_style_set_border_color(&s_style_btn_active, c(p.nav_active_border));
    lv_style_set_text_color(&s_style_btn_active, c(p.text));

   app_lvgl_display_settings_t st;
    app_lvgl_display_get_settings(&st);

    if (st.theme_mode == APP_LVGL_THEME_LIGHT) {
        lv_style_set_bg_color(&s_style_btn_danger, c(0xd93025));
        lv_style_set_bg_grad_color(&s_style_btn_danger, c(0xb3261e));
        lv_style_set_bg_grad_dir(&s_style_btn_danger, LV_GRAD_DIR_VER);
        lv_style_set_bg_opa(&s_style_btn_danger, LV_OPA_COVER);
        lv_style_set_border_width(&s_style_btn_danger, 1);
        lv_style_set_border_color(&s_style_btn_danger, c(0x8c1d18));
        lv_style_set_text_color(&s_style_btn_danger, c(0xffffff));
    } else {
        lv_style_set_bg_color(&s_style_btn_danger, c(p.danger));
        lv_style_set_bg_grad_color(&s_style_btn_danger, c(p.danger));
        lv_style_set_bg_grad_dir(&s_style_btn_danger, LV_GRAD_DIR_VER);
        lv_style_set_bg_opa(&s_style_btn_danger, LV_OPA_COVER);
        lv_style_set_border_width(&s_style_btn_danger, 1);
        lv_style_set_border_color(&s_style_btn_danger, c(p.danger));
        lv_style_set_text_color(&s_style_btn_danger, c(0xffffff));
    }

    lv_style_set_text_color(&s_style_muted, c(p.muted));
    lv_style_set_text_color(&s_style_cyan, c(p.cyan));
    lv_style_set_text_color(&s_style_green, c(p.good));
    lv_style_set_text_color(&s_style_red, c(p.danger));
}

void app_ui_config_apply_theme(void)
{
    if (!s_style_inited) {
        return;
    }

    apply_config_theme_styles();
}

static void init_styles(void)
{
    if (s_style_inited) return;
    s_style_inited = true;

    lv_style_init(&s_style_page);
    lv_style_set_bg_color(&s_style_page, c(0x061625));
    lv_style_set_bg_grad_color(&s_style_page, c(0x08263a));
    lv_style_set_bg_grad_dir(&s_style_page, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_style_page, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_page, c(0xf4f8ff));
    lv_style_set_text_font(&s_style_page, ui_font());

    lv_style_init(&s_style_card);
    lv_style_set_radius(&s_style_card, 6);
    lv_style_set_bg_color(&s_style_card, c(0x082035));
    lv_style_set_bg_opa(&s_style_card, LV_OPA_80);
    lv_style_set_border_color(&s_style_card, c(0x1aa2ff));
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_pad_all(&s_style_card, 10);
    lv_style_set_text_color(&s_style_card, c(0xf4f8ff));
    lv_style_set_text_font(&s_style_card, ui_font());

    lv_style_init(&s_style_btn);
    lv_style_set_radius(&s_style_btn, 6);
    lv_style_set_bg_color(&s_style_btn, c(0x08243a));
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_80);
    lv_style_set_border_width(&s_style_btn, 1);
    lv_style_set_border_color(&s_style_btn, c(0x2b8fd8));
    lv_style_set_text_color(&s_style_btn, c(0xf4f8ff));
    lv_style_set_text_font(&s_style_btn, ui_font());

    lv_style_init(&s_style_btn_active);
    lv_style_set_radius(&s_style_btn_active, 6);
    lv_style_set_bg_color(&s_style_btn_active, c(0x0b3f8f));
    lv_style_set_bg_grad_color(&s_style_btn_active, c(0x1269c8));
    lv_style_set_bg_grad_dir(&s_style_btn_active, LV_GRAD_DIR_VER);
    lv_style_set_border_width(&s_style_btn_active, 1);
    lv_style_set_border_color(&s_style_btn_active, c(0x66ccff));
    lv_style_set_text_color(&s_style_btn_active, c(0xffffff));
    lv_style_set_text_font(&s_style_btn_active, ui_font());

    lv_style_init(&s_style_btn_danger);
    lv_style_set_radius(&s_style_btn_danger, 6);
    lv_style_set_bg_color(&s_style_btn_danger, c(0x4a1420));
    lv_style_set_bg_grad_color(&s_style_btn_danger, c(0xa8202c));
    lv_style_set_bg_grad_dir(&s_style_btn_danger, LV_GRAD_DIR_VER);
    lv_style_set_border_width(&s_style_btn_danger, 1);
    lv_style_set_border_color(&s_style_btn_danger, c(0xff3333));
    lv_style_set_text_color(&s_style_btn_danger, c(0xffffff));
    lv_style_set_text_font(&s_style_btn_danger, ui_font());

    lv_style_init(&s_style_muted);
    lv_style_set_text_color(&s_style_muted, c(0xc9d7ea));

    lv_style_init(&s_style_cyan);
    lv_style_set_text_color(&s_style_cyan, c(0x2ceaff));

    lv_style_init(&s_style_green);
    lv_style_set_text_color(&s_style_green, c(0x9cff3a));

    lv_style_init(&s_style_red);
    lv_style_set_text_color(&s_style_red, c(0xff3434));
        apply_config_theme_styles();
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}

static void make_title(lv_obj_t *parent, const char *text)
{
    make_label(parent, text, 10, 8);
}
static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_style_card, 0);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                             int32_t w, int32_t h, const lv_style_t *style)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, style, 0);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_center(label);
    return btn;
}

static void metric_row(lv_obj_t *parent, const char *name, const char *init,
                       int32_t y, lv_obj_t **out)
{
    lv_obj_t *key = make_label(parent, name, 10, y);
    lv_obj_set_width(key, 128);

    lv_obj_t *val = make_label(parent, init ? init : "--", 132, y);
    lv_obj_set_width(val, 126);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_style(val, &s_style_cyan, 0);

    if (out) *out = val;
}

static void format_bytes(char *buf, size_t len, size_t bytes)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024U) {
        snprintf(buf, len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, len, "%u B", (unsigned)bytes);
    }
}

static void set_slider_common_style(lv_obj_t *slider, uint32_t color)
{
    lv_obj_set_style_bg_color(slider, c(0x163c5b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, c(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, c(0xe9eef7), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, c(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
}

static void refresh_threshold_view(void)
{
    app_lvgl_anxiety_thresholds_t th;
    app_lvgl_get_anxiety_thresholds(&th);

    s_threshold_updating = true;

    lv_slider_set_start_value(s_low_slider, 0, LV_ANIM_OFF);
    lv_slider_set_value(s_low_slider, th.low_max, LV_ANIM_OFF);

    lv_slider_set_start_value(s_mid_slider, th.low_max, LV_ANIM_OFF);
    lv_slider_set_value(s_mid_slider, th.mid_max, LV_ANIM_OFF);

    lv_slider_set_start_value(s_high_slider, th.mid_max, LV_ANIM_OFF);
    lv_slider_set_value(s_high_slider, 100, LV_ANIM_OFF);

    lv_label_set_text_fmt(s_low_right_label, "%u", (unsigned int)th.low_max);
    lv_label_set_text_fmt(s_mid_left_label, "%u", (unsigned int)th.low_max);
    lv_label_set_text_fmt(s_mid_right_label, "%u",(unsigned int) th.mid_max);
    lv_label_set_text_fmt(s_high_left_label, "%u", (unsigned int)th.mid_max);

    s_threshold_updating = false;
}

static void threshold_event_cb(lv_event_t *e)
{
    if (s_threshold_updating) return;

    lv_obj_t *target = lv_event_get_target_obj(e);
    app_lvgl_anxiety_thresholds_t th;
    app_lvgl_get_anxiety_thresholds(&th);

    uint8_t low = th.low_max;
    uint8_t mid = th.mid_max;

    if (target == s_low_slider) {
        low = (uint8_t)lv_slider_get_value(s_low_slider);
    } else if (target == s_mid_slider) {
        low = (uint8_t)lv_slider_get_left_value(s_mid_slider);
        mid = (uint8_t)lv_slider_get_value(s_mid_slider);
    } else if (target == s_high_slider) {
        mid = (uint8_t)lv_slider_get_left_value(s_high_slider);
    }

    app_lvgl_set_anxiety_thresholds(low, mid);
    refresh_threshold_view();
}

static void refresh_display_view(void)
{
    app_lvgl_display_settings_t st;
    app_lvgl_display_get_settings(&st);

    lv_slider_set_value(s_brightness_slider, st.brightness_pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_brightness_label, "%u%%", st.brightness_pct);

    if (st.sleep_sec == 0U) lv_dropdown_set_selected(s_sleep_dropdown, 0);
    else if (st.sleep_sec == 60U) lv_dropdown_set_selected(s_sleep_dropdown, 1);
    else if (st.sleep_sec == 300U) lv_dropdown_set_selected(s_sleep_dropdown, 2);
    else lv_dropdown_set_selected(s_sleep_dropdown, 3);

    if (st.theme_mode <= 3U) {
        lv_dropdown_set_selected(s_theme_dropdown, st.theme_mode);
    } else {
        lv_dropdown_set_selected(s_theme_dropdown, 0);
    }
}

static void refresh_config_values(void)
{
    lv_label_set_text_fmt(s_history_total_label, "%" PRIu32 " 条",
                          app_ui_history_get_record_count());

    size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t free_size = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t used = (total > free_size) ? (total - free_size) : 0U;

    char tmp[32];
    format_bytes(tmp, sizeof(tmp), used);
    lv_label_set_text(s_mem_used_label, tmp);

    format_bytes(tmp, sizeof(tmp), free_size);
    lv_label_set_text(s_mem_free_label, tmp);

    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) != NULL) {
        snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        lv_label_set_text(s_refresh_time_label, tmp);
    } else {
        lv_label_set_text(s_refresh_time_label, "--:--:--");
    }

    lv_label_set_text_fmt(s_log_used_label, "%" PRIu32 " / %" PRIu32 " 条",
                          app_lvgl_log_get_used_count(),
                          app_lvgl_log_get_keep_limit());

    if (app_lvgl_log_is_enabled()) {
        lv_obj_add_state(s_log_switch, LV_STATE_CHECKED);
        lv_label_set_text(s_log_status_label, "日志记录已开启");
    } else {
        lv_obj_remove_state(s_log_switch, LV_STATE_CHECKED);
        lv_label_set_text(s_log_status_label, "日志记录已关闭");
    }

    uint32_t keep = app_lvgl_log_get_keep_limit();
    if (keep <= 50U) lv_dropdown_set_selected(s_log_keep_dropdown, 0);
    else if (keep <= 100U) lv_dropdown_set_selected(s_log_keep_dropdown, 1);
    else lv_dropdown_set_selected(s_log_keep_dropdown, 2);

    refresh_threshold_view();
    refresh_display_view();
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_config_values();
}

static void refresh_btn_event_cb(lv_event_t *e)
{
    (void)e;
    refresh_config_values();
}

static void log_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    app_lvgl_log_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
    refresh_config_values();
}

static void log_keep_event_cb(lv_event_t *e)
{
    (void)e;

    uint16_t selected = lv_dropdown_get_selected(s_log_keep_dropdown);
    uint32_t keep = 200U;
    if (selected == 0U) keep = 50U;
    else if (selected == 1U) keep = 100U;

    app_lvgl_log_set_keep_limit(keep);
    refresh_config_values();
}

static void export_log_event_cb(lv_event_t *e)
{
    (void)e;

    esp_err_t ret = app_lvgl_log_export_uart();
    lv_label_set_text(s_log_status_label,
                      ret == ESP_OK ? "日志已发送到串口" : "日志发送失败");
    refresh_config_values();
}

static void clear_log_event_cb(lv_event_t *e)
{
    (void)e;

    app_lvgl_log_clear();
    lv_label_set_text(s_log_status_label, "日志已清空");
    refresh_config_values();
}

static void brightness_event_cb(lv_event_t *e)
{
    (void)e;

    uint8_t pct = (uint8_t)lv_slider_get_value(s_brightness_slider);
    app_lvgl_display_set_brightness(pct);
    lv_label_set_text_fmt(s_brightness_label, "%u%%", pct);
}

static void sleep_event_cb(lv_event_t *e)
{
    (void)e;

    uint16_t selected = lv_dropdown_get_selected(s_sleep_dropdown);
    uint16_t sec = 0U;

    if (selected == 1U) sec = 60U;
    else if (selected == 2U) sec = 300U;
    else if (selected == 3U) sec = 600U;

    app_lvgl_display_set_sleep_sec(sec);
}

static void theme_event_cb(lv_event_t *e)
{
    (void)e;

    uint8_t theme = (uint8_t)lv_dropdown_get_selected(s_theme_dropdown);
    app_lvgl_display_set_theme(theme);
}

static void build_system_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 8, 276, 176);
    make_title(card, APP_SYMBOL_INFO " 系统信息");

    metric_row(card, "当前固件版本", "V1.2.0", 44, NULL);
    metric_row(card, "硬件平台", "ESP32-P4C5", 76, NULL);
    metric_row(card, "日志上限", "200 条", 108, NULL);
    metric_row(card, "时间服务器", APP_RTC_SNTP_SERVER, 140, NULL);
}

static void build_data_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 194, 276, 296);
    make_title(card, LV_SYMBOL_LIST " 数据管理");

    metric_row(card, "历史记录总数", "--", 50, &s_history_total_label);
    metric_row(card, "运行内存使用", "--", 88, &s_mem_used_label);
    metric_row(card, "可用内存", "--", 126, &s_mem_free_label);
    metric_row(card, "最近刷新时间", "--", 164, &s_refresh_time_label);

    lv_obj_t *refresh_btn = make_button(card, LV_SYMBOL_REFRESH " 刷新",
                                        82, 226, 118, 38, &s_style_btn_active);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

static void build_log_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 294, 8, 276, 482);
    make_title(card, LV_SYMBOL_LIST " 日志管理");

    make_label(card, "日志开关", 18, 54);
    s_log_switch = lv_switch_create(card);
    lv_obj_set_pos(s_log_switch, 198, 46);
    lv_obj_set_size(s_log_switch, 58, 32);
    lv_obj_add_event_cb(s_log_switch, log_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_label(card, "日志保留条数", 18, 104);
    s_log_keep_dropdown = lv_dropdown_create(card);
    lv_dropdown_set_options(s_log_keep_dropdown, "50 条\n100 条\n200 条");
    lv_obj_set_pos(s_log_keep_dropdown, 168, 94);
    lv_obj_set_size(s_log_keep_dropdown, 88, 38);
    lv_obj_set_style_text_font(s_log_keep_dropdown, ui_font(), 0);
    lv_obj_add_event_cb(s_log_keep_dropdown, log_keep_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_label(card, "当前日志数量", 18, 156);
    s_log_used_label = make_label(card, "--", 168, 156);
    lv_obj_add_style(s_log_used_label, &s_style_cyan, 0);

    lv_obj_t *export_btn = make_button(card, LV_SYMBOL_DOWNLOAD " 导出日志",
                                       18, 218, 112, 42, &s_style_btn_active);
    lv_obj_add_event_cb(export_btn, export_log_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_btn = make_button(card, LV_SYMBOL_TRASH " 清空日志",
                                      146, 218, 112, 42, &s_style_btn_danger);
    lv_obj_add_event_cb(clear_btn, clear_log_event_cb, LV_EVENT_CLICKED, NULL);

    s_log_status_label = make_label(card, "日志记录已开启", 18, 304);
    lv_obj_add_style(s_log_status_label, &s_style_muted, 0);

    make_label(card, "导出方式", 18, 356);
    lv_obj_t *mode = make_label(card, "串口 CSV 同步", 144, 356);
    lv_obj_add_style(mode, &s_style_green, 0);

    make_label(card, "导出标记", 18, 398);
    lv_obj_t *mark = make_label(card, "PPI_LOG_CSV", 144, 398);
    lv_obj_add_style(mark, &s_style_green, 0);
}

static lv_obj_t *make_range_slider(lv_obj_t *parent, int32_t x, int32_t y, uint32_t color)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_slider_set_mode(slider, LV_SLIDER_MODE_RANGE);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, 132, 8);
    set_slider_common_style(slider, color);
    lv_obj_add_event_cb(slider, threshold_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

static void build_threshold_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 580, 8, 436, 226);
    make_title(card, APP_SYMBOL_SHIELD " 焦虑等级阈值设置");

    make_label(card, "低焦虑", 20, 56);
    lv_obj_t *l0 = make_label(card, "0", 138, 56);
    lv_obj_add_style(l0, &s_style_cyan, 0);
    s_low_right_label = make_label(card, "40", 378, 56);
    lv_obj_add_style(s_low_right_label, &s_style_cyan, 0);
    s_low_slider = make_range_slider(card, 184, 62, 0x2ceaff);

    make_label(card, "中等焦虑", 20, 100);
    s_mid_left_label = make_label(card, "40", 138, 100);
    lv_obj_add_style(s_mid_left_label, &s_style_green, 0);
    s_mid_right_label = make_label(card, "70", 378, 100);
    lv_obj_add_style(s_mid_right_label, &s_style_green, 0);
    s_mid_slider = make_range_slider(card, 184, 106, 0x9cff3a);

    lv_obj_t *hi = make_label(card, "高焦虑", 20, 144);
    lv_obj_add_style(hi, &s_style_red, 0);
    s_high_left_label = make_label(card, "70", 138, 144);
    lv_obj_add_style(s_high_left_label, &s_style_red, 0);
    lv_obj_t *h100 = make_label(card, "100", 372, 144);
    lv_obj_add_style(h100, &s_style_red, 0);
    s_high_slider = make_range_slider(card, 184, 150, 0xff3434);

    make_label(card, APP_SYMBOL_INFO " 分值范围: 0-100，拖动滑块调整阈值分界线", 20, 194);
}

static void build_display_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 580, 244, 436, 246);
    make_title(card, LV_SYMBOL_SETTINGS " 显示设置");

    make_label(card, "屏幕亮度", 18, 58);

    s_brightness_slider = lv_slider_create(card);
    lv_slider_set_range(s_brightness_slider, 20, 100);
    lv_obj_set_pos(s_brightness_slider, 154, 64);
    lv_obj_set_size(s_brightness_slider, 144, 8);
    set_slider_common_style(s_brightness_slider, 0x2ceaff);
    lv_obj_add_event_cb(s_brightness_slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_brightness_label = make_label(card, "80%", 330, 58);
    lv_obj_add_style(s_brightness_label, &s_style_cyan, 0);

    make_label(card, "主题模式", 18, 112);
    s_theme_dropdown = lv_dropdown_create(card);
    lv_dropdown_set_options(s_theme_dropdown, "深色\n浅色\n护眼\n高对比");
    lv_obj_set_pos(s_theme_dropdown, 286, 102);
    lv_obj_set_size(s_theme_dropdown, 118, 38);
    lv_obj_set_style_text_font(s_theme_dropdown, ui_font(), 0);
    lv_obj_add_event_cb(s_theme_dropdown, theme_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_label(card, "自动息屏时间", 18, 166);
    s_sleep_dropdown = lv_dropdown_create(card);
    lv_dropdown_set_options(s_sleep_dropdown, "从不\n1 分钟\n5 分钟\n10 分钟");
    lv_obj_set_pos(s_sleep_dropdown, 286, 156);
    lv_obj_set_size(s_sleep_dropdown, 118, 38);
    lv_obj_set_style_text_font(s_sleep_dropdown, ui_font(), 0);
    lv_obj_add_event_cb(s_sleep_dropdown, sleep_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *app_ui_config_create(lv_obj_t *root)
{
    init_styles();

    s_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_page);
    lv_obj_add_style(s_page, &s_style_page, 0);
    lv_obj_set_pos(s_page, 0, APP_UI_HEADER_H);
    lv_obj_set_size(s_page, APP_LCD_H_RES, APP_UI_PAGE_H);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    build_system_panel(s_page);
    build_data_panel(s_page);
    build_log_panel(s_page);
    build_threshold_panel(s_page);
    build_display_panel(s_page);

    refresh_config_values();

    if (s_refresh_timer == NULL) {
        s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    }

    return s_page;
}