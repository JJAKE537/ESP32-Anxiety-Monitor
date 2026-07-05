#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "app_message.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 采样控制回调函数类型：LVGL 的 Start/Stop 按钮通过它调用 FreeRTOS 采样控制逻辑。 */
typedef esp_err_t (*app_lvgl_sampling_cb_t)(void *ctx);

/* LVGL 对外回调集合：由 app_FreeRtos.c 传入 start/stop 控制函数。 */
typedef struct {
    app_lvgl_sampling_cb_t start;
    app_lvgl_sampling_cb_t stop;
    void *ctx;
} app_lvgl_callbacks_t;

/* 页面枚举 */
typedef enum {
    APP_LVGL_PAGE_MONITOR = 0,
    APP_LVGL_PAGE_HISTORY,
    APP_LVGL_PAGE_STATS,
    APP_LVGL_PAGE_SETTINGS,
    APP_LVGL_PAGE_ABOUT,
    APP_LVGL_PAGE_COUNT
} app_lvgl_page_t;

/* 启动 LCD、LVGL、触摸输入和实时监测界面。 */
esp_err_t app_lvgl_start(const app_lvgl_callbacks_t *callbacks);

/* 同步采样运行状态到 UI，按键任务和屏幕按钮都应调用。 */
void app_lvgl_set_sampling_running(bool running);

/* 清空实时监测界面的曲线、数值和日志，通常在开始采样时调用。 */
void app_lvgl_reset_monitoring(void);

/* 从 PPI 处理任务投递一帧 PPI 结果副本给 UI，不消费原有队列。 */
void app_lvgl_post_ppi_result(uint32_t win_id, const PPI_RESULT_T *result);

/* 从焦虑指数任务投递一帧焦虑指数结果副本给 UI。 */
void app_lvgl_post_anxiety_result(const ANXIETY_MSG_T *msg);

void app_lvgl_post_ai_analysis(uint32_t win_id, const char *text);

/* 切换底部 5 个模块页面。 */
void app_lvgl_show_page(app_lvgl_page_t page);

/* 触摸注册钩子：默认可为空实现，也可由 app_touch_gt911.c 覆盖实现。 */
esp_err_t app_lvgl_register_touch(lv_display_t *display);

/*系统设置界面函数*/
void app_lvgl_log_set_enabled(bool enabled);
bool app_lvgl_log_is_enabled(void);
void app_lvgl_log_set_keep_limit(uint32_t keep_limit);
uint32_t app_lvgl_log_get_keep_limit(void);
uint32_t app_lvgl_log_get_used_count(void);
void app_lvgl_log_clear(void);
esp_err_t app_lvgl_log_export_uart(void);

typedef struct {
    float low_max;
    float mid_max;
} app_lvgl_anxiety_thresholds_t;

/*主题模式*/
typedef enum {
    APP_LVGL_THEME_DARK = 0,
    APP_LVGL_THEME_LIGHT,
    APP_LVGL_THEME_EYE_CARE,
    APP_LVGL_THEME_HIGH_CONTRAST,
} app_lvgl_theme_mode_t;
/*主题颜色配置*/
typedef struct {
    uint32_t bg;                                        //主背景色
    uint32_t bg_grad;                                   //背景渐变色
    uint32_t card;                                      //卡片背景色
    uint32_t card_border;                               //卡片边框颜色
    uint32_t nav;
    uint32_t nav_border;
    uint32_t nav_active;
    uint32_t nav_active_border;
    uint32_t text;                                      //主要文字颜色
    uint32_t muted;                                     //弱化文字颜色
    uint32_t accent;                                    //强调色                  用于主题主色、默认主题初始化、部分高亮信息
    uint32_t cyan;                                      //青色强调色               用于关键数值，比如焦虑指数、特征值等
    uint32_t good;                                      //良好/正常状态颜色
    uint32_t danger;                                    //危险/停止/高风险颜色。   用于 Stop 按钮、高焦虑等级、错误或危险状态。
    uint32_t warning;
    lv_opa_t card_opa;                                  //卡片背景透明度。         控制卡片背景是不透明还是半透明。
    lv_opa_t nav_opa;                                   //导航按钮背景透明度。      控制底部导航按钮普通状态的透明度。
} app_lvgl_theme_palette_t;

typedef struct {
    uint8_t brightness_pct;
    uint16_t sleep_sec;
    uint8_t theme_mode;
} app_lvgl_display_settings_t;

void app_lvgl_get_anxiety_thresholds(app_lvgl_anxiety_thresholds_t *out);
void app_lvgl_set_anxiety_thresholds(uint8_t low_max, uint8_t mid_max);
const char *app_lvgl_anxiety_level_text(float score);
lv_color_t app_lvgl_anxiety_level_color(float score);

void app_lvgl_display_get_settings(app_lvgl_display_settings_t *out);
void app_lvgl_display_set_brightness(uint8_t pct);
void app_lvgl_display_set_sleep_sec(uint16_t sec);
void app_lvgl_display_set_theme(uint8_t theme_mode);

/*获取当前选择的主题的颜色配置结构体内容给*out*/
void app_lvgl_theme_get_palette(app_lvgl_theme_palette_t *out);
void app_ui_config_apply_theme(void);

/*历史界面函数*/
lv_obj_t *app_ui_history_create(lv_obj_t *root);
void app_ui_history_add_ppi(uint32_t win_id, const PPI_RESULT_T *result);
void app_ui_history_add_anxiety(const ANXIETY_MSG_T *msg);
void app_ui_history_reset(void);
uint32_t app_ui_history_get_record_count(void);
void app_ui_history_apply_theme(void);
void app_ui_history_on_show(void);
/* 数据统计界面函数 */
lv_obj_t *app_ui_statics_create(lv_obj_t *root);
void app_ui_statics_add_ppi(uint32_t win_id, const PPI_RESULT_T *result);
void app_ui_statics_add_anxiety(const ANXIETY_MSG_T *msg);
void app_ui_statics_reset(void);
lv_obj_t *app_ui_config_create(lv_obj_t *root);
void app_ui_statics_apply_theme(void);
void app_ui_statics_on_show(void);
/*关于界面*/
lv_obj_t *app_ui_about_create(lv_obj_t *root);
void app_ui_about_apply_theme(void);
void app_ui_about_on_show(void);


#ifdef __cplusplus
}
#endif

/*==========================================================================================*/