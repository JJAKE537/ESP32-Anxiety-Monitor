#include "app_lvgl.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_flash_encrypt.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/ledc.h"

#include "src/draw/lv_draw.h"
#include "src/draw/lv_draw_line.h"
#include "src/draw/lv_draw_rect.h"
#include "src/draw/lv_draw_triangle.h"
/*
app_main()
  -> app_lvgl_start()
      -> create_display()           初始化 LCD + LVGL display + 刷屏缓冲
      -> app_lvgl_register_touch()  初始化 GT911 触摸输入
      -> init_styles()              初始化样式
      -> xTaskCreate(lvgl_task)     创建 LVGL 主循环任务
      -> build_ui()                 创建所有 UI 控件和页面

*/
static const char *TAG = "APP_LVGL";
/*LCD刷新时序配置--和屏幕规格匹配*/
#define APP_LCD_H_RES                  1024
#define APP_LCD_V_RES                  600
#define APP_MIPI_DSI_DPI_CLK_MHZ       48                                       //DPI像素时钟 48MHZ

#define APP_MIPI_DSI_LCD_HSYNC         10                                       //水平同步脉冲宽度
#define APP_MIPI_DSI_LCD_HBP           120                                      //水平后肩，HSYNC之后到有效显示区前的空白周期
#define APP_MIPI_DSI_LCD_HFP           120                                      //水平前肩，有效显示区结束到下一行同步前的空白周期
#define APP_MIPI_DSI_LCD_VSYNC         1                                        //垂直同步脉冲宽度
#define APP_MIPI_DSI_LCD_VBP           20                                       //垂直后肩
#define APP_MIPI_DSI_LCD_VFP           10                                       //垂直前肩

#define APP_MIPI_DSI_LANE_NUM          2                                        //MIPI-DSI使用2条数据Lane
#define APP_MIPI_DSI_LANE_BITRATE_MBPS 1000                                     //每条Lane的速率为1000 Mbps
/*MIPI PHY供电与屏幕控制引脚*/
#define APP_MIPI_DSI_PHY_PWR_LDO_CHAN       3                                   //给MIPI DSI PHY供电使用的LDO通道编号
#define APP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500                                //MIPI PHY供电电压为2500 mV
#define APP_LCD_BK_LIGHT_ON_LEVEL           1                                   //背光打开时GPIO输出高电平
#define APP_LCD_BK_LIGHT_OFF_LEVEL          (!APP_LCD_BK_LIGHT_ON_LEVEL)        //背光关闭电平，与开启电平相反
#define APP_PIN_NUM_BK_LIGHT                26                                  //LCD背光控制引脚为GPIO26
#define APP_PIN_NUM_LCD_RST                 27                                  //LCD复位引脚为GPIO27
/*DMA2D拷贝配置*/
#ifndef APP_LVGL_USE_DMA2D_COPY_FRAME
#define APP_LVGL_USE_DMA2D_COPY_FRAME 1                                         //使用DMA2D进行帧缓冲拷贝，提高图像刷新效率
#endif
/*GT911触摸屏I2C配置*/
#ifndef APP_LVGL_TOUCH_I2C_PORT
#define APP_LVGL_TOUCH_I2C_PORT             I2C_NUM_1
#endif
#ifndef APP_LVGL_TOUCH_I2C_SDA
#define APP_LVGL_TOUCH_I2C_SDA              GPIO_NUM_7
#endif
#ifndef APP_LVGL_TOUCH_I2C_SCL
#define APP_LVGL_TOUCH_I2C_SCL              GPIO_NUM_8
#endif
#ifndef APP_LVGL_TOUCH_INT
#define APP_LVGL_TOUCH_INT                  GPIO_NUM_NC
#endif
#ifndef APP_LVGL_TOUCH_RST
#define APP_LVGL_TOUCH_RST                  GPIO_NUM_NC
#endif
#ifndef APP_LVGL_TOUCH_I2C_FREQ_HZ
#define APP_LVGL_TOUCH_I2C_FREQ_HZ          400000                              //400KHZ
#endif
#ifndef APP_LVGL_TOUCH_SWAP_XY
#define APP_LVGL_TOUCH_SWAP_XY              0                                   //不交换X/Y坐标
#endif
#ifndef APP_LVGL_TOUCH_MIRROR_X
#define APP_LVGL_TOUCH_MIRROR_X             0                                   //X轴不镜像
#endif
#ifndef APP_LVGL_TOUCH_MIRROR_Y
#define APP_LVGL_TOUCH_MIRROR_Y             0                                   //Y轴不镜像
#endif
/*====================================================================LVGL初始配置============================================================================================================*/
/*LVGL绘图缓存和任务参数*/
#define APP_LVGL_DRAW_BUF_LINES        (APP_LCD_V_RES / 10)                     //LVGL绘图缓冲区高度为屏幕高度的1/10，即60行
#define APP_LVGL_TICK_PERIOD_MS        2                                        //LVGL系统tick周期为2 ms
#define APP_LVGL_TASK_STACK_SIZE       (8 * 1024)                               //LVGL任务栈大小8KB                          
#define APP_LVGL_TASK_PRIORITY         6                                        //LVGL任务优先级
#define APP_LVGL_TASK_MAX_DELAY_MS     500
#define APP_LVGL_TASK_MIN_DELAY_MS     (1000 / CONFIG_FREERTOS_HZ)
/*曲线、PPI和日志缓存参数*/
#define APP_TREND_POINTS               31                                       //焦虑指数/脉率趋势曲线保存31个点≈5分钟    30s---40s---50s---60s---70s......   
#define APP_PPI_POINTS                 120                                      //PPI曲线最多显示120个点
#define APP_PPI_TAIL_MS                10000U                                   
#define APP_LOG_HISTORY_LINES          200                                      //日志历史最多保存200行
#define APP_LOG_TEXT_LEN               80                                       //每行日志最大80个字符
#define APP_FEATURE_COUNT              9

static uint32_t s_log_keep_limit = APP_LOG_HISTORY_LINES;
static bool s_log_enabled = true;
/*地址/内存对齐宏*/
#define ALIGN_UP(num, align)           (((num) + ((align) - 1)) & ~((align) - 1))       //向上对齐  align是2的幂指数
#define ALIGN_DOWN(num, align)         ((num) & ~((align) - 1))                         //向下对齐
/*UI界面布局参数*/
#define APP_UI_HEADER_H 42                                                      //顶部标题栏高度为42像素                        顶部导航栏0-41
#define APP_UI_NAV_Y 548                                                        //底部导航栏起始Y坐标为548                      页面内容区42-541
#define APP_UI_NAV_H 42                                                         //底部导航栏高度为42像素                        底部导航区548-589
#define APP_UI_PAGE_Y APP_UI_HEADER_H                                           //页面内容从顶部标题栏下方开始，即Y=42           底部剩余   10
#define APP_UI_PAGE_H (APP_UI_NAV_Y - APP_UI_PAGE_Y - 6)                        //页面内容区高度，避免和底部导航栏重叠      500

/*LCD背光PWM配置参数---LEDC外设*/
#define APP_LCD_BK_LEDC_TIMER       LEDC_TIMER_0                                //LEDC 0号定时器
#define APP_LCD_BK_LEDC_MODE        LEDC_LOW_SPEED_MODE                         //低速LEDC模式
#define APP_LCD_BK_LEDC_CHANNEL     LEDC_CHANNEL_0                              //LEDC 0号通道输出PWM
#define APP_LCD_BK_LEDC_DUTY_RES    LEDC_TIMER_13_BIT                           //PWM占空比分辨率是13位
#define APP_LCD_BK_LEDC_FREQ_HZ     5000U                                       //PWM频率5KHZ
#define APP_LCD_BK_LEDC_DUTY_MAX    ((1U << 13) - 1U)                           //13位PWM的最大占空比8191  0-8191  配置 LCD 背光引脚 GPIO26 通过 LEDC 输出 5kHz PWM，从而实现亮度调节。

#define APP_LOG_VISIBLE_LINES          8                                        //日志区域只显示最近8行

#define APP_AI_TEXT_LEN 256U
/*===========================================================上电默认配置=====================================================================================================================*/
/*焦虑指数默认阈值设置*/
static app_lvgl_anxiety_thresholds_t s_anxiety_thresholds = {
    .low_max = 26.4,
    .mid_max = 36.5,
};

/*上电屏幕默认配置*/
static app_lvgl_display_settings_t s_display_settings = {
    .brightness_pct = 80,                                                       //80%背光亮度
    .sleep_sec = 0,                                                             //息屏时间
    .theme_mode = APP_LVGL_THEME_LIGHT,                                      //默认浅色屏幕
};

static bool s_backlight_pwm_ready;
static bool s_backlight_dimmed;
/*===========================================================LVGL界面管理配置==================================================================================================================*/
/*管理特征参数显示项      PRV特征参数显示项  名称-单位-显示数值标签控件-显示单位标签控件*/
typedef struct {
    const char *title;
    const char *unit;
    lv_obj_t *value;
    lv_obj_t *unit_label;
} feature_view_t;
/*管理曲线图模块         曲线图模块  图表外面的卡片容器-LVGL图表控件本体-图表中的数据曲线序列*/
typedef struct {
    lv_obj_t *card;
    lv_obj_t *chart;
    lv_chart_series_t *series;
} chart_view_t;
/*管理底部导航按钮和页面  底部导航按钮和页面的对应关系  底部导航栏对应的按钮控件-按钮控件对应的页面编号*/
typedef struct {
    lv_obj_t *btn;
    app_lvgl_page_t page;
} nav_view_t;
/*管理系统运行状态：PPI结果、焦虑指数结果---LVGL界面当前需要显示的核心运行数据。*/
typedef struct {
    bool running;                                                               //当前是否正在采样？
    bool reset_requested;                                                       //清空Ui？
    bool ppi_dirty;                                                             //PPI数据是否有更新
    bool anxiety_dirty;                                                         //焦虑指数数据是否有更新           dirty即数据已更新，需要刷新界面。
    TickType_t start_tick;                                                      //开始采集时的系统tick时间
    uint32_t elapsed_sec;                                                       //已运行时间，单位秒
    uint32_t win_id;
    PPI_RESULT_T ppi;                                                           //最新PPI结果
    ANXIETY_MSG_T anxiety;                                                      //最新焦虑指数结果
    /*AI分析任务参数*/
    bool ai_dirty;
    uint32_t ai_win_id;
    char ai_text[APP_AI_TEXT_LEN];
} app_data_t;
/*===========================================================LVGL界面信息配置==================================================================================================================*/
/*保存 LVGL 界面对象、样式、图表数据、日志缓存和运行状态*/
static _lock_t s_lvgl_lock;                                                     
static SemaphoreHandle_t s_data_mutex;
static app_data_t s_data;                                                       /*保存当前采集状态、PPI结果、焦虑指数结果等核心数据---UI核心数据缓存*/
static app_lvgl_callbacks_t s_callbacks;                                        /*保存Start/Stop回调函数，LVGL按钮通过它调用采样控制逻辑*/

static lv_display_t *s_display;                                                 //LVGL显示器对象指针
static lv_obj_t *s_root;                                                        //当前屏幕的根对象---LVGL的主画布
static lv_obj_t *s_pages[APP_LVGL_PAGE_COUNT];
static nav_view_t s_nav[APP_LVGL_PAGE_COUNT];

/*实时页曲线分帧刷新*/
static app_lvgl_page_t s_current_page = APP_LVGL_PAGE_MONITOR;

static bool s_anxiety_chart_dirty;
static bool s_pr_chart_dirty;
static bool s_ppi_chart_dirty;
static uint8_t s_chart_flush_step;                                              //分步刷新Anxiety_idx【0】、PR【1】、PPI【2】曲线
/*LVGL样式对象*/
static lv_style_t s_style_bg;                                                   //背景样式---用于根屏幕和页面对象
static lv_style_t s_style_card;                                                 //卡片容器样式
static lv_style_t s_style_button;
static lv_style_t s_style_button_stop;

static lv_style_t s_style_nav;                                                  //未选中的导航按钮样式
static lv_style_t s_style_nav_active;                                           //航按钮的选中状态样式
static lv_style_t s_style_title_box;                                            //顶部标题栏外框样式
static lv_style_t s_style_title_text;                                           
static lv_obj_t *s_title_box;                                                   //顶部标题栏容器对象
static lv_obj_t *s_title_label;                                                 

static lv_style_t s_style_muted;                                                //弱化文字样式---用于次要信息文字，例如坐标轴标签、日志、停止状态、单位文字等
static lv_style_t s_style_good;                                                 //正常/良好状态文字样式
static lv_style_t s_style_cyan;                                                 //数值文字样式
/*界面控件*/
static lv_obj_t *s_status_dot;                                                  //顶部状态圆点对象
static lv_obj_t *s_status_label;                                                //顶部状态文字对象
static lv_obj_t *s_time_label;                                                  //顶部时间显示对象

static lv_obj_t *s_run_state_label;                                             //采集状态卡片里的运行状态文字对象
static lv_obj_t *s_elapsed_label;                                               
static lv_obj_t *s_window_id_label;

static lv_obj_t *s_latest_anxiety_label;                                        //实时信息卡片里的最新焦虑指数对象
static lv_obj_t *s_latest_level_label;
static lv_obj_t *s_latest_pr_label;
static lv_obj_t *s_latest_ppi_label;
static lv_obj_t *s_ppi_count_label;

/*焦虑指数仪表盘*/
static lv_obj_t *s_score_gauge_box;
static lv_obj_t *s_score_base_arc;
static lv_obj_t *s_score_low_arc;
static lv_obj_t *s_score_mid_arc;
static lv_obj_t *s_score_high_arc;
static lv_obj_t *s_score_arc;
static lv_obj_t *s_score_pointer;
static lv_obj_t *s_score_ticks[11];
static lv_point_precise_t s_score_pointer_points[2];
static lv_point_precise_t s_score_tick_points[11][2];

static lv_obj_t *s_score_label;
static lv_obj_t *s_score_level_box;
static lv_obj_t *s_score_level_label;
static lv_obj_t *s_score_title_label;
static lv_obj_t *s_score_pointer_dot;
static lv_obj_t *s_score_low_mark_label;
static lv_obj_t *s_score_mid_mark_label;



static feature_view_t s_features[APP_FEATURE_COUNT];
static chart_view_t s_anxiety_chart;
static chart_view_t s_pr_chart;
static chart_view_t s_ppi_chart;

