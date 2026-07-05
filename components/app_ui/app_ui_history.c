/* app_ui_history.c */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_lvgl.h"
#include "esp_err.h"
#include "lvgl.h"

#include "src/draw/lv_draw.h"
#include "src/draw/lv_draw_label.h"
#include "src/draw/lv_draw_line.h"
#include "src/draw/lv_draw_rect.h"
#include "src/draw/lv_draw_triangle.h"

LV_FONT_DECLARE(app_ui_font_16);

#define APP_LCD_H_RES      1024
#define APP_UI_HEADER_H    42
#define APP_UI_NAV_Y       548
#define APP_UI_PAGE_H      (APP_UI_NAV_Y - APP_UI_HEADER_H - 6)

#define APP_HISTORY_TREND_POINTS 8U
/*excel表格存储路径设置*/
#ifndef APP_HISTORY_CSV_PATH
#define APP_HISTORY_CSV_PATH "/sdcard/ppi_history.csv"
#endif

typedef enum {
    HISTORY_FILTER_ALL = 0,
    HISTORY_FILTER_LOW,
    HISTORY_FILTER_MID,
    HISTORY_FILTER_HIGH,
} history_filter_t;

typedef struct {
    uint32_t win_id;
    time_t ts;
    char time_text[12];

    bool has_ppi;
    bool has_anxiety;

    PPI_RESULT_T ppi;
    ANXIETY_MSG_T anxiety;
} history_record_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *chart;
    lv_chart_series_t *series;
} history_chart_t;

static lv_style_t s_style_page;
static lv_style_t s_style_card;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_active;
static lv_style_t s_style_btn_danger;
static lv_style_t s_style_muted;
static lv_style_t s_style_cyan;
static lv_style_t s_style_good;
static lv_style_t s_style_warn;
static lv_style_t s_style_bad;
static bool s_style_inited;

static history_record_t *s_records;
static uint32_t s_record_count;
static uint32_t s_record_cap;

static uint32_t *s_filtered;
static uint32_t s_filtered_count;
static uint32_t s_filtered_cap;

static history_filter_t s_filter = HISTORY_FILTER_ALL;
static uint32_t s_selected_idx = UINT32_MAX;
static bool s_follow_latest = true;

static lv_obj_t *s_page;
static lv_obj_t *s_table_header;
static lv_obj_t *s_table;
static lv_obj_t *s_filter_btns[4];
static lv_obj_t *s_export_status;

static lv_obj_t *s_total_label;
static lv_obj_t *s_filtered_label;
static lv_obj_t *s_avg_score_label;
static lv_obj_t *s_max_score_label;
static lv_obj_t *s_min_score_label;
static lv_obj_t *s_avg_pr_label;

static lv_obj_t *s_detail_time;
static lv_obj_t *s_detail_win;
static lv_obj_t *s_detail_score;
static lv_obj_t *s_detail_level;
static lv_obj_t *s_detail_pr;
static lv_obj_t *s_detail_ppi;
static lv_obj_t *s_detail_count;

static lv_obj_t *s_feature_value[9];
static lv_obj_t *s_feature_unit[9];

static history_chart_t s_trend;
static int32_t s_trend_values[APP_HISTORY_TREND_POINTS];
static lv_obj_t *s_trend_xlabels[APP_HISTORY_TREND_POINTS];
static lv_obj_t *s_trend_score_label;

static bool s_history_dirty;
/*===========================================================================================================================*/
uint32_t app_ui_history_get_record_count(void);


static const lv_font_t *ui_font(void)
{
    return &app_ui_font_16;
}

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

void app_ui_history_apply_theme(void)
{
    if (!s_style_inited) {
        return;
    }

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

    lv_style_set_bg_color(&s_style_btn_danger, c(p.danger));
    lv_style_set_bg_grad_color(&s_style_btn_danger, c(p.danger));
    lv_style_set_border_color(&s_style_btn_danger, c(p.danger));
    lv_style_set_text_color(&s_style_btn_danger, c(0xffffff));

    lv_style_set_text_color(&s_style_muted, c(p.muted));
    lv_style_set_text_color(&s_style_cyan, c(p.cyan));
    lv_style_set_text_color(&s_style_good, c(p.good));
    lv_style_set_text_color(&s_style_warn, c(p.warning));
    lv_style_set_text_color(&s_style_bad, c(p.danger));
}

static void format_float(char *buf, size_t len, float value, uint8_t decimals, const char *suffix)
{
    if (!isfinite(value)) {
        snprintf(buf, len, "--%s", suffix ? suffix : "");
        return;
    }

    int32_t scale = 1;
    for (uint8_t i = 0; i < decimals; i++) {
        scale *= 10;
    }

    const char *sign = value < 0.0f ? "-" : "";
    int32_t scaled = (int32_t)lroundf(fabsf(value) * (float)scale);
    int32_t whole = scaled / scale;
    int32_t frac = scaled % scale;

    if (decimals == 0) {
        snprintf(buf, len, "%s%ld%s", sign, (long)whole, suffix ? suffix : "");
    } else {
        snprintf(buf, len, "%s%ld.%0*ld%s",
                 sign, (long)whole, (int)decimals, (long)frac, suffix ? suffix : "");
    }
}

