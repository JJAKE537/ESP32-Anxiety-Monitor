#include "app_lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "app_config.h"
#include "lvgl.h"

LV_FONT_DECLARE(app_ui_font_16);

#define APP_LCD_H_RES      1024
#define APP_UI_HEADER_H    42
#define APP_UI_NAV_Y       548
#define APP_UI_PAGE_H      (APP_UI_NAV_Y - APP_UI_HEADER_H - 6)

static lv_style_t s_style_page;
static lv_style_t s_style_card;
static lv_style_t s_style_card_soft;
static lv_style_t s_style_title;
static lv_style_t s_style_text;
static lv_style_t s_style_muted;
static lv_style_t s_style_cyan;
static lv_style_t s_style_green;
static lv_style_t s_style_warn;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_monitor;
static lv_style_t s_style_btn_history;
static lv_style_t s_style_btn_stats;
static lv_style_t s_style_btn_settings;
static lv_style_t s_style_flow_box;
static lv_style_t s_style_badge;

static bool s_style_inited;
static lv_obj_t *s_page;

static lv_obj_t *s_threshold_label;
static lv_obj_t *s_theme_label;
static lv_obj_t *s_log_limit_label;
/*badge文字样式*/
static lv_style_t s_style_badge_text;

static const lv_font_t *ui_font(void)
{
    return &app_ui_font_16;
}

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static const char *theme_name(uint8_t theme)
{
    switch (theme) {
    case APP_LVGL_THEME_DARK:
        return "深色";
    case APP_LVGL_THEME_LIGHT:
        return "浅色";
    case APP_LVGL_THEME_EYE_CARE:
        return "护眼";
    case APP_LVGL_THEME_HIGH_CONTRAST:
        return "高对比";
    default:
        return "--";
    }
}

