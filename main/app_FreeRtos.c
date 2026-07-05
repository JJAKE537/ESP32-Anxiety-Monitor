#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hal/adc_types.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "app_message.h"
#include "ppi_process.h"
#include "app_lvgl.h"

#include "app_ai_client.h"


static const char * TAG = "PPI_APP";
/*======================================================全局变量定义区===================================================================*/
/*环形缓冲区参数配置*/
static uint16_t s_host_array[APP_PPI_WIN_SAMPLINGS];
static uint16_t s_snapshot[APP_PPI_WIN_SAMPLINGS];
static uint16_t s_write;
static uint32_t s_total_samplings;
static uint16_t s_hop_samplings;
static uint32_t s_WIN_ID;

static volatile bool s_sampling_enabled;
static volatile bool s_snapshot_busy;

float anxiety_idx;


/*数据传递参数配置*/
static QueueHandle_t s_WIN_q;                   //窗口消息队列
static QueueHandle_t s_PPI_q;                   //ppi结果队列
static QueueHandle_t s_Features_q;              //特征结果队列
static QueueHandle_t s_Anxiety_q;               //焦虑结果队列
static TaskHandle_t Acqutasion_task;        
static EventGroupHandle_t s_Evt;                //事件标志组

/*ADC采样参数配置*/
static adc_continuous_handle_t s_adc_handle;
static uint8_t s_adc_read_buff[APP_ADC_RBYTES];         //一帧PPI数据读256个ADC结果结构体
static uint16_t s_dec_accum;
static uint8_t s_dec_count;

/*start/stop 加互斥锁---后续会有两个入口：BOOT按键任务和LVGL触摸按钮*/
static SemaphoreHandle_t s_sampling_ctrl_mutex;

void app_main(void);
static esp_err_t start_sampling(void);
static esp_err_t stop_sampling(void);
/*
程序说明：
按键 BOOT
  -> start_sampling()
  -> adc_continuous_start()
  -> ESP-IDF ADC 连续采样驱动启动硬件采样/DMA
  -> 每产生一帧 ADC 数据触发 on_conv_done 回调
  -> 回调通知 AcquisitionTask
  -> AcquisitionTask 用 adc_continuous_read() 读出一帧
  -> 4 点平均降采样：1000Hz -> 250Hz
  -> 写入 30 秒环形缓冲区
  -> 满 30 秒发窗口消息
  -> 后续每 10 秒发一次窗口消息
  -> PpiProcessTask 做滤波、峰值检测、PPI计算
  -> FeatureTask 计算9个特征参数
  -> AnxietyTask 计算焦虑指数
  -> uiTask显示焦虑指数与特征参数的变化趋势
*/
/*=======================================================ui控制区工具函数================================================================*/
static esp_err_t sampling_start_locked(void)
{
    esp_err_t ret;
    xSemaphoreTake(s_sampling_ctrl_mutex,portMAX_DELAY);
    ret =  start_sampling();
    xSemaphoreGive(s_sampling_ctrl_mutex);
    return ret;
}

static esp_err_t sampling_stop_locked(void)
{
    esp_err_t ret;
    xSemaphoreTake(s_sampling_ctrl_mutex,portMAX_DELAY);
    ret = stop_sampling();
    xSemaphoreGive(s_sampling_ctrl_mutex);
    return ret;
}

static esp_err_t ui_start_cb(void *ctx)
{
    (void)ctx;
    return sampling_start_locked();
}

static esp_err_t ui_stop_cb(void *ctx)
{
    (void)ctx;
    return sampling_stop_locked();
}

/*======================================================ADC连续采样-初始配置区===========================================================*/
/*ADC存满一帧数据后的中断回调函数*/
static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle,
                                       const adc_continuous_evt_data_t * edata,
                                       void * user_data)
{
    (void)handle;
    (void)edata;
    TaskHandle_t acq_task = (TaskHandle_t)user_data;
    BaseType_t PXpriorityHigh_yield = pdFALSE;
    vTaskNotifyGiveFromISR(acq_task,&PXpriorityHigh_yield);
    return (PXpriorityHigh_yield == pdTRUE);
}

