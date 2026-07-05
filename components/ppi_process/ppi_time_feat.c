#include "app_config.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool ppi_time_args_ok(const uint16_t *ppi_ms, uint16_t ppi_len, const PPI_TIME_FEATURES_T *out)
{
    return (ppi_ms != NULL) && (out != NULL) && (ppi_len >= APP_PPI_FEATURE_MIN_COUNT);
}

esp_err_t ppi_time_features_calculate(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_TIME_FEATURES_T *out)
{
    float sum = 0.0f;
    float sum_square = 0.0f;
    float sum_rmssd = 0.0f;
    float sum_log = 0.0f;
    float sum_pr = 0.0f;
    uint16_t log_count = 0U;
    uint16_t pair_len;
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float mean_x;
    float mean_y;
    float sum_c00 = 0.0f;
    float sum_c01 = 0.0f;
    float sum_c11 = 0.0f;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    if (!ppi_time_args_ok(ppi_ms, ppi_len, out)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t i = 0; i < ppi_len; i++) {
        if (ppi_ms[i] == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        sum += (float)ppi_ms[i];
        sum_pr += 60000.0f / (float)ppi_ms[i];
    }

    out->MEAN = sum / (float)ppi_len;
    out->Ave_PR = sum_pr / (float)ppi_len;                                              /*计算Ave_PR参数*/

    for (uint16_t i = 0; i < ppi_len; i++) {
        float delta = (float)ppi_ms[i] - out->MEAN;
        sum_square += delta * delta;
    }

    out->SDNN = sqrtf(sum_square / (float)(ppi_len - 1U));
    out->HRD = (out->MEAN > 1.0e-6f) ? (out->SDNN / out->MEAN) : 0.0f;

    for (uint16_t i = 0; i < (uint16_t)(ppi_len - 1U); i++) {
        float delta = (float)ppi_ms[i + 1U] - (float)ppi_ms[i];
        float abs_delta = fabsf(delta);

        sum_rmssd += delta * delta;
        if (abs_delta > 0.0f) {
            sum_log += log2f(abs_delta);
            log_count++;
        }
    }

    out->RMSSD = sqrtf(sum_rmssd / (float)(ppi_len - 1U));                              /*计算RMSSD参数*/
    out->HLE = (log_count > 0U) ? (sum_log / (float)log_count) : 0.0f;

    pair_len = (uint16_t)(ppi_len - 1U);                                                /*基于几何矩的椭圆拟合计算*/
    for (uint16_t i = 0; i < pair_len; i++) {
        float pr_i = 60000.0f / (float)ppi_ms[i];
        float pr_next = 60000.0f / (float)ppi_ms[i + 1U];

        sum_x += pr_i;
        sum_y += pr_next;
    }
    /*1.计算x=PR[i],y=PR[i+1]的均值*/
    mean_x = sum_x / (float)pair_len;
    mean_y = sum_y / (float)pair_len;
    /*2.计算累积协方差矩阵*/
    for (uint16_t i = 0; i < pair_len; i++) {
        float pr_i = 60000.0f / (float)ppi_ms[i];
        float pr_next = 60000.0f / (float)ppi_ms[i + 1U];
        float dx = pr_i - mean_x;
        float dy = pr_next - mean_y;

        sum_c00 += dx * dx;
        sum_c01 += dx * dy;
        sum_c11 += dy * dy;
    }

    {
        float cov_div = (ppi_len > 2U) ? (float)(ppi_len - 2U) : 1.0f;
        float trace;
        float diff;
        float root;
        float lmd1;
        float lmd2;

        sum_c00 /= cov_div;
        sum_c01 /= cov_div;
        sum_c11 /= cov_div;
        /* 2*2 对称协方差矩阵特征值计算*/
        trace = sum_c00 + sum_c11;
        diff = sum_c00 - sum_c11;
        root = sqrtf(diff * diff + 4.0f * sum_c01 * sum_c01);
        lmd1 = 0.5f * (trace + root);
        lmd2 = 0.5f * (trace - root);

        if (lmd1 < 0.0f) {
            lmd1 = 0.0f;
        }
        if (lmd2 < 0.0f) {
            lmd2 = 0.0f;
        }

        out->SD1 = 2.0f * sqrtf(lmd1);
        out->SD2 = 2.0f * sqrtf(lmd2);                                                     /*SD2参数计算*/
        if (out->SD1 > out->SD2) {
            float temp = out->SD1;
            out->SD1 = out->SD2;
            out->SD2 = temp;
        }

        out->PAI = APP_PI_F * out->SD1 * out->SD2;
    }

    return ESP_OK;
}