static int32_t s_anxiety_values[APP_TREND_POINTS];                              //曲线Y轴数组
static int32_t s_pr_values[APP_TREND_POINTS];
static int32_t s_ppi_values[APP_PPI_POINTS];

static lv_obj_t *s_log_area;                                                    //系统日志区域的外层滚动容器
static lv_obj_t *s_log_text_label;                                              //真正显示日志文字的 label 控件

static lv_obj_t *s_ai_card;
static lv_obj_t *s_ai_title_label;
static lv_obj_t *s_ai_text_label;

/*WIFI图标*/
static lv_obj_t *s_wifi_label;  
static lv_obj_t *s_wifi_bars[4];

static char s_log_history[APP_LOG_HISTORY_LINES][APP_LOG_TEXT_LEN];
static char s_log_text[APP_LOG_HISTORY_LINES * APP_LOG_TEXT_LEN];

static uint32_t s_log_write;
static uint32_t s_log_used;
static uint32_t s_log_count;

static lv_font_t *s_ai_font;

extern const uint8_t ai_font_start[]
    asm("_binary_ai_chinese_ttf_start");

extern const uint8_t ai_font_end[]
    asm("_binary_ai_chinese_ttf_end");
static void init_ai_font(void)
{
    size_t size = (size_t)(ai_font_end - ai_font_start);

    s_ai_font = lv_tiny_ttf_create_data(
        ai_font_start,
        size,
        16
    );

    if (s_ai_font == NULL) {
        ESP_LOGE(TAG, "create AI Chinese font failed");
    }
}
/*===========================================================LVGL界面实现配置==================================================================================================================*/
/*字体文件添加修改*/
LV_FONT_DECLARE(app_ui_font_16);

static const lv_font_t *ui_font(void)
{
    return &app_ui_font_16;
}

/* 把一个 16 进制 RGB 颜色值转换成 LVGL 使用的 lv_color_t 颜色结构体。 */
static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static const app_lvgl_theme_palette_t s_theme_palettes[] = {
    [APP_LVGL_THEME_DARK] = {//深色主题
        .bg = 0x061625, .bg_grad = 0x08263a,
        .card = 0x082035, .card_border = 0x1aa2ff,
        .nav = 0x08243a, .nav_border = 0x24506c,
        .nav_active = 0x0d4ea0, .nav_active_border = 0x39c6ff,
        .text = 0xf4f8ff, .muted = 0xc9d7ea,
        .accent = 0x2ceaff, .cyan = 0x2ceaff,
        .good = 0x9cff3a, .danger = 0xff3434, .warning = 0xffd23a,
        .card_opa = LV_OPA_80, .nav_opa = LV_OPA_70,
    },
    [APP_LVGL_THEME_LIGHT] = {//浅色主题
        .bg = 0xf3f8ff, .bg_grad = 0xe7f1ff,
        .card = 0xffffff, .card_border = 0x6eb7e8,
        .nav = 0xeaf4ff, .nav_border = 0x8bbfe8,
        .nav_active = 0xd5eaff, .nav_active_border = 0x1677ff,
        .text = 0x17324d, .muted = 0x557086,
        .accent = 0x1677ff, .cyan = 0x008eb8,
        .good = 0x2f8f35, .danger = 0xd93535, .warning = 0xb67800,
        .card_opa = LV_OPA_COVER, .nav_opa = LV_OPA_COVER,
    },
    [APP_LVGL_THEME_EYE_CARE] = {//护眼主题
        .bg = 0x142923, .bg_grad = 0x203a32,
        .card = 0x1c332d, .card_border = 0x6aa08d,
        .nav = 0x1b332d, .nav_border = 0x6b9a8b,
        .nav_active = 0x25493e, .nav_active_border = 0x89d8bb,
        .text = 0xe6f1e8, .muted = 0xb3c6ba,
        .accent = 0x70d6b2, .cyan = 0x83d8c4,
        .good = 0xb8df77, .danger = 0xff6b6b, .warning = 0xe8c46a,
        .card_opa = LV_OPA_80, .nav_opa = LV_OPA_70,
    },
    [APP_LVGL_THEME_HIGH_CONTRAST] = {//高对比主题
        .bg = 0x000000, .bg_grad = 0x111111,
        .card = 0x050505, .card_border = 0xffff00,
        .nav = 0x000000, .nav_border = 0x00e5ff,
        .nav_active = 0x1a1a00, .nav_active_border = 0xffff00,
        .text = 0xffffff, .muted = 0xe6e6e6,
        .accent = 0x00ffff, .cyan = 0x00ffff,
        .good = 0x00ff00, .danger = 0xff3030, .warning = 0xffff00,
        .card_opa = LV_OPA_COVER, .nav_opa = LV_OPA_COVER,
    },
};
/*返回值：主题颜色配置结构体指针---s_display_settings配置背光强度、息屏时间、主题*/
static const app_lvgl_theme_palette_t *current_theme_palette(void)
{
    uint8_t mode = s_display_settings.theme_mode;
    if (mode >= (uint8_t)(sizeof(s_theme_palettes) / sizeof(s_theme_palettes[0]))) {
        mode = APP_LVGL_THEME_DARK;
    }
    return &s_theme_palettes[mode];
}
/*获取当前选择的主题的颜色配置结构体内容给*out*/
void app_lvgl_theme_get_palette(app_lvgl_theme_palette_t *out)
{
    if (out) {
        *out = *current_theme_palette();
    }
}
/*把当前主题颜色应用到 LVGL 的各个样式对象上。更新样式
当前主题颜色表
      ↓
apply_main_theme_styles()
      ↓
写入各个 lv_style_t 样式对象
      ↓
已经绑定这些样式的控件外观发生变化
*/
static void apply_main_theme_styles(void)
{
    const app_lvgl_theme_palette_t *p = current_theme_palette();
    /*设置主背景样式：*/
    lv_style_set_bg_color(&s_style_bg, c(p->bg));
    lv_style_set_bg_grad_color(&s_style_bg, c(p->bg_grad));
    lv_style_set_text_color(&s_style_bg, c(p->text));
    /*设置卡片样式：*/
    lv_style_set_bg_color(&s_style_card, c(p->card));
    lv_style_set_bg_opa(&s_style_card, p->card_opa);
    lv_style_set_border_color(&s_style_card, c(p->card_border));
    lv_style_set_text_color(&s_style_card, c(p->text));
    /*底部界面按钮背景样式设置---四种主题下，底部按钮都有明显背景色和边框，高对比模式下选中按钮会变成黄底黑字。*/
    uint32_t nav_bg = p->nav;
    uint32_t nav_grad = p->nav;
    uint32_t nav_border = p->nav_border;
    uint32_t nav_text = p->text;
    uint32_t nav_active_bg = p->nav_active;
    uint32_t nav_active_grad = p->accent;
    uint32_t nav_active_border = p->nav_active_border;
    uint32_t nav_active_text = p->text;

    uint32_t title_bg = p->nav_active;
    uint32_t title_border = p->nav_active_border;
    uint32_t title_text = p->text;
    /*不同主题下，专门调整底部导航按钮和顶部标题栏的颜色*/
    switch (s_display_settings.theme_mode) {
    case APP_LVGL_THEME_LIGHT:          //浅色
    nav_bg = 0xeaf2fb;
    nav_grad = 0xd8e8f7;
    nav_border = 0x6aa3cf;
    nav_text = 0x1f4057;

    nav_active_bg = 0x2563eb;
    nav_active_grad = 0x1d4ed8;
    nav_active_border = 0x1e40af;
    nav_active_text = 0xffffff;

    title_bg = 0xdbeafe;
    title_border = 0x60a5fa;
    title_text = 0x123452;
        break;

    case APP_LVGL_THEME_EYE_CARE:       //护眼
        nav_bg = 0x24473d;
        nav_grad = 0x1b352e;
        nav_border = 0x93d8bd;
        nav_text = 0xf1fff7;

        nav_active_bg = 0x3d7d68;
        nav_active_grad = 0x70d6b2;
        nav_active_border = 0xb8f5dc;
        nav_active_text = 0xffffff;

        title_bg = 0x2f6655;
        title_border = 0x9be7c7;
        title_text = 0xffffff;
    break;

    case APP_LVGL_THEME_HIGH_CONTRAST:  //高对比
        nav_bg = 0x202020;
        nav_grad = 0x000000;
        nav_border = 0x00ffff;
        nav_text = 0xffffff;

        nav_active_bg = 0xffff00;
        nav_active_grad = 0xffd000;
        nav_active_border = 0xffffff;
        nav_active_text = 0x000000;

        title_bg = 0xffff00;
        title_border = 0xffffff;
        title_text = 0x000000;
    break;

    case APP_LVGL_THEME_DARK:       //深色
    default:
    nav_bg = 0x0b2a3d;
    nav_grad = 0x103a52;
    nav_border = 0x2b6d8c;
    nav_text = 0xd8ecf7;

    nav_active_bg = 0x1e6aa7;
    nav_active_grad = 0x28a7d9;
    nav_active_border = 0x75d8ff;
    nav_active_text = 0xffffff;

    title_bg = 0x123c5a;
    title_border = 0x3aaeea;
    title_text = 0xf3fbff;
    break;
    }
    /*应用普通导航按钮样式：影响未选中的底部导航按钮*/
    lv_style_set_bg_color(&s_style_nav, c(nav_bg));
    lv_style_set_bg_grad_color(&s_style_nav, c(nav_grad));
    lv_style_set_bg_grad_dir(&s_style_nav, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_style_nav, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_nav, 2);
    lv_style_set_border_color(&s_style_nav, c(nav_border));
    lv_style_set_text_color(&s_style_nav, c(nav_text));
    /*应用选中导航按钮样式：影响当前页面对应的底部导航按钮*/
    lv_style_set_bg_color(&s_style_nav_active, c(nav_active_bg));
    lv_style_set_bg_grad_color(&s_style_nav_active, c(nav_active_grad));
    lv_style_set_bg_grad_dir(&s_style_nav_active, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_style_nav_active, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_nav_active, 2);
    lv_style_set_border_color(&s_style_nav_active, c(nav_active_border));
    lv_style_set_text_color(&s_style_nav_active, c(nav_active_text));
    /*=============================================================*/
    /*应用顶部标题栏样式：影响顶部标题框和标题文字*/
    lv_style_set_bg_color(&s_style_title_box, c(title_bg));
    lv_style_set_border_color(&s_style_title_box, c(title_border));
    lv_style_set_text_color(&s_style_title_text, c(title_text));
    /*设置次要、正常/良好状态、关键数值强调文字颜色*/
    lv_style_set_text_color(&s_style_muted, c(p->muted));
    lv_style_set_text_color(&s_style_good, c(p->good));
    lv_style_set_text_color(&s_style_cyan, c(p->cyan));
    /*设置 Start 按钮样式：*/
    lv_style_set_bg_color(&s_style_button, c(p->good));
    lv_style_set_bg_grad_color(&s_style_button, c(p->good));
    lv_style_set_border_color(&s_style_button, c(p->good));
    lv_style_set_text_color(&s_style_button, c(p->text));
    /*设置 Stop 按钮样式：*/
    lv_style_set_bg_color(&s_style_button_stop, c(p->danger));
    lv_style_set_bg_grad_color(&s_style_button_stop, c(p->danger));
    lv_style_set_border_color(&s_style_button_stop, c(p->danger));
    lv_style_set_text_color(&s_style_button_stop, c(0xffffff));
}

/* 限制整数数值范围，避免图表或进度条越界。 */
static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* 根据 FreeRTOS tick 计算已经运行的秒数。 */
static uint32_t elapsed_sec_from_tick(TickType_t start_tick)
{
    if (start_tick == 0) {
        return 0;
    }
    return (uint32_t)((xTaskGetTickCount() - start_tick) / configTICK_RATE_HZ);
}

/* 把秒数格式化成 HH:MM:SS 字符串。 */
static void format_elapsed(char *buf, size_t len, uint32_t sec)
{
    uint32_t h = sec / 3600U;
    uint32_t m = (sec % 3600U) / 60U;
    uint32_t s = sec % 60U;
    snprintf(buf, len, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, h, m, s);
}
/*=============================float-LVGL显示辅助函数===============================================================*/
/*把一个 float 数值格式化成固定小数位字符串，并可在后面加单位后缀   --ms  --bpm   显示平均PPI、焦虑指数、特征参数*/
static void format_float_fixed(char *buf, size_t len, float value, uint8_t decimals, const char *suffix)
{
    if (!isfinite(value)) {
        snprintf(buf, len, "--%s", suffix ? suffix : "");
        return;
    }

    if (decimals > 4) {
        decimals = 4;
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
        /*%0*ld小数部分前面补0，宽度由decimals决定*/
        snprintf(buf, len, "%s%ld.%0*ld%s",sign, (long)whole, (int)decimals, (long)frac, suffix ? suffix : "");
    }
}
/*float 数据 -> 格式化字符串 -> 更新 LVGL label 显示
params:              label     要更新的 LVGL 文本控件          value     要显示的浮点数   
                    decimals  保留几位小数                    suffix    后缀/单位，比如 " ms"
*/
static void set_label_float_fixed(lv_obj_t *label, float value, uint8_t decimals, const char *suffix)
{
    char text[32];
    format_float_fixed(text, sizeof(text), value, decimals, suffix);
    lv_label_set_text(label, text);
}
/*=============================焦虑指数等级、颜色判定辅助函数=========================================================*/
/*基于焦虑指数获取焦虑等级判定字符串*/
const char *app_lvgl_anxiety_level_text(float score)
{
    if (score <= (float)s_anxiety_thresholds.low_max) return "低焦虑";
    if (score <= (float)s_anxiety_thresholds.mid_max) return "中等焦虑";
    return "高焦虑";
}
/*基于焦虑指数获取焦虑等级对应的颜色对象*/
lv_color_t app_lvgl_anxiety_level_color(float score)
{
    if (score <= (float)s_anxiety_thresholds.low_max) return c(0x2ceaff);
    if (score <= (float)s_anxiety_thresholds.mid_max) return c(0x9cff3a);
    return c(0xff3434);
}

static const char *anxiety_level_text(float score)
{
    return app_lvgl_anxiety_level_text(score);
}

static lv_color_t anxiety_level_color(float score)
{
    return app_lvgl_anxiety_level_color(score);
}

