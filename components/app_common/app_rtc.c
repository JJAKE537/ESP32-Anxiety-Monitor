/*
app_rtc_start()
 -> 创建 app_rtc_task
 -> 初始化 NVS / netif / event loop / Wi-Fi STA
 -> esp_wifi_start()
 -> 通过 C5 联网
 -> 拿到 IP
 -> SNTP 校时
 -> UI 通过 app_rtc_get_time_string() 显示真实时间

 P4联网+SNTP校时代码                    使用标准 esp_wifi_* API，但实际 Wi-Fi 会通过 ESP-Hosted 转给 C5 执行

P4 应用层 / lwIP TCP/IP
    ↓ SDIO + ESP-Hosted
C5 Wi-Fi 射频联网

*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif_defaults.h"
#include "esp_wifi_default.h"

#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"


#include "app_config.h"

static const char *TAG = "APP_RTC";

#define APP_RTC_WIFI_CONNECTED_BIT BIT0
#define APP_RTC_WIFI_FAIL_BIT      BIT1

#define APP_RTC_MAXIMUM_RETRY      10
#define APP_RTC_VALID_UNIX_TIME    1704067200LL

#ifndef APP_RTC_TASK_STACK_BYTES
#define APP_RTC_TASK_STACK_BYTES   6144
#endif

#ifndef APP_RTC_TASK_PRIORITY
#define APP_RTC_TASK_PRIORITY      4
#endif

#define APP_RTC_WIFI_STARTED_BIT    BIT2

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static TaskHandle_t s_rtc_task;
static int s_retry_num;
static bool s_started;
static bool s_sntp_started;
static volatile bool s_time_valid;
static bool s_wifi_netif_started;

/* 判断当前 Unix 时间戳是否已经是可信时间。
 * ESP32 刚上电且未校时时，time(NULL) 通常是 1970 附近；
 * 这里用 2024-01-01 作为阈值，超过该时间才认为系统已经校时。
 */
static bool app_rtc_is_unix_time_valid(time_t now)
{
    return now >= (time_t)APP_RTC_VALID_UNIX_TIME;
}
/* 判断系统时间是否已经有效。
 * UI 层调用这个函数，决定显示真实时间还是显示“校时中/占位符”。
 */
bool app_rtc_time_is_valid(void)
{
    time_t now = time(NULL);

    if (app_rtc_is_unix_time_valid(now)) {
        s_time_valid = true;
        return true;
    }

    return false;
}
/* 获取格式化后的本地时间字符串。
 * 如果还没有完成 SNTP 校时，返回占位时间；
 * 如果已经校时，返回 YYYY-MM-DD HH:MM:SS 格式字符串。
 */
void app_rtc_get_time_string(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;

    if (!app_rtc_is_unix_time_valid(now) ||
        localtime_r(&now, &tm_now) == NULL) {
        snprintf(buf, len, "----/--/-- --:--:--");
        return;
    }

    snprintf(buf, len, "%04d-%02d-%02d  %02d:%02d:%02d",
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec);
}
/* 初始化 NVS。
 * Wi-Fi 驱动需要使用 NVS 保存或读取一些参数；
 * 如果 NVS 分区版本不匹配或空间不足，则擦除后重新初始化。
 */
static esp_err_t app_rtc_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }

    return ret;
}