static void apply_about_theme_styles(void)
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

    lv_style_set_bg_color(&s_style_card_soft, c(p.nav));
    lv_style_set_bg_opa(&s_style_card_soft, p.nav_opa);
    lv_style_set_border_color(&s_style_card_soft, c(p.nav_border));
    lv_style_set_text_color(&s_style_card_soft, c(p.text));

    lv_style_set_text_color(&s_style_title, c(p.text));
    lv_style_set_text_color(&s_style_text, c(p.text));
    lv_style_set_text_color(&s_style_muted, c(p.muted));
    lv_style_set_text_color(&s_style_cyan, c(p.cyan));
    lv_style_set_text_color(&s_style_green, c(p.good));
    lv_style_set_text_color(&s_style_warn, c(p.warning));

    lv_style_set_bg_color(&s_style_btn, c(p.nav));
    lv_style_set_bg_grad_color(&s_style_btn, c(p.nav));
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_btn, c(p.nav_border));
    lv_style_set_text_color(&s_style_btn, c(p.text));

    lv_style_set_bg_color(&s_style_flow_box, c(p.nav));
    lv_style_set_bg_grad_color(&s_style_flow_box, c(p.card));
    lv_style_set_bg_opa(&s_style_flow_box, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_flow_box, c(p.accent));
    lv_style_set_text_color(&s_style_flow_box, c(p.text));
    /*修改badge颜色*/
    uint32_t badge_bg = p.nav_active;
    uint32_t badge_grad = p.accent;
    uint32_t badge_border = p.nav_active_border;
    uint32_t badge_text = 0xffffff;

    app_lvgl_display_settings_t ds;
    app_lvgl_display_get_settings(&ds);

    if (ds.theme_mode == APP_LVGL_THEME_LIGHT) {
        badge_bg = 0x2563eb;
        badge_grad = 0x1d4ed8;
        badge_border = 0x1e40af;
        badge_text = 0xffffff;
    } else if (ds.theme_mode == APP_LVGL_THEME_EYE_CARE) {
        badge_bg = 0x2f6655;
        badge_grad = 0x3f8a72;
        badge_border = 0x9be7c7;
        badge_text = 0xffffff;
    } else if (ds.theme_mode == APP_LVGL_THEME_HIGH_CONTRAST) {
        badge_bg = 0xffff00;
        badge_grad = 0xffd000;
        badge_border = 0xffffff;
        badge_text = 0x000000;
    } else {
        badge_bg = 0x123c5a;
        badge_grad = 0x0d6ea8;
        badge_border = 0x63dfff;
        badge_text = 0xf3fbff;
    }

    lv_style_set_bg_color(&s_style_badge, c(badge_bg));
    lv_style_set_bg_grad_color(&s_style_badge, c(badge_grad));
    lv_style_set_bg_grad_dir(&s_style_badge, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_style_badge, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_badge, c(badge_border));
    lv_style_set_text_color(&s_style_badge_text, c(badge_text));

    if (ds.theme_mode == APP_LVGL_THEME_LIGHT) {
        lv_style_set_bg_color(&s_style_btn_monitor, c(0xdff7ff));
        lv_style_set_bg_grad_color(&s_style_btn_monitor, c(0xc9efff));
        lv_style_set_border_color(&s_style_btn_monitor, c(0x0ea5e9));
        lv_style_set_text_color(&s_style_btn_monitor, c(0x0f3d56));

        lv_style_set_bg_color(&s_style_btn_history, c(0xe8f8e8));
        lv_style_set_bg_grad_color(&s_style_btn_history, c(0xd6f2d6));
        lv_style_set_border_color(&s_style_btn_history, c(0x2f8f35));
        lv_style_set_text_color(&s_style_btn_history, c(0x1f4d25));

        lv_style_set_bg_color(&s_style_btn_stats, c(0xfff2d7));
        lv_style_set_bg_grad_color(&s_style_btn_stats, c(0xffe3a8));
        lv_style_set_border_color(&s_style_btn_stats, c(0xd18a00));
        lv_style_set_text_color(&s_style_btn_stats, c(0x5c3d00));

        lv_style_set_bg_color(&s_style_btn_settings, c(0xf0e7ff));
        lv_style_set_bg_grad_color(&s_style_btn_settings, c(0xe2d2ff));
        lv_style_set_border_color(&s_style_btn_settings, c(0x7c3aed));
        lv_style_set_text_color(&s_style_btn_settings, c(0x3b1768));
    } else if (ds.theme_mode == APP_LVGL_THEME_HIGH_CONTRAST) {
        lv_style_set_bg_color(&s_style_btn_monitor, c(0x001f24));
        lv_style_set_bg_grad_color(&s_style_btn_monitor, c(0x003944));
        lv_style_set_border_color(&s_style_btn_monitor, c(0x00ffff));
        lv_style_set_text_color(&s_style_btn_monitor, c(0xffffff));

        lv_style_set_bg_color(&s_style_btn_history, c(0x002400));
        lv_style_set_bg_grad_color(&s_style_btn_history, c(0x003800));
        lv_style_set_border_color(&s_style_btn_history, c(0x00ff00));
        lv_style_set_text_color(&s_style_btn_history, c(0xffffff));

        lv_style_set_bg_color(&s_style_btn_stats, c(0x292000));
        lv_style_set_bg_grad_color(&s_style_btn_stats, c(0x403400));
        lv_style_set_border_color(&s_style_btn_stats, c(0xffff00));
        lv_style_set_text_color(&s_style_btn_stats, c(0xffffff));

        lv_style_set_bg_color(&s_style_btn_settings, c(0x25002e));
        lv_style_set_bg_grad_color(&s_style_btn_settings, c(0x3a0048));
        lv_style_set_border_color(&s_style_btn_settings, c(0xff66ff));
        lv_style_set_text_color(&s_style_btn_settings, c(0xffffff));
    } else {
        lv_style_set_bg_color(&s_style_btn_monitor, c(0x07384a));
        lv_style_set_bg_grad_color(&s_style_btn_monitor, c(0x0b5e73));
        lv_style_set_border_color(&s_style_btn_monitor, c(0x2ceaff));
        lv_style_set_text_color(&s_style_btn_monitor, c(0xf4ffff));

        lv_style_set_bg_color(&s_style_btn_history, c(0x12391f));
        lv_style_set_bg_grad_color(&s_style_btn_history, c(0x1d5a2e));
        lv_style_set_border_color(&s_style_btn_history, c(0x6ee77d));
        lv_style_set_text_color(&s_style_btn_history, c(0xf4fff4));

        lv_style_set_bg_color(&s_style_btn_stats, c(0x3b2b08));
        lv_style_set_bg_grad_color(&s_style_btn_stats, c(0x6a4a0a));
        lv_style_set_border_color(&s_style_btn_stats, c(0xffc247));
        lv_style_set_text_color(&s_style_btn_stats, c(0xfffff0));

        lv_style_set_bg_color(&s_style_btn_settings, c(0x2e174b));
        lv_style_set_bg_grad_color(&s_style_btn_settings, c(0x51307c));
        lv_style_set_border_color(&s_style_btn_settings, c(0xd197ff));
        lv_style_set_text_color(&s_style_btn_settings, c(0xffffff));
    }

    lv_style_t *module_styles[] = {
        &s_style_btn_monitor,
        &s_style_btn_history,
        &s_style_btn_stats,
        &s_style_btn_settings,
    };

    for (uint32_t i = 0; i < 4U; i++) {
        lv_style_set_radius(module_styles[i], 8);
        lv_style_set_bg_opa(module_styles[i], LV_OPA_COVER);
        lv_style_set_bg_grad_dir(module_styles[i], LV_GRAD_DIR_VER);
        lv_style_set_border_width(module_styles[i], 1);
        lv_style_set_pad_all(module_styles[i], 0);
        lv_style_set_text_font(module_styles[i], ui_font());
    }
}