/*清空焦虑趋势图、PR趋势图、PPI图的数据---所有点设为“不显示”*/
static void init_series_none(void)
{
    for (uint32_t i = 0; i < APP_TREND_POINTS; i++) {
        s_anxiety_values[i] = LV_CHART_POINT_NONE;
        s_pr_values[i] = LV_CHART_POINT_NONE;
    }
    for (uint32_t i = 0; i < APP_PPI_POINTS; i++) {
        s_ppi_values[i] = LV_CHART_POINT_NONE;
    }
}
/* 数组整体左移一位，新数据放到最后一个位置 
params: buf 待操作的数组    count  数组元素数量      value  新追加的数据
*/
static void shift_append_i32(int32_t *buf, uint32_t count, int32_t value)
{
    if (count <= 1) {
        return;
    }
    memmove(&buf[0], &buf[1], sizeof(buf[0]) * (count - 1U));
    buf[count - 1U] = value;
}
/*==================================================曲线淡色填充=========================================================================*/
#if LV_DRAW_SW_COMPLEX
/*根据曲线点在图表里的 Y 坐标，计算曲线下方渐变填充的透明度
params:  y 当前点的y坐标        top 图表顶部的y坐标     full_h 图表总高度
*/
static lv_opa_t trend_area_opa_from_y(int32_t y, int32_t top, int32_t full_h)
{
    if (full_h <= 0) {
        return LV_OPA_TRANSP;//全透明
    }
    /*图表顶部    -> frac 接近 0                图表底部    -> frac 接近 255*/
    int32_t frac = ((y - top) * 255) / full_h;
    if (frac < 0) {
        frac = 0;
    }
    if (frac > 255) {
        frac = 255;
    }
    /*填充颜色深度      越靠上 -> opa 越大 -> 填充颜色更明显      越靠下 -> opa 越小 -> 填充颜色更淡*/
    //int32_t opa = (255 - frac) / 2;
    int32_t opa = (255 - frac);
    //if (opa > 78) {
    //    opa = 78;
    //}
    if(opa>255){
        opa = 255;
    }
    if (opa < 0) {
        opa = 0;
    }
    /*透明度*/
    return (lv_opa_t)opa;
}
/*在 LVGL 图表折线下面额外画一层淡色渐变填充区域---处理的是绘制事件*/
static void add_trend_faded_area(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_target_obj(e);               //取得触发事件的对象，也就是图表控件 chart
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);      //取得当前 LVGL 正在执行的绘制任务  画背景/网格线/折线/点
    lv_draw_dsc_base_t *base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
                                                                //取得绘制描述符基础信息
    if (base_dsc == NULL || base_dsc->part != LV_PART_ITEMS) {
        return;                                                 //LV_PART_ITEMS图表里的数据项，折线、点等
    }

    if (lv_draw_task_get_type(draw_task) != LV_DRAW_TASK_TYPE_LINE) {
        return;                                                 //只处理线条绘制任务
    }

    lv_draw_line_dsc_t *line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (line_dsc == NULL) {                                     //取得当前线段的绘制描述符
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);                          //取得chart在屏幕上的坐标范围

    int32_t full_h = lv_obj_get_height(chart);                  //取得chart的高度
    int32_t y_top = (int32_t)LV_MIN(line_dsc->p1.y, line_dsc->p2.y);    //当前线段更靠上的点
    int32_t y_bot = (int32_t)LV_MAX(line_dsc->p1.y, line_dsc->p2.y);    //当前线段更靠下的点

    lv_color_t area_color = line_dsc->color;                            //填充区域使用和折线一样的颜色

    lv_draw_triangle_dsc_t tri_dsc;                                     //初始化三角形绘制描述符
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.p[0].x = line_dsc->p1.x;                                    //三角形前两个点就是折线当前线段的两个端点
    tri_dsc.p[0].y = line_dsc->p1.y;
    tri_dsc.p[1].x = line_dsc->p2.x;                                    
    tri_dsc.p[1].y = line_dsc->p2.y;
    tri_dsc.p[2].x = line_dsc->p1.y < line_dsc->p2.y ? line_dsc->p1.x : line_dsc->p2.x;
    tri_dsc.p[2].y = y_bot;                                             //第三个点放在线段较高端点的正下方，Y 坐标等于较低端点的 Y
    tri_dsc.grad.dir = LV_GRAD_DIR_VER;
    tri_dsc.grad.stops[0].color = area_color;                           //三角形使用垂直渐变
    tri_dsc.grad.stops[0].opa = trend_area_opa_from_y(y_top, coords.y1, full_h);
    tri_dsc.grad.stops[0].frac = 0;
    tri_dsc.grad.stops[1].color = area_color;
    tri_dsc.grad.stops[1].opa = trend_area_opa_from_y(y_bot, coords.y1, full_h);
    tri_dsc.grad.stops[1].frac = 255;
    lv_draw_triangle(base_dsc->layer, &tri_dsc);                        //三角形画到当前图层上

    lv_draw_rect_dsc_t rect_dsc;                                        //绘制一个矩形---垂直渐变
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    rect_dsc.bg_grad.stops[0].color = area_color;
    rect_dsc.bg_grad.stops[0].opa = trend_area_opa_from_y(y_bot, coords.y1, full_h);
    rect_dsc.bg_grad.stops[0].frac = 0;
    rect_dsc.bg_grad.stops[1].color = area_color;
    rect_dsc.bg_grad.stops[1].opa = LV_OPA_TRANSP;
    rect_dsc.bg_grad.stops[1].frac = 255;

    lv_area_t rect_area;
    rect_area.x1 = (int32_t)line_dsc->p1.x;
    rect_area.x2 = (int32_t)line_dsc->p2.x - 1;
    rect_area.y1 = y_bot - 1;
    rect_area.y2 = coords.y2;
    lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
}

static void trend_faded_area_event_cb(lv_event_t *e)
{
    add_trend_faded_area(e);
}

static void enable_trend_faded_area(lv_obj_t *chart)
{
    lv_obj_add_event_cb(chart, trend_faded_area_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
}
#else
static void enable_trend_faded_area(lv_obj_t *chart)
{
    (void)chart;
}
#endif

/*==================================================曲线淡色填充=========================================================================*/

/* LVGL 刷屏回调，把绘图缓冲区提交给 MIPI-DPI 面板。 
params  disp:LVGL显示对象       area:本次需要刷新的屏幕区域     px_map:LVGL已经画好的像素区域  
*/
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);  //获取LCD面板句柄
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}                                                                   //LVGL画好的像素数据px_map发送到LCD

/* LVGL tick 定时器回调，为 LVGL 提供毫秒时间基准。 */
static void increase_lvgl_tick(void *arg)
{
    (void)arg;                          //每2ms被ESP定时器调用一次
    lv_tick_inc(APP_LVGL_TICK_PERIOD_MS);
}
/*==================================================LVGL主处理任务=========================================================================*/
/* LVGL 主处理任务，周期调用 lv_timer_handler 刷新界面和处理事件。 */
static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started");

    while (1) {
        _lock_acquire(&s_lvgl_lock);
        uint32_t wait_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);

        wait_ms = MAX(wait_ms, APP_LVGL_TASK_MIN_DELAY_MS);
        wait_ms = MIN(wait_ms, APP_LVGL_TASK_MAX_DELAY_MS);
        usleep(wait_ms * 1000U);        //10000-500000us  10-500ms之间  
    }
}
/*==================================================LVGL主处理任务=========================================================================*/
/* LCD 传输完成回调，通知 LVGL 当前刷屏缓冲区可复用---LCD面板驱动回调函数
params  panel:LCD面板句柄  edata:LCD事件数据    user_ctx:用户传入的数据，这里传的是lv_display_t *
*/
static bool notify_lvgl_flush_ready(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;       //无唤醒更高优先级任务，不需要立即切换任务
}

#if APP_LVGL_USE_DMA2D_COPY_FRAME
/* DMA2D 加密场景下对刷新区域做 16 字节对齐。 */
static void rounder_flush_area_cb(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_invalidated_area(event);
    area->x1 = ALIGN_DOWN(area->x1, 16);
    area->x2 = ALIGN_UP(area->x2, 16) - 1;
}
#endif

/* 打开 ESP32-P4 MIPI DSI PHY 使用的内部 LDO 电源。 */
static void enable_dsi_phy_power(void)
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = APP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = APP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
}
/*界面显示设置*/
static void apply_lcd_backlight_percent(uint8_t pct)
{
    if (pct > 100U) pct = 100U;

    if (!s_backlight_pwm_ready) {
        gpio_set_level(APP_PIN_NUM_BK_LIGHT, pct > 0U ? APP_LCD_BK_LIGHT_ON_LEVEL : APP_LCD_BK_LIGHT_OFF_LEVEL);
        return;
    }

    uint32_t duty = (APP_LCD_BK_LEDC_DUTY_MAX * (uint32_t)pct) / 100U;
    ledc_set_duty(APP_LCD_BK_LEDC_MODE, APP_LCD_BK_LEDC_CHANNEL, duty);
    ledc_update_duty(APP_LCD_BK_LEDC_MODE, APP_LCD_BK_LEDC_CHANNEL);
}

static void init_lcd_backlight(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = APP_LCD_BK_LEDC_MODE,
        .duty_resolution = APP_LCD_BK_LEDC_DUTY_RES,
        .timer_num = APP_LCD_BK_LEDC_TIMER,
        .freq_hz = APP_LCD_BK_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    if (ledc_timer_config(&timer_cfg) != ESP_OK) {
        gpio_config_t bk_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << APP_PIN_NUM_BK_LIGHT,
        };
        ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
        s_backlight_pwm_ready = false;
        return;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num = APP_PIN_NUM_BK_LIGHT,
        .speed_mode = APP_LCD_BK_LEDC_MODE,
        .channel = APP_LCD_BK_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = APP_LCD_BK_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
    s_backlight_pwm_ready = true;
    apply_lcd_backlight_percent(0);
}

static void set_lcd_backlight(uint32_t level)
{
    if (level == APP_LCD_BK_LIGHT_OFF_LEVEL) {
        apply_lcd_backlight_percent(0);
        s_backlight_dimmed = true;
    } else {
        apply_lcd_backlight_percent(s_display_settings.brightness_pct);
        s_backlight_dimmed = false;
    }
}

/* 初始化实时监测界面使用的通用 LVGL 样式。 */
static void init_styles(void)
{
    lv_style_init(&s_style_bg);
    lv_style_set_bg_color(&s_style_bg, c(0x061625));
    lv_style_set_bg_grad_color(&s_style_bg, c(0x08263a));
    lv_style_set_bg_grad_dir(&s_style_bg, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_style_bg, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_bg, c(0xf4f8ff));
    lv_style_set_text_font(&s_style_bg, ui_font());

    lv_style_init(&s_style_card);
    lv_style_set_radius(&s_style_card, 6);
    lv_style_set_bg_color(&s_style_card, c(0x082035));
    lv_style_set_bg_opa(&s_style_card, LV_OPA_80);
    lv_style_set_border_color(&s_style_card, c(0x24506c));
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_pad_all(&s_style_card, 10);
    lv_style_set_text_color(&s_style_card, c(0xf4f8ff));

    lv_style_init(&s_style_button);
    lv_style_set_radius(&s_style_button, 6);
    lv_style_set_bg_color(&s_style_button, c(0x11b82e));
    lv_style_set_bg_grad_color(&s_style_button, c(0x1ddc45));
    lv_style_set_bg_grad_dir(&s_style_button, LV_GRAD_DIR_VER);
    lv_style_set_text_color(&s_style_button, c(0xffffff));
    lv_style_set_border_width(&s_style_button, 0);

    lv_style_init(&s_style_button_stop);
    lv_style_set_radius(&s_style_button_stop, 6);
    lv_style_set_bg_color(&s_style_button_stop, c(0xc72222));
    lv_style_set_bg_grad_color(&s_style_button_stop, c(0xff4a43));
    lv_style_set_bg_grad_dir(&s_style_button_stop, LV_GRAD_DIR_VER);
    lv_style_set_text_color(&s_style_button_stop, c(0xffffff));
    lv_style_set_border_width(&s_style_button_stop, 0);

    lv_style_init(&s_style_nav);
    lv_style_set_radius(&s_style_nav, 8);
    lv_style_set_bg_color(&s_style_nav, c(0x08243a));
    lv_style_set_bg_opa(&s_style_nav, LV_OPA_70);
    lv_style_set_border_width(&s_style_nav, 1);
    lv_style_set_border_color(&s_style_nav, c(0x24506c));
    lv_style_set_text_color(&s_style_nav, c(0xc7d4e0));

    lv_style_init(&s_style_nav_active);
    lv_style_set_radius(&s_style_nav_active, 8);
    lv_style_set_bg_color(&s_style_nav_active, c(0x0d4ea0));
    lv_style_set_bg_grad_color(&s_style_nav_active, c(0x1762be));
    lv_style_set_bg_grad_dir(&s_style_nav_active, LV_GRAD_DIR_HOR);
    lv_style_set_border_width(&s_style_nav_active, 1);
    lv_style_set_border_color(&s_style_nav_active, c(0x39c6ff));
    lv_style_set_text_color(&s_style_nav_active, c(0xffffff));
    /*底部界面按钮背景样式*/
    lv_style_init(&s_style_title_box);
    lv_style_set_radius(&s_style_title_box, 8);
    lv_style_set_bg_opa(&s_style_title_box, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_title_box, 1);
    lv_style_set_pad_all(&s_style_title_box, 0);

    lv_style_init(&s_style_title_text);
    lv_style_set_text_font(&s_style_title_text, ui_font());
    lv_style_set_text_color(&s_style_title_text, c(0xffffff));

    lv_style_init(&s_style_muted);
    lv_style_set_text_color(&s_style_muted, c(0x91a9ba));

    lv_style_init(&s_style_good);
    lv_style_set_text_color(&s_style_good, c(0x8cff47));

    lv_style_init(&s_style_cyan);
    lv_style_set_text_color(&s_style_cyan, c(0x2ceaff));

    /*按钮样式改明显*/
    lv_style_set_bg_opa(&s_style_button, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_button, 1);
    lv_style_set_border_color(&s_style_button, c(0x72ff84));

    lv_style_set_bg_opa(&s_style_button_stop, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_button_stop, 1);
    lv_style_set_border_color(&s_style_button_stop, c(0xff928c));

        apply_main_theme_styles();
}

