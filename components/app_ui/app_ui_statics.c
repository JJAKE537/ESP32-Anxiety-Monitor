/* app_ui_statics.c */
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_lvgl.h"
#include "lvgl.h"

#include "src/draw/lv_draw.h"
#include "src/draw/lv_draw_label.h"
#include "src/draw/lv_draw_line.h"
#include "src/draw/lv_draw_rect.h"
#include "src/draw/lv_draw_triangle.h"

LV_FONT_DECLARE(app_ui_font_16);

#define APP_LCD_H_RES              1024
#define APP_UI_HEADER_H            42
#define APP_UI_NAV_Y               548
#define APP_UI_PAGE_H              (APP_UI_NAV_Y - APP_UI_HEADER_H - 6)

#define APP_STATS_TREND_POINTS     31U
#define APP_STATS_AXIS_LABELS      5U
#define APP_STATS_FEATURE_COUNT    9U
#define APP_STATS_LEVEL_COUNT      3U
#define APP_STATS_MIN_VALID_PPI    25U



typedef enum {
    STATS_RANGE_SESSION = 0,
    STATS_RANGE_5_MIN,
    STATS_RANGE_30_MIN,
    STATS_RANGE_COUNT
} stats_range_t;

typedef enum {
    STATS_LEVEL_LOW = 0,
    STATS_LEVEL_MID,
    STATS_LEVEL_HIGH
} stats_level_t;

typedef struct {
    uint32_t win_id;
    time_t ts;
    char time_text[12];

    bool has_ppi;
    bool has_anxiety;

    PPI_RESULT_T ppi;
    ANXIETY_MSG_T anxiety;
} stats_record_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *chart;
    lv_chart_series_t *series;
} stats_chart_t;

typedef struct {
    uint32_t count;
    float sum;
    float min;
    float max;
    float first;
    float last;
} feature_acc_t;

typedef struct {
    uint32_t range_count;
    uint32_t valid_count;
    uint32_t abnormal_count;
    uint32_t level_count[APP_STATS_LEVEL_COUNT];

    float sum_score;
    float max_score;
    float min_score;
    float sum_pr;
    float sum_ppi;

    uint32_t duration_sec;
    feature_acc_t feature[APP_STATS_FEATURE_COUNT];
} stats_snapshot_t;

static lv_style_t s_style_page;
static lv_style_t s_style_card;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_active;
static lv_style_t s_style_muted;
static lv_style_t s_style_cyan;
static lv_style_t s_style_good;
static lv_style_t s_style_warn;
static lv_style_t s_style_bad;
static bool s_style_inited;

static stats_record_t *s_records;
static uint32_t s_record_count;
static uint32_t s_record_cap;
static time_t s_session_start_ts;

static stats_range_t s_range = STATS_RANGE_SESSION;

static lv_obj_t *s_page;
static lv_obj_t *s_range_btns[STATS_RANGE_COUNT];

static lv_obj_t *s_avg_score_label;
static lv_obj_t *s_max_score_label;
static lv_obj_t *s_min_score_label;
static lv_obj_t *s_avg_pr_label;
static lv_obj_t *s_avg_ppi_label;
static lv_obj_t *s_valid_count_label;
static lv_obj_t *s_abnormal_count_label;
static lv_obj_t *s_duration_label;

static stats_chart_t s_anxiety_chart;
static stats_chart_t s_pr_chart;
static int32_t s_anxiety_values[APP_STATS_TREND_POINTS];
static int32_t s_pr_values[APP_STATS_TREND_POINTS];
static lv_obj_t *s_anxiety_xlabels[APP_STATS_AXIS_LABELS];
static lv_obj_t *s_pr_xlabels[APP_STATS_AXIS_LABELS];

static lv_obj_t *s_ratio_base_arc;
static lv_obj_t *s_ratio_arcs[APP_STATS_LEVEL_COUNT];
static lv_obj_t *s_ratio_pct_labels[APP_STATS_LEVEL_COUNT];

static lv_obj_t *s_dist_fill[APP_STATS_LEVEL_COUNT];
static lv_obj_t *s_dist_count_labels[APP_STATS_LEVEL_COUNT];
static lv_obj_t *s_dist_total_label;

static lv_obj_t *s_feature_table_header;
static lv_obj_t *s_feature_table;
static int8_t s_feature_trend[APP_STATS_FEATURE_COUNT];

static bool s_stats_dirty;
/*======================================================================================================================*/
static const lv_font_t *ui_font(void)
{
    return &app_ui_font_16;
}

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

void app_ui_statics_apply_theme(void)
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

    if (decimals > 4) decimals = 4;

    int32_t scale = 1;
    for (uint8_t i = 0; i < decimals; i++) scale *= 10;

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