void app_ui_about_apply_theme(void)
{
    if (!s_style_inited) {
        return;
    }

    apply_about_theme_styles();
}

static void init_styles(void)
{
    if (s_style_inited) {
        return;
    }

    s_style_inited = true;

    lv_style_init(&s_style_page);
    lv_style_set_bg_color(&s_style_page, c(0x061625));
    lv_style_set_bg_grad_color(&s_style_page, c(0x08263a));
    lv_style_set_bg_grad_dir(&s_style_page, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_style_page, LV_OPA_COVER);
    lv_style_set_text_font(&s_style_page, ui_font());

    lv_style_init(&s_style_card);
    lv_style_set_radius(&s_style_card, 8);
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_pad_all(&s_style_card, 10);
    lv_style_set_text_font(&s_style_card, ui_font());

    lv_style_init(&s_style_card_soft);
    lv_style_set_radius(&s_style_card_soft, 8);
    lv_style_set_border_width(&s_style_card_soft, 1);
    lv_style_set_pad_all(&s_style_card_soft, 8);
    lv_style_set_text_font(&s_style_card_soft, ui_font());

    lv_style_init(&s_style_title);
    lv_style_set_text_font(&s_style_title, ui_font());

    lv_style_init(&s_style_text);
    lv_style_set_text_font(&s_style_text, ui_font());

    lv_style_init(&s_style_muted);
    lv_style_set_text_font(&s_style_muted, ui_font());

    lv_style_init(&s_style_cyan);
    lv_style_set_text_font(&s_style_cyan, ui_font());

    lv_style_init(&s_style_green);
    lv_style_set_text_font(&s_style_green, ui_font());

    lv_style_init(&s_style_warn);
    lv_style_set_text_font(&s_style_warn, ui_font());

    lv_style_init(&s_style_btn);
    lv_style_set_radius(&s_style_btn, 8);
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_btn, 1);
    lv_style_set_pad_all(&s_style_btn, 0);
    lv_style_set_text_font(&s_style_btn, ui_font());

    lv_style_init(&s_style_btn_monitor);
    lv_style_init(&s_style_btn_history);
    lv_style_init(&s_style_btn_stats);
    lv_style_init(&s_style_btn_settings);

    lv_style_init(&s_style_flow_box);
    lv_style_set_radius(&s_style_flow_box, 8);
    lv_style_set_border_width(&s_style_flow_box, 1);
    lv_style_set_pad_all(&s_style_flow_box, 0);
    lv_style_set_text_font(&s_style_flow_box, ui_font());

    lv_style_init(&s_style_badge);
    lv_style_set_radius(&s_style_badge, 8);
    lv_style_set_border_width(&s_style_badge, 1);
    lv_style_set_pad_all(&s_style_badge, 0);
    lv_style_set_text_font(&s_style_badge, ui_font());

    lv_style_init(&s_style_badge_text);
    lv_style_set_text_font(&s_style_badge_text, ui_font());
    lv_style_set_text_align(&s_style_badge_text, LV_TEXT_ALIGN_CENTER);

    apply_about_theme_styles();
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