static const char *level_text(float score)
{
    return app_lvgl_anxiety_level_text(score);
}

static lv_color_t level_color(float score)
{
    return app_lvgl_anxiety_level_color(score);
}

static history_filter_t score_filter(float score)
{
    app_lvgl_anxiety_thresholds_t th;
    app_lvgl_get_anxiety_thresholds(&th);

    if (score < (float)th.low_max) return HISTORY_FILTER_LOW;
    if (score < (float)th.mid_max) return HISTORY_FILTER_MID;
    return HISTORY_FILTER_HIGH;
}

static bool record_visible(const history_record_t *rec)
{
    if (!rec || !rec->has_anxiety || rec->anxiety.valid == 0U) return false;
    if (s_filter == HISTORY_FILTER_ALL) return true;
    return score_filter(rec->anxiety.anxiety_idx) == s_filter;
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
    lv_style_set_border_color(&s_style_card, c(0x24506c));
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_pad_all(&s_style_card, 10);
    lv_style_set_text_color(&s_style_card, c(0xf4f8ff));
    lv_style_set_text_font(&s_style_card, ui_font());

    lv_style_init(&s_style_btn);
    lv_style_set_radius(&s_style_btn, 6);
    lv_style_set_bg_color(&s_style_btn, c(0x08243a));
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_80);
    lv_style_set_border_width(&s_style_btn, 1);
    lv_style_set_border_color(&s_style_btn, c(0x24506c));
    lv_style_set_text_color(&s_style_btn, c(0xdce8f2));
    lv_style_set_text_font(&s_style_btn, ui_font());

    lv_style_init(&s_style_btn_active);
    lv_style_set_radius(&s_style_btn_active, 6);
    lv_style_set_bg_color(&s_style_btn_active, c(0x0d4ea0));
    lv_style_set_bg_grad_color(&s_style_btn_active, c(0x14a9dc));
    lv_style_set_bg_grad_dir(&s_style_btn_active, LV_GRAD_DIR_VER);
    lv_style_set_border_width(&s_style_btn_active, 1);
    lv_style_set_border_color(&s_style_btn_active, c(0x39c6ff));
    lv_style_set_text_color(&s_style_btn_active, c(0xffffff));
    lv_style_set_text_font(&s_style_btn_active, ui_font());

    lv_style_init(&s_style_btn_danger);
    lv_style_set_radius(&s_style_btn_danger, 6);
    lv_style_set_bg_color(&s_style_btn_danger, c(0x35161b));
    lv_style_set_bg_opa(&s_style_btn_danger, LV_OPA_80);
    lv_style_set_border_width(&s_style_btn_danger, 1);
    lv_style_set_border_color(&s_style_btn_danger, c(0x7a343d));
    lv_style_set_text_color(&s_style_btn_danger, c(0xffb7b7));
    lv_style_set_text_font(&s_style_btn_danger, ui_font());

    lv_style_init(&s_style_muted);
    lv_style_set_text_color(&s_style_muted, c(0x91a9ba));

    lv_style_init(&s_style_cyan);
    lv_style_set_text_color(&s_style_cyan, c(0x2ceaff));

    lv_style_init(&s_style_good);
    lv_style_set_text_color(&s_style_good, c(0x93ff4f));

    lv_style_init(&s_style_warn);
    lv_style_set_text_color(&s_style_warn, c(0xffd34d));

    lv_style_init(&s_style_bad);
    lv_style_set_text_color(&s_style_bad, c(0xff5a5a));

        app_ui_history_apply_theme();
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, ui_font(), 0);
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

static void metric_row(lv_obj_t *parent, const char *name, int32_t y, lv_obj_t **out)
{
    make_label(parent, name, 10, y);
    lv_obj_t *val = make_label(parent, "--", 112, y);
    if (out) *out = val;
}

static bool ensure_record_capacity(uint32_t need)
{
    if (need <= s_record_cap) return true;

    uint32_t new_cap = s_record_cap ? s_record_cap * 2U : 64U;
    while (new_cap < need) new_cap *= 2U;

    history_record_t *new_records = realloc(s_records, sizeof(history_record_t) * new_cap);
    if (!new_records) return false;

    memset(&new_records[s_record_cap], 0, sizeof(history_record_t) * (new_cap - s_record_cap));
    s_records = new_records;
    s_record_cap = new_cap;
    return true;
}

static bool ensure_filtered_capacity(uint32_t need)
{
    if (need <= s_filtered_cap) return true;

    uint32_t new_cap = s_filtered_cap ? s_filtered_cap * 2U : 64U;
    while (new_cap < need) new_cap *= 2U;

    uint32_t *new_filtered = realloc(s_filtered, sizeof(uint32_t) * new_cap);
    if (!new_filtered) return false;

    s_filtered = new_filtered;
    s_filtered_cap = new_cap;
    return true;
}