static void format_elapsed(char *buf, size_t len, uint32_t sec)
{
    uint32_t h = sec / 3600U;
    uint32_t m = (sec % 3600U) / 60U;
    uint32_t s = sec % 60U;
    snprintf(buf, len, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, h, m, s);
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

static void format_time_short(const stats_record_t *rec, char *buf, size_t len)
{
    if (!rec || len < 6U) {
        snprintf(buf, len, "--:--");
        return;
    }
    snprintf(buf, len, "%.5s", rec->time_text);
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
        app_ui_statics_apply_theme();
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
    make_label(parent, name, 14, y);

    lv_obj_t *val = make_label(parent, "--", 146, y);
    lv_obj_set_width(val, 86);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);

    if (out) *out = val;
}

static bool ensure_record_capacity(uint32_t need)
{
    if (need <= s_record_cap) return true;

    uint32_t new_cap = s_record_cap ? s_record_cap * 2U : 64U;
    while (new_cap < need) new_cap *= 2U;

    stats_record_t *new_records = realloc(s_records, sizeof(stats_record_t) * new_cap);
    if (!new_records) return false;

    memset(&new_records[s_record_cap], 0, sizeof(stats_record_t) * (new_cap - s_record_cap));
    s_records = new_records;
    s_record_cap = new_cap;
    return true;
}

static int32_t find_record(uint32_t win_id)
{
    for (int32_t i = (int32_t)s_record_count - 1; i >= 0; i--) {
        if (s_records[i].win_id == win_id) return i;
    }
    return -1;
}

static stats_record_t *upsert_record(uint32_t win_id)
{
    int32_t idx = find_record(win_id);
    if (idx >= 0) return &s_records[idx];

    if (!ensure_record_capacity(s_record_count + 1U)) return NULL;

    stats_record_t *rec = &s_records[s_record_count++];
    memset(rec, 0, sizeof(*rec));
    rec->win_id = win_id;
    rec->ts = time(NULL);
    format_time_text(rec->ts, rec->time_text, sizeof(rec->time_text));
    return rec;
}

static uint32_t range_limit_sec(stats_range_t range)
{
    if (range == STATS_RANGE_5_MIN) return 5U * 60U;
    if (range == STATS_RANGE_30_MIN) return 30U * 60U;
    return UINT32_MAX;
}

static bool record_in_current_range(const stats_record_t *rec, time_t now)
{
    if (!rec) return false;
    if (s_range == STATS_RANGE_SESSION) return true;

    double age = difftime(now, rec->ts);
    if (age < 0.0) return true;

    return age <= (double)range_limit_sec(s_range);
}

static bool record_is_abnormal(const stats_record_t *rec)
{
    return rec && rec->has_ppi && rec->ppi.ppi_count < APP_STATS_MIN_VALID_PPI;
}

static bool record_is_valid_sample(const stats_record_t *rec)
{
    if (!rec) return false;
    if (!rec->has_ppi || !rec->has_anxiety) return false;
    if (rec->ppi.ppi_count < APP_STATS_MIN_VALID_PPI) return false;
    if (rec->ppi.valid == 0U || rec->anxiety.valid == 0U) return false;
    return isfinite(rec->anxiety.anxiety_idx);
}

static stats_level_t level_from_score(float score)
{
    app_lvgl_anxiety_thresholds_t th;
    app_lvgl_get_anxiety_thresholds(&th);

    if (score < (float)th.low_max) return STATS_LEVEL_LOW;
    if (score < (float)th.mid_max) return STATS_LEVEL_MID;
    return STATS_LEVEL_HIGH;
}

static lv_color_t level_color(stats_level_t level)
{
    if (level == STATS_LEVEL_LOW) return c(0x93e348);
    if (level == STATS_LEVEL_MID) return c(0xffca19);
    return c(0xff3f37);
}

static float feature_value(const PPI_Features_T *f, uint32_t idx)
{
    switch (idx) {
    case 0: return (float)f->PR;
    case 1: return f->RMSSD;
    case 2: return f->SD2;
    case 3: return f->HF;
    case 4: return f->LF;
    case 5: return f->LF_HF;
    case 6: return f->WE;
    case 7: return f->DFA;
    case 8: return f->VAI;
    default: return NAN;
    }
}

static void snapshot_init(stats_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->max_score = -FLT_MAX;
    snap->min_score = FLT_MAX;

    for (uint32_t i = 0; i < APP_STATS_FEATURE_COUNT; i++) {
        snap->feature[i].min = FLT_MAX;
        snap->feature[i].max = -FLT_MAX;
    }
}

static void feature_acc_add(feature_acc_t *acc, float value)
{
    if (!acc || !isfinite(value)) return;

    if (acc->count == 0U) {
        acc->first = value;
        acc->min = value;
        acc->max = value;
    } else {
        if (value < acc->min) acc->min = value;
        if (value > acc->max) acc->max = value;
    }

    acc->last = value;
    acc->sum += value;
    acc->count++;
}