static esp_err_t adc_continuous_Init(void)
{
    adc_continuous_handle_cfg_t handle_cfg = 
    {
        .max_store_buf_size = APP_ADC_PBYTES,                  //pool_buff 2048U
        .conv_frame_size = APP_ADC_RBYTES,                     //a frame   256U
        .flags.flush_pool = 1,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg,&s_adc_handle),TAG,"adc_handle");

    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = APP_PPI_ADC_CHANNEL,                         //ADC_CHANNEL_4
        .unit = APP_PPI_ADC_UNIT,                               //ADC_UNIT_1
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,                 //12
    };

    adc_continuous_config_t dig_cfg={
        .pattern_num  =1,
        .adc_pattern = &pattern,
        .sample_freq_hz = APP_ADC_ORIGINAL_FRE,                 //1000HZ
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,                    //1
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,                 //1 TYPE2格式打包ADC的原始输出结果
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc_handle,&dig_cfg),TAG,"adc config");

    /*
    adc_conv_done_cb() 是 ADC 一帧完成后的中断回调。它只做一件事：
        vTaskNotifyGiveFromISR(task, &PXpriorityHigh_yield);       连续采样 + 后台搬运 + 任务唤醒
    */
    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = adc_conv_done_cb,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_register_event_callbacks(s_adc_handle,&cbs,Acqutasion_task),TAG,"adc callback");
    /*Acqutasion_ppi_task的任务句柄Acqutasion_task，会在回调里当 user_data 传回去*/

    int io_num = -1;
    if(adc_continuous_channel_to_io(APP_PPI_ADC_UNIT,APP_PPI_ADC_CHANNEL,&io_num) == ESP_OK)
    {
        ESP_LOGI(TAG,"ADC_INPUT: GPIO%d ADC%d_CH%d, hw = %uHZ, PPI = %uHZ",
            io_num, APP_PPI_ADC_UNIT+1,APP_PPI_ADC_CHANNEL,
            APP_ADC_ORIGINAL_FRE,APP_PPW_FRE
        );
    }

    return ESP_OK;
}
/*======================================================提取一帧PPI数据--工具函数定义区===================================================*/
/* reset_fre_ppi_accum在 reset_sampling() 前面加函数声明 */
static void reset_sampling(void)
{
    memset(s_host_array,0,sizeof(s_host_array));
    memset(s_snapshot,0,sizeof(s_snapshot));
    memset(s_adc_read_buff,0,sizeof(s_adc_read_buff));
    s_write = 0;
    s_total_samplings=0;
    s_hop_samplings = 0;
    s_WIN_ID = 0;
    s_dec_accum = 0;
    s_dec_count = 0;
    //s_sampling_enabled = false;
    s_snapshot_busy = false;

    xQueueReset(s_WIN_q);
    xQueueReset(s_PPI_q);
    xQueueReset(s_Features_q);
    xQueueReset(s_Anxiety_q);
}

static esp_err_t start_sampling(void)
{
    if(s_sampling_enabled)
    {
        return  ESP_OK;
    }

    reset_sampling();
    ESP_RETURN_ON_ERROR(adc_continuous_flush_pool(s_adc_handle),TAG,"adc flush");
    ESP_RETURN_ON_ERROR(adc_continuous_start(s_adc_handle),TAG,"adc start");
    
    s_sampling_enabled = true;
    xEventGroupSetBits(s_Evt,APP_EVT_SYS_RUN);
    xEventGroupClearBits(s_Evt,APP_EVT_SYS_STOPPED | APP_EVT_OVERRUN);
    ESP_LOGI(TAG,"sampling started");

    return ESP_OK;
}

static esp_err_t stop_sampling(void)
{
    if(!s_sampling_enabled)
    {
        return  ESP_OK;
    }
    s_sampling_enabled  = false;
    
    esp_err_t ret = adc_continuous_stop(s_adc_handle);
    if((ret != ESP_OK)&& (ret != ESP_ERR_INVALID_STATE))/*如果ret既不是ESP_OK，也不是ESP_ERR_INVALID_STATE，才认为是错误并返回*/
    {
        return ret;
    }

    xEventGroupClearBits(s_Evt,APP_EVT_SYS_RUN);
    xEventGroupSetBits(s_Evt,APP_EVT_SYS_STOPPED);
    return ESP_OK;
}

