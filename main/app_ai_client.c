#include "app_ai_client.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "app_lvgl.h"
static const char *TAG = "APP_AI";

static const char *DISCOVERY_REQUEST = "ESP32_AI_DISCOVER_V1";

static QueueHandle_t s_ai_queue;
static char s_server_url[96];

typedef struct {
    char data[APP_AI_RESPONSE_LEN];
    size_t length;
} ai_http_response_t;

static double finite_number(float value)
{
    return isfinite(value) ? (double)value : 0.0;
}

static bool discover_ai_server(char *url, size_t url_size)
{
    if (!app_rtc_wifi_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi not connected");
        return false;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "create discovery socket failed: errno=%d", errno);
        return false;
    }

    int broadcast = 1;
    struct timeval timeout = {
        .tv_sec = APP_AI_DISCOVERY_TIMEOUT_MS / 1000U,
        .tv_usec = (APP_AI_DISCOVERY_TIMEOUT_MS % 1000U) * 1000U,
    };

    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(APP_AI_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    int sent = sendto(sock,
                      DISCOVERY_REQUEST,
                      strlen(DISCOVERY_REQUEST),
                      0,
                      (struct sockaddr *)&destination,
                      sizeof(destination));
    if (sent < 0) {
        ESP_LOGE(TAG, "send discovery failed: errno=%d", errno);
        close(sock);
        return false;
    }

    char reply[64];
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    int received = recvfrom(sock,
                            reply,
                            sizeof(reply) - 1U,
                            0,
                            (struct sockaddr *)&source,
                            &source_length);
    if (received <= 0) {
        ESP_LOGW(TAG, "AI server discovery timeout");
        close(sock);
        return false;
    }

    reply[received] = '\0';

    unsigned int http_port = 0;
    if (sscanf(reply, "ESP32_AI_BRIDGE_V1:%u", &http_port) != 1 ||
        http_port == 0U || http_port > 65535U) {
        ESP_LOGW(TAG, "invalid discovery reply: %s", reply);
        close(sock);
        return false;
    }

    char server_ip[INET_ADDRSTRLEN];
    if (inet_ntoa_r(source.sin_addr, server_ip, sizeof(server_ip)) == NULL) {
        close(sock);
        return false;
    }

    snprintf(url, url_size, "http://%s:%u/analyze", server_ip, http_port);
    close(sock);

    ESP_LOGI(TAG, "AI server discovered: %s", url);
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    ai_http_response_t *response = event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA &&
        response != NULL &&
        event->data != NULL &&
        event->data_len > 0) {
        size_t available = sizeof(response->data) - 1U - response->length;
        size_t copy_length = (size_t)event->data_len;
        if (copy_length > available) {
            copy_length = available;
        }

        memcpy(response->data + response->length, event->data, copy_length);
        response->length += copy_length;
        response->data[response->length] = '\0';
    }

    return ESP_OK;
}

static char *build_request_json(const ANXIETY_MSG_T *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *features = cJSON_CreateObject();
    if (root == NULL || features == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(features);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "seq", msg->WIN_ID);
    cJSON_AddNumberToObject(root, "window_s", APP_PPI_WIN_SEC);
    cJSON_AddNumberToObject(root, "anxiety_index",
                            finite_number(msg->anxiety_idx));
    cJSON_AddNumberToObject(root, "pr_bpm", msg->features.PR);

    cJSON_AddNumberToObject(features, "rmssd",
                            finite_number(msg->features.RMSSD));
    cJSON_AddNumberToObject(features, "sd2",
                            finite_number(msg->features.SD2));
    cJSON_AddNumberToObject(features, "hf",
                            finite_number(msg->features.HF));
    cJSON_AddNumberToObject(features, "lf",
                            finite_number(msg->features.LF));
    cJSON_AddNumberToObject(features, "lf_hf",
                            finite_number(msg->features.LF_HF));
    cJSON_AddNumberToObject(features, "we",
                            finite_number(msg->features.WE));
    cJSON_AddNumberToObject(features, "dfa",
                            finite_number(msg->features.DFA));
    cJSON_AddNumberToObject(features, "vai",
                            finite_number(msg->features.VAI));
    cJSON_AddItemToObject(root, "features", features);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static bool request_ai_analysis(const ANXIETY_MSG_T *msg)
{
    char *request_json = build_request_json(msg);
    if (request_json == NULL) {
        ESP_LOGE(TAG, "create request JSON failed");
        return false;
    }

    ai_http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = s_server_url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = APP_AI_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cJSON_free(request_json);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type",
                               "application/json; charset=utf-8");
    esp_http_client_set_post_field(client,
                                   request_json,
                                   strlen(request_json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    cJSON_free(request_json);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP request failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        return false;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid HTTP response: %s", response.data);
        return false;
    }

    cJSON *analysis = cJSON_GetObjectItemCaseSensitive(root, "analysis");
    cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
    bool valid = cJSON_IsString(analysis) && analysis->valuestring != NULL;

    if (valid) {
        ESP_LOGI(TAG, "AI source: %s",
                 cJSON_IsString(source) ? source->valuestring : "unknown");
        ESP_LOGI(TAG, "AI analysis: %s", analysis->valuestring);
        app_lvgl_post_ai_analysis(msg->WIN_ID, analysis->valuestring);
    }

    cJSON_Delete(root);
    return valid;
}

static void ai_client_task(void *argument)
{
    (void)argument;
    ANXIETY_MSG_T message;

    ESP_LOGI(TAG, "AI client task ready");

    while (1) {
        if (xQueueReceive(s_ai_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (message.valid == 0U) {
            continue;
        }

        if (!app_rtc_wifi_is_connected()) {
            s_server_url[0] = '\0';
            ESP_LOGW(TAG, "skip AI request: Wi-Fi disconnected");
            continue;
        }

        if (s_server_url[0] == '\0' &&
            !discover_ai_server(s_server_url, sizeof(s_server_url))) {
            continue;
        }

        if (!request_ai_analysis(&message)) {
            s_server_url[0] = '\0';
        }
    }
}

esp_err_t app_ai_client_start(void)
{
    if (s_ai_queue != NULL) {
        return ESP_OK;
    }

    s_ai_queue = xQueueCreate(1, sizeof(ANXIETY_MSG_T));
    if (s_ai_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(ai_client_task,
                                    "ai_client",
                                    APP_AI_TASK_STACK_BYTES,
                                    NULL,
                                    APP_AI_TASK_PRI,
                                    NULL);
    if (result != pdPASS) {
        vQueueDelete(s_ai_queue);
        s_ai_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool app_ai_client_submit(const ANXIETY_MSG_T *msg)
{
    if (s_ai_queue == NULL || msg == NULL || msg->valid == 0U) {
        return false;
    }

    return xQueueOverwrite(s_ai_queue, msg) == pdTRUE;
}
