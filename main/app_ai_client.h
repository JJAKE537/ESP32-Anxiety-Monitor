#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "app_message.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_ai_client_start(void);
bool app_ai_client_submit(const ANXIETY_MSG_T *msg);

#ifdef __cplusplus
}
#endif