static void format_time_text(time_t ts, char *buf, size_t len)
{
    struct tm tm_now;
    if (localtime_r(&ts, &tm_now) == NULL) {
        snprintf(buf, len, "--:--:--");
        return;
    }
    snprintf(buf, len, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

static int32_t find_record(uint32_t win_id)
{
    for (int32_t i = (int32_t)s_record_count - 1; i >= 0; i--) {
        if (s_records[i].win_id == win_id) return i;
    }
    return -1;
}

static history_record_t *upsert_record(uint32_t win_id)
{
    int32_t idx = find_record(win_id);
    if (idx >= 0) return &s_records[idx];

    if (!ensure_record_capacity(s_record_count + 1U)) {
        if (s_export_status) lv_label_set_text(s_export_status, "历史记录内存不足");
        return NULL;
    }

    history_record_t *rec = &s_records[s_record_count++];
    memset(rec, 0, sizeof(*rec));
    rec->win_id = win_id;
    rec->ts = time(NULL);
    format_time_text(rec->ts, rec->time_text, sizeof(rec->time_text));
    return rec;
}

static void rebuild_filtered_index(void)
{
    s_filtered_count = 0;

    for (uint32_t i = 0; i < s_record_count; i++) {
        if (!record_visible(&s_records[i])) continue;
        if (!ensure_filtered_capacity(s_filtered_count + 1U)) break;
        s_filtered[s_filtered_count++] = i;
    }

    bool selected_visible = false;
    for (uint32_t i = 0; i < s_filtered_count; i++) {
        if (s_filtered[i] == s_selected_idx) {
            selected_visible = true;
            break;
        }
    }

    if (!selected_visible) {
        s_selected_idx = (s_filtered_count > 0U) ? s_filtered[s_filtered_count - 1U] : UINT32_MAX;
    }
}

static void update_filter_buttons(void)
{
    for (uint32_t i = 0; i < 4U; i++) {
        lv_obj_remove_style(s_filter_btns[i], &s_style_btn, 0);
        lv_obj_remove_style(s_filter_btns[i], &s_style_btn_active, 0);
        lv_obj_add_style(s_filter_btns[i],
                         (i == (uint32_t)s_filter) ? &s_style_btn_active : &s_style_btn,
                         0);
    }
}

static void refresh_detail(void)
{
    if (s_selected_idx == UINT32_MAX || s_selected_idx >= s_record_count) {
        lv_label_set_text(s_detail_time, "--");
        lv_label_set_text(s_detail_win, "--");
        lv_label_set_text(s_detail_score, "--");
        lv_label_set_text(s_detail_level, "--");
        lv_label_set_text(s_detail_pr, "-- bpm");
        lv_label_set_text(s_detail_ppi, "-- ms");
        lv_label_set_text(s_detail_count, "--");

        for (uint32_t i = 0; i < 9U; i++) {
            lv_label_set_text(s_feature_value[i], "--");
        }
        return;
    }

    const history_record_t *rec = &s_records[s_selected_idx];
    const PPI_Features_T *f = &rec->anxiety.features;
    char tmp[32];

    lv_label_set_text(s_detail_time, rec->time_text);
    lv_label_set_text_fmt(s_detail_win, "#%" PRIu32, rec->win_id);

    format_float(tmp, sizeof(tmp), rec->anxiety.anxiety_idx, 1, "");
    lv_label_set_text(s_detail_score, tmp);
    lv_obj_set_style_text_color(s_detail_score, c(0x2ceaff), 0);

    lv_label_set_text(s_detail_level, level_text(rec->anxiety.anxiety_idx));
    lv_obj_set_style_text_color(s_detail_level, level_color(rec->anxiety.anxiety_idx), 0);

    lv_label_set_text_fmt(s_detail_pr, "%u bpm", f->PR);
    format_float(tmp, sizeof(tmp), rec->has_ppi ? rec->ppi.mean_ppi : 0.0f, 1, " ms");
    lv_label_set_text(s_detail_ppi, rec->has_ppi ? tmp : "-- ms");
    lv_label_set_text_fmt(s_detail_count, "%u", rec->has_ppi ? rec->ppi.ppi_count : 0U);

    lv_label_set_text_fmt(s_feature_value[0], "%u", f->PR);
    format_float(tmp, sizeof(tmp), f->RMSSD, 1, ""); lv_label_set_text(s_feature_value[1], tmp);
    format_float(tmp, sizeof(tmp), f->SD2, 1, "");   lv_label_set_text(s_feature_value[2], tmp);
    format_float(tmp, sizeof(tmp), f->HF, 1, "");    lv_label_set_text(s_feature_value[3], tmp);
    format_float(tmp, sizeof(tmp), f->LF, 1, "");    lv_label_set_text(s_feature_value[4], tmp);
    format_float(tmp, sizeof(tmp), f->LF_HF, 2, ""); lv_label_set_text(s_feature_value[5], tmp);
    format_float(tmp, sizeof(tmp), f->WE, 2, "");    lv_label_set_text(s_feature_value[6], tmp);
    format_float(tmp, sizeof(tmp), f->DFA, 2, "");   lv_label_set_text(s_feature_value[7], tmp);
    format_float(tmp, sizeof(tmp), f->VAI, 1, "");   lv_label_set_text(s_feature_value[8], tmp);
}

#if LV_DRAW_SW_COMPLEX
static lv_opa_t trend_area_opa_from_y(int32_t y, int32_t top, int32_t full_h)
{
    if (full_h <= 0) return LV_OPA_TRANSP;

    int32_t frac = ((y - top) * 255) / full_h;
    if (frac < 0) frac = 0;
    if (frac > 255) frac = 255;

    int32_t opa = (255 - frac) / 3;
    if (opa > 70) opa = 70;
    if (opa < 0) opa = 0;
    return (lv_opa_t)opa;
}

static void trend_faded_area_event_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_target_obj(e);
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);

    if (base_dsc == NULL || base_dsc->part != LV_PART_ITEMS) return;
    if (lv_draw_task_get_type(draw_task) != LV_DRAW_TASK_TYPE_LINE) return;

    lv_draw_line_dsc_t *line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (line_dsc == NULL) return;

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);

    int32_t full_h = lv_obj_get_height(chart);
    int32_t y_top = LV_MIN(line_dsc->p1.y, line_dsc->p2.y);
    int32_t y_bot = LV_MAX(line_dsc->p1.y, line_dsc->p2.y);

    lv_draw_triangle_dsc_t tri_dsc;
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.p[0].x = line_dsc->p1.x;
    tri_dsc.p[0].y = line_dsc->p1.y;
    tri_dsc.p[1].x = line_dsc->p2.x;
    tri_dsc.p[1].y = line_dsc->p2.y;
    tri_dsc.p[2].x = line_dsc->p1.y < line_dsc->p2.y ? line_dsc->p1.x : line_dsc->p2.x;
    tri_dsc.p[2].y = y_bot;
    tri_dsc.grad.dir = LV_GRAD_DIR_VER;
    tri_dsc.grad.stops[0].color = line_dsc->color;
    tri_dsc.grad.stops[0].opa = trend_area_opa_from_y(y_top, coords.y1, full_h);
    tri_dsc.grad.stops[0].frac = 0;
    tri_dsc.grad.stops[1].color = line_dsc->color;
    tri_dsc.grad.stops[1].opa = trend_area_opa_from_y(y_bot, coords.y1, full_h);
    tri_dsc.grad.stops[1].frac = 255;
    lv_draw_triangle(base_dsc->layer, &tri_dsc);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    rect_dsc.bg_grad.stops[0].color = line_dsc->color;
    rect_dsc.bg_grad.stops[0].opa = trend_area_opa_from_y(y_bot, coords.y1, full_h);
    rect_dsc.bg_grad.stops[0].frac = 0;
    rect_dsc.bg_grad.stops[1].color = line_dsc->color;
    rect_dsc.bg_grad.stops[1].opa = LV_OPA_TRANSP;
    rect_dsc.bg_grad.stops[1].frac = 255;

    lv_area_t rect_area = {
        .x1 = line_dsc->p1.x,
        .x2 = line_dsc->p2.x - 1,
        .y1 = y_bot - 1,
        .y2 = coords.y2,
    };
    lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
}
#endif

