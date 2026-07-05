#include "app_message.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#ifndef APP_ANXIETY_FEATURE_NUM
#define APP_ANXIETY_FEATURE_NUM 9U
#endif

#define ANXIETY_TOPSIS_EPS      1.0e-10f
#define ANXIETY_DIR_POSITIVE    1U
#define ANXIETY_DIR_NEGATIVE    2U

static const uint8_t s_positive_idx[] = {1U, 3U, 5U, 6U, 8U};                       /*正向指标索引*/
static const uint8_t s_negative_idx[] = {2U, 4U, 7U,9U};                                   /*负向指标索引*/

static const float s_combined_weights[APP_ANXIETY_FEATURE_NUM] = {
    0.0644f, 0.0965f, 0.1773f,
    0.1791f, 0.1269f, 0.1674f,
    0.0473f, 0.0326f, 0.1085f
};

static const float s_feature_pr_matrix[2][APP_ANXIETY_FEATURE_NUM] = {
    {55.0f,  10.0f,  5.0f,   0.0f,  0.0f,  0.1f,  0.1f, 0.1f, 0.5f},
    {120.0f, 200.0f, 80.0f,  1.0f,  1.0f,  15.0f, 2.0f, 2.0f, 20.0f}
};
/*确定方向---正向/负向---direction[9]*/
static esp_err_t mark_direction(uint8_t *direction,
                                const uint8_t *idx_1based,
                                uint8_t count,
                                uint8_t dir)
{
    if ((direction == NULL) || ((idx_1based == NULL) && (count > 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0U; i < count; i++) {
        uint8_t idx = idx_1based[i];

        if ((idx < 1U) || (idx > APP_ANXIETY_FEATURE_NUM)) {
            return ESP_ERR_INVALID_ARG;
        }

        idx--;
        if ((direction[idx] != 0U) && (direction[idx] != dir)) {
            return ESP_ERR_INVALID_ARG;
        }

        direction[idx] = dir;
    }

    return ESP_OK;
}

esp_err_t anxiety_topsis_calculate(const uint8_t *positive_idx,
                                   uint8_t positive_count,
                                   const uint8_t *negative_idx,
                                   uint8_t negative_count,
                                   const float sample_features_solo[APP_ANXIETY_FEATURE_NUM],
                                   const float combined_weights[APP_ANXIETY_FEATURE_NUM],
                                   const float feature_pr_matrix[2][APP_ANXIETY_FEATURE_NUM],
                                   float *out_score)
{
    uint8_t direction[APP_ANXIETY_FEATURE_NUM] = {0U};
    float d_plus_sum = 0.0f;
    float d_minus_sum = 0.0f;
    esp_err_t ret;

    if ((sample_features_solo == NULL) ||
        (combined_weights == NULL) ||
        (feature_pr_matrix == NULL) ||
        (out_score == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_score = 0.0f;

    ret = mark_direction(direction, positive_idx, positive_count, ANXIETY_DIR_POSITIVE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = mark_direction(direction, negative_idx, negative_count, ANXIETY_DIR_NEGATIVE);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint8_t j = 0U; j < APP_ANXIETY_FEATURE_NUM; j++) {
        float x = sample_features_solo[j];
        float w = combined_weights[j];
        float f_min = feature_pr_matrix[0][j];
        float f_max = feature_pr_matrix[1][j];
        float denominator = f_max - f_min;
        float y = 0.0f;
        float a_plus = 0.0f;
        float a_minus = 0.0f;
        float diff_plus;
        float diff_minus;

        if ((direction[j] == 0U) ||
            !isfinite(x) ||
            !isfinite(w) ||
            !isfinite(f_min) ||
            !isfinite(f_max) ||
            (w < 0.0f)) {
            return ESP_ERR_INVALID_ARG;
        }
        /*数据标准化*/
        if (direction[j] == ANXIETY_DIR_POSITIVE) {
            y = (x - f_min) / denominator;
        } else if (direction[j] == ANXIETY_DIR_NEGATIVE){
            y = (f_max - x) / denominator;
        }
        /*设置正理想解和负理想解*/
        if (direction[j] == ANXIETY_DIR_POSITIVE) {
            a_plus = 1.0f;
            a_minus = 0.0f;
        } else if (direction[j] == ANXIETY_DIR_NEGATIVE){
            a_plus = 0.0f;
            a_minus = 1.0f;
        }

        diff_plus = y - a_plus;
        diff_minus = y - a_minus;

        d_plus_sum += w * diff_plus * diff_plus;
        d_minus_sum += w * diff_minus * diff_minus;
    }

    {/*d_plus---与最焦虑状态的距离         d_minus---与最不焦虑状态的距离*/
        float d_plus = sqrtf(d_plus_sum);
        float d_minus = sqrtf(d_minus_sum);
        float d_sum = d_plus + d_minus;

        if (d_sum < ANXIETY_TOPSIS_EPS) {
            *out_score = 0.0f;
        } else {
            *out_score = (d_minus / d_sum) * 100.0f;
        }
    }

    return ESP_OK;
}

esp_err_t anxiety_index_calculate(const PPI_Features_T *features, float *out_score)
{
    float sample_features_solo[APP_ANXIETY_FEATURE_NUM];

    if ((features == NULL) || (out_score == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_features_solo[0] = (float)features->PR;
    sample_features_solo[1] = features->RMSSD;
    sample_features_solo[2] = features->SD2;
    sample_features_solo[3] = features->HF;
    sample_features_solo[4] = features->LF;
    sample_features_solo[5] = features->LF_HF;
    sample_features_solo[6] = features->WE;
    sample_features_solo[7] = features->DFA;
    sample_features_solo[8] = features->VAI;

    return anxiety_topsis_calculate(s_positive_idx,
                                    sizeof(s_positive_idx) / sizeof(s_positive_idx[0]),
                                    s_negative_idx,
                                    sizeof(s_negative_idx) / sizeof(s_negative_idx[0]),
                                    sample_features_solo,
                                    s_combined_weights,
                                    s_feature_pr_matrix,
                                    out_score);
}