static void app_rtc_wifi_netif_event_handler(void *arg,
                                             esp_event_base_t event_base,
                                             int32_t event_id,
                                             void *event_data)
{
    (void)arg;

    if (s_sta_netif == NULL || event_base != WIFI_EVENT) {
        return;
    }

  if (event_id == WIFI_EVENT_STA_START) {
    if (!s_wifi_netif_started) {
        uint8_t mac[6];
        wifi_netif_driver_t driver = esp_netif_get_io_driver(s_sta_netif);

        if (esp_wifi_get_if_mac(driver, mac) == ESP_OK) {
            esp_netif_set_mac(s_sta_netif, mac);
        }

        esp_wifi_internal_reg_netstack_buf_cb(
            esp_netif_netstack_buf_ref,
            esp_netif_netstack_buf_free
        );

        s_wifi_netif_started = true;
        esp_netif_action_start(s_sta_netif, event_base, event_id, event_data);
    } else {
        ESP_LOGW(TAG, "ignore duplicate wifi sta start");
    }
    return;
    }

   if (event_id == WIFI_EVENT_STA_CONNECTED) {
    wifi_netif_driver_t driver = esp_netif_get_io_driver(s_sta_netif);

    if (!esp_wifi_is_if_ready_when_started(driver)) {
        esp_err_t ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, s_sta_netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register wifi rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ESP_LOGI(TAG, "wifi sta connected event");
    esp_netif_action_connected(s_sta_netif, event_base, event_id, event_data);
    return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_netif_action_disconnected(s_sta_netif, event_base, event_id, event_data);
        return;
    }

    if (event_id == WIFI_EVENT_STA_STOP) {
        esp_netif_action_stop(s_sta_netif, event_base, event_id, event_data);
        s_wifi_netif_started = false;
        return;
    }
}


/* Wi-Fi 和 IP 事件回调函数。
 * 处理三类事件：
 * 1. WIFI_EVENT_STA_START：STA 启动后开始连接路由器；
 * 2. WIFI_EVENT_STA_DISCONNECTED：断开后自动重连，超过次数后置失败标志；
 * 3. IP_EVENT_STA_GOT_IP：拿到 IP，说明 Wi-Fi 已经连接成功。
 */
static void app_rtc_wifi_event_handler(void *arg,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "wifi sta started");
            xEventGroupSetBits(s_wifi_event_group, APP_RTC_WIFI_STARTED_BIT);
            return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;

        if (s_retry_num < APP_RTC_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "wifi disconnected, reason=%d, retry=%d/%d",
                     disc ? disc->reason : -1,
                     s_retry_num,
                     APP_RTC_MAXIMUM_RETRY);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        } else {
            ESP_LOGE(TAG, "wifi connect failed after %d retries", APP_RTC_MAXIMUM_RETRY);
            xEventGroupSetBits(s_wifi_event_group, APP_RTC_WIFI_FAIL_BIT);
        }

        xEventGroupClearBits(s_wifi_event_group, APP_RTC_WIFI_CONNECTED_BIT);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupClearBits(s_wifi_event_group, APP_RTC_WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, APP_RTC_WIFI_CONNECTED_BIT);
        return;
    }
}
/* 初始化 P4 侧 Wi-Fi STA。
 * 这里调用的是标准 esp_wifi_* API；
 * 但由于工程已经启用了 ESP-Hosted，实际 Wi-Fi 射频工作由 C5 完成，
 * P4 只负责通过 SDIO/ESP-Hosted 控制 C5 并接收网络数据。
 */
static esp_err_t app_rtc_wifi_init_sta(void)
{
    ESP_RETURN_ON_ERROR(app_rtc_nvs_init(), TAG, "nvs init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "event loop create");
    }

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "wifi event group");

    esp_netif_inherent_config_t sta_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &sta_netif_cfg);

    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "create wifi sta netif");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage ram");

    esp_event_handler_instance_t wifi_any_id;
    esp_event_handler_instance_t ip_got_ip;
    esp_event_handler_instance_t wifi_netif_events;

    ESP_RETURN_ON_ERROR(
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        app_rtc_wifi_netif_event_handler,
                                        NULL,
                                        &wifi_netif_events),
    TAG,
    "register wifi netif event");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT,
                                            ESP_EVENT_ANY_ID,
                                            app_rtc_wifi_event_handler,
                                            NULL,
                                            &wifi_any_id),
        TAG,
        "register wifi event");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT,
                                            IP_EVENT_STA_GOT_IP,
                                            app_rtc_wifi_event_handler,
                                            NULL,
                                            &ip_got_ip),
        TAG,
        "register ip event");

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    strlcpy((char *)wifi_config.sta.ssid,
            APP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            APP_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

   wifi_config.sta.threshold.authmode =
    (strlen(APP_WIFI_PASSWORD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
wifi_config.sta.pmf_cfg.capable = true;
wifi_config.sta.pmf_cfg.required = false;
wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "wifi init finished, ssid=%s", APP_WIFI_SSID);
    return ESP_OK;
}
/* SNTP 校时完成回调函数。
 * 当 SNTP 从网络时间服务器同步到真实时间后会进入这里；
 * 这里更新 s_time_valid，并打印当前真实时间。
 */
static void app_rtc_sntp_sync_cb(struct timeval *tv)
{
    (void)tv;

    s_time_valid = app_rtc_time_is_valid();

    if (s_time_valid) {
        char time_buf[32];
        app_rtc_get_time_string(time_buf, sizeof(time_buf));
        ESP_LOGI(TAG, "sntp synced: %s", time_buf);
    }
}
/* 启动 SNTP 网络校时服务。
 * 设置中国时区 CST-8，然后配置 NTP 服务器；
 * 这个函数只允许启动一次，避免重复初始化 SNTP。
 */
static esp_err_t app_rtc_sntp_start_once(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }

    setenv("TZ", APP_RTC_TIMEZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_RTC_SNTP_SERVER);
    config.sync_cb = app_rtc_sntp_sync_cb;

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret == ESP_ERR_INVALID_STATE) {
        s_sntp_started = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "sntp init");

    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp started, server=%s, timezone=%s",
             APP_RTC_SNTP_SERVER,
             APP_RTC_TIMEZONE);

    return ESP_OK;
}
/* 阻塞等待 SNTP 校时完成。
 * 每 2 秒检查一次，最多等待 20 次；
 * 成功后打印真实时间，失败则打印超时日志。
 */