static void refresh_trend(void)
{
    uint32_t trend_idx[APP_HISTORY_TREND_POINTS];
    uint32_t trend_count = 0;

    for (uint32_t i = 0; i < APP_HISTORY_TREND_POINTS; i++) {
        s_trend_values[i] = LV_CHART_POINT_NONE;
        lv_label_set_text(s_trend_xlabels[i], "");
    }

    if (s_selected_idx != UINT32_MAX) {
        for (uint32_t i = 0; i <= s_selected_idx && i < s_record_count; i++) {
            if (!s_records[i].has_anxiety || s_records[i].anxiety.valid == 0U) continue;

            if (trend_count < APP_HISTORY_TREND_POINTS) {
                trend_idx[trend_count++] = i;
            } else {
                memmove(&trend_idx[0], &trend_idx[1], sizeof(trend_idx[0]) * (APP_HISTORY_TREND_POINTS - 1U));
                trend_idx[APP_HISTORY_TREND_POINTS - 1U] = i;
            }
        }
    }

    uint32_t offset = APP_HISTORY_TREND_POINTS - trend_count;
    for (uint32_t i = 0; i < trend_count; i++) {
        const history_record_t *rec = &s_records[trend_idx[i]];
        uint32_t dst = offset + i;

        int32_t value = (int32_t)lroundf(rec->anxiety.anxiety_idx);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        s_trend_values[dst] = value;

        lv_label_set_text_fmt(s_trend_xlabels[dst], "#%" PRIu32, rec->win_id);
    }

    for (uint32_t i = 0; i < APP_HISTORY_TREND_POINTS; i++) {
        lv_chart_set_series_value_by_id(s_trend.chart, s_trend.series, i, s_trend_values[i]);
    }
    lv_chart_refresh(s_trend.chart);

    if (s_selected_idx != UINT32_MAX && s_selected_idx < s_record_count) {
        char tmp[32];
        format_float(tmp, sizeof(tmp), s_records[s_selected_idx].anxiety.anxiety_idx, 1, "");
        lv_label_set_text(s_trend_score_label, tmp);
    } else {
        lv_label_set_text(s_trend_score_label, "--");
    }
}

