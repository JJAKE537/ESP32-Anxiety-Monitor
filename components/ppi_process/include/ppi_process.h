#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "app_message.h"
#include "stdlib.h"

/*
定义一帧PPI的数量阈值
*/
#ifndef APP_AFRAME_PPI_MAXS
#define APP_AFRAME_PPI_MAXS         64U
#endif


esp_err_t ppi_process_block(const uint16_t * adc_samples,uint16_t sample_count,PPI_RESULT_T *out);