/* 创建固定位置文本标签。 */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    return label;
}

/* 创建带边框和背景的界面区域容器。 */
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

/* 创建带文本的按钮，并应用指定按钮样式。 */
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

/* 创建左侧名称和右侧数值组成的信息行。 */
static void create_metric_row(lv_obj_t *parent, const char *name, const char *value,
                              int32_t y, lv_obj_t **out_value)
{
    make_label(parent, name, 10, y);
    lv_obj_t *val = make_label(parent, value, 118, y);
    if (out_value) {
        *out_value = val;
    }
}

/* 设置图表控件的统一背景、网格和线条样式。 */
static void chart_set_common_style(lv_obj_t *chart)
{
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_left(chart, 8, 0);
    lv_obj_set_style_pad_right(chart, 6, 0);
    lv_obj_set_style_pad_top(chart, 8, 0);
    lv_obj_set_style_pad_bottom(chart, 4, 0);
    lv_obj_set_style_line_color(chart, c(0x24495e), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 4, 4,LV_PART_INDICATOR);/*对象, 宽度, 高度, selector作用部件*/
}

/* 在图表底部创建自定义横轴标签。 */
static void create_axis_labels(lv_obj_t *card, int32_t left, int32_t right, int32_t y,
                               const char *const *labels, uint32_t count, bool compact)
{
    if (count == 0) {
        return;
    }

    int32_t span = right - left;
    if (compact) {
        left += 2;
        right -= 2;
        span = right - left;
    }

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *label = make_label(card, labels[i], 0, y);
        lv_obj_add_style(label, &s_style_muted, 0);

        if (compact) {
             #if LV_FONT_MONTSERRAT_16
                 lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
             #endif
        }

        lv_obj_update_layout(label);

        int32_t label_w = lv_obj_get_width(label);
        int32_t x = (count > 1U) ? left + (span * (int32_t)i) / (int32_t)(count - 1U) : left;

        if (i == 0U) {
            lv_obj_set_pos(label, left, y);
        } else if (i + 1U == count) {
            lv_obj_set_pos(label, right - label_w, y);
        } else {
            lv_obj_set_pos(label, x - label_w / 2, y);
        }
    }
}

static void create_y_axis_labels(lv_obj_t *card, int32_t chart_y, int32_t chart_h,
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

/* 创建趋势图卡片，包括标题、坐标标签、图表和曲线序列。 */
static void create_trend_chart(lv_obj_t *parent, chart_view_t *view, const char *title,
                               const char *axis, int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t min, int32_t max, uint32_t color,
                               const char *const *xlabels, uint32_t xlabel_count,
                               uint32_t point_count)
{
    view->card = make_card(parent, x, y, w, h);
    make_label(view->card, title, 10, 8);

    if (axis != NULL && axis[0] != '\0') {
        lv_obj_t *axis_label = make_label(view->card, axis, w - 147, 8);
        lv_obj_add_style(axis_label, &s_style_muted, 0);
    }

    int32_t chart_x = 50;
    int32_t chart_y = 34;
    int32_t chart_w = w - 74;
    int32_t chart_h = h - 72;

    create_y_axis_labels(view->card, chart_y, chart_h, min, max);

    view->chart = lv_chart_create(view->card);
    lv_obj_set_pos(view->chart, chart_x, chart_y);
    lv_obj_set_size(view->chart, chart_w, chart_h);
    lv_chart_set_type(view->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(view->chart, point_count);
    lv_chart_set_axis_range(view->chart, LV_CHART_AXIS_PRIMARY_Y, min, max);
    lv_chart_set_div_line_count(view->chart, 4, 6);
    lv_chart_set_update_mode(view->chart, LV_CHART_UPDATE_MODE_SHIFT);
    chart_set_common_style(view->chart);
    view->series = lv_chart_add_series(view->chart, c(color), LV_CHART_AXIS_PRIMARY_Y);

    bool compact_x_axis = (w == 334 && h == 142);
    int32_t xlabel_y = chart_y + chart_h + 2;

    if (compact_x_axis) {
        xlabel_y = chart_y + chart_h - 2;
    }

    create_axis_labels(view->card, chart_x, chart_x + chart_w, xlabel_y,
                      xlabels, xlabel_count, compact_x_axis);
}

/* 把缓存数据写入 LVGL 图表并刷新显示。 */
static void update_chart_values(chart_view_t *view, int32_t *values, uint32_t count)
{
    if (!view->chart || !view->series) {
        return;
    }
    lv_chart_set_series_values(view->chart, view->series, values, count);
    lv_chart_refresh(view->chart);
}

static bool monitor_page_visible(void)
{
    return s_current_page == APP_LVGL_PAGE_MONITOR &&
           s_pages[APP_LVGL_PAGE_MONITOR] != NULL &&
           !lv_obj_has_flag(s_pages[APP_LVGL_PAGE_MONITOR], LV_OBJ_FLAG_HIDDEN);
}

static void realtime_chart_flush_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!monitor_page_visible()) {
        return;
    }

    for (uint8_t i = 0; i < 3U; i++) {
        uint8_t step = s_chart_flush_step;
        s_chart_flush_step = (uint8_t)((s_chart_flush_step + 1U) % 3U);

        if (step == 0U && s_anxiety_chart_dirty) {
            s_anxiety_chart_dirty = false;
            update_chart_values(&s_anxiety_chart, s_anxiety_values, APP_TREND_POINTS);
            return;
        }

        if (step == 1U && s_pr_chart_dirty) {
            s_pr_chart_dirty = false;
            update_chart_values(&s_pr_chart, s_pr_values, APP_TREND_POINTS);
            return;
        }

        if (step == 2U && s_ppi_chart_dirty) {
            s_ppi_chart_dirty = false;
            update_chart_values(&s_ppi_chart, s_ppi_values, APP_PPI_POINTS);
            return;
        }
    }
}

/* 创建 9 项特征参数卡片，顺序为 PR、RMSSD、SD2、HF、LF、LF/HF、WE、DFA、VAI。 */
static void create_feature_cards(lv_obj_t *parent)
{
    static const char *const names[APP_FEATURE_COUNT] = {
        "1. PR", "2.RMSSD", "3. SD2", "4. HF", "5. LF",
        "6. LF/HF", "7. WE", "8. DFA", "9. VAI",
    };
    static const char *const units[APP_FEATURE_COUNT] = {
        "bpm", "ms", "bpm", "", "", "", "", "", "",
    };

    int32_t card_w = 74;
    int32_t card_h = 42;
    int32_t gap = 6;
    int32_t x0 = 10;
    int32_t y0 = 36;

    for (uint32_t i = 0; i < APP_FEATURE_COUNT; i++) {
        int32_t col = (int32_t)(i % 5U);
        int32_t row = (int32_t)(i / 5U);

        lv_obj_t *box = make_card(parent,
                                  x0 + col * (card_w + gap),
                                  y0 + row * (card_h + gap),
                                  card_w,
                                  card_h);
        lv_obj_set_style_pad_all(box, 4, 0);

        make_label(box, names[i], 3, 1);

        s_features[i].title = names[i];
        s_features[i].unit = units[i];

        s_features[i].value = make_label(box, "--", 3, 21);
        lv_obj_add_style(s_features[i].value, &s_style_cyan, 0);

        s_features[i].unit_label = make_label(box, units[i], 34, 19);
        lv_obj_add_style(s_features[i].unit_label, &s_style_muted, 0);
    }
}
/*系统标题颜色适配主题*/
static void update_header_title_style(app_lvgl_page_t page)
{
    uint32_t bg = 0x123c5a;
    uint32_t grad = 0x0d6ea8;
    uint32_t border = 0x3aaeea;
    uint32_t text = 0xf3fbff;

    switch (s_display_settings.theme_mode) {
    case APP_LVGL_THEME_LIGHT://浅色
        switch (page) {
        case APP_LVGL_PAGE_MONITOR:
            bg = 0xe0f2fe; grad = 0xbae6fd; border = 0x0284c7; text = 0x075985;
            break;
        case APP_LVGL_PAGE_HISTORY:
            bg = 0xdcfce7; grad = 0xbbf7d0; border = 0x16a34a; text = 0x14532d;
            break;
        case APP_LVGL_PAGE_STATS:
            bg = 0xfef3c7; grad = 0xfde68a; border = 0xd97706; text = 0x78350f;
            break;
        case APP_LVGL_PAGE_SETTINGS:
            bg = 0xede9fe; grad = 0xddd6fe; border = 0x7c3aed; text = 0x3b0764;
            break;
        case APP_LVGL_PAGE_ABOUT:
            bg = 0xe0e7ff; grad = 0xc7d2fe; border = 0x4f46e5; text = 0x1e1b4b;
            break;
        default:
            break;
        }
        break;

    case APP_LVGL_THEME_EYE_CARE://护眼
        switch (page) {
        case APP_LVGL_PAGE_MONITOR:
            bg = 0x294b43; grad = 0x356c5d; border = 0x9be7c7; text = 0xf1fff7;
            break;
        case APP_LVGL_PAGE_HISTORY:
            bg = 0x31533d; grad = 0x47734f; border = 0xb8e6b0; text = 0xf5fff0;
            break;
        case APP_LVGL_PAGE_STATS:
            bg = 0x4a452c; grad = 0x6f6336; border = 0xe2d38c; text = 0xfff8dc;
            break;
        case APP_LVGL_PAGE_SETTINGS:
            bg = 0x40364d; grad = 0x5b4b72; border = 0xd6c7f2; text = 0xf7f0ff;
            break;
        case APP_LVGL_PAGE_ABOUT:
            bg = 0x2d4255; grad = 0x3d5f78; border = 0xb6d7f2; text = 0xf2fbff;
            break;
        default:
            break;
        }
        break;

    case APP_LVGL_THEME_HIGH_CONTRAST://高对比
        switch (page) {
        case APP_LVGL_PAGE_MONITOR:
            bg = 0x001f24; grad = 0x00414a; border = 0x00ffff; text = 0xffffff;
            break;
        case APP_LVGL_PAGE_HISTORY:
            bg = 0x00280f; grad = 0x004d22; border = 0x00ff66; text = 0xffffff;
            break;
        case APP_LVGL_PAGE_STATS:
            bg = 0x302800; grad = 0x5a4a00; border = 0xffff00; text = 0xffffff;
            break;
        case APP_LVGL_PAGE_SETTINGS:
            bg = 0x2a0030; grad = 0x53005e; border = 0xff66ff; text = 0xffffff;
            break;
        case APP_LVGL_PAGE_ABOUT:
            bg = 0x111111; grad = 0x333333; border = 0xffffff; text = 0xffffff;
            break;
        default:
            break;
        }
        break;

    case APP_LVGL_THEME_DARK://深色
    default:
        switch (page) {
        case APP_LVGL_PAGE_MONITOR:
            bg = 0x0e3b5a; grad = 0x116c9c; border = 0x41dfff; text = 0xf3fbff;
            break;
        case APP_LVGL_PAGE_HISTORY:
            bg = 0x113f31; grad = 0x1f7a5b; border = 0x65e7b8; text = 0xecfff8;
            break;
        case APP_LVGL_PAGE_STATS:
            bg = 0x46370d; grad = 0x8a5e06; border = 0xfacc15; text = 0xfffbeb;
            break;
        case APP_LVGL_PAGE_SETTINGS:
            bg = 0x32235d; grad = 0x5b3fb1; border = 0xc4b5fd; text = 0xf7f3ff;
            break;
        case APP_LVGL_PAGE_ABOUT:
            bg = 0x0f2f54; grad = 0x1d4f8d; border = 0x93c5fd; text = 0xf1f5ff;
            break;
        default:
            break;
        }
        break;
    }

    lv_style_set_bg_color(&s_style_title_box, c(bg));
    lv_style_set_bg_grad_color(&s_style_title_box, c(grad));
    lv_style_set_bg_grad_dir(&s_style_title_box, LV_GRAD_DIR_HOR);
    lv_style_set_bg_opa(&s_style_title_box, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_title_box, c(border));
    lv_style_set_border_width(&s_style_title_box, 2);
    lv_style_set_text_color(&s_style_title_text, c(text));

    if (s_title_box != NULL) {
        lv_obj_invalidate(s_title_box);
    }
}

/* 根据当前页面更新底部导航按钮的选中样式。 */
static void update_nav_styles(app_lvgl_page_t page)
{
    for (uint32_t i = 0; i < APP_LVGL_PAGE_COUNT; i++) {
        lv_obj_remove_style(s_nav[i].btn, &s_style_nav_active, 0);
        lv_obj_remove_style(s_nav[i].btn, &s_style_nav, 0);
        lv_obj_add_style(s_nav[i].btn, (i == (uint32_t)page) ? &s_style_nav_active : &s_style_nav, 0);
    }
}

/* 切换当前显示页面---切到历史记录页时，如果后台有新记录，才刷新一次；切到数据统计页时，如果后台有新数据，才刷新一次。*/
void app_lvgl_show_page(app_lvgl_page_t page)
{
    if (page >= APP_LVGL_PAGE_COUNT || s_root == NULL) {
        return;
    }
    s_current_page = page;
    update_header_title_style(page);

    for (uint32_t i = 0; i < APP_LVGL_PAGE_COUNT; i++) 
    {
        if (i == (uint32_t)page) 
        {
           lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }    
        else 
        {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_nav_styles(page);
    if (page == APP_LVGL_PAGE_HISTORY) {
        app_ui_history_on_show();
    } else if (page == APP_LVGL_PAGE_STATS) {
        app_ui_statics_on_show();
    } else if (page == APP_LVGL_PAGE_ABOUT) {
        app_ui_about_on_show();
}
}


/* 底部导航按钮事件回调，用于切换页面。 */
static void nav_event_cb(lv_event_t *event)
{
    app_lvgl_page_t page = (app_lvgl_page_t)(uintptr_t)lv_event_get_user_data(event);
    app_lvgl_show_page(page);
}


/* 屏幕 Start 按钮事件回调，通过用户回调启动采样。 */
static void start_btn_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_callbacks.start == NULL) {
        return;
    }

    esp_err_t ret = s_callbacks.start(s_callbacks.ctx);
    if (ret == ESP_OK) {
        app_lvgl_reset_monitoring();
        app_lvgl_set_sampling_running(true);
    } else {
        ESP_LOGW(TAG, "start callback failed: %s", esp_err_to_name(ret));
    }
}