static void refresh_summary(void)
{
    float sum_score = 0.0f;
    float sum_pr = 0.0f;
    float max_score = -1.0f;
    float min_score = 101.0f;

    for (uint32_t i = 0; i < s_filtered_count; i++) {
        const history_record_t *rec = &s_records[s_filtered[i]];
        float score = rec->anxiety.anxiety_idx;
        sum_score += score;
        sum_pr += rec->anxiety.features.PR;

        if (score > max_score) max_score = score;
        if (score < min_score) min_score = score;
    }

    lv_label_set_text_fmt(s_total_label, "%" PRIu32, s_record_count);
    lv_label_set_text_fmt(s_filtered_label, "%" PRIu32, s_filtered_count);

    char tmp[32];
    if (s_filtered_count == 0U) {
        lv_label_set_text(s_avg_score_label, "--");
        lv_label_set_text(s_max_score_label, "--");
        lv_label_set_text(s_min_score_label, "--");
        lv_label_set_text(s_avg_pr_label, "-- bpm");
        return;
    }

    format_float(tmp, sizeof(tmp), sum_score / (float)s_filtered_count, 1, "");
    lv_label_set_text(s_avg_score_label, tmp);

    format_float(tmp, sizeof(tmp), max_score, 1, "");
    lv_label_set_text(s_max_score_label, tmp);

    format_float(tmp, sizeof(tmp), min_score, 1, "");
    lv_label_set_text(s_min_score_label, tmp);

    format_float(tmp, sizeof(tmp), sum_pr / (float)s_filtered_count, 0, " bpm");
    lv_label_set_text(s_avg_pr_label, tmp);
}

static void setup_history_table_columns(lv_obj_t *table)
{
    lv_table_set_column_count(table, 7);
    lv_table_set_column_width(table, 0, 76);
    lv_table_set_column_width(table, 1, 56);
    lv_table_set_column_width(table, 2, 70);
    lv_table_set_column_width(table, 3, 86);
    lv_table_set_column_width(table, 4, 70);
    lv_table_set_column_width(table, 5, 82);
    lv_table_set_column_width(table, 6, 58);
}
static void style_history_table(lv_obj_t *table)
{
    lv_obj_set_style_text_font(table, ui_font(), LV_PART_ITEMS);
    lv_obj_set_style_text_align(table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, c(0x1d4258), LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 7, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 7, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 2, LV_PART_ITEMS);
}

static void table_draw_event_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    bool is_header = (target == s_table_header);

    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc == NULL || base_dsc->part != LV_PART_ITEMS) return;

    uint32_t row = base_dsc->id1;
    uint32_t col = base_dsc->id2;

    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(draw_task);
    if (label) {
        label->align = LV_TEXT_ALIGN_CENTER;
        if (is_header) {
            label->color = c(0xf4f8ff);
        } else if (col == 3U && row < s_filtered_count) {
            const history_record_t *rec = &s_records[s_filtered[row]];
            label->color = level_color(rec->anxiety.anxiety_idx);
        }
    }

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(draw_task);
    if (!fill) return;

    if (is_header) {
        fill->color = c(0x0b3654);
        fill->opa = LV_OPA_COVER;
        return;
    }

    if (row < s_filtered_count && s_filtered[row] == s_selected_idx) {
        fill->color = c(0x075f92);
        fill->opa = LV_OPA_70;
    } else if ((row % 2U) == 0U) {
        fill->color = c(0x061f31);
        fill->opa = LV_OPA_70;
    } else {
        fill->color = c(0x08283d);
        fill->opa = LV_OPA_70;
    }
}
/*列表自动滚动*/
static void history_table_scroll_latest_async(void *user_data)
{
    (void)user_data;

    if (s_table == NULL || s_filtered_count == 0U) {
        return;
    }

    lv_obj_update_layout(s_table);
    lv_obj_scroll_to_y(s_table, LV_COORD_MAX, LV_ANIM_ON);
}

static void history_table_scroll_to_latest(void)
{
    if (s_table == NULL || s_filtered_count == 0U) {
        return;
    }

    lv_async_call(history_table_scroll_latest_async, NULL);
}

/*不显示界面时只存数据，切换显示界面时刷新界面数据*/
static void refresh_table(void);