static void copy_latest_datas_to_snap(void)
{
    uint16_t idx = s_write;/*7500数组满之后，s_write指向的是最旧的样本数据点*/
    uint16_t i = 0;
    for(i = 0;i<APP_PPI_WIN_SAMPLINGS;i++)
    {
        s_snapshot[i] = s_host_array[idx++];
        if(idx >= APP_PPI_WIN_SAMPLINGS)
        {
            idx = 0;
        }
    }
}

static void WIN_IF_ready(bool state)
{
    if(s_snapshot_busy)
    {
        xEventGroupSetBits(s_Evt,APP_EVT_OVERRUN);
        ESP_LOGW(TAG,"WIN skipped: PPI task still onws snapshot");
        return;
    }

    copy_latest_datas_to_snap();                             /*完成一帧数据赋值到待处理的数组s_snapshot中去*/
    s_snapshot_busy = true;
    PPI_WIN_INFO ppi_win_msg = {
        .WIN_ID = ++s_WIN_ID,
        .sample_count = APP_PPI_WIN_SAMPLINGS,
        .tick = xTaskGetTickCount(),
    };
    if(xQueueSend(s_WIN_q,&ppi_win_msg,0)!= pdTRUE)         /*如果窗口信息发送成功的话，ppi_task开始处理7500采样的原始数据*/
    {
        s_snapshot_busy = false;
        xEventGroupSetBits(s_Evt,APP_EVT_OVERRUN);
        ESP_LOGW(TAG,"WIN queue full");
        return;
    }

    xEventGroupSetBits(s_Evt,APP_EVT_WIN_READY);

    ESP_LOGI(TAG,"%s WIN ready: id = %" PRIu32,
             state ? "first frame" : "10s hop", ppi_win_msg.WIN_ID);
}

static void push_ppi_to_host(uint16_t  sample)
{
    s_host_array[s_write++] = sample;

    if(s_write >= APP_PPI_WIN_SAMPLINGS)
    {
        s_write = 0;
    }
    if(s_total_samplings<UINT32_MAX)
    {
        s_total_samplings++;
    }

    if(s_total_samplings < APP_PPI_WIN_SAMPLINGS)
    {
        return;
    }

    if(s_total_samplings == APP_PPI_WIN_SAMPLINGS)          //第一次采集到一帧数据
    {
        s_hop_samplings = 0;
        WIN_IF_ready(true);
        return;
    }

    if(++s_hop_samplings >= APP_PPI_HOP_SAMPLINGS)          //第二次及之后采集到一帧数据
    {
        s_hop_samplings = 0;
        WIN_IF_ready(false);
    }
}

static void Adc_read_Frame(uint32_t bytes_read)
{
    const uint32_t raw_quantity = bytes_read/SOC_ADC_DIGI_RESULT_BYTES;  /*256/4=64,即一次读满256字节时，对应的是64个原始ADC结果项*/
    uint32_t i;
    uint16_t sample;
    const adc_digi_output_data_t * raw_adc = (adc_digi_output_data_t *)s_adc_read_buff;
    for(i=0;i<raw_quantity;i++)
    {
        if((raw_adc[i].type2.unit!=APP_PPI_ADC_UNIT) || raw_adc[i].type2.channel != APP_PPI_ADC_CHANNEL)
        {
            continue;
        }

        s_dec_accum += raw_adc[i].type2.data;
        s_dec_count++;
        if(s_dec_count >=APP_DECM_FAC)
        {
            sample = s_dec_accum / APP_DECM_FAC;
            s_dec_accum = 0;
            s_dec_count = 0;
            push_ppi_to_host(sample);
        }
    }
}
/*=======================================================工具函数定义区==================================================================*/
static uint8_t float_to_u8_sat(float value)
{
    if (!isfinite(value) || (value <= 0.0f)) {
        return 0U;
    }
    if (value >= (float)UINT8_MAX) {
        return UINT8_MAX;
    }

    return (uint8_t)lroundf(value);
}