static lv_obj_t *make_wrapped_label(lv_obj_t *parent, const char *text,
                                    int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *label = make_label(parent, text, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
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

static void make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, 10, 8);
    lv_obj_add_style(label, &s_style_title, 0);
}

static void metric_row(lv_obj_t *parent, const char *name, const char *value,
                       int32_t y, lv_obj_t **out)
{
    lv_obj_t *key = make_label(parent, name, 12, y);
    lv_obj_set_width(key, 120);
    lv_obj_add_style(key, &s_style_muted, 0);

    lv_obj_t *val = make_label(parent, value ? value : "--", 138, y);
    lv_obj_set_width(val, 150);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_style(val, &s_style_cyan, 0);

    if (out) {
        *out = val;
    }
}

static void nav_event_cb(lv_event_t *e)
{
    app_lvgl_page_t page = (app_lvgl_page_t)(uintptr_t)lv_event_get_user_data(e);
    app_lvgl_show_page(page);
}

static void add_flow_step(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_style_flow_box, 0);
    lv_obj_set_pos(box, 56, y);
    lv_obj_set_size(box, 218, 28);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_center(label);
}

static void add_flow_arrow(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *arrow = make_label(parent, "v", 160, y);
    lv_obj_add_style(arrow, &s_style_cyan, 0);
}

static void build_intro_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 8, 330, 334);
    make_title(card, LV_SYMBOL_TINT " 系统简介");

    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_remove_style_all(badge);
    lv_obj_add_style(badge, &s_style_badge, 0);
    lv_obj_set_pos(badge, 18, 46);
    lv_obj_set_size(badge, 294, 76);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge_text = lv_label_create(badge);
    lv_label_set_text(badge_text, "PPI + PRV\n焦虑指数实时分析");
    lv_obj_add_style(badge_text, &s_style_badge_text, 0);
    lv_obj_set_width(badge_text, 280);
    lv_label_set_long_mode(badge_text, LV_LABEL_LONG_WRAP);
    lv_obj_center(badge_text);

    lv_obj_t *desc = make_wrapped_label(
        card,
        "本系统基于脉搏波 PPI 序列和 PRV 特征参数进行焦虑状态评估。"
        "系统采用 ADC-DMA 采集脉搏波信号，以 30 s 滑动窗口、10 s 步长进行实时分析，"
        "完成峰值检测、PPI 提取、9 项特征计算与焦虑指数输出，并通过 LVGL 界面可视化显示。",
        22,                             // x，整体向左/向右
        159,                            // y，整体向上/向下
        286);                           // 宽度，影响自动换行
    lv_obj_add_style(desc, &s_style_text, 0);
}

static void build_flow_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 348, 8, 330, 334);
    make_title(card, LV_SYMBOL_LIST " 算法流程");

    add_flow_step(card, "ADC-DMA 采集", 42);
    add_flow_arrow(card, 67);

    add_flow_step(card, "信号滤波与重构", 82);
    add_flow_arrow(card, 107);

    add_flow_step(card, "峰值检测", 122);
    add_flow_arrow(card, 147);

    add_flow_step(card, "PPI 序列提取", 162);
    add_flow_arrow(card, 187);

    add_flow_step(card, "9项特征计算", 202);
    add_flow_arrow(card, 227);

    add_flow_step(card, "焦虑指数计算", 242);
    add_flow_arrow(card, 267);

    add_flow_step(card, "LVGL 实时显示", 282);
}
static void build_version_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 688, 8, 328, 176);
    make_title(card, LV_SYMBOL_OK " 版本信息");

    metric_row(card, "软件版本:", "V1.0.0", 48, NULL);
    metric_row(card, "硬件平台:", "ESP32-P4C5", 78, NULL);
    metric_row(card, "显示框架:", "LVGL 9.4", 108, NULL);
    metric_row(card, "刷新周期:", "10 s", 138, NULL);
}