static bool history_page_visible(void)
{
    return s_page != NULL && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

static void history_request_refresh(void)
{
    s_history_dirty = true;

    if (!history_page_visible()) {
        return;
    }

    s_history_dirty = false;
    refresh_table();
}

void app_ui_history_on_show(void)
{
    if (!s_page) {
        return;
    }

    if (s_history_dirty) {
        s_history_dirty = false;
        refresh_table();
    }
}

static void refresh_table(void)
{
    rebuild_filtered_index();

    setup_history_table_columns(s_table_header);
    lv_table_set_row_count(s_table_header, 1);
    lv_table_set_cell_value(s_table_header, 0, 0, "时间");
    lv_table_set_cell_value(s_table_header, 0, 1, "窗口号");
    lv_table_set_cell_value(s_table_header, 0, 2, "焦虑指数");
    lv_table_set_cell_value(s_table_header, 0, 3, "等级");
    lv_table_set_cell_value(s_table_header, 0, 4, "PR");
    lv_table_set_cell_value(s_table_header, 0, 5, "平均PPI");
    lv_table_set_cell_value(s_table_header, 0, 6, "PPI数");

    setup_history_table_columns(s_table);
    lv_table_set_row_count(s_table, s_filtered_count > 0U ? s_filtered_count : 1U);

    if (s_filtered_count == 0U) {
        for (uint32_t col = 0; col < 7U; col++) {
            lv_table_set_cell_value(s_table, 0, col, col == 0U ? "--" : "");
        }
    }

    char tmp[32];
    for (uint32_t i = 0; i < s_filtered_count; i++) {
        const history_record_t *rec = &s_records[s_filtered[i]];
        uint32_t row = i;

        lv_table_set_cell_value(s_table, row, 0, rec->time_text);
        lv_table_set_cell_value_fmt(s_table, row, 1, "#%" PRIu32, rec->win_id);

        format_float(tmp, sizeof(tmp), rec->anxiety.anxiety_idx, 1, "");
        lv_table_set_cell_value(s_table, row, 2, tmp);

        lv_table_set_cell_value(s_table, row, 3, level_text(rec->anxiety.anxiety_idx));
        lv_table_set_cell_value_fmt(s_table, row, 4, "%u bpm", rec->anxiety.features.PR);

        if (rec->has_ppi) {
            format_float(tmp, sizeof(tmp), rec->ppi.mean_ppi, 0, " ms");
            lv_table_set_cell_value(s_table, row, 5, tmp);
            lv_table_set_cell_value_fmt(s_table, row, 6, "%u", rec->ppi.ppi_count);
        } else {
            lv_table_set_cell_value(s_table, row, 5, "-- ms");
            lv_table_set_cell_value(s_table, row, 6, "--");
        }

        for (uint32_t col = 0; col < 7U; col++) {
            lv_table_set_cell_ctrl(s_table, row, col, LV_TABLE_CELL_CTRL_TEXT_CROP);
        }
    }

    uint16_t selected_row = LV_TABLE_CELL_NONE;
    for (uint32_t i = 0; i < s_filtered_count; i++) {
        if (s_filtered[i] == s_selected_idx) {
            selected_row = (uint16_t)i;
            break;
        }
    }
    lv_table_set_selected_cell(s_table, selected_row, 0);

    if (s_follow_latest && s_filtered_count > 0U) {
         history_table_scroll_to_latest();
    }

    refresh_summary();
    refresh_detail();
    refresh_trend();
}


static esp_err_t export_csv_to_uart(void)
{
    rebuild_filtered_index();

    printf("\n===PPI_HISTORY_CSV_BEGIN===\n");
    printf("No,Time,Window,AnxietyIndex,Level,Ave_PR(bpm),MeanPPI(ms),PPICount,"
           "RMSSD(ms),SD2(bpm),HF(ms2),LF(ms2),LF/HF,WE,DFA,VAI(deg)\n");

    for (uint32_t i = 0; i < s_filtered_count; i++) {
        const history_record_t *rec = &s_records[s_filtered[i]];
        const PPI_Features_T *f = &rec->anxiety.features;

        printf("%" PRIu32 ",%s,#%" PRIu32 ",%.1f,%s,%u,%.1f,%u,"
               "%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f\n",
               i + 1U,
               rec->time_text,
               rec->win_id,
               (double)rec->anxiety.anxiety_idx,
               level_text(rec->anxiety.anxiety_idx),
               f->PR,
               rec->has_ppi ? (double)rec->ppi.mean_ppi : 0.0,
               rec->has_ppi ? rec->ppi.ppi_count : 0U,
               (double)f->RMSSD,
               (double)f->SD2,
               (double)f->HF,
               (double)f->LF,
               (double)f->LF_HF,
               (double)f->WE,
               (double)f->DFA,
               (double)f->VAI);
    }

    printf("===PPI_HISTORY_CSV_END===\n");
    fflush(stdout);
    return ESP_OK;
}
/*导出按钮事件*/
static void export_event_cb(lv_event_t *e)
{
    (void)e;

    esp_err_t ret = export_csv_to_uart();
    if (ret == ESP_OK) {
        lv_label_set_text(s_export_status, "CSV已发送到串口");
    } else {
        lv_label_set_text(s_export_status, "CSV发送失败");
    }
}

static void clear_event_cb(lv_event_t *e)
{
    (void)e;

    s_record_count = 0;
    s_filtered_count = 0;
    s_selected_idx = UINT32_MAX;
    s_follow_latest = true;
    lv_label_set_text(s_export_status, "已清空本次历史记录");
    refresh_table();
}

static void filter_event_cb(lv_event_t *e)
{
    history_filter_t filter = (history_filter_t)(uintptr_t)lv_event_get_user_data(e);
    s_filter = filter;
    s_follow_latest = false;
    update_filter_buttons();
    refresh_table();
}

static void table_event_cb(lv_event_t *e)
{
    lv_obj_t *table = lv_event_get_target_obj(e);
    uint32_t row = LV_TABLE_CELL_NONE;
    uint32_t col = LV_TABLE_CELL_NONE;

    lv_table_get_selected_cell(table, &row, &col);
    if (row == LV_TABLE_CELL_NONE) return;
    if (row >= s_filtered_count) return;

    s_selected_idx = s_filtered[row];
    /*手动点历史记录时，允许查看旧记录*/
    s_follow_latest = false;
    refresh_detail();
    refresh_trend();
    lv_obj_invalidate(s_table);
}



static void build_query_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 8, 226, 232);
    make_label(card, "历史查询", 10, 8);

    make_label(card, "状态筛选:", 10, 46);

    s_filter_btns[0] = make_button(card, "全部", 10, 76, 48, 32, &s_style_btn_active);
    s_filter_btns[1] = make_button(card, "低焦虑", 64, 76, 66, 32, &s_style_btn);
    s_filter_btns[2] = make_button(card, "中等", 136, 76, 50, 32, &s_style_btn);
    s_filter_btns[3] = make_button(card, "高焦虑", 10, 116, 66, 32, &s_style_btn);

    for (uint32_t i = 0; i < 4U; i++) {
        lv_obj_add_event_cb(s_filter_btns[i], filter_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    lv_obj_t *export_btn = make_button(card, LV_SYMBOL_DOWNLOAD " 导出CSV", 10, 155, 96, 34, &s_style_btn_active);
    lv_obj_add_event_cb(export_btn, export_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_btn = make_button(card, LV_SYMBOL_TRASH " 清空", 116, 155, 82, 34, &s_style_btn_danger);
    lv_obj_add_event_cb(clear_btn, clear_event_cb, LV_EVENT_CLICKED, NULL);

    s_export_status = make_label(card, "CSV: 等待导出", 10, 200);
    lv_obj_add_style(s_export_status, &s_style_muted, 0);
}

static void build_summary_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 248, 226, 242);
    make_label(card, "记录概览", 10, 8);

    metric_row(card, "总记录数:", 46, &s_total_label);
    metric_row(card, "当前筛选:", 78, &s_filtered_label);
    metric_row(card, "平均焦虑:", 110, &s_avg_score_label);
    metric_row(card, "最高焦虑:", 142, &s_max_score_label);
    metric_row(card, "最低焦虑:", 174, &s_min_score_label);
    metric_row(card, "平均PR:", 206, &s_avg_pr_label);

    lv_obj_add_style(s_total_label, &s_style_cyan, 0);
    lv_obj_add_style(s_filtered_label, &s_style_cyan, 0);
    lv_obj_add_style(s_avg_score_label, &s_style_cyan, 0);
    lv_obj_add_style(s_max_score_label, &s_style_bad, 0);
    lv_obj_add_style(s_min_score_label, &s_style_good, 0);
}