static bool send_features_msg(const PPI_Features_T *msg)
{
    if (xQueueSend(s_Features_q, msg, 0) == pdTRUE) {
        return true;
    }

    {
        PPI_Features_T dropped_msg;
        (void)xQueueReceive(s_Features_q, &dropped_msg, 0);
    }

    return (xQueueSend(s_Features_q, msg, 0) == pdTRUE);
}

static bool send_anxiety_msg(const ANXIETY_MSG_T *msg)
{
    if (xQueueSend(s_Anxiety_q, msg, 0) == pdTRUE) {
        return true;
    }

    {
        ANXIETY_MSG_T dropped_msg;
        (void)xQueueReceive(s_Anxiety_q, &dropped_msg, 0);
    }

    return (xQueueSend(s_Anxiety_q, msg, 0) == pdTRUE);
}
/*=======================================================任务函数定义区==================================================================*/
static void Acqutasion_ppi_task(void * argument)
{
    uint32_t bytes_read = 0;

    ESP_ERROR_CHECK(adc_continuous_Init());
    ESP_LOGI(TAG,"Acqutasion task ready");
    while(1)
    {
        /*任务通知信号量，等待到并取走通知值，退出前清零，一直阻塞等待ADC回调通知，直到收到通知才继续执行*/
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);

        if(!s_sampling_enabled)
        {
            continue;
        }
        /*s_adc_read_buff是ADC驱动中读出的原始帧数据*/
        while(adc_continuous_read(s_adc_handle,
                            s_adc_read_buff,
                            APP_ADC_RBYTES,
                            &bytes_read,
                            0)==ESP_OK)
        {
            Adc_read_Frame(bytes_read);
        };
    }
}