static void append_trend_index(uint32_t *idx, uint32_t *count, uint32_t record_idx)
{
    if (*count < APP_STATS_TREND_POINTS) {
        idx[(*count)++] = record_idx;
        return;
    }

    memmove(&idx[0], &idx[1], sizeof(idx[0]) * (APP_STATS_TREND_POINTS - 1U));
    idx[APP_STATS_TREND_POINTS - 1U] = record_idx;
}

static void build_snapshot(stats_snapshot_t *snap, uint32_t *trend_idx, uint32_t *trend_count)
{
    snapshot_init(snap);
    *trend_count = 0;

    time_t now = time(NULL);
    time_t first_ts = 0;

    for (uint32_t i = 0; i < s_record_count; i++) {
        const stats_record_t *rec = &s_records[i];

        if (!rec->has_ppi) continue;
        if (!record_in_current_range(rec, now)) continue;

        if (first_ts == 0 || rec->ts < first_ts) first_ts = rec->ts;

        snap->range_count++;

        if (record_is_abnormal(rec)) {
            snap->abnormal_count++;
            continue;
        }

        if (!record_is_valid_sample(rec)) continue;

        const ANXIETY_MSG_T *msg = &rec->anxiety;
        const PPI_Features_T *f = &msg->features;
        float score = msg->anxiety_idx;

        snap->valid_count++;
        snap->sum_score += score;
        snap->sum_pr += (float)f->PR;
        snap->sum_ppi += rec->ppi.mean_ppi;

        if (score > snap->max_score) snap->max_score = score;
        if (score < snap->min_score) snap->min_score = score;

        stats_level_t level = level_from_score(score);
        snap->level_count[level]++;

        for (uint32_t j = 0; j < APP_STATS_FEATURE_COUNT; j++) {
            feature_acc_add(&snap->feature[j], feature_value(f, j));
        }

        append_trend_index(trend_idx, trend_count, i);
    }

    if (snap->range_count == 0U) {
        snap->duration_sec = 0;
    } else if (s_range == STATS_RANGE_SESSION) {
        time_t start = s_session_start_ts ? s_session_start_ts : first_ts;
        snap->duration_sec = (now > start) ? (uint32_t)difftime(now, start) : 0U;
    } else {
        uint32_t limit = range_limit_sec(s_range);
        uint32_t have = (first_ts > 0 && now > first_ts) ? (uint32_t)difftime(now, first_ts) : 0U;
        snap->duration_sec = have > limit ? limit : have;
    }
}

static void update_range_buttons(void)
{
    for (uint32_t i = 0; i < STATS_RANGE_COUNT; i++) {
        if (!s_range_btns[i]) continue;

        lv_obj_remove_style(s_range_btns[i], &s_style_btn, 0);
        lv_obj_remove_style(s_range_btns[i], &s_style_btn_active, 0);
        lv_obj_add_style(s_range_btns[i],
                         i == (uint32_t)s_range ? &s_style_btn_active : &s_style_btn,
                         0);
    }
}

#if LV_DRAW_SW_COMPLEX
static lv_opa_t trend_area_opa_from_y(int32_t y, int32_t top, int32_t full_h)
{
    if (full_h <= 0) return LV_OPA_TRANSP;

    int32_t frac = ((y - top) * 255) / full_h;
    if (frac < 0) frac = 0;
    if (frac > 255) frac = 255;

    int32_t opa = (255 - frac) * 110 / 255;
    if (opa > 110) opa = 110;
    if (opa < 0) opa = 0;
    return (lv_opa_t)opa;
}

static void trend_deep_area_event_cb(lv_event_t *e)
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

static void enable_trend_deep_area(lv_obj_t *chart)
{
#if LV_DRAW_SW_COMPLEX
    lv_obj_add_event_cb(chart, trend_deep_area_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
#else
    (void)chart;
#endif
}

static void chart_common_style(lv_obj_t *chart)
{
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, c(0x24495e), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 5, 5, LV_PART_INDICATOR);
}

static void make_y_axis_labels(lv_obj_t *card, int32_t chart_y, int32_t chart_h,
                               int32_t min, int32_t max)
{
    char top[12];
    char mid[12];
    char bot[12];

    snprintf(top, sizeof(top), "%ld", (long)max);
    snprintf(mid, sizeof(mid), "%ld", (long)((min + max) / 2));
    snprintf(bot, sizeof(bot), "%ld", (long)min);

    lv_obj_add_style(make_label(card, top, 8, chart_y - 2), &s_style_muted, 0);
    lv_obj_add_style(make_label(card, mid, 8, chart_y + chart_h / 2 - 8), &s_style_muted, 0);
    lv_obj_add_style(make_label(card, bot, 8, chart_y + chart_h - 16), &s_style_muted, 0);
}