static void build_table_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 244, 8, 520, 300);
    make_label(card, "历史记录列表", 4, 8);

    s_table_header = lv_table_create(card);
    lv_obj_set_pos(s_table_header, 4, 38);
    lv_obj_set_size(s_table_header, 500, 36);
    style_history_table(s_table_header);
    lv_obj_clear_flag(s_table_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_table_header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_table_header, table_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(s_table_header, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);

    s_table = lv_table_create(card);
    lv_obj_set_pos(s_table, 4, 74);
    lv_obj_set_size(s_table, 500, 216);
    style_history_table(s_table);

    lv_obj_set_scroll_dir(s_table, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_table, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_width(s_table, 10, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_table, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_table, c(0x6fa7c4), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_table, LV_OPA_70, LV_PART_SCROLLBAR);

    lv_obj_add_event_cb(s_table, table_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_table, table_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(s_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
}

static void build_trend_panel(lv_obj_t *parent)
{
    s_trend.card = make_card(parent, 244, 316, 520, 174);
    make_label(s_trend.card, "所选记录趋势预览（焦虑指数）", 10, 8);

    make_label(s_trend.card, "100", 12, 34);
    make_label(s_trend.card, "50", 20, 85);
    make_label(s_trend.card, "0", 28, 134);

    s_trend.chart = lv_chart_create(s_trend.card);
    lv_obj_set_pos(s_trend.chart, 50, 34);
    lv_obj_set_size(s_trend.chart, 450, 108);
    lv_chart_set_type(s_trend.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_trend.chart, APP_HISTORY_TREND_POINTS);
    lv_chart_set_axis_range(s_trend.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_trend.chart, 4, 6);
    lv_obj_set_style_bg_opa(s_trend.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_trend.chart, 0, 0);
    lv_obj_set_style_line_color(s_trend.chart, c(0x24495e), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_trend.chart, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_trend.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_trend.chart, 5, 5, LV_PART_INDICATOR);
    s_trend.series = lv_chart_add_series(s_trend.chart, c(0x2ceaff), LV_CHART_AXIS_PRIMARY_Y);

#if LV_DRAW_SW_COMPLEX
    lv_obj_add_event_cb(s_trend.chart, trend_faded_area_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(s_trend.chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
#endif

    for (uint32_t i = 0; i < APP_HISTORY_TREND_POINTS; i++) {
        int32_t x = 48 + (int32_t)i * 58;
        s_trend_xlabels[i] = make_label(s_trend.card, "", x, 146);
        lv_obj_add_style(s_trend_xlabels[i], &s_style_muted, 0);
    }

    s_trend_score_label = make_label(s_trend.card, "--", 455, 10);
    lv_obj_add_style(s_trend_score_label, &s_style_cyan, 0);
}

static void build_detail_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 772, 8, 244, 182);
    make_label(card, "记录详情", 7, 8);

    metric_row(card, "时间:", 29, &s_detail_time);
    metric_row(card, "窗口号:", 53, &s_detail_win);
    metric_row(card, "焦虑指数:", 77, &s_detail_score);
    metric_row(card, "等级:", 101, &s_detail_level);
    metric_row(card, "平均PR:", 125, &s_detail_pr);
    metric_row(card, "平均PPI:", 149, &s_detail_ppi);
    metric_row(card, "PPI数:", 173, &s_detail_count);

    
    s_detail_count = make_label(card, "--", 0, 0);
     lv_obj_add_flag(s_detail_count, LV_OBJ_FLAG_HIDDEN);
}

static void build_features_panel(lv_obj_t *parent)
{
    static const char *const names[9] = {
        "1.PR", "2.RMSSD", "3.SD2",
        "4.HF", "5.LF", "6.LF/HF",
        "7.WE", "8.DFA", "9.VAI",
    };
    static const char *const units[9] = {
        "bpm", "ms", "bpm", "ms2", "ms2", "", "", "", "deg",
    };

    lv_obj_t *card = make_card(parent, 772, 198, 244, 292);
    make_label(card, "9项特征参数(所选记录)", 10, 8);

    int32_t box_w = 68;
    int32_t box_h = 62;
    int32_t gap = 7;
    int32_t x0 = 10;
    int32_t y0 = 42;

    for (uint32_t i = 0; i < 9U; i++) {
        int32_t col = (int32_t)(i % 3U);
        int32_t row = (int32_t)(i / 3U);

        lv_obj_t *box = make_card(card,
                                  x0 + col * (box_w + gap),
                                  y0 + row * (box_h + gap),
                                  box_w,
                                  box_h);
        lv_obj_set_style_pad_all(box, 4, 0);

        make_label(box, names[i], 1, 2);

        s_feature_value[i] = make_label(box, "--", 8, 25);
        lv_obj_add_style(s_feature_value[i], &s_style_cyan, 0);

        s_feature_unit[i] = make_label(box, units[i], 18, 39);
        lv_obj_add_style(s_feature_unit[i], &s_style_muted, 0);
    }
}

lv_obj_t *app_ui_history_create(lv_obj_t *root)
{
    init_styles();

    s_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_page);
    lv_obj_add_style(s_page, &s_style_page, 0);
    lv_obj_set_pos(s_page, 0, APP_UI_HEADER_H);
    lv_obj_set_size(s_page, APP_LCD_H_RES, APP_UI_PAGE_H);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    build_query_panel(s_page);
    build_summary_panel(s_page);
    build_table_panel(s_page);
    build_trend_panel(s_page);
    build_detail_panel(s_page);
    build_features_panel(s_page);

    refresh_table();
    return s_page;
}

void app_ui_history_add_ppi(uint32_t win_id, const PPI_RESULT_T *result)
{
    if (!result) return;

    history_record_t *rec = upsert_record(win_id);
    if (!rec) return;

    rec->ppi = *result;
    rec->has_ppi = true;

   if (rec->has_anxiety) {
    history_request_refresh();
    }
}

void app_ui_history_add_anxiety(const ANXIETY_MSG_T *msg)
{
    if (!msg) return;

    history_record_t *rec = upsert_record(msg->WIN_ID);
    if (!rec) return;

    rec->anxiety = *msg;
    rec->has_anxiety = true;
    /*新窗口数据强制跟随最新记录*/
    if (msg->valid != 0U) {
         uint32_t idx = (uint32_t)(rec - s_records);
         s_selected_idx = idx;
         s_follow_latest = true;
    }

    history_request_refresh();
}

void app_ui_history_reset(void)
{
    s_record_count = 0;
    s_filtered_count = 0;
    s_selected_idx = UINT32_MAX;
    s_follow_latest = true;

    history_request_refresh();
}

uint32_t app_ui_history_get_record_count(void)
{
    return s_record_count;
}