/* 屏幕 Stop 按钮事件回调，通过用户回调停止采样。 */
static void stop_btn_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_callbacks.stop == NULL) {
        return;
    }

    esp_err_t ret = s_callbacks.stop(s_callbacks.ctx);
    if (ret == ESP_OK) {
        app_lvgl_set_sampling_running(false);
    } else {
        ESP_LOGW(TAG, "stop callback failed: %s", esp_err_to_name(ret));
    }
}

/* 创建顶部标题、运行状态、时间和设置入口。 */
static void build_header(void)
{
    /*修改系统名称*/
    s_title_box = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_title_box);
    lv_obj_add_style(s_title_box, &s_style_title_box, 0);
    lv_obj_set_pos(s_title_box, 10, 6);
    lv_obj_set_size(s_title_box, 210, 36);                                //框 标题框
    lv_obj_clear_flag(s_title_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_title_box, LV_OBJ_FLAG_CLICKABLE);

    s_title_label = lv_label_create(s_title_box);
    lv_label_set_text(s_title_label, "焦虑指数监测系统");
    lv_obj_add_style(s_title_label, &s_style_title_text, 0);
    lv_obj_set_style_transform_scale(s_title_label, 351, 0);              //  305/256  字体大小
    lv_obj_set_pos(s_title_label, 10, 6);

    update_header_title_style(s_current_page);
    /*============*/
    s_status_dot = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 12, 12);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, c(0x7b8794), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_status_dot, 468, 18);

    s_status_label = make_label(s_root, "已停止", 486, 13);
    lv_obj_add_style(s_status_label, &s_style_muted, 0);
    s_wifi_label = make_label(s_root, "", 552, 13);
    lv_obj_add_style(s_wifi_label, &s_style_muted, 0);

    for (uint8_t i = 0; i < 4; i++) {
        static const int8_t bar_h[4] = {4, 7, 10, 13};

        s_wifi_bars[i] = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_wifi_bars[i]);
        lv_obj_set_size(s_wifi_bars[i], 4, bar_h[i]);
        lv_obj_set_pos(s_wifi_bars[i], 622 + i * 6, 29 - bar_h[i]);
        lv_obj_set_style_radius(s_wifi_bars[i], 2, 0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i], LV_OPA_30, 0);
        lv_obj_add_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_time_label = make_label(s_root, "--:--:--", 670, 13);

    lv_obj_t *settings = make_button(s_root, LV_SYMBOL_SETTINGS " 设置", 922, 8, 90, 34, &s_style_nav);
    lv_obj_add_event_cb(settings, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)APP_LVGL_PAGE_SETTINGS);
}

/* 创建系统控制区域，包含 Start 和 Stop 按钮。 */
static void build_control_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 8, 226, 96);
    make_label(card, "系统控制", 10, 10);

    lv_obj_t *start = make_button(card, LV_SYMBOL_PLAY " Start", 14, 42, 88, 34, &s_style_button);
    lv_obj_add_event_cb(start, start_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop = make_button(card, LV_SYMBOL_STOP " Stop", 120, 42, 88, 34, &s_style_button_stop);
    lv_obj_add_event_cb(stop, stop_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

/* 创建采集状态区域，显示采样率、窗口参数、运行时间和窗口号。 */
static void build_acq_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 112, 226, 184);
    make_label(card, "采集状态", 5, 8);
    create_metric_row(card, "状态:", "已停止", 27, &s_run_state_label);
    create_metric_row(card, "ADC采样率:", "1000 Hz", 48, NULL);
    create_metric_row(card, "PPI采样率:", "250 Hz", 69, NULL);
    create_metric_row(card, "当前窗口:", "30.0 s", 90, NULL);
    create_metric_row(card, "滑动步长:", "10.0 s", 111, NULL);
    create_metric_row(card, "已运行时间:", "00:00:00", 132, &s_elapsed_label);
    create_metric_row(card, "窗口序号:", "0", 153, &s_window_id_label);
}

/* 创建实时信息区域，显示最新焦虑指数、PR、PPI 和 PPI 数量。 */
static void build_realtime_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 8, 304, 226, 186);
    make_label(card, "实时信息", 10, 10);
    create_metric_row(card, "最新焦虑指数:", "--", 38, &s_latest_anxiety_label);
    lv_obj_add_style(s_latest_anxiety_label, &s_style_cyan, 0);
    create_metric_row(card, "等级:", "--", 62, &s_latest_level_label);
    lv_obj_add_style(s_latest_level_label, &s_style_good, 0);
    create_metric_row(card, "当前平均脉率:", "-- bpm", 86, &s_latest_pr_label);
    create_metric_row(card, "当前平均PPI:", "-- ms", 110, &s_latest_ppi_label);
    create_metric_row(card, "PPI数量:", "--", 134, &s_ppi_count_label);
}

/* 创建焦虑指数仪表盘区域。 */
#define APP_SCORE_GAUGE_ROTATION       140
#define APP_SCORE_GAUGE_SPAN           260
#define APP_SCORE_GAUGE_SIZE           138
#define APP_SCORE_GAUGE_X              53
#define APP_SCORE_GAUGE_Y              20
#define APP_SCORE_GAUGE_CENTER_X       122
#define APP_SCORE_GAUGE_CENTER_Y       89
#define APP_SCORE_GAUGE_POINTER_R      52
#define APP_SCORE_GAUGE_TICK_COUNT     11
#define APP_SCORE_GAUGE_PI             3.14159265f


#define APP_SCORE_GAUGE_LOW_SPAN       92
#define APP_SCORE_GAUGE_MID_SPAN       62
#define APP_SCORE_GAUGE_HIGH_SPAN      (APP_SCORE_GAUGE_SPAN - APP_SCORE_GAUGE_LOW_SPAN - APP_SCORE_GAUGE_MID_SPAN)
#define APP_SCORE_GAUGE_MARK_R         88


typedef struct {
    uint32_t panel_bg;
    uint32_t panel_grad;
    uint32_t panel_border;
    uint32_t ring_bg;
    uint32_t low;
    uint32_t mid;
    uint32_t high;
    uint32_t tick;
    uint32_t pointer;
    uint32_t center;
    uint32_t muted;
    uint32_t pill_bg;
    uint32_t pill_text;
    lv_opa_t panel_opa;
} score_gauge_palette_t;

static score_gauge_palette_t current_score_gauge_palette(void)
{
    switch (s_display_settings.theme_mode) {
    case APP_LVGL_THEME_LIGHT:
    return (score_gauge_palette_t){
        .panel_bg = 0x174a72, .panel_grad = 0x236f9e, .panel_border = 0x57c9ff,
        .ring_bg = 0x23506e, .low = 0x24c4ff, .mid = 0x9cff3a, .high = 0xff8a36,
        .tick = 0xbdefff, .pointer = 0xf2ffff, .center = 0x45eaff,
        .muted = 0xe2f8ff, .pill_bg = 0x145d91, .pill_text = 0xe8ffff,
        .panel_opa = LV_OPA_COVER,
    };

    case APP_LVGL_THEME_EYE_CARE:
    return (score_gauge_palette_t){
        .panel_bg = 0x214b3f, .panel_grad = 0x2f6a57, .panel_border = 0x9be7c7,
        .ring_bg = 0x345d51, .low = 0x55d7ff, .mid = 0xb6ef64, .high = 0xffb24a,
        .tick = 0xd8fff0, .pointer = 0xf4fff9, .center = 0x7df7d0,
        .muted = 0xe4fff3, .pill_bg = 0x2d705f, .pill_text = 0xf2fff8,
        .panel_opa = LV_OPA_COVER,
    };

    case APP_LVGL_THEME_HIGH_CONTRAST:
    return (score_gauge_palette_t){
        .panel_bg = 0x101010, .panel_grad = 0x242424, .panel_border = 0xffff00,
        .ring_bg = 0x444444, .low = 0x00ffff, .mid = 0xffff00, .high = 0xff3030,
        .tick = 0xffffff, .pointer = 0xffffff, .center = 0x00ffff,
        .muted = 0xffffff, .pill_bg = 0x202000, .pill_text = 0xffff00,
        .panel_opa = LV_OPA_COVER,
    };

   case APP_LVGL_THEME_DARK:
    default:
    return (score_gauge_palette_t){
        .panel_bg = 0x0b2b46, .panel_grad = 0x15466a, .panel_border = 0x35bfff,
        .ring_bg = 0x1d405a, .low = 0x24c4ff, .mid = 0x9cff3a, .high = 0xff8a36,
        .tick = 0xaeeaff, .pointer = 0xe8fbff, .center = 0x2ceaff,
        .muted = 0xd4f3ff, .pill_bg = 0x0d5484, .pill_text = 0xe8ffff,
        .panel_opa = LV_OPA_COVER,
    };
 }
}

static int32_t score_to_gauge_angle(float score)
{
    if (!isfinite(score)) return 0;
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;

    float low = s_anxiety_thresholds.low_max;
    float mid = s_anxiety_thresholds.mid_max;

    if (low < 1.0f) low = 1.0f;
    if (mid <= low + 0.1f) mid = low + 0.1f;
    if (mid > 99.0f) mid = 99.0f;

    float angle;
    if (score <= low) {
        angle = (score / low) * (float)APP_SCORE_GAUGE_LOW_SPAN;
    } else if (score <= mid) {
        angle = (float)APP_SCORE_GAUGE_LOW_SPAN +
                ((score - low) / (mid - low)) * (float)APP_SCORE_GAUGE_MID_SPAN;
    } else {
        angle = (float)(APP_SCORE_GAUGE_LOW_SPAN + APP_SCORE_GAUGE_MID_SPAN) +
                ((score - mid) / (100.0f - mid)) * (float)APP_SCORE_GAUGE_HIGH_SPAN;
    }

    return clamp_i32((int32_t)lroundf(angle), 0, APP_SCORE_GAUGE_SPAN);
}

static void place_score_mark_label(lv_obj_t *label, float score, lv_color_t color)
{
    int32_t gauge_angle = score_to_gauge_angle(score);
    float rad = ((float)(APP_SCORE_GAUGE_ROTATION + gauge_angle) * APP_SCORE_GAUGE_PI) / 180.0f;

    int32_t mark_r = APP_SCORE_GAUGE_MARK_R;
    int32_t x = APP_SCORE_GAUGE_CENTER_X + (int32_t)lroundf(cosf(rad) * mark_r) - 12;
    int32_t y = APP_SCORE_GAUGE_CENTER_Y + (int32_t)lroundf(sinf(rad) * mark_r) - 8;

    x = clamp_i32(x, 18, 204);
    y = clamp_i32(y, 2, 126);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, 28);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, color, 0);
}

static lv_color_t score_gauge_level_color(float score)
{
    score_gauge_palette_t gp = current_score_gauge_palette();

    if (score <= (float)s_anxiety_thresholds.low_max) return c(gp.low);
    if (score <= (float)s_anxiety_thresholds.mid_max) return c(gp.mid);
    return c(gp.high);
}