static void build_chart_panel(lv_obj_t *parent, stats_chart_t *view, lv_obj_t **xlabels,
                              const char *title, const char *legend,
                              int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t min, int32_t max, uint32_t color)
{
    view->card = make_card(parent, x, y, w, h);
    make_label(view->card, title, 10, 8);

    lv_obj_t *legend_label = make_label(view->card, legend, w - 112, 8);
    lv_obj_set_style_text_color(legend_label, c(color), 0);

    int32_t chart_x = 48;
    int32_t chart_y = 34;
    int32_t chart_w = w - 68;
    int32_t chart_h = h - 70;
    if (chart_h < 34) chart_h = 34;

    make_y_axis_labels(view->card, chart_y, chart_h, min, max);

    view->chart = lv_chart_create(view->card);
    lv_obj_set_pos(view->chart, chart_x, chart_y);
    lv_obj_set_size(view->chart, chart_w, chart_h);
    lv_chart_set_type(view->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(view->chart, APP_STATS_TREND_POINTS);
    lv_chart_set_axis_range(view->chart, LV_CHART_AXIS_PRIMARY_Y, min, max);
    lv_chart_set_div_line_count(view->chart, 4, 6);
    lv_chart_set_update_mode(view->chart, LV_CHART_UPDATE_MODE_SHIFT);
    chart_common_style(view->chart);

    view->series = lv_chart_add_series(view->chart, c(color), LV_CHART_AXIS_PRIMARY_Y);
    enable_trend_deep_area(view->chart);

    int32_t label_y = chart_y + chart_h + 4;
    for (uint32_t i = 0; i < APP_STATS_AXIS_LABELS; i++) {
        int32_t lx = chart_x + (chart_w * (int32_t)i) / (int32_t)(APP_STATS_AXIS_LABELS - 1U) - 18;
        xlabels[i] = make_label(view->card, "", lx, label_y);
        lv_obj_add_style(xlabels[i], &s_style_muted, 0);
    }
}

static void update_chart_values(stats_chart_t *view, int32_t *values)
{
    if (!view->chart || !view->series) return;
    lv_chart_set_series_values(view->chart, view->series, values, APP_STATS_TREND_POINTS);
    lv_chart_refresh(view->chart);
}

static void refresh_metric_panel(const stats_snapshot_t *snap)
{
    char tmp[32];

    if (snap->valid_count == 0U) {
        lv_label_set_text(s_avg_score_label, "--");
        lv_label_set_text(s_max_score_label, "--");
        lv_label_set_text(s_min_score_label, "--");
        lv_label_set_text(s_avg_pr_label, "-- bpm");
        lv_label_set_text(s_avg_ppi_label, "-- ms");
    } else {
        format_float(tmp, sizeof(tmp), snap->sum_score / (float)snap->valid_count, 1, "");
        lv_label_set_text(s_avg_score_label, tmp);

        format_float(tmp, sizeof(tmp), snap->max_score, 1, "");
        lv_label_set_text(s_max_score_label, tmp);

        format_float(tmp, sizeof(tmp), snap->min_score, 1, "");
        lv_label_set_text(s_min_score_label, tmp);

        format_float(tmp, sizeof(tmp), snap->sum_pr / (float)snap->valid_count, 0, " bpm");
        lv_label_set_text(s_avg_pr_label, tmp);

        format_float(tmp, sizeof(tmp), snap->sum_ppi / (float)snap->valid_count, 0, " ms");
        lv_label_set_text(s_avg_ppi_label, tmp);
    }

    lv_label_set_text_fmt(s_valid_count_label, "%" PRIu32, snap->valid_count);
    lv_label_set_text_fmt(s_abnormal_count_label, "%" PRIu32, snap->abnormal_count);

    format_elapsed(tmp, sizeof(tmp), snap->duration_sec);
    lv_label_set_text(s_duration_label, tmp);
}

static void refresh_trend_charts(const uint32_t *trend_idx, uint32_t trend_count)
{
    for (uint32_t i = 0; i < APP_STATS_TREND_POINTS; i++) {
        s_anxiety_values[i] = LV_CHART_POINT_NONE;
        s_pr_values[i] = LV_CHART_POINT_NONE;
    }

    if (trend_count > 0U) {
        uint32_t offset = APP_STATS_TREND_POINTS - trend_count;

        for (uint32_t i = 0; i < trend_count; i++) {
            const stats_record_t *rec = &s_records[trend_idx[i]];
            uint32_t dst = offset + i;

            int32_t score = (int32_t)lroundf(rec->anxiety.anxiety_idx);
            if (score < 0) score = 0;
            if (score > 100) score = 100;

            int32_t pr = (int32_t)rec->anxiety.features.PR;
            if (pr < 40) pr = 40;
            if (pr > 140) pr = 140;

            s_anxiety_values[dst] = score;
            s_pr_values[dst] = pr;
        }

        for (uint32_t i = 0; i < APP_STATS_AXIS_LABELS; i++) {
            uint32_t src = (trend_count == 1U) ? 0U :
                           (uint32_t)(((uint64_t)i * (uint64_t)(trend_count - 1U)) /
                                      (uint64_t)(APP_STATS_AXIS_LABELS - 1U));

            char label[8];
            format_time_short(&s_records[trend_idx[src]], label, sizeof(label));
            lv_label_set_text(s_anxiety_xlabels[i], label);
            if(s_pr_xlabels[i]){
                lv_label_set_text(s_pr_xlabels[i], label);
            }
        }
    } else {
        for (uint32_t i = 0; i < APP_STATS_AXIS_LABELS; i++) {
            lv_label_set_text(s_anxiety_xlabels[i], "");
            if(s_pr_xlabels[i]){
                lv_label_set_text(s_pr_xlabels[i], "");
            }
        }
    }

    update_chart_values(&s_anxiety_chart, s_anxiety_values);
    update_chart_values(&s_pr_chart, s_pr_values);
}

static void refresh_ratio_panel(const stats_snapshot_t *snap)
{
    uint32_t total = snap->valid_count;
    uint32_t nonzero = 0U;

    for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
        if (snap->level_count[i] > 0U) nonzero++;

        uint32_t pct = total ? (uint32_t)lroundf((float)snap->level_count[i] * 100.0f / (float)total) : 0U;
        lv_label_set_text_fmt(s_ratio_pct_labels[i], "%" PRIu32 "%%", pct);
    }

    if (total == 0U || nonzero == 0U) {
        for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
            lv_arc_set_angles(s_ratio_arcs[i], 0, 0);
        }
        return;
    }

    uint32_t available = 358U - ((nonzero > 1U) ? 2U * (nonzero - 1U) : 0U);
    uint32_t used_angle = 0U;
    uint32_t remain_segments = nonzero;
    int32_t start = 0;

    for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
        uint32_t count = snap->level_count[i];

        if (count == 0U) {
            lv_arc_set_angles(s_ratio_arcs[i], 0, 0);
            continue;
        }

        uint32_t span;
        if (remain_segments == 1U) {
            span = available - used_angle;
        } else {
            span = (uint32_t)(((uint64_t)count * (uint64_t)available) / (uint64_t)total);
            if (span < 2U) span = 2U;
            if (used_angle + span > available) span = available - used_angle;
        }

        lv_arc_set_angles(s_ratio_arcs[i], start, start + (int32_t)span);
        start += (int32_t)span + 2;
        used_angle += span;
        remain_segments--;
    }
}