static void PPI_process_task(void * argument)
{
    (void)argument;
    PPI_WIN_INFO win_msg;
    PPI_MSG_T ppi_msg;

    ESP_LOGI(TAG,"PPI process task ready");

    while(1)
    {
        if(xQueueReceive(s_WIN_q,&win_msg,portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        memset(&ppi_msg,0,sizeof(ppi_msg));
        ppi_msg.WIN_ID = win_msg.WIN_ID;

        esp_err_t ret = ppi_process_block(s_snapshot,
                                          win_msg.sample_count,
                                          &ppi_msg.result);
        /*复制一份PPI数据给 UI， UI 不直接消费 `s_PPI_q`：*/
        app_lvgl_post_ppi_result(ppi_msg.WIN_ID, &ppi_msg.result);
                                          
        s_snapshot_busy = false;
        xEventGroupClearBits(s_Evt,APP_EVT_WIN_READY);

        if(xQueueSend(s_PPI_q,&ppi_msg,0) != pdTRUE)
        {
            PPI_MSG_T dropped_msg;
            (void)xQueueReceive(s_PPI_q,&dropped_msg,0);
            if(xQueueSend(s_PPI_q,&ppi_msg,0) != pdTRUE)
            {/*PPI处理的时间赶不上下一帧数据的发送时间，程序运行错误*/
                xEventGroupSetBits(s_Evt,APP_EVT_OVERRUN);
                ESP_LOGW(TAG,"PPI queue full: id=%" PRIu32,win_msg.WIN_ID);
            }
        }

        if((ret == ESP_OK) && (ppi_msg.result.valid != 0U))
        {
            xEventGroupClearBits(s_Evt,APP_EVT_SENSOR_FAULT);
            xEventGroupSetBits(s_Evt,APP_EVT_PPI_VALID);
            ESP_LOGI(TAG,"PPI valid: id=%" PRIu32 ", count=%u, mean_pr=%u, mean_ppi=%.1fms",
                     ppi_msg.WIN_ID,
                     ppi_msg.result.ppi_count,
                     ppi_msg.result.mean_pr,
                     (double)ppi_msg.result.mean_ppi);
        }
        else
        {
            xEventGroupSetBits(s_Evt,APP_EVT_SENSOR_FAULT);
            ESP_LOGW(TAG,"PPI invalid: id=%" PRIu32 ", err=%s, count=%u",
                     ppi_msg.WIN_ID,
                     esp_err_to_name(ret),
                     ppi_msg.result.ppi_count);
        }
    }
}

static void Features_process_task(void * argument)
{
    (void)argument;

    PPI_MSG_T ppi_msg;
    PPI_TIME_FEATURES_T time_features;
    PPI_FRE_FEATURES_T fre_features;
    PPI_NL_FEATURES_T nl_features;

    ESP_LOGI(TAG, "Features process task ready");

    while (1)
    {
        PPI_Features_T feature_msg;
        esp_err_t time_ret;
        esp_err_t fre_ret;
        esp_err_t nl_ret;

        if (xQueueReceive(s_PPI_q, &ppi_msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        memset(&feature_msg, 0, sizeof(feature_msg));
        memset(&time_features, 0, sizeof(time_features));
        memset(&fre_features, 0, sizeof(fre_features));
        memset(&nl_features, 0, sizeof(nl_features));

        feature_msg.WIN_ID = ppi_msg.WIN_ID;
        feature_msg.PR = ppi_msg.result.mean_pr;

        if ((ppi_msg.result.valid == 0U) ||
            (ppi_msg.result.ppi_count < APP_PPI_FEATURE_MIN_COUNT))
        {
            xEventGroupClearBits(s_Evt, APP_EVT_FEATURE_VALID);

            if (!send_features_msg(&feature_msg))
            {/*特征结果队列发送失败*/
                xEventGroupSetBits(s_Evt, APP_EVT_OVERRUN);
                ESP_LOGW(TAG, "Features queue full: id=%" PRIu32, ppi_msg.WIN_ID);
            }

            ESP_LOGW(TAG, "Features skipped: id=%" PRIu32 ", valid=%u, ppi_count=%u",
                     ppi_msg.WIN_ID,
                     ppi_msg.result.valid,
                     ppi_msg.result.ppi_count);
            continue;
        }

        time_ret = ppi_time_features_calculate(ppi_msg.result.ppi_ms_aframe,
                                               ppi_msg.result.ppi_count,
                                               &time_features);

        fre_ret = ppi_frequency_features_calculate(ppi_msg.result.ppi_ms_aframe,
                                                   ppi_msg.result.ppi_count,
                                                   &fre_features);

        nl_ret = ppi_nonlinear_features_calculate(ppi_msg.result.ppi_ms_aframe,
                                                  ppi_msg.result.ppi_count,
                                                  &nl_features);

        feature_msg.valid = ((time_ret == ESP_OK) &&
                             (fre_ret == ESP_OK) &&
                             (nl_ret == ESP_OK)) ? 1U : 0U;

        feature_msg.PR = (ppi_msg.result.mean_pr != 0U) ?
                          ppi_msg.result.mean_pr :
                          float_to_u8_sat(time_features.Ave_PR);

        feature_msg.RMSSD = time_features.RMSSD;
        feature_msg.SD2 = time_features.SD2;

        feature_msg.LF = fre_features.LF;
        feature_msg.HF = fre_features.HF;
        feature_msg.LF_HF = fre_features.LF_HF;

        feature_msg.WE = nl_features.WE;
        feature_msg.DFA = nl_features.DFA;
        feature_msg.VAI = nl_features.VAI;

        if (!send_features_msg(&feature_msg))
        {
            xEventGroupSetBits(s_Evt, APP_EVT_OVERRUN);
            ESP_LOGW(TAG, "Features queue full: id=%" PRIu32, ppi_msg.WIN_ID);
            continue;
        }

        if (feature_msg.valid != 0U)
        {
            xEventGroupSetBits(s_Evt, APP_EVT_FEATURE_VALID);
            /*
            ESP_LOGI(TAG,
                     "Features valid: id=%" PRIu32 ", PR=%u, RMSSD=%.2f, SD2=%.2f, LF=%.4f, HF=%.4f, LF/HF=%.3f, WE=%.3f, DFA=%.3f, VAI=%.3f",
                     feature_msg.WIN_ID,
                     feature_msg.PR,
                     (double)feature_msg.RMSSD,
                     (double)feature_msg.SD2,
                     (double)feature_msg.LF,
                     (double)feature_msg.HF,
                     (double)feature_msg.LF_HF,
                     (double)feature_msg.WE,
                     (double)feature_msg.DFA,
                     (double)feature_msg.VAI);*/
            ESP_LOGI(TAG, "Features valid: id=%" PRIu32, feature_msg.WIN_ID);
        }
        else
        {
            xEventGroupClearBits(s_Evt, APP_EVT_FEATURE_VALID);

            ESP_LOGW(TAG,
                     "Features invalid: id=%" PRIu32 ", time=%s, fre=%s, nl=%s",
                     feature_msg.WIN_ID,
                     esp_err_to_name(time_ret),
                     esp_err_to_name(fre_ret),
                     esp_err_to_name(nl_ret));
        }
    }
}


static void Anxiety_idx_task(void * argument)
{
    (void)argument;

    PPI_Features_T feature_msg;

    ESP_LOGI(TAG, "Anxiety task ready");

    while (1)
    {
        ANXIETY_MSG_T anxiety_msg;
        esp_err_t ret = ESP_ERR_INVALID_STATE;

        if (xQueueReceive(s_Features_q, &feature_msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        memset(&anxiety_msg, 0, sizeof(anxiety_msg));
        anxiety_msg.WIN_ID = feature_msg.WIN_ID;
        anxiety_msg.features = feature_msg;

        if (feature_msg.valid != 0U)
        {
            ret = anxiety_index_calculate(&feature_msg, &anxiety_msg.anxiety_idx);

            if (ret == ESP_OK)
            {

                anxiety_msg.valid = 1U;
                anxiety_idx = anxiety_msg.anxiety_idx;
                xEventGroupSetBits(s_Evt, APP_EVT_ANXIETY_VALID);
                /*anxiety_msg填好后投递给Ui---焦虑指数有效时的代码*/
                app_lvgl_post_anxiety_result(&anxiety_msg);
                if (!app_ai_client_submit(&anxiety_msg))
                 {
                    ESP_LOGW(TAG, "submit AI frame failed");
                 }
            }
            else
            {
                anxiety_msg.valid = 0U;
                anxiety_msg.anxiety_idx = 0.0f;
                anxiety_idx = 0.0f;
                xEventGroupClearBits(s_Evt, APP_EVT_ANXIETY_VALID);
            }
        }
        else
        {
            anxiety_msg.valid = 0U;
            anxiety_msg.anxiety_idx = 0.0f;
            anxiety_idx = 0.0f;
            xEventGroupClearBits(s_Evt, APP_EVT_ANXIETY_VALID);
        }

        if (!send_anxiety_msg(&anxiety_msg))
        {
            xEventGroupSetBits(s_Evt, APP_EVT_OVERRUN);
            ESP_LOGW(TAG, "Anxiety queue full: id=%" PRIu32, feature_msg.WIN_ID);
            continue;
        }

        if (anxiety_msg.valid != 0U)
        {
            ESP_LOGI(TAG, "Anxiety valid: id=%" PRIu32 ", idx=%.3f",
                     anxiety_msg.WIN_ID,
                     (double)anxiety_msg.anxiety_idx);
        }
        else
        {
            ESP_LOGW(TAG, "Anxiety invalid: id=%" PRIu32 ", feature_valid=%u, err=%s",
                     anxiety_msg.WIN_ID,
                     feature_msg.valid,
                     esp_err_to_name(ret));
        }
    }
}

static void Key_ctr_task(void *argument)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << APP_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_LOGI(TAG, "KEY_CTRTASK ready: GPIO:%d active = %d",
             APP_KEY_GPIO, APP_KEY_ACTIVE_LEVEL);

    while (1)
    {
        int level = gpio_get_level(APP_KEY_GPIO);

        if (level == APP_KEY_ACTIVE_LEVEL)
        {
            /*
             * 第一次检测到低电平后，延时 30ms 消抖
             */
            vTaskDelay(pdMS_TO_TICKS(30));

            /*
             * 30ms 后再次确认仍然是低电平，说明是真正按下
             */
            if (gpio_get_level(APP_KEY_GPIO) == APP_KEY_ACTIVE_LEVEL)
            {
                ESP_LOGI(TAG, "BOOT_KEY pressed");

                if (s_sampling_enabled)
                {
                    ESP_ERROR_CHECK(sampling_stop_locked());
                    app_lvgl_set_sampling_running(false);/*同步Ui状态*/
                    ESP_LOGI(TAG, "sampling stopped");
                }
                else
                {
                    ESP_ERROR_CHECK(sampling_start_locked());
                    app_lvgl_reset_monitoring();
                    app_lvgl_set_sampling_running(true);/*同步Ui状态*/
                }

                /*
                 * 等待按键松开，避免一次长按触发多次
                 */
                while (gpio_get_level(APP_KEY_GPIO) == APP_KEY_ACTIVE_LEVEL)
                {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }

                /*
                 * 松开后再延时一点，做释放消抖
                 */
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }

    
/*
         int level = gpio_get_level(APP_KEY_GPIO);
        ESP_LOGI(TAG, "GPIO35 level = %d", level);
         vTaskDelay(pdMS_TO_TICKS(500));
         */

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
    

/*===========================================================主函数定义区================================================================*/
void app_main(void)
{
   ESP_LOGI(TAG,"ESP32_P4/C5 PPI FreeRTOS pipeline");
   ESP_LOGI(TAG,"free heap = %" PRIu32 ",internal=%u,psram=%u",
            esp_get_free_heap_size(),                               /*当前系统剩余总堆内存*/
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),           /*内部RAM剩余量*/
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));            /*PSRAM剩余量*/

    /*启动RTC/Wi-Fi任务*/
    ESP_ERROR_CHECK(app_rtc_start());
    ESP_ERROR_CHECK(app_ai_client_start());

    s_WIN_q = xQueueCreate(APP_QUEUE_DEPTH_SMALL,sizeof(PPI_WIN_INFO));
    s_PPI_q = xQueueCreate(APP_QUEUE_DEPTH_SMALL,sizeof(PPI_MSG_T));
    s_Features_q = xQueueCreate(APP_QUEUE_DEPTH_SMALL,sizeof(PPI_Features_T));
    s_Anxiety_q = xQueueCreate(APP_QUEUE_DEPTH_SMALL,sizeof(ANXIETY_MSG_T));

    /*ui初始化*/
    s_sampling_ctrl_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_sampling_ctrl_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    static const app_lvgl_callbacks_t ui_callbacks = {
    .start = ui_start_cb,
    .stop = ui_stop_cb,
    .ctx = NULL,
    };

    s_Evt = xEventGroupCreate();                                    /*系统状态事件*/
    ESP_ERROR_CHECK((s_WIN_q && s_PPI_q && s_Features_q && s_Anxiety_q && s_Evt)? ESP_OK:ESP_ERR_NO_MEM);
    /*ESP_OK 继续运行             ESP_ERR_NO_MEM  打印错误并abort*/
    xEventGroupSetBits(s_Evt,APP_EVT_SYS_STOPPED);

    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(Acqutasion_ppi_task,"Acqtask",APP_ACQUTASION_TASK_BYTES,NULL,APP_ACQUTASION_TASK_PRI,&Acqutasion_task);
    ok &= xTaskCreate(PPI_process_task,"PPIPTASK",APP_PPI_PPTASK_BYTES,NULL,APP_PPI_PTASK_PRI,NULL);
    ok &= xTaskCreate(Features_process_task,"FeaTask",APP_FEATURES_PTASK_BYTES,NULL,APP_FEATURES_PTASK_PRI,NULL);
    ok &= xTaskCreate(Anxiety_idx_task,"Andxtask",APP_ANXIETY_PTASK_BYTES,NULL,APP_ANXIETY_PTASK_PRI,NULL);
    ok &= xTaskCreate(Key_ctr_task,"keytask",APP_KEYCTR_TASK_BYTES,NULL,APP_KEYCTR_TASK_PRI,NULL);
    ESP_ERROR_CHECK((ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM);

    /*启动ui*/
    ESP_ERROR_CHECK(app_lvgl_start(&ui_callbacks));

}