static lv_obj_t *make_score_arc(lv_obj_t *parent, int32_t width, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_pos(arc, APP_SCORE_GAUGE_X, APP_SCORE_GAUGE_Y);
    lv_obj_set_size(arc, APP_SCORE_GAUGE_SIZE, APP_SCORE_GAUGE_SIZE);
    lv_arc_set_rotation(arc, APP_SCORE_GAUGE_ROTATION);
    lv_arc_set_bg_angles(arc, 0, APP_SCORE_GAUGE_SPAN);
    lv_arc_set_angles(arc, 0, 0);

    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, c(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    return arc;
}



static void set_score_pointer(float score, bool visible)
{
    if (s_score_pointer == NULL) return;

    if (!visible) {
        lv_obj_add_flag(s_score_pointer, LV_OBJ_FLAG_HIDDEN);
        if (s_score_pointer_dot) lv_obj_add_flag(s_score_pointer_dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int32_t gauge_angle = score_to_gauge_angle(score);
    float rad = ((float)(APP_SCORE_GAUGE_ROTATION + gauge_angle) * APP_SCORE_GAUGE_PI) / 180.0f;

    lv_value_precise_t end_x = APP_SCORE_GAUGE_CENTER_X +
                               (lv_value_precise_t)lroundf(cosf(rad) * APP_SCORE_GAUGE_POINTER_R);
    lv_value_precise_t end_y = APP_SCORE_GAUGE_CENTER_Y +
                               (lv_value_precise_t)lroundf(sinf(rad) * APP_SCORE_GAUGE_POINTER_R);

    s_score_pointer_points[0].x = APP_SCORE_GAUGE_CENTER_X;
    s_score_pointer_points[0].y = APP_SCORE_GAUGE_CENTER_Y;
    s_score_pointer_points[1].x = end_x;
    s_score_pointer_points[1].y = end_y;

    lv_line_set_points_mutable(s_score_pointer, s_score_pointer_points, 2);
    lv_obj_clear_flag(s_score_pointer, LV_OBJ_FLAG_HIDDEN);

    if (s_score_pointer_dot) {
        lv_obj_set_pos(s_score_pointer_dot, (int32_t)end_x - 3, (int32_t)end_y - 3);
        lv_obj_clear_flag(s_score_pointer_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_score_gauge_thresholds(void)
{
    if (s_score_low_arc == NULL) return;

    score_gauge_palette_t gp = current_score_gauge_palette();

    int32_t low_end = score_to_gauge_angle(s_anxiety_thresholds.low_max);
    int32_t mid_end = score_to_gauge_angle(s_anxiety_thresholds.mid_max);

    lv_arc_set_angles(s_score_low_arc, 0, low_end);
    lv_arc_set_angles(s_score_mid_arc, low_end + 2, mid_end);
    lv_arc_set_angles(s_score_high_arc, mid_end + 2, APP_SCORE_GAUGE_SPAN);

    lv_obj_set_style_arc_color(s_score_low_arc, c(gp.low), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_score_mid_arc, c(gp.mid), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_score_high_arc, c(gp.high), LV_PART_INDICATOR);

    char buf[16];

    snprintf(buf, sizeof(buf), "%.0f", (double)s_anxiety_thresholds.low_max);
    lv_label_set_text(s_score_low_mark_label, buf);
    place_score_mark_label(s_score_low_mark_label,
                           s_anxiety_thresholds.low_max,
                           c(gp.low));

    snprintf(buf, sizeof(buf), "%.0f", (double)s_anxiety_thresholds.mid_max);
    lv_label_set_text(s_score_mid_mark_label, buf);
    place_score_mark_label(s_score_mid_mark_label,
                           s_anxiety_thresholds.mid_max,
                           c(gp.mid));
}

static void apply_score_gauge_theme(void)
{
    if (s_score_gauge_box == NULL) return;

    score_gauge_palette_t gp = current_score_gauge_palette();
    if (s_score_title_label) {
    lv_obj_set_style_text_color(s_score_title_label, c(gp.muted), 0);
}
    lv_obj_set_style_bg_color(s_score_gauge_box, c(gp.panel_bg), 0);
    lv_obj_set_style_bg_grad_color(s_score_gauge_box, c(gp.panel_grad), 0);
    lv_obj_set_style_bg_grad_dir(s_score_gauge_box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_score_gauge_box, gp.panel_opa, 0);
    lv_obj_set_style_border_color(s_score_gauge_box, c(gp.panel_border), 0);
    lv_obj_set_style_shadow_color(s_score_gauge_box, c(gp.panel_border), 0);
    lv_obj_set_style_shadow_width(s_score_gauge_box, 8, 0);
    lv_obj_set_style_shadow_opa(s_score_gauge_box, LV_OPA_20, 0);

    lv_obj_set_style_arc_color(s_score_base_arc, c(gp.ring_bg), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_score_arc, c(gp.pointer), LV_PART_INDICATOR);

    for (uint32_t i = 0; i < APP_SCORE_GAUGE_TICK_COUNT; i++) {
        if (s_score_ticks[i]) {
            lv_obj_set_style_line_color(s_score_ticks[i], c(gp.tick), 0);
            lv_obj_set_style_line_opa(s_score_ticks[i], (i % 5U == 0U) ? LV_OPA_COVER : LV_OPA_60, 0);
        }
    }

    if (s_score_pointer) {
        lv_obj_set_style_line_color(s_score_pointer, c(gp.pointer), 0);
        lv_obj_set_style_line_opa(s_score_pointer, LV_OPA_COVER, 0);
    }

    if (s_score_pointer_dot) {
        lv_obj_set_style_bg_color(s_score_pointer_dot, c(gp.pointer), 0);
        lv_obj_set_style_border_color(s_score_pointer_dot, c(gp.center), 0);
    }

    lv_obj_set_style_text_color(s_score_label, c(gp.center), 0);
    lv_obj_set_style_bg_color(s_score_level_box, c(gp.pill_bg), 0);
    lv_obj_set_style_border_color(s_score_level_box, c(gp.center), 0);
    lv_obj_set_style_text_color(s_score_level_label, c(gp.pill_text), 0);

    lv_obj_set_style_text_color(s_score_low_mark_label, c(gp.muted), 0);
    lv_obj_set_style_text_color(s_score_mid_mark_label, c(gp.muted), 0);

    refresh_score_gauge_thresholds();
}


static void create_score_tick(lv_obj_t *parent, uint32_t idx)
{
    float score = (float)idx * 10.0f;
    int32_t gauge_angle = score_to_gauge_angle(score);
    float rad = ((float)(APP_SCORE_GAUGE_ROTATION + gauge_angle) * APP_SCORE_GAUGE_PI) / 180.0f;

    int32_t outer_r = 64;
    int32_t inner_r = (idx % 5U == 0U) ? 52 : 57;

    s_score_tick_points[idx][0].x = APP_SCORE_GAUGE_CENTER_X +
                                    (lv_value_precise_t)lroundf(cosf(rad) * outer_r);
    s_score_tick_points[idx][0].y = APP_SCORE_GAUGE_CENTER_Y +
                                    (lv_value_precise_t)lroundf(sinf(rad) * outer_r);
    s_score_tick_points[idx][1].x = APP_SCORE_GAUGE_CENTER_X +
                                    (lv_value_precise_t)lroundf(cosf(rad) * inner_r);
    s_score_tick_points[idx][1].y = APP_SCORE_GAUGE_CENTER_Y +
                                    (lv_value_precise_t)lroundf(sinf(rad) * inner_r);

    s_score_ticks[idx] = lv_line_create(parent);
    lv_obj_set_pos(s_score_ticks[idx], 0, 0);
    lv_obj_set_size(s_score_ticks[idx], 244, 154);
    lv_line_set_points_mutable(s_score_ticks[idx], s_score_tick_points[idx], 2);
    lv_obj_set_style_line_width(s_score_ticks[idx], (idx % 5U == 0U) ? 2 : 1, 0);
    lv_obj_clear_flag(s_score_ticks[idx], LV_OBJ_FLAG_CLICKABLE);
}
static void update_score_gauge(float score)
{
    int32_t angle = score_to_gauge_angle(score);
int32_t start = clamp_i32(angle - 1, 0, APP_SCORE_GAUGE_SPAN);
int32_t end = clamp_i32(angle + 1, 0, APP_SCORE_GAUGE_SPAN);

    lv_arc_set_angles(s_score_arc, start, end);
    lv_obj_set_style_arc_color(s_score_arc, score_gauge_level_color(score), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_score_level_label, score_gauge_level_color(score), 0);
    set_score_pointer(score, true);
}

static void reset_score_gauge(void)
{
    if (s_score_arc) {
        lv_arc_set_angles(s_score_arc, 0, 0);
    }
    set_score_pointer(0.0f, false);
    apply_score_gauge_theme();
}

static void build_score_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 772, 8, 244, 154);

    s_score_gauge_box = lv_obj_create(card);
    lv_obj_remove_style_all(s_score_gauge_box);
    lv_obj_set_pos(s_score_gauge_box, -10, -10);
    lv_obj_set_size(s_score_gauge_box, 244, 154);
    lv_obj_set_style_radius(s_score_gauge_box, 8, 0);
    lv_obj_set_style_border_width(s_score_gauge_box, 2, 0);
    lv_obj_clear_flag(s_score_gauge_box, LV_OBJ_FLAG_SCROLLABLE);

    s_score_title_label = make_label(card, "焦\n虑\n指\n数", 7, 34);
    lv_obj_set_width(s_score_title_label, 22);
    lv_obj_set_style_text_align(s_score_title_label, LV_TEXT_ALIGN_CENTER, 0);

    s_score_base_arc = make_score_arc(card, 21, 0x15344b, LV_OPA_70);
    lv_arc_set_angles(s_score_base_arc, 0, APP_SCORE_GAUGE_SPAN);

    s_score_low_arc = make_score_arc(card, 15, 0x24c4ff, LV_OPA_COVER);
    s_score_mid_arc = make_score_arc(card, 15, 0x9cff3a, LV_OPA_COVER);
    s_score_high_arc = make_score_arc(card, 15, 0xff6a2a, LV_OPA_COVER);

    for (uint32_t i = 0; i < APP_SCORE_GAUGE_TICK_COUNT; i++) {
        create_score_tick(card, i);
    }

    s_score_arc = make_score_arc(card, 5, 0xe8fbff, LV_OPA_COVER);

    s_score_pointer = lv_line_create(card);
    lv_obj_set_pos(s_score_pointer, 0, 0);
    lv_obj_set_size(s_score_pointer, 244, 154);
    lv_obj_set_style_line_width(s_score_pointer, 3, 0);
    lv_obj_set_style_line_rounded(s_score_pointer, true, 0);
    lv_obj_clear_flag(s_score_pointer, LV_OBJ_FLAG_CLICKABLE);

    s_score_pointer_dot = lv_obj_create(card);
    lv_obj_remove_style_all(s_score_pointer_dot);
    lv_obj_set_size(s_score_pointer_dot, 7, 7);
    lv_obj_set_style_radius(s_score_pointer_dot, 4, 0);
    lv_obj_set_style_bg_opa(s_score_pointer_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_score_pointer_dot, 1, 0);
    lv_obj_clear_flag(s_score_pointer_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_score_pointer_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_score_pointer_dot, LV_OBJ_FLAG_HIDDEN);

    s_score_low_mark_label = make_label(card, "", 0, 0);
    s_score_mid_mark_label = make_label(card, "", 0, 0);

    make_label(card, "0", 35, 124);
    make_label(card, "100", 188, 124);

    s_score_label = make_label(card, "--", 82, 82);
    lv_obj_set_width(s_score_label, 80);
    lv_obj_set_style_text_align(s_score_label, LV_TEXT_ALIGN_CENTER, 0);

    s_score_level_box = lv_obj_create(card);
    lv_obj_remove_style_all(s_score_level_box);
    lv_obj_set_pos(s_score_level_box, 84, 120);
    lv_obj_set_size(s_score_level_box, 76, 22);
    lv_obj_set_style_radius(s_score_level_box, 5, 0);
    lv_obj_set_style_bg_opa(s_score_level_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_score_level_box, 1, 0);
    lv_obj_clear_flag(s_score_level_box, LV_OBJ_FLAG_SCROLLABLE);

    s_score_level_label = make_label(s_score_level_box, "--", 0, 2);
    lv_obj_set_width(s_score_level_label, 76);
    lv_obj_set_style_text_align(s_score_level_label, LV_TEXT_ALIGN_CENTER, 0);

    apply_score_gauge_theme();
    reset_score_gauge();
}

/* 创建焦虑指数仪表盘区域。 */


/* 创建 9 项特征参数区域。 */
static void build_feature_panel(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 244, 170, 430, 142);
    make_label(card, "9项特征参数(当前窗口)", 10, 8);
    create_feature_cards(card);
}
/*主题函数*/
static void apply_ai_panel_theme(void)
{
    if (s_ai_card == NULL) return;

    uint32_t bg;
    uint32_t bg_grad;
    uint32_t border;
    uint32_t title;
    uint32_t text;

    switch (s_display_settings.theme_mode) {
    case APP_LVGL_THEME_LIGHT:
        bg = 0xc7e6f7;
        bg_grad = 0xa9d4ee;
        border = 0x2784bd;
        title = 0x075985;
        text = 0x173f59;
        break;

    case APP_LVGL_THEME_EYE_CARE:
        bg = 0x31594d;
        bg_grad = 0x294b41;
        border = 0x79b59f;
        title = 0xb8ead7;
        text = 0xe2f2e9;
        break;

    case APP_LVGL_THEME_HIGH_CONTRAST:
        bg = 0x202020;
        bg_grad = 0x101010;
        border = 0x00ffff;
        title = 0xffff00;
        text = 0xffffff;
        break;

    case APP_LVGL_THEME_DARK:
    default:
        bg = 0x174b68;
        bg_grad = 0x10364d;
        border = 0x35b8ed;
        title = 0x64d8ff;
        text = 0xe8f8ff;
        break;
    }

    /* 直接设置整个卡片背景 */
    lv_obj_set_style_bg_color(s_ai_card, c(bg), 0);
    lv_obj_set_style_bg_grad_color(s_ai_card, c(bg_grad), 0);
    lv_obj_set_style_bg_grad_dir(s_ai_card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_ai_card, LV_OPA_COVER, 0);

    lv_obj_set_style_border_width(s_ai_card, 2, 0);
    lv_obj_set_style_border_color(s_ai_card, c(border), 0);
    lv_obj_set_style_border_opa(s_ai_card, LV_OPA_COVER, 0);

   if (s_ai_title_label != NULL) {
        lv_obj_set_style_text_color(s_ai_title_label, c(title), 0);

        /* 只加粗“智能调节建议”标题 */
        lv_obj_set_style_text_outline_stroke_color(
            s_ai_title_label, c(title), 0);
        lv_obj_set_style_text_outline_stroke_width(
            s_ai_title_label, 1, 0);
        lv_obj_set_style_text_outline_stroke_opa(
            s_ai_title_label, LV_OPA_80, 0);
    }

    if (s_ai_text_label != NULL) {
        lv_obj_set_style_text_color(s_ai_text_label, c(text), 0);
    }
}
/* 大模型智能调节建议区域 */
static void build_ai_panel(lv_obj_t *parent)
{
    s_ai_card = make_card(parent, 682, 320, 334, 170);

    s_ai_title_label =
        make_label(s_ai_card, "智能调节建议", 14, 9);

    s_ai_text_label = lv_label_create(s_ai_card);
    lv_label_set_text(s_ai_text_label, "等待30秒窗口结果...");
    lv_obj_set_style_text_font(
         s_ai_text_label,
         s_ai_font != NULL ? s_ai_font : ui_font(),
         0
    );


    lv_obj_set_style_text_line_space(s_ai_text_label, 5, 0);
    lv_obj_set_style_text_align(s_ai_text_label,
                                LV_TEXT_ALIGN_LEFT, 0);

    lv_label_set_long_mode(s_ai_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ai_text_label, 12, 38);
    lv_obj_set_size(s_ai_text_label, 310, 120);

    apply_ai_panel_theme();
}
/* 创建底部 5 个模块导航入口。 */
static void build_bottom_nav(void)
{
    static const char *const labels[APP_LVGL_PAGE_COUNT] = {
        LV_SYMBOL_HOME " 实时监测",
        LV_SYMBOL_LIST " 历史记录",
        LV_SYMBOL_BARS " 数据统计",
        LV_SYMBOL_SETTINGS " 系统设置",
        "i 关于",
    };

    int32_t y = APP_UI_NAV_Y;
    int32_t h = APP_UI_NAV_H;
    int32_t x = 32;
    int32_t widths[APP_LVGL_PAGE_COUNT] = {168, 168, 168, 168, 168};
    int32_t gap = 28;

    for (uint32_t i = 0; i < APP_LVGL_PAGE_COUNT; i++) {
        s_nav[i].page = (app_lvgl_page_t)i;
        s_nav[i].btn = make_button(s_root, labels[i], x, y, widths[i], h, &s_style_nav);
        lv_obj_add_event_cb(s_nav[i].btn, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        x += widths[i] + gap;
    }
}

/* 创建暂未实现页面的占位界面，保留页面切换接口。 */
/*
static void build_placeholder_page(app_lvgl_page_t page, const char *title)
{
    lv_obj_t *obj = lv_obj_create(s_root);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &s_style_bg, 0);
    lv_obj_set_pos(obj, 0, APP_UI_PAGE_Y);
    lv_obj_set_size(obj, APP_LCD_H_RES, APP_UI_PAGE_H);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[page] = obj;

    lv_obj_t *card = make_card(obj, 276, 160, 472, 160);
    make_label(card, title, 188, 58);
}
*/
/* 创建实时监测主页面，并布局所有实时监测控件。 */
static void build_monitor_page(void)
{
    lv_obj_t *page = lv_obj_create(s_root);
    lv_obj_remove_style_all(page);
    lv_obj_add_style(page, &s_style_bg, 0);
    lv_obj_set_pos(page, 0, APP_UI_PAGE_Y);
    lv_obj_set_size(page, APP_LCD_H_RES, APP_UI_PAGE_H);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[APP_LVGL_PAGE_MONITOR] = page;

    build_control_panel(page);     /* 内部改成 y=8, h=96 */
    build_acq_panel(page);         /* 内部改成 y=112, h=184 */
    build_realtime_panel(page);    /* 内部改成 y=304, h=186 */

    static const char *const trend_labels[] = {"-5 min", "-4 min", "-3 min", "-2 min", "now"};
    static const char *const ppi_labels[] = { "-30s", "-20s", "-10s", "now" };

    create_trend_chart(page, &s_anxiety_chart, "焦虑指数趋势(每10秒更新)", "",
                   244, 8, 520, 154, 0, 100, 0x2ceaff,
                   trend_labels, sizeof(trend_labels) / sizeof(trend_labels[0]),
                   APP_TREND_POINTS);
                   enable_trend_faded_area(s_anxiety_chart.chart);

    build_score_panel(page);       /* 内部改成 x=772, y=8, h=154 */
    build_feature_panel(page);     /* 内部改成 x=244, y=170, h=142 */

    create_trend_chart(page, &s_ppi_chart, "脉搏峰值间期序列", "y-axis:ms   x-axis:s",
                   682, 170, 334, 142, 0, 1500, 0x6cff3f,
                   ppi_labels, sizeof(ppi_labels) / sizeof(ppi_labels[0]),
                   APP_PPI_POINTS);

    create_trend_chart(page, &s_pr_chart, "平均PR趋势(每10秒更新)", "平均PR (bpm)",
                   244, 320, 430, 170, 40, 140, 0xa8ff2f,
                   trend_labels, sizeof(trend_labels) / sizeof(trend_labels[0]),
                   APP_TREND_POINTS);
                   enable_trend_faded_area(s_pr_chart.chart);

    build_ai_panel(page);        /* 内部改成 x=682, y=320, h=170 */

}
/*=====================================================================================================================================*/
void app_lvgl_get_anxiety_thresholds(app_lvgl_anxiety_thresholds_t *out)
{
    if (out) *out = s_anxiety_thresholds;
}

void app_lvgl_set_anxiety_thresholds(uint8_t low_max, uint8_t mid_max)
{
    if (low_max > 90U) low_max = 90U;
    if (mid_max <= low_max + 5U) mid_max = low_max + 5U;
    if (mid_max > 100U) mid_max = 100U;

    s_anxiety_thresholds.low_max = low_max;
    s_anxiety_thresholds.mid_max = mid_max;
    refresh_score_gauge_thresholds();

    app_ui_about_on_show();
}

void app_lvgl_display_get_settings(app_lvgl_display_settings_t *out)
{
    if (out) *out = s_display_settings;
}

void app_lvgl_display_set_brightness(uint8_t pct)
{
    if (pct > 100U) pct = 100U;
    s_display_settings.brightness_pct = pct;

    if (!s_backlight_dimmed) {
        apply_lcd_backlight_percent(pct);
    }
}

void app_lvgl_display_set_sleep_sec(uint16_t sec)
{
    s_display_settings.sleep_sec = sec;
    if (sec == 0U && s_backlight_dimmed) {
        apply_lcd_backlight_percent(s_display_settings.brightness_pct);
        s_backlight_dimmed = false;
    }
}

void app_lvgl_display_set_theme(uint8_t theme_mode)
{
    if (theme_mode > APP_LVGL_THEME_HIGH_CONTRAST) {
        theme_mode = APP_LVGL_THEME_DARK;
    }

    s_display_settings.theme_mode = theme_mode;

    if (s_display != NULL) {
        const app_lvgl_theme_palette_t *p = current_theme_palette();
        bool dark = theme_mode != APP_LVGL_THEME_LIGHT;

        lv_theme_default_init(s_display, c(p->accent), c(p->danger),
                              dark,
                              ui_font());
    }

    apply_main_theme_styles();
    apply_score_gauge_theme();
    apply_ai_panel_theme();
    update_header_title_style(s_current_page);
    /*
    实时监测、历史记录、数据统计、系统设置、关于，每个页面的顶部系统标题颜色不同。
    深色、浅色、护眼、高对比四种主题下都会重新适配标题背景、边框和文字颜色。
    切换主题和切换页面都会刷新标题样式。
    */
    app_ui_history_apply_theme();
    app_ui_statics_apply_theme();
    app_ui_config_apply_theme();
    app_ui_about_apply_theme();

    if (s_root != NULL) {
        lv_obj_report_style_change(NULL);
        lv_obj_invalidate(s_root);
    }
}

/*=====================================================================================================================================*/
static void update_wifi_label(void)
{
    if (s_wifi_label == NULL) {
        return;
    }

    const app_lvgl_theme_palette_t *p = current_theme_palette();

    if (!app_rtc_wifi_is_connected()) {
        lv_label_set_text(s_wifi_label, "");
        for (uint8_t i = 0; i < 4; i++) {
            if (s_wifi_bars[i]) {
                lv_obj_add_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    uint8_t pct = app_rtc_wifi_get_signal_percent();
    uint8_t level = 1;

    if (pct >= 75U) level = 4;
    else if (pct >= 50U) level = 3;
    else if (pct >= 25U) level = 2;

    lv_label_set_text_fmt(s_wifi_label, LV_SYMBOL_WIFI " %u%%", pct);
    lv_obj_set_style_text_color(s_wifi_label, c(p->text), 0);

    uint32_t active_color = p->good;
    if (pct < 25U) active_color = p->danger;
    else if (pct < 50U) active_color = p->warning;

    for (uint8_t i = 0; i < 4; i++) {
        if (s_wifi_bars[i] == NULL) continue;

        lv_obj_clear_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_wifi_bars[i],
                                  c(i < level ? active_color : p->muted),
                                  0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i],
                                i < level ? LV_OPA_COVER : LV_OPA_30,
                                0);
    }
}

/* 刷新顶部时间显示，未校时时显示占位符。 */
static void update_time_label(void)
{
    update_wifi_label();

    char time_buf[32];

    if (!app_rtc_time_is_valid()) {
        lv_label_set_text(s_time_label, "校时中...");
        return;
    }

    app_rtc_get_time_string(time_buf, sizeof(time_buf));
    lv_label_set_text(s_time_label, time_buf);
}

/* 根据运行状态刷新状态文字、状态点和运行时间。 */
static void update_running_view(bool running, uint32_t elapsed_sec)
{
    char elapsed[16];
    format_elapsed(elapsed, sizeof(elapsed), elapsed_sec);

    lv_label_set_text(s_status_label, running ? "运行中" : "已停止");
    lv_label_set_text(s_run_state_label, running ? "运行中" : "已停止");
    lv_label_set_text(s_elapsed_label, elapsed);
    lv_obj_set_style_bg_color(s_status_dot, running ? c(0x00d53f) : c(0x7b8794), 0);
    lv_obj_set_style_text_color(s_status_label, running ? c(0x00ff4c) : c(0x91a9ba), 0);
    lv_obj_set_style_text_color(s_run_state_label, running ? c(0x8cff47) : c(0x91a9ba), 0);
}
/*日志刷新函数*/
static uint32_t log_oldest_index(void)
{
    if (s_log_used == 0U) return s_log_write;
    return (s_log_write + APP_LOG_HISTORY_LINES - s_log_used) % APP_LOG_HISTORY_LINES;
}
static void log_trim_to_limit(void)
{
    if (s_log_keep_limit == 0U) s_log_keep_limit = APP_LOG_HISTORY_LINES;
    if (s_log_keep_limit > APP_LOG_HISTORY_LINES) s_log_keep_limit = APP_LOG_HISTORY_LINES;
    if (s_log_used > s_log_keep_limit) s_log_used = s_log_keep_limit;
}
static void refresh_log_view(bool scroll_to_bottom)
{
    if (s_log_text_label == NULL) return;

    log_trim_to_limit();

    if (s_log_used == 0) {
        lv_label_set_text(s_log_text_label, "--");
        return;
    }

    size_t off = 0;
    s_log_text[0] = '\0';

        uint32_t display_count = s_log_used;
        if (display_count > APP_LOG_VISIBLE_LINES) {
            display_count = APP_LOG_VISIBLE_LINES;
        }
        uint32_t start = (s_log_write + APP_LOG_HISTORY_LINES - display_count) % APP_LOG_HISTORY_LINES;
    for (uint32_t i = 0; i < display_count; i++) {
        uint32_t idx = (start + i) % APP_LOG_HISTORY_LINES;
        size_t remain = sizeof(s_log_text) - off;
        if (remain <= 1) break;

        int n = snprintf(&s_log_text[off], remain, "%s%s",
                         s_log_history[idx],
                         ((i + 1U < display_count) ? "\n" : ""));
        if (n < 0) break;
        if ((size_t)n >= remain) {
            off = sizeof(s_log_text) - 1;
            break;
        }
        off += (size_t)n;
    }

    lv_label_set_text(s_log_text_label, s_log_text);

    if (scroll_to_bottom && s_log_area != NULL) {
        lv_obj_update_layout(s_log_area);
        lv_obj_scroll_to_y(s_log_area, LV_COORD_MAX, LV_ANIM_OFF);
    }
}
void app_lvgl_log_set_enabled(bool enabled)
{
    s_log_enabled = enabled;
}

bool app_lvgl_log_is_enabled(void)
{
    return s_log_enabled;
}

void app_lvgl_log_set_keep_limit(uint32_t keep_limit)
{
    if (keep_limit < 1U) keep_limit = 1U;
    if (keep_limit > APP_LOG_HISTORY_LINES) keep_limit = APP_LOG_HISTORY_LINES;

    s_log_keep_limit = keep_limit;
    log_trim_to_limit();
    refresh_log_view(false);
}

uint32_t app_lvgl_log_get_keep_limit(void)
{
    return s_log_keep_limit;
}

uint32_t app_lvgl_log_get_used_count(void)
{
    log_trim_to_limit();
    return s_log_used;
}

void app_lvgl_log_clear(void)
{
    memset(s_log_history, 0, sizeof(s_log_history));
    s_log_text[0] = '\0';
    s_log_write = 0;
    s_log_used = 0;
    s_log_count = 0;
    refresh_log_view(false);
}
esp_err_t app_lvgl_log_export_uart(void)
{
    log_trim_to_limit();

    printf("\n===PPI_LOG_CSV_BEGIN===\n");
    printf("No,Log\n");

    uint32_t start = log_oldest_index();
    for (uint32_t i = 0; i < s_log_used; i++) {
        uint32_t idx = (start + i) % APP_LOG_HISTORY_LINES;
        printf("%" PRIu32 ",\"", i + 1U);

        const char *p = s_log_history[idx];
        while (*p != '\0') {
            if (*p == '"') {
                putchar('"');
                putchar('"');
            } else {
                putchar((unsigned char)*p);
            }
            p++;
        }

        printf("\"\n");
    }

    printf("===PPI_LOG_CSV_END===\n");
    fflush(stdout);
    return ESP_OK;
}
/* 向系统日志区域追加一条简短日志---只有实时监测页显示时才刷新日志区域。*/
static void add_log_line(const char *text)
{
    if (!s_log_enabled || text == NULL) return;

    char line[APP_LOG_TEXT_LEN];
    uint32_t idx = s_log_count++;

    snprintf(line, sizeof(line), "%02" PRIu32 "  %s", idx, text);

    strncpy(s_log_history[s_log_write], line, APP_LOG_TEXT_LEN - 1);
    s_log_history[s_log_write][APP_LOG_TEXT_LEN - 1] = '\0';

    s_log_write = (s_log_write + 1U) % APP_LOG_HISTORY_LINES;

    if (s_log_used < APP_LOG_HISTORY_LINES) s_log_used++;

    log_trim_to_limit();
    if (s_pages[APP_LVGL_PAGE_MONITOR] != NULL && !lv_obj_has_flag(s_pages[APP_LVGL_PAGE_MONITOR], LV_OBJ_FLAG_HIDDEN)) {
        refresh_log_view(true);
    }
}

/* 清空实时监测界面数据，回到等待采样状态。 */
static void apply_ui_reset(void)
{
    init_series_none();
    update_chart_values(&s_anxiety_chart, s_anxiety_values, APP_TREND_POINTS);
    update_chart_values(&s_pr_chart, s_pr_values, APP_TREND_POINTS);
    update_chart_values(&s_ppi_chart, s_ppi_values, APP_PPI_POINTS);

    lv_label_set_text(s_window_id_label, "0");
    lv_label_set_text(s_latest_anxiety_label, "--");
    lv_label_set_text(s_latest_level_label, "--");
    lv_label_set_text(s_latest_pr_label, "-- bpm");
    lv_label_set_text(s_latest_ppi_label, "-- ms");
    lv_label_set_text(s_ppi_count_label, "--");
    lv_label_set_text(s_score_label, "--");
    lv_label_set_text(s_score_level_label, "--");
   reset_score_gauge();

    memset(s_log_history, 0, sizeof(s_log_history));
    s_log_text[0] = '\0';
    s_log_write = 0;
    s_log_used = 0;
    s_log_count = 0;
    refresh_log_view(true);
    app_ui_statics_reset();

    if (s_ai_text_label != NULL) {
    lv_label_set_text(s_ai_text_label, "等待30秒窗口结果...");
    }
}

/* 从当前帧 PPI 序列尾部回溯复制约 10 秒 PPI 数据。 */
static void fill_ppi_chart_from_30s_frame(const PPI_RESULT_T *result)
{
    if (result == NULL || result->ppi_count == 0U) {
        for (uint32_t i = 0; i < APP_PPI_POINTS; i++) {
            s_ppi_values[i] = LV_CHART_POINT_NONE;
        }
        return;
    }

    uint16_t src_count = result->ppi_count;
    if (src_count > APP_AFRAME_PPI_MAXS) {
        src_count = APP_AFRAME_PPI_MAXS;
    }

    if (src_count == 1U) {
        int32_t v = clamp_i32((int32_t)result->ppi_ms_aframe[0], 0, 1500);
        for (uint32_t i = 0; i < APP_PPI_POINTS; i++) {
            s_ppi_values[i] = v;
        }
        return;
    }

    for (uint32_t i = 0; i < APP_PPI_POINTS; i++) {
        uint32_t src_idx = (uint32_t)(((uint64_t)i * (uint64_t)(src_count - 1U)) /
                                      (uint64_t)(APP_PPI_POINTS - 1U));

        s_ppi_values[i] = clamp_i32((int32_t)result->ppi_ms_aframe[src_idx], 0, 1500);
    }
}

/* 把一帧 PPI 结果应用到实时信息和 PPI 曲线。 */
static void apply_ppi_result(uint32_t win_id, const PPI_RESULT_T *result)
{
    lv_label_set_text_fmt(s_window_id_label, "%" PRIu32, win_id);
    lv_label_set_text_fmt(s_latest_pr_label, "%u bpm", result->mean_pr);
    set_label_float_fixed(s_latest_ppi_label, result->mean_ppi, 1, " ms");
    lv_label_set_text_fmt(s_ppi_count_label, "%u", result->ppi_count);

    fill_ppi_chart_from_30s_frame(result);
    s_ppi_chart_dirty = true;
    app_ui_history_add_ppi(win_id, result);
    app_ui_statics_add_ppi(win_id, result);
}

/* 格式化并更新单个特征参数数值。 */
static void set_feature_value(uint32_t idx, float value, uint8_t decimals)
{
    if (idx >= APP_FEATURE_COUNT) {
        return;
    }
    set_label_float_fixed(s_features[idx].value, value, decimals, "");
}
static void apply_ai_analysis(uint32_t win_id, const char *text)
{
    if (s_ai_text_label == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    lv_label_set_text_fmt(s_ai_text_label,
                          "窗口 #%" PRIu32 "\n%s",
                          win_id,
                          text);
}
/* 把焦虑指数和特征结果应用到数值卡片、趋势图和日志。 */
static void apply_anxiety_result(const ANXIETY_MSG_T *msg)
{
    const PPI_Features_T *f = &msg->features;

    if (msg->valid == 0U) {
        lv_label_set_text(s_latest_anxiety_label, "--");
        lv_label_set_text(s_latest_level_label, "--");
        lv_label_set_text(s_score_label, "--");
        lv_label_set_text(s_score_level_label, "--");
        add_log_line("焦虑指数无效");
        reset_score_gauge();
        return;
    }
    app_ui_history_add_anxiety(msg);
    app_ui_statics_add_anxiety(msg);

    float score = msg->anxiety_idx;
    const char *level = anxiety_level_text(score);
    lv_color_t level_color = anxiety_level_color(score);

    set_label_float_fixed(s_latest_anxiety_label, score, 1, "");
    lv_label_set_text(s_latest_level_label, level);
    lv_obj_set_style_text_color(s_latest_level_label, level_color, 0);

    set_label_float_fixed(s_score_label, score, 1, "");
    lv_label_set_text(s_score_level_label, level);
    update_score_gauge(score);
   

    lv_label_set_text_fmt(s_features[0].value, "%u", f->PR);
    set_feature_value(1, f->RMSSD, 0);
    set_feature_value(2, f->SD2, 0);
    set_feature_value(3, f->HF, 2);
    set_feature_value(4, f->LF, 2);
    set_feature_value(5, f->LF_HF, 2);
    set_feature_value(6, f->WE, 2);
    set_feature_value(7, f->DFA, 2);
    set_feature_value(8, f->VAI, 2);

    shift_append_i32(s_anxiety_values, APP_TREND_POINTS, clamp_i32((int32_t)lroundf(score), 0, 100));
    shift_append_i32(s_pr_values, APP_TREND_POINTS, clamp_i32((int32_t)f->PR, 40, 140));
    s_anxiety_chart_dirty = true;
    s_pr_chart_dirty = true;

    char score_text[16];
    char line[72];
    format_float_fixed(score_text, sizeof(score_text), score, 1, "");
    snprintf(line, sizeof(line), "窗口 #%" PRIu32 " 焦虑指数 %s", msg->WIN_ID, score_text);
    add_log_line(line);
}

/*自动息屏*/
static void apply_auto_sleep(void)
{
    if (s_display == NULL) return;

    uint16_t sec = s_display_settings.sleep_sec;
    if (sec == 0U) return;

    uint32_t inactive_ms = lv_display_get_inactive_time(s_display);

    if (!s_backlight_dimmed && inactive_ms >= ((uint32_t)sec * 1000U)) {
        apply_lcd_backlight_percent(0);
        s_backlight_dimmed = true;
    } else if (s_backlight_dimmed && inactive_ms < 1000U) {
        apply_lcd_backlight_percent(s_display_settings.brightness_pct);
        s_backlight_dimmed = false;
    }
}


/* LVGL 定时器回调，从线程安全缓存取数据并刷新界面。 */
static void ui_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    app_data_t copy;
    memset(&copy, 0, sizeof(copy));

    if (s_data_mutex == NULL || xSemaphoreTake(s_data_mutex, 0) != pdTRUE) {
        update_time_label();
        return;
    }

    copy = s_data;
    s_data.reset_requested = false;
    s_data.ppi_dirty = false;
    s_data.anxiety_dirty = false;
    s_data.ai_dirty = false;
    xSemaphoreGive(s_data_mutex);

    if (copy.reset_requested) {
        apply_ui_reset();
    }
    if (copy.ppi_dirty) {
        apply_ppi_result(copy.win_id, &copy.ppi);
    }
    if (copy.anxiety_dirty) {
        apply_anxiety_result(&copy.anxiety);
    }
    if (copy.ai_dirty) {
    apply_ai_analysis(copy.ai_win_id, copy.ai_text);
    }

    uint32_t elapsed_sec = copy.running ? elapsed_sec_from_tick(copy.start_tick) : copy.elapsed_sec;
    update_running_view(copy.running, elapsed_sec);
    update_time_label();

    apply_auto_sleep();
}

/* 初始化主题并创建所有页面和刷新定时器。 */
static void build_ui(lv_display_t *display)
{
   const app_lvgl_theme_palette_t *p = current_theme_palette();
    lv_theme_default_init(display, c(p->accent), c(p->danger),
                      s_display_settings.theme_mode != APP_LVGL_THEME_LIGHT,
                      ui_font());

    s_root = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(s_root);
    lv_obj_add_style(s_root, &s_style_bg, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    build_header();
    init_ai_font();
    build_monitor_page();
    //build_placeholder_page(APP_LVGL_PAGE_HISTORY, "历史记录");
    s_pages[APP_LVGL_PAGE_HISTORY] = app_ui_history_create(s_root);
    //build_placeholder_page(APP_LVGL_PAGE_STATS, "数据统计");
    s_pages[APP_LVGL_PAGE_STATS] = app_ui_statics_create(s_root);
    //build_placeholder_page(APP_LVGL_PAGE_SETTINGS, "系统设置");
    s_pages[APP_LVGL_PAGE_SETTINGS] = app_ui_config_create(s_root);
    //build_placeholder_page(APP_LVGL_PAGE_ABOUT, "关于");
    s_pages[APP_LVGL_PAGE_ABOUT] = app_ui_about_create(s_root);
    build_bottom_nav();

    app_lvgl_show_page(APP_LVGL_PAGE_MONITOR);
    apply_ui_reset();
    lv_timer_create(ui_refresh_timer_cb, 200, NULL);
    lv_timer_create(realtime_chart_flush_timer_cb, 80, NULL);
    /*一帧数据来了以后，不会同一瞬间刷新 3 条曲线，而是拆成几次刷新。*/
}

/* 初始化 MIPI-DSI LCD、LVGL 显示对象、绘图缓冲和刷屏回调。 */
static esp_err_t create_display(lv_display_t **out_display)
{
    enable_dsi_phy_power();
    init_lcd_backlight();
    set_lcd_backlight(APP_LCD_BK_LIGHT_OFF_LEVEL);

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = APP_MIPI_DSI_LANE_NUM,
        .lane_bit_rate_mbps = APP_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), TAG, "new dsi bus");

    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io),
                        TAG, "new panel io dbi");

    esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = APP_MIPI_DSI_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .video_timing = {
            .h_size = APP_LCD_H_RES,
            .v_size = APP_LCD_V_RES,
            .hsync_back_porch = APP_MIPI_DSI_LCD_HBP,
            .hsync_pulse_width = APP_MIPI_DSI_LCD_HSYNC,
            .hsync_front_porch = APP_MIPI_DSI_LCD_HFP,
            .vsync_back_porch = APP_MIPI_DSI_LCD_VBP,
            .vsync_pulse_width = APP_MIPI_DSI_LCD_VSYNC,
            .vsync_front_porch = APP_MIPI_DSI_LCD_VFP,
        },
#if APP_LVGL_USE_DMA2D_COPY_FRAME
        .flags.use_dma2d = true,
#endif
    };

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = APP_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(mipi_dbi_io, &lcd_dev_config, &mipi_dpi_panel),
                        TAG, "new ek79007 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(mipi_dpi_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(mipi_dpi_panel), TAG, "panel init");
    set_lcd_backlight(APP_LCD_BK_LIGHT_ON_LEVEL);

    lv_init();

    lv_display_t *display = lv_display_create(APP_LCD_H_RES, APP_LCD_V_RES);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_NO_MEM, TAG, "lv display create");
    lv_display_set_user_data(display, mipi_dpi_panel);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);

    void *buf1 = NULL;
    void *buf2 = NULL;
    size_t alignment = 1;