static void refresh_distribution_panel(const stats_snapshot_t *snap)
{
    uint32_t total = snap->valid_count;
    const int32_t bar_w = 120;

    for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
        uint32_t count = snap->level_count[i];
        int32_t fill_w = total ? (int32_t)(((uint64_t)count * (uint64_t)bar_w) / (uint64_t)total) : 0;
        if (count > 0U && fill_w < 4) fill_w = 4;

        lv_obj_set_width(s_dist_fill[i], fill_w);
        lv_label_set_text_fmt(s_dist_count_labels[i], "%" PRIu32, count);
    }

    lv_label_set_text_fmt(s_dist_total_label, "总计：%" PRIu32, total);
}

static void format_feature(char *buf, size_t len, uint32_t idx, float value)
{
    switch (idx) {
    case 0: format_float(buf, len, value, 0, " bpm"); break;
    case 1: format_float(buf, len, value, 1, " ms"); break;
    case 2: format_float(buf, len, value, 1, " ms"); break;
    case 3: format_float(buf, len, value, 1, " ms2"); break;
    case 4: format_float(buf, len, value, 1, " ms2"); break;
    case 5: format_float(buf, len, value, 2, ""); break;
    case 6: format_float(buf, len, value, 2, ""); break;
    case 7: format_float(buf, len, value, 2, ""); break;
    case 8: format_float(buf, len, value, 1, " deg"); break;
    default: snprintf(buf, len, "--"); break;
    }
}

static int8_t calc_feature_trend(uint32_t idx, float first, float last)
{
    float base = fabsf(first);
    float tol = base * 0.05f;

    if (idx == 0U) tol = 2.0f;
    if (idx == 5U || idx == 6U || idx == 7U) tol = 0.03f;
    if (tol < 0.5f && idx != 5U && idx != 6U && idx != 7U) tol = 0.5f;

    float diff = last - first;
    if (fabsf(diff) <= tol) return 0;
    return diff > 0.0f ? 1 : -1;
}

static const char *trend_text(int8_t trend)
{
    if (trend > 0) return LV_SYMBOL_UP " 上升";
    if (trend < 0) return LV_SYMBOL_DOWN " 下降";
    return LV_SYMBOL_RIGHT " 平稳";
}