static void build_project_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 688, 194, 328, 150);
    make_title(card, LV_SYMBOL_FILE " 项目说明");

    metric_row(card, "研究方向:", "脉搏波状态监测", 46, NULL);
    metric_row(card, "核心输入:", "脉搏波 / PPI", 76, NULL);
    metric_row(card, "核心输出:", "焦虑指数 / 特征参数", 106, NULL);
}

static void build_runtime_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 688, 354, 328, 138);
    make_title(card, LV_SYMBOL_SETTINGS " 当前配置");

    metric_row(card, "主题模式:", "--", 44, &s_theme_label);
    metric_row(card, "焦虑阈值:", "--", 74, &s_threshold_label);
    lv_obj_set_pos(s_threshold_label, 104, 74);
    lv_obj_set_width(s_threshold_label, 210);
    lv_obj_set_style_text_align(s_threshold_label, LV_TEXT_ALIGN_RIGHT, 0);

    metric_row(card, "日志上限:", "--", 104, &s_log_limit_label);
}

static void build_module_button(lv_obj_t *parent, const char *title, const char *sub,
                                int32_t x, int32_t y, int32_t w,
                                app_lvgl_page_t page, const lv_style_t *style)
{
    lv_obj_t *btn = make_button(parent, "", x, y, w, 64, style);
    lv_obj_add_event_cb(btn, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

    lv_obj_t *title_label = lv_label_create(btn);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, ui_font(), 0);
    lv_obj_set_pos(title_label, 18, 11);

    lv_obj_t *sub_label = lv_label_create(btn);
    lv_label_set_text(sub_label, sub);
    lv_obj_set_style_text_font(sub_label, ui_font(), 0);
    lv_obj_set_pos(sub_label, 18, 38);
}

static void build_module_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 352, 670, 140);
    make_title(card, LV_SYMBOL_HOME " 功能模块");

    build_module_button(card, LV_SYMBOL_HOME " 实时监测", "采集状态 / 曲线",
                        14, 50, 150, APP_LVGL_PAGE_MONITOR, &s_style_btn_monitor);
    build_module_button(card, LV_SYMBOL_LIST " 历史记录", "筛选 / 详情 / CSV",
                        178, 50, 150, APP_LVGL_PAGE_HISTORY, &s_style_btn_history);
    build_module_button(card, LV_SYMBOL_BARS " 数据统计", "趋势 / 占比 / 特征",
                        342, 50, 150, APP_LVGL_PAGE_STATS, &s_style_btn_stats);
    build_module_button(card, LV_SYMBOL_SETTINGS " 系统设置", "阈值 / 主题 / 日志",
                        506, 50, 150, APP_LVGL_PAGE_SETTINGS, &s_style_btn_settings);
}

static void refresh_about_values(void)
{
    if (!s_page) {
        return;
    }

    app_lvgl_display_settings_t ds;
    app_lvgl_display_get_settings(&ds);

    app_lvgl_anxiety_thresholds_t th;
    app_lvgl_get_anxiety_thresholds(&th);

    if (s_theme_label) {
        lv_label_set_text(s_theme_label, theme_name(ds.theme_mode));
    }

    if (s_threshold_label) {
     lv_label_set_text_fmt(s_threshold_label, "0-%u/%u-%u/%u-100",
                      (unsigned int)th.low_max,
                      (unsigned int)th.low_max,
                      (unsigned int)th.mid_max,
                      (unsigned int)th.mid_max);
    }

    if (s_log_limit_label) {
        lv_label_set_text_fmt(s_log_limit_label, "%" PRIu32 " 条",
                              app_lvgl_log_get_keep_limit());
    }
}

void app_ui_about_on_show(void)
{
    refresh_about_values();
}

lv_obj_t *app_ui_about_create(lv_obj_t *root)
{
    init_styles();

    s_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_page);
    lv_obj_add_style(s_page, &s_style_page, 0);
    lv_obj_set_pos(s_page, 0, APP_UI_HEADER_H);
    lv_obj_set_size(s_page, APP_LCD_H_RES, APP_UI_PAGE_H);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    build_intro_card(s_page);
    build_flow_card(s_page);
    build_version_card(s_page);
    build_project_card(s_page);
    build_runtime_card(s_page);
    build_module_card(s_page);

    refresh_about_values();
    return s_page;
}