#if APP_LVGL_USE_DMA2D_COPY_FRAME
    if (esp_flash_encryption_enabled()) {
        alignment = SOC_GDMA_EXT_MEM_ENC_ALIGNMENT;
    }
#endif
    size_t draw_buffer_sz = APP_LCD_H_RES * APP_LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    buf1 = heap_caps_aligned_calloc(alignment, 1, draw_buffer_sz, MALLOC_CAP_SPIRAM);
    buf2 = heap_caps_aligned_calloc(alignment, 1, draw_buffer_sz, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf1 != NULL && buf2 != NULL, ESP_ERR_NO_MEM, TAG, "lv draw buffers");

    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

#if APP_LVGL_USE_DMA2D_COPY_FRAME
    if (esp_flash_encryption_enabled()) {
        lv_display_add_event_cb(display, rounder_flush_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
#endif

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(mipi_dpi_panel, &cbs, display),
                        TAG, "register dpi callback");

    const esp_timer_create_args_t tick_timer_args = {
        .callback = increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create lvgl tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, APP_LVGL_TICK_PERIOD_MS * 1000U),
                        TAG, "start lvgl tick timer");

    *out_display = display;
    return ESP_OK;
}

/* 启动 LVGL 显示、触摸、任务和实时监测界面。 */
esp_err_t app_lvgl_start(const app_lvgl_callbacks_t *callbacks)
{
    if (s_display != NULL) {
        return ESP_OK;
    }

    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    } else {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
    }

    s_data_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_data_mutex != NULL, ESP_ERR_NO_MEM, TAG, "data mutex");
    init_series_none();

    ESP_RETURN_ON_ERROR(create_display(&s_display), TAG, "create display");
    esp_err_t touch_ret = app_lvgl_register_touch(s_display);