static void app_rtc_wait_sntp_sync(void)
{
    const int retry_count = 20;

    for (int retry = 0; retry < retry_count; retry++) {
        esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));

        if (ret == ESP_OK && app_rtc_time_is_valid()) {
            char time_buf[32];
            app_rtc_get_time_string(time_buf, sizeof(time_buf));
            ESP_LOGI(TAG, "time ready: %s", time_buf);
            return;
        }

        ESP_LOGW(TAG, "waiting for sntp sync... %d/%d",
                 retry + 1,
                 retry_count);
    }

    ESP_LOGE(TAG, "sntp sync timeout");
}
/* RTC 后台任务。
 * 完整流程：
 * 1. 初始化 Wi-Fi STA；
 * 2. 等待 Wi-Fi 连接并拿到 IP；
 * 3. 启动 SNTP 校时；
 * 4. 后台持续检查系统时间是否有效。
 */
static void app_rtc_task(void *arg)
{
    (void)arg;

    esp_err_t ret = app_rtc_wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(ret));
        s_rtc_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    xEventGroupWaitBits(s_wifi_event_group,
                    APP_RTC_WIFI_STARTED_BIT,
                    pdFALSE,
                    pdFALSE,
                    pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "wifi started, connecting to %s", APP_WIFI_SSID);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               APP_RTC_WIFI_CONNECTED_BIT | APP_RTC_WIFI_FAIL_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(30000));

        if (bits & APP_RTC_WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "wifi connected, start sntp");
            break;
        }

        if (bits & APP_RTC_WIFI_FAIL_BIT) {
            ESP_LOGW(TAG, "wifi failed, reconnect after delay");
            xEventGroupClearBits(s_wifi_event_group, APP_RTC_WIFI_FAIL_BIT);
            s_retry_num = 0;
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
            continue;
        }

        ESP_LOGW(TAG, "wifi connect timeout, retry");
        s_retry_num = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    }

    ret = app_rtc_sntp_start_once();
    if (ret == ESP_OK) {
        app_rtc_wait_sntp_sync();
    } else {
        ESP_LOGE(TAG, "sntp start failed: %s", esp_err_to_name(ret));
    }

    while (1) {
        if (!app_rtc_time_is_valid()) {
            ESP_LOGW(TAG, "time invalid, waiting for sntp");
            if (s_sntp_started) {
                (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
/* 启动 RTC/Wi-Fi/SNTP 功能。
 * app_main() 调用这个函数后，会创建 app_rtc_task 后台任务；
 * 函数本身不阻塞 UI 和 PPI 采样流程。
 */

esp_err_t app_rtc_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_started = true;

    BaseType_t ok = xTaskCreate(app_rtc_task,
                                "app_rtc",
                                APP_RTC_TASK_STACK_BYTES,
                                NULL,
                                APP_RTC_TASK_PRIORITY,
                                &s_rtc_task);

    if (ok != pdPASS) {
        s_started = false;
        s_rtc_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
/*WIFI状态读取函数*/
bool app_rtc_wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & APP_RTC_WIFI_CONNECTED_BIT) != 0;
}

int app_rtc_wifi_get_rssi(void)
{
    if (!app_rtc_wifi_is_connected()) {
        return -127;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return -127;
    }

    return ap_info.rssi;
}

uint8_t app_rtc_wifi_get_signal_percent(void)
{
    int rssi = app_rtc_wifi_get_rssi();

    if (rssi <= -90) return 0;
    if (rssi >= -50) return 100;

    return (uint8_t)(((rssi + 90) * 100) / 40);
}