static lv_color_t trend_color(int8_t trend)
{
    if (trend > 0) return c(0x93ff4f);
    if (trend < 0) return c(0xff5a5a);
    return c(0x2ceaff);
}

static void refresh_feature_table(const stats_snapshot_t *snap)
{
    static const char *const names[APP_STATS_FEATURE_COUNT] = {
        "Ave_PR", "RMSSD", "SD2", "HF", "LF", "LF/HF", "WE", "DFA", "VAI",
    };

    char tmp[32];

    for (uint32_t i = 0; i < APP_STATS_FEATURE_COUNT; i++) {
        uint32_t row = i;
        const feature_acc_t *acc = &snap->feature[i];

        lv_table_set_cell_value(s_feature_table, row, 0, names[i]);

        if (acc->count == 0U) {
            lv_table_set_cell_value(s_feature_table, row, 1, "--");
            lv_table_set_cell_value(s_feature_table, row, 2, "--");
            lv_table_set_cell_value(s_feature_table, row, 3, "--");
            lv_table_set_cell_value(s_feature_table, row, 4, "--");
            s_feature_trend[i] = 0;
            continue;
        }

        format_feature(tmp, sizeof(tmp), i, acc->sum / (float)acc->count);
        lv_table_set_cell_value(s_feature_table, row, 1, tmp);

        format_feature(tmp, sizeof(tmp), i, acc->max);
        lv_table_set_cell_value(s_feature_table, row, 2, tmp);

        format_feature(tmp, sizeof(tmp), i, acc->min);
        lv_table_set_cell_value(s_feature_table, row, 3, tmp);

        s_feature_trend[i] = acc->count >= 2U ? calc_feature_trend(i, acc->first, acc->last) : 0;
        lv_table_set_cell_value(s_feature_table, row, 4, trend_text(s_feature_trend[i]));

        for (uint32_t col = 0; col < 5U; col++) {
            lv_table_set_cell_ctrl(s_feature_table, row, col, LV_TABLE_CELL_CTRL_TEXT_CROP);
        }
    }
}

static void feature_table_draw_event_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    bool is_header = (target == s_feature_table_header);

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
        } else if (col == 4U && row < APP_STATS_FEATURE_COUNT) {
            label->color = trend_color(s_feature_trend[row]);
        }
    }

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(draw_task);
    if (!fill) return;

    if (is_header) {
        fill->color = c(0x0b3654);
        fill->opa = LV_OPA_COVER;
    } else if ((row % 2U) == 0U) {
        fill->color = c(0x061f31);
        fill->opa = LV_OPA_70;
    } else {
        fill->color = c(0x08283d);
        fill->opa = LV_OPA_70;
    }
}

/*不显示界面时只存数据，切换显示界面时刷新界面数据*/
static void refresh_stats(void);