if (touch_ret != ESP_OK) {
    ESP_LOGW(TAG, "touch init failed: %s, continue without touch", esp_err_to_name(touch_ret));
}

    init_styles();

    BaseType_t ok = xTaskCreate(lvgl_task, "LVGL", APP_LVGL_TASK_STACK_SIZE,
                                NULL, APP_LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "lvgl task");

    _lock_acquire(&s_lvgl_lock);
    build_ui(s_display);
    _lock_release(&s_lvgl_lock);

    ESP_LOGI(TAG, "LVGL UI ready, free heap=%" PRIu32 ", internal=%" PRIu32 ", psram=%" PRIu32,
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

/* 从外部同步采样运行/停止状态到 UI 缓存。 */
void app_lvgl_set_sampling_running(bool running)
{
    if (s_data_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (running) {
            s_data.start_tick = xTaskGetTickCount();
            s_data.elapsed_sec = 0;
        } else if (s_data.running) {
            s_data.elapsed_sec = elapsed_sec_from_tick(s_data.start_tick);
        }

        s_data.running = running;
        xSemaphoreGive(s_data_mutex);
    }
}

/* 请求 UI 清空曲线和实时数据，通常在开始新采样时调用。 */
void app_lvgl_reset_monitoring(void)
{
    if (s_data_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memset(&s_data.ppi, 0, sizeof(s_data.ppi));
        memset(&s_data.anxiety, 0, sizeof(s_data.anxiety));
        s_data.win_id = 0;
        s_data.reset_requested = true;
        s_data.ppi_dirty = false;
        s_data.anxiety_dirty = false;
        s_data.start_tick = xTaskGetTickCount();
        s_data.elapsed_sec = 0;

        s_data.ai_dirty = false;
        s_data.ai_win_id = 0;
        s_data.ai_text[0] = '\0';

        xSemaphoreGive(s_data_mutex);
    }
}

/* 接收 PPI 处理任务投递的一帧 PPI 结果副本。 */
void app_lvgl_post_ppi_result(uint32_t win_id, const PPI_RESULT_T *result)
{
    if (s_data_mutex == NULL || result == NULL) {
        return;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_data.win_id = win_id;
        s_data.ppi = *result;
        s_data.ppi_dirty = true;
        xSemaphoreGive(s_data_mutex);
    }
}

/* 接收焦虑指数任务投递的一帧焦虑结果副本。 */
void app_lvgl_post_anxiety_result(const ANXIETY_MSG_T *msg)
{
    if (s_data_mutex == NULL || msg == NULL) {
        return;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_data.anxiety = *msg;
        s_data.anxiety_dirty = true;
        xSemaphoreGive(s_data_mutex);
    }
}


void app_lvgl_post_ai_analysis(uint32_t win_id, const char *text)
{
    if (s_data_mutex == NULL || text == NULL) {
        return;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_data.ai_win_id = win_id;

        strncpy(s_data.ai_text, text, sizeof(s_data.ai_text) - 1U);
        s_data.ai_text[sizeof(s_data.ai_text) - 1U] = '\0';

        s_data.ai_dirty = true;
        xSemaphoreGive(s_data_mutex);
    }
}

