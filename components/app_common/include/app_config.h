#pragma once
#include <stdint.h>

#include "driver/gpio.h"      
#include "hal/adc_types.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>


/*
adc1_channel4       GPIO20
BOOT_KEY            GPIO35
KEY_DOWN_VALUE      0
*/
#define APP_PPI_ADC_UNIT        ADC_UNIT_1
#define APP_PPI_ADC_CHANNEL     ADC_CHANNEL_4
#define APP_PPI_ADC_GPIO        GPIO_NUM_20
#define APP_KEY_GPIO            GPIO_NUM_35
#define APP_KEY_ACTIVE_LEVEL    0

/*
ADC-DMA采样率1000HZ     脉搏波采样率250HZ   降采样系数4    
*/
#define APP_ADC_ORIGINAL_FRE    1000U
#define APP_PPW_FRE             250U
#define APP_DECM_FAC            (APP_ADC_ORIGINAL_FRE/APP_PPW_FRE)
/*
一帧PPI数据时间30秒【7500数据点】，步长10秒【2500数据点】
*/
#define APP_PPI_WIN_SEC         30U
#define APP_PPI_HOP_SEC         10U
#define APP_PPI_WIN_SAMPLINGS   (APP_PPI_WIN_SEC*APP_PPW_FRE)
#define APP_PPI_HOP_SAMPLINGS   (APP_PPI_HOP_SEC*APP_PPW_FRE)
#define APP_AFRAME_PPI_MAXS     64U

#define APP_FRE_ACC_FRAME_COUNT         4U
#define APP_FRE_PPI_MAXS                (APP_AFRAME_PPI_MAXS * APP_FRE_ACC_FRAME_COUNT)             /*用于一次分析频域功率特征参数的PPI数组长度最大是256*/



/*
一次从ADC驱动读出来的字节数
*/
#define APP_ADC_RBYTES          256U
#define APP_ADC_PBYTES          2048U
/*
任务栈大小
*/
#define APP_ACQUTASION_TASK_BYTES       4096U
#define APP_PPI_PPTASK_BYTES            8192U
#define APP_FEATURES_PTASK_BYTES        8192U
#define APP_ANXIETY_PTASK_BYTES         3072U
#define APP_KEYCTR_TASK_BYTES           2048U
/*
任务优先级---采集任务优先级最高，LVGL 6高于后处理任务。这样每帧计算时，UI 不会被 PPI/特征/焦虑任务长时间压住。
*/
#define APP_ACQUTASION_TASK_PRI         8U
#define APP_PPI_PTASK_PRI               5U
#define APP_FEATURES_PTASK_PRI          4U
#define APP_ANXIETY_PTASK_PRI           4U
#define APP_KEYCTR_TASK_PRI             3U

/*=======================================================================================================================================*/

#define APP_PI_F                        3.14159265358979323846f
#define APP_PPI_FEATURE_MIN_COUNT       4U

#define APP_FRE_FS_HZ                   (4.0f)
#define APP_FRE_DT                      (1.0f / APP_FRE_FS_HZ)

/* 30s 短时 PRV 小波频带能量，只分析 LF/HF 范围 */
#define APP_FRE_F_MIN_HZ                (0.04f)
#define APP_FRE_F_MAX_HZ                (0.5f)
#define APP_FRE_NUM_SCALES              96U

#define APP_FRE_VLF_LOW_HZ              (0.003f)
#define APP_FRE_VLF_HIGH_HZ             (0.04f)
#define APP_FRE_LF_LOW_HZ               (0.04f)
#define APP_FRE_LF_HIGH_HZ              (0.15f)
#define APP_FRE_HF_LOW_HZ               (0.15f)
#define APP_FRE_HF_HIGH_HZ              (0.5f)
#define APP_FRE_TP_LOW_HZ               (0.04f)
#define APP_FRE_TP_HIGH_HZ              (0.5f)

/* 短窗复 Morlet 参数。 */
#define APP_FRE_CMOR_B                  (2.0f)
#define APP_FRE_CMOR_C                  (1.0f)
#define APP_FRE_WAVELET_HALF_WIDTH      (3.0f)
#define APP_FRE_CWT_MIN_SUPPORT         (0.05f)

/* 30s * 4Hz = 120 点 */
#define APP_FRE_MAX_UNIFORM_LEN         160U






#define APP_WE_FS_HZ                    (4.0f)
#define APP_WE_DT                       (1.0f / APP_WE_FS_HZ)
#define APP_WE_LEVEL_TARGET             9U
#define APP_WE_DWT_DB_LEN               12U
#define APP_WE_MAX_UNIFORM_LEN          320U

typedef struct {
    float MEAN;         //
    float SDNN;         //
    float RMSSD;
    float HRD;          //
    float HLE;          //
    float PAI;
    float SD1;
    float SD2;
    float Ave_PR;       //
} PPI_TIME_FEATURES_T;

typedef struct {
    float VLF;
    float LF;
    float HF;
    float LF_HF;
    float TP;
    float K;
    float time_domain_power;
    float freq_domain_integral;
    uint16_t uniform_len;                           /*重采样后的等间隔序列长度*/
    uint16_t num_scales;
} PPI_FRE_FEATURES_T;

typedef struct {
    float VAI;
    float DFA;
    float DFA_R2;
    float WE;
} PPI_NL_FEATURES_T;

esp_err_t ppi_time_features_calculate(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_TIME_FEATURES_T *out);
esp_err_t ppi_frequency_features_calculate(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_FRE_FEATURES_T *out);
esp_err_t ppi_nonlinear_features_calculate(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_NL_FEATURES_T *out);

/*================================================WI-Fi/RTC配置=======================================================================================*/

#define APP_WIFI_SSID          "JjakeBlen"
#define APP_WIFI_PASSWORD      "2644124566"
#define APP_RTC_SNTP_SERVER    "ntp.aliyun.com"
#define APP_RTC_TIMEZONE       "CST-8"

esp_err_t app_rtc_start(void);
bool app_rtc_time_is_valid(void);
void app_rtc_get_time_string(char *buf, size_t len);

bool app_rtc_wifi_is_connected(void);
int app_rtc_wifi_get_rssi(void);
uint8_t app_rtc_wifi_get_signal_percent(void);

/*================================================WI-Fi/AI服务配置=======================================================================================*/
#define APP_AI_DISCOVERY_PORT        8001U
#define APP_AI_DISCOVERY_TIMEOUT_MS  3000U
#define APP_AI_HTTP_TIMEOUT_MS       8000U
#define APP_AI_RESPONSE_LEN          512U
#define APP_AI_TASK_STACK_BYTES      8192U
#define APP_AI_TASK_PRI              3U