static bool stats_page_visible(void)
{
    return s_page != NULL && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

static void stats_request_refresh(void)
{
    s_stats_dirty = true;
}

void app_ui_statics_on_show(void)
{
    if (!s_page) {
        return;
    }

    if (s_stats_dirty) {
        s_stats_dirty = false;
        refresh_stats();
    }
}

static void refresh_stats(void)
{
    if (!s_page) return;

    stats_snapshot_t snap;
    uint32_t trend_idx[APP_STATS_TREND_POINTS];
    uint32_t trend_count = 0;

    build_snapshot(&snap, trend_idx, &trend_count);

    refresh_metric_panel(&snap);
    refresh_trend_charts(trend_idx, trend_count);
    refresh_ratio_panel(&snap);
    refresh_distribution_panel(&snap);
    refresh_feature_table(&snap);
}

static void range_event_cb(lv_event_t *e)
{
    s_range = (stats_range_t)(uintptr_t)lv_event_get_user_data(e);
    update_range_buttons();
    refresh_stats();
}

static lv_obj_t *make_dot(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    return dot;
}

static lv_obj_t *make_arc(lv_obj_t *parent, int32_t x, int32_t y, int32_t size,
                          int32_t width, lv_color_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 0);

    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

static void build_range_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 8, 256, 484);
    make_label(card, "统计范围", 14, 8);

    s_range_btns[0] = make_button(card, "完整采集", 3, 48, 72, 34, &s_style_btn_active);
    s_range_btns[1] = make_button(card, "最近5分钟", 79, 48, 76, 34, &s_style_btn);
    s_range_btns[2] = make_button(card, "最近30分钟", 159, 48, 86, 34, &s_style_btn);

    for (uint32_t i = 0; i < STATS_RANGE_COUNT; i++) {
        lv_obj_add_event_cb(s_range_btns[i], range_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    metric_row(card, "平均焦虑指数:", 110, &s_avg_score_label);
    metric_row(card, "最高焦虑指数:", 150, &s_max_score_label);
    metric_row(card, "最低焦虑指数:", 190, &s_min_score_label);
    metric_row(card, "平均PR:", 230, &s_avg_pr_label);
    metric_row(card, "平均PPI:", 270, &s_avg_ppi_label);
    metric_row(card, "有效窗口数:", 300, &s_valid_count_label);
    metric_row(card, "异常窗口数:", 340, &s_abnormal_count_label);
    metric_row(card, "总监测时长:", 380, &s_duration_label);

    lv_obj_add_style(s_avg_score_label, &s_style_cyan, 0);
    lv_obj_add_style(s_max_score_label, &s_style_bad, 0);
    lv_obj_add_style(s_min_score_label, &s_style_good, 0);
    lv_obj_add_style(s_avg_pr_label, &s_style_cyan, 0);
    lv_obj_add_style(s_avg_ppi_label, &s_style_cyan, 0);
    lv_obj_add_style(s_valid_count_label, &s_style_cyan, 0);
    lv_obj_add_style(s_abnormal_count_label, &s_style_bad, 0);
    lv_obj_add_style(s_duration_label, &s_style_cyan, 0);
}

static void build_ratio_panel(lv_obj_t *parent)
{
    static const char *const names[APP_STATS_LEVEL_COUNT] = {
        "低焦虑", "中等焦虑", "高焦虑",
    };

    lv_obj_t *card = make_card(parent, 760, 8, 256, 178);
    make_label(card, "焦虑等级占比", 5, 8);

    s_ratio_base_arc = make_arc(card, 3, 40, 112, 20, c(0x15344b));
    lv_arc_set_angles(s_ratio_base_arc, 0, 358);

    for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
        s_ratio_arcs[i] = make_arc(card, 8, 40, 112, 20, level_color((stats_level_t)i));
    }

    for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
        int32_t y = 54 + (int32_t)i * 38;
        make_dot(card, 124, y + 3, level_color((stats_level_t)i));
        make_label(card, names[i], 138, y - 3);

        s_ratio_pct_labels[i] = make_label(card, "0%", 198, y - 3);
        lv_obj_set_width(s_ratio_pct_labels[i], 50);
        lv_label_set_long_mode(s_ratio_pct_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_ratio_pct_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build_feature_table_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 272, 194, 480, 298);
    make_label(card, "特征参数统计", 10, 8);

    s_feature_table_header = lv_table_create(card);
    lv_obj_set_pos(s_feature_table_header, 6, 34);
    lv_obj_set_size(s_feature_table_header, 464, 30);

    s_feature_table = lv_table_create(card);
    lv_obj_set_pos(s_feature_table, 6, 64);
    lv_obj_set_size(s_feature_table, 464, 224);

    lv_obj_t *tables[2] = {s_feature_table_header, s_feature_table};
    for (uint32_t t = 0; t < 2U; t++) {
        lv_table_set_column_count(tables[t], 5);
        lv_table_set_column_width(tables[t], 0, 82);
        lv_table_set_column_width(tables[t], 1, 100);
        lv_table_set_column_width(tables[t], 2, 100);
        lv_table_set_column_width(tables[t], 3, 100);
        lv_table_set_column_width(tables[t], 4, 78);

        lv_obj_set_style_text_font(tables[t], ui_font(), LV_PART_ITEMS);
        lv_obj_set_style_text_align(tables[t], LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
        lv_obj_set_style_border_width(tables[t], 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(tables[t], c(0x1d4258), LV_PART_ITEMS);
        lv_obj_set_style_pad_top(tables[t], 2, LV_PART_ITEMS);
        lv_obj_set_style_pad_bottom(tables[t], 2, LV_PART_ITEMS);
        lv_obj_set_style_pad_left(tables[t], 1, LV_PART_ITEMS);
        lv_obj_set_style_pad_right(tables[t], 1, LV_PART_ITEMS);

        lv_obj_add_event_cb(tables[t], feature_table_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
        lv_obj_add_flag(tables[t], LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }

    lv_table_set_row_count(s_feature_table_header, 1);
    lv_table_set_cell_value(s_feature_table_header, 0, 0, "特征");
    lv_table_set_cell_value(s_feature_table_header, 0, 1, "均值");
    lv_table_set_cell_value(s_feature_table_header, 0, 2, "最大值");
    lv_table_set_cell_value(s_feature_table_header, 0, 3, "最小值");
    lv_table_set_cell_value(s_feature_table_header, 0, 4, "趋势");
    lv_obj_clear_flag(s_feature_table_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_feature_table_header, LV_SCROLLBAR_MODE_OFF);

    lv_table_set_row_count(s_feature_table, APP_STATS_FEATURE_COUNT);
    lv_obj_set_scroll_dir(s_feature_table, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_feature_table, LV_SCROLLBAR_MODE_AUTO);
}

static void build_distribution_panel(lv_obj_t *parent)
{
    static const char *const names[APP_STATS_LEVEL_COUNT] = {
        "低焦虑", "中等焦虑", "高焦虑",
    };

    lv_obj_t *card = make_card(parent, 760, 194, 256, 298);
    make_label(card, "焦虑等级分布", 10, 8);

    for (uint32_t i = 0; i < APP_STATS_LEVEL_COUNT; i++) {
        int32_t y = 78 + (int32_t)i * 56;

        make_label(card, names[i], 10, y - 4);

        lv_obj_t *bar_bg = lv_obj_create(card);
        lv_obj_remove_style_all(bar_bg);
        lv_obj_set_pos(bar_bg, 78, y);
        lv_obj_set_size(bar_bg, 120, 14);
        lv_obj_set_style_radius(bar_bg, 4, 0);
        lv_obj_set_style_bg_color(bar_bg, c(0x092941), 0);
        lv_obj_set_style_bg_opa(bar_bg, LV_OPA_80, 0);
        lv_obj_set_style_border_width(bar_bg, 1, 0);
        lv_obj_set_style_border_color(bar_bg, c(0x17425b), 0);
        lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);

        s_dist_fill[i] = lv_obj_create(bar_bg);
        lv_obj_remove_style_all(s_dist_fill[i]);
        lv_obj_set_pos(s_dist_fill[i], 0, 0);
        lv_obj_set_size(s_dist_fill[i], 0, 14);
        lv_obj_set_style_radius(s_dist_fill[i], 4, 0);
        lv_obj_set_style_bg_color(s_dist_fill[i], level_color((stats_level_t)i), 0);
        lv_obj_set_style_bg_opa(s_dist_fill[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dist_fill[i], LV_OBJ_FLAG_SCROLLABLE);

        s_dist_count_labels[i] = make_label(card, "0", 204, y - 4);
        lv_obj_set_width(s_dist_count_labels[i], 32);
        lv_obj_set_style_text_align(s_dist_count_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *unit = make_label(card, "单位：窗口数", 19, 254);
    lv_obj_add_style(unit, &s_style_muted, 0);
    lv_obj_set_style_transform_scale(unit, 300, 0);

    s_dist_total_label = make_label(card, "总计:0", 134, 254);
    lv_obj_add_style(s_dist_total_label, &s_style_cyan, 0);
    lv_obj_set_style_transform_scale(s_dist_total_label, 300, 0);
}

static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!stats_page_visible()) {
        return;
    }

    if (s_stats_dirty) {
        s_stats_dirty = false;
        refresh_stats();
    }
}

lv_obj_t *app_ui_statics_create(lv_obj_t *root)
{
    init_styles();

    if (s_session_start_ts == 0) {
        s_session_start_ts = time(NULL);
    }

    s_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_page);
    lv_obj_add_style(s_page, &s_style_page, 0);
    lv_obj_set_pos(s_page, 0, APP_UI_HEADER_H);
    lv_obj_set_size(s_page, APP_LCD_H_RES, APP_UI_PAGE_H);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    build_range_panel(s_page);

    build_chart_panel(s_page, &s_anxiety_chart, s_anxiety_xlabels,
                      "焦虑指数统计趋势", "焦虑指数",
                      272, 8, 480, 178, 0, 100, 0x2ceaff);

    build_ratio_panel(s_page);
    build_feature_table_panel(s_page);
    build_distribution_panel(s_page);
/*
    build_chart_panel(s_page, &s_pr_chart, s_pr_xlabels,
                      "平均PR统计趋势", "平均PR(bpm)",
                      8, 390, 744, 102, 40, 140, 0xa8ff2f);
*/
    update_range_buttons();
    refresh_stats();

    lv_timer_create(stats_timer_cb, 1000, NULL);
    return s_page;
}

void app_ui_statics_add_ppi(uint32_t win_id, const PPI_RESULT_T *result)
{
    if (!result) return;

    stats_record_t *rec = upsert_record(win_id);
    if (!rec) return;

    rec->ppi = *result;
    rec->has_ppi = true;

    stats_request_refresh();
}

void app_ui_statics_add_anxiety(const ANXIETY_MSG_T *msg)
{
    if (!msg) return;

    stats_record_t *rec = upsert_record(msg->WIN_ID);
    if (!rec) return;

    rec->anxiety = *msg;
    rec->has_anxiety = true;

    stats_request_refresh();
}

void app_ui_statics_reset(void)
{
    s_record_count = 0;
    s_range = STATS_RANGE_SESSION;
    s_session_start_ts = time(NULL);

    s_stats_dirty = true;

    if (stats_page_visible()) {
        update_range_buttons();
        s_stats_dirty = false;
        refresh_stats();
    }
}