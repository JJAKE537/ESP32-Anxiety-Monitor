#include "ppi_process.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
static const char *TAG = "ppi_process";

#define PPI_FIR_TAP_NUM         75U                                                     //74阶FIR带通滤波器

/* PCA-ITD参数 */
#define PPI_ITD_LEVEL_MAX         5U
#define PPI_ITD_MIN_EXTREMA       6U

#define PPI_ITD_AMP_LAMBDA        0.10f
#define PPI_ITD_MIN_DISTANCE      6U
#define PPI_ITD_DISTANCE_ETA      0.30f

#define PPI_ITD_BASE_ALPHA        0.50f

#define PPI_ITD_FREQ_LOW_HZ       0.80f
#define PPI_ITD_FREQ_HIGH_HZ      6.00f

#define PPI_ITD_RHO_LAMBDA        0.75f
#define PPI_ITD_ENERGY_LAMBDA     0.50f
#define PPI_ITD_RESIDUAL_GAMMA    0.10f

#define PPI_ITD_EPS               1.0e-8f

#define PPI_ITD_EXT_MAX  ((APP_PPI_WIN_SAMPLINGS / PPI_ITD_MIN_DISTANCE) + 4U)


#define PPI_BASELINE_SMOOTH_MS  50U
#define PPI_SMOOTH_WIN_MS       80U
#define PPI_REFINE_HALF_WIN_MS  120U

#define PPI_BPM_LOW             55.0f
#define PPI_BPM_HIGH            150.0f
#define PPI_LOW_MS              555U
#define PPI_HIGH_MS             1090U
#define PPI_EPS_F               1.0e-6f

/*====================================================================================================================*/

/*
ppi_process_block()
  -> build_filtered_signal()
     -> ADC uint16 转 float
     -> 四阶多项式基线漂移校正
     -> 去均值
     -> 0.5-5Hz 75 taps FIR
     -> PCA-ITD分解和重构
  -> 平滑
  -> 稳健归一化
  -> 正峰检测
  -> 再次生成滤波信号
  -> 峰值精修
  -> 去近峰
  -> 计算 PPI / 瞬时脉率 / 平均脉率 / 平均 PPI

*/

/*=======================================================滤波器系数配置==================================================================*/
/* 75 taps, fs = 250 Hz, bandpass = 0.5~5 Hz.     74阶FIR带通滤波系数    b[k]=b[M-k]*/
static const float s_fir1_b[PPI_FIR_TAP_NUM] = {
    -1.3620415457e-03f, -1.4021941871e-03f, -1.4839741655e-03f, -1.6026838321e-03f, -1.7493695107e-03f,
    -1.9108075134e-03f, -2.0696753550e-03f, -2.2049095939e-03f, -2.2922444108e-03f, -2.3049177703e-03f,
    -2.2145250553e-03f, -1.9919937159e-03f, -1.6086469765e-03f, -1.0373202249e-03f, -2.5349056712e-04f,
     7.6362170176e-04f,  2.0300211792e-03f,  3.5560151376e-03f,  5.3454383127e-03f,  7.3950577154e-03f,
     9.6941849006e-03f,  1.2224516412e-02f,  1.4960215534e-02f,  1.7868240257e-02f,  2.0908913843e-02f,
     2.4036725801e-02f,  2.7201342842e-02f,  3.0348801726e-02f,  3.3422849146e-02f,  3.6366388186e-02f,
     3.9122986616e-02f,  4.1638399571e-02f,  4.3862058064e-02f,  4.5748475371e-02f,  4.7258525618e-02f,
     4.8360552789e-02f,  4.9031273707e-02f,  4.9256445247e-02f,  4.9031273707e-02f,  4.8360552789e-02f,
     4.7258525618e-02f,  4.5748475371e-02f,  4.3862058064e-02f,  4.1638399571e-02f,  3.9122986616e-02f,
     3.6366388186e-02f,  3.3422849146e-02f,  3.0348801726e-02f,  2.7201342842e-02f,  2.4036725801e-02f,
     2.0908913843e-02f,  1.7868240257e-02f,  1.4960215534e-02f,  1.2224516412e-02f,  9.6941849006e-03f,
     7.3950577154e-03f,  5.3454383127e-03f,  3.5560151376e-03f,  2.0300211792e-03f,  7.6362170176e-04f,
    -2.5349056712e-04f, -1.0373202249e-03f, -1.6086469765e-03f, -1.9919937159e-03f, -2.2145250553e-03f,
    -2.3049177703e-03f, -2.2922444108e-03f, -2.2049095939e-03f, -2.0696753550e-03f, -1.9108075134e-03f,
    -1.7493695107e-03f, -1.6026838321e-03f, -1.4839741655e-03f, -1.4021941871e-03f, -1.3620415457e-03f
};
/*====================================================缓冲数组配置=======================================================================*/
static float s_buf0[APP_PPI_WIN_SAMPLINGS];                 //7500*4=30000B
static float s_work[APP_PPI_WIN_SAMPLINGS];                 //30000B
/* PCA-ITD基线及极值点缓存 */
static float s_itd_baseline[APP_PPI_WIN_SAMPLINGS];

static uint16_t s_itd_ext_idx[PPI_ITD_EXT_MAX];

/* 1表示极大值，-1表示极小值 */
static int8_t s_itd_ext_type[PPI_ITD_EXT_MAX];

static uint16_t s_peak_idx[APP_AFRAME_PPI_MAXS + 1U];       //130B
static float s_peak_amp[APP_AFRAME_PPI_MAXS + 1U];          //260B      60642B = 59.2KB  基本在.bss静态区，不在任务栈上

/*=====================================================基础小工具函数配置=================================================================*/
/*返回两个uint16_t的较小值*/
static inline uint16_t u16_min(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}
/*float绝对值*/
static inline float f_abs(float x)
{
    return (x >= 0.0f) ? x : -x;
}
/*交换两个float*/
static inline void swap_f32(float *a, float *b)
{
    float t = *a;
    *a = *b;
    *b = t;
}
/*将毫秒窗口换算成采样点数，保证是奇数---平滑窗口计算*/
static uint16_t win_samples_from_ms(uint16_t ms)
{
    uint32_t samples = ((uint32_t)APP_PPW_FRE * (uint32_t)ms + 500U) / 1000U;/*将毫秒转换为采样点数，向上取整*/

    if (samples < 3U) 
    {
        samples = 3U;
    }
    if ((samples & 1U) == 0U)                                                /*判断最低位是否为1，保证奇数*/ 
    {
        samples++;
    }
    if (samples > APP_PPI_WIN_SAMPLINGS) {
        samples = APP_PPI_WIN_SAMPLINGS;
    }

    return (uint16_t)samples;
}
/*计算float数组均值---去均值工具*/
static float mean_f32(const float *x, uint16_t n)
{
    float s = 0.0f;

    for (uint16_t i = 0; i < n; i++) {
        s += x[i];
    }

    return s / (float)n;
}
/*========================================================平滑函数配置===================================================================*/
/*移动平均-边界处自动缩短窗口---平滑处理-x原始信号数组-n数组长度-win平滑窗口长度（采样点数，通常为奇数）-y平滑结果输出*/
static void moving_average(const float *x, float *y, uint16_t n, uint16_t win)
{
    uint16_t half;
    float acc = 0.0f;
    uint16_t l = 0U;
    uint16_t r = 0U;

    if (win < 3U) {
        win = 3U;
    }
    if ((win & 1U) == 0U) {                                                    /*窗口长度是奇数，便于中心对称*/
        win++;
    }

    half = (uint16_t)(win / 2U);                                               /*窗口半宽，确定左右窗口范围*/

    for (uint16_t i = 0; i < n; i++) {
        uint16_t nl = (i > half) ? (uint16_t)(i - half) : 0U;
        uint16_t nr = u16_min((uint16_t)(i + half), (uint16_t)(n - 1U));

        while (r <= nr) {
            acc += x[r];
            r++;
        }
        while (l < nl) {
            acc -= x[l];
            l++;
        }

        y[i] = acc / (float)(nr - nl + 1U);
    }
}
/*用scratch缓冲完成原地平滑---平滑处理*/
static void moving_average_inplace(float *x, float *scratch, uint16_t n, uint16_t win)
{
    moving_average(x, scratch, n, win);
    memcpy(x, scratch, sizeof(float) * n);
}
/*=========================================================基线校正与趋势处理============================================================*/
/*
a[5][6]增广矩阵，前5列是系数矩阵，最后一列是常数b   coef[5]存储x1---x5解  true成功求解，false矩阵奇异（无解或无限解）
四阶多项式拟合：c0+c1x+c2x^2+c3x^3+c4x^4         求AX = b的解，初等变换求解      即求Ac = b;
*/
static bool solve_5x5(double a[5][6], double coef[5])
{
    for (uint8_t col = 0U; col < 5U; col++) {             
        uint8_t pivot = col;
        double pivot_abs = fabs(a[col][col]);
    
        for (uint8_t row = (uint8_t)(col + 1U); row < 5U; row++) {
            double v = fabs(a[row][col]);
            if (v > pivot_abs) {
                pivot_abs = v;
                pivot = row;
            }
        }
        /*矩阵奇异，返回*/
        if (pivot_abs < 1.0e-18) {
            return false;
        }

        if (pivot != col) {
            for (uint8_t k = col; k < 6U; k++) {
                double tmp = a[col][k];
                a[col][k] = a[pivot][k];
                a[pivot][k] = tmp;
            }
        }
        
        {
            double div = a[col][col];
            for (uint8_t k = col; k < 6U; k++) {
                a[col][k] /= div;
            }
        }
        
        for (uint8_t row = 0U; row < 5U; row++) {
            if (row == col) {                           
                continue;
            }

            double factor = a[row][col];
            if (fabs(factor) < 1.0e-24) {               
                continue;
            }

            for (uint8_t k = col; k < 6U; k++) {        
                a[row][k] -= factor * a[col][k];
            }
        }
    }

    for (uint8_t i = 0U; i < 5U; i++) {                
        coef[i] = a[i][5];
    }

    return true;
}
/*用归一化时间轴0-1建立四阶最小二乘方程---基线拟合*/
static bool polyfit4_normalized(const float *x, uint16_t n, double coef[5])
{
    float sum_t[9] = {0.0};
    float rhs[5] = {0.0};
    double aug[5][6];
    float denom = (float)(n - 1U);

    if (n < 5U) {
        return false;
    }

    for (uint16_t i = 0; i < n; i++) {
        float t = (float)i / denom;
        float p = 1.0;                                  /*初始化ti^0 = 1*/

        for (uint8_t k = 0U; k < 9U; k++) { 
            sum_t[k] += p;                              /*sum_t[k]存储的是ti^k对i的累加   ti^0---ti^8*/                              
            if (k < 5U) {
                rhs[k] += (float)x[i] * p;              /*rhs[k]存储的是x[i]*ti^k对i的累加    ti^0---ti^5*/
            }
            p *= t;
        }
    }

    for (uint8_t row = 0U; row < 5U; row++) {
        for (uint8_t col = 0U; col < 5U; col++) {
            aug[row][col] = sum_t[row + col];           /*A矩阵---5*5矩阵中每个元素aij  存储的是 sum_i ti^(i+j)*/
        }
        aug[row][5] = rhs[row];
    }

    return solve_5x5(aug, coef);
}

/*先50ms平滑，再四阶多项式拟合基线并相减，最后再50ms平滑---四阶多项式拟合去除基线漂移*/
static void baseline_poly4_correct_inplace(float *x, float *scratch, uint16_t n)
{
    double coef[5] = {0.0};
    double denom = (double)(n - 1U);
    uint16_t smooth_win = win_samples_from_ms(PPI_BASELINE_SMOOTH_MS);              /*50ms 13点平滑*/

    moving_average(x, scratch, n, smooth_win);

    if (polyfit4_normalized(scratch, n, coef)) {
        for (uint16_t i = 0; i < n; i++) {
            double t = (double)i / denom;
            double baseline = (((coef[4] * t + coef[3]) * t + coef[2]) * t + coef[1]) * t + coef[0];
            x[i] = scratch[i] - (float)baseline;
        }
    } else {
        ESP_LOGW(TAG, "baseline_poly4_failed!");
        memcpy(x, scratch, sizeof(float) * n);                                     /*找基线失败---只平滑处理*/
    }

    moving_average_inplace(x, scratch, n, smooth_win);                             /*再50ms平滑*/
}

/*去均值*/
static void remove_mean_inplace(float *x, uint16_t n)
{
    float m = mean_f32(x, n);

    for (uint16_t i = 0; i < n; i++) {
        x[i] -= m;
    }
}
/*===================================================信号滤波处理========================================================================*/
/*74阶FIR带通滤波处理---75Taps FIR系数，0.5-5HZ带通滤波处理，补偿37点群延时*/
static void fir_bandpass_stage1(const float *x, float *y, uint16_t n)
{
    uint16_t gd = (PPI_FIR_TAP_NUM - 1U) / 2U;

    for (uint16_t i = 0; i < n; i++) {
        float acc = 0.0f;
        uint16_t kmax = (i < (PPI_FIR_TAP_NUM - 1U)) ? i : (PPI_FIR_TAP_NUM - 1U);

        for (uint16_t k = 0; k <= kmax; k++) {
            acc += s_fir1_b[k] * x[i - k];
        }
        y[i] = acc;
    }
    /*0-7461 == 37-7498   7462-7499 == 7461*/
    if (n > gd) {
        for (uint16_t i = 0; i < (uint16_t)(n - gd); i++) {
            y[i] = y[i + gd];
        }

        for (uint16_t i = (uint16_t)(n - gd); i < n; i++) {
            y[i] = y[n - gd - 1U];
        }
    }
}

/*判断局部极值*/
static bool itd_is_extreme(const float *x, uint16_t i, int8_t *type)
{
    if (((x[i] >= x[i - 1U]) && (x[i] > x[i + 1U])) ||
        ((x[i] > x[i - 1U]) && (x[i] >= x[i + 1U]))) {
        *type = 1;
        return true;
    }

    if (((x[i] <= x[i - 1U]) && (x[i] < x[i + 1U])) ||
        ((x[i] < x[i - 1U]) && (x[i] <= x[i + 1U]))) {
        *type = -1;
        return true;
    }

    return false;
}

/*极值点约束*/
static uint16_t itd_find_extrema(const float *x, uint16_t n)
{
    uint16_t candidate_count = 0U;
    uint16_t last_candidate = 0U;
    float last_value = 0.0f;
    float amplitude_sum = 0.0f;
    uint32_t distance_sum = 0U;

    /* 第一次遍历：计算平均幅值变化和平均间距 */
    for (uint16_t i = 1U; i < n - 1U; i++) {
        int8_t type;

        if (!itd_is_extreme(x, i, &type)) {
            continue;
        }

        if (candidate_count > 0U) {
            amplitude_sum += fabsf(x[i] - last_value);
            distance_sum += i - last_candidate;
        }

        last_candidate = i;
        last_value = x[i];
        candidate_count++;
    }

    if (candidate_count < PPI_ITD_MIN_EXTREMA) {
        return 0U;
    }

    float mean_amp =
        amplitude_sum / (float)(candidate_count - 1U);

    float mean_distance =
        (float)distance_sum / (float)(candidate_count - 1U);

    float amp_threshold =
        PPI_ITD_AMP_LAMBDA * mean_amp;

    uint16_t distance_threshold =
        (uint16_t)lroundf(
            PPI_ITD_DISTANCE_ETA * mean_distance);

    if (distance_threshold < PPI_ITD_MIN_DISTANCE) {
        distance_threshold = PPI_ITD_MIN_DISTANCE;
    }

    uint16_t accepted = 0U;

    for (uint16_t i = 1U; i < n - 1U; i++) {
        int8_t type;

        if (!itd_is_extreme(x, i, &type)) {
            continue;
        }

        if (accepted == 0U) {
            s_itd_ext_idx[1] = i;
            s_itd_ext_type[1] = type;
            accepted = 1U;
            continue;
        }

        uint16_t last_slot = accepted;
        uint16_t last_idx = s_itd_ext_idx[last_slot];
        int8_t last_type = s_itd_ext_type[last_slot];

        /* 连续同类型极值只保留更明显的一个 */
        if (type == last_type) {
            bool stronger =
                ((type > 0) && (x[i] > x[last_idx])) ||
                ((type < 0) && (x[i] < x[last_idx]));

            if (stronger) {
                s_itd_ext_idx[last_slot] = i;
            }

            continue;
        }

        if ((i - last_idx < distance_threshold) ||
            (fabsf(x[i] - x[last_idx]) < amp_threshold)) {
            continue;
        }

        if (accepted + 2U >= PPI_ITD_EXT_MAX) {
            break;
        }

        accepted++;
        s_itd_ext_idx[accepted] = i;
        s_itd_ext_type[accepted] = type;
    }

    if (accepted < PPI_ITD_MIN_EXTREMA) {
        return 0U;
    }

    /* 将首尾点加入插值控制点 */
    s_itd_ext_idx[0] = 0U;
    s_itd_ext_type[0] = 0;

    s_itd_ext_idx[accepted + 1U] = n - 1U;
    s_itd_ext_type[accepted + 1U] = 0;

    return accepted + 2U;
}

/*计算基线控制点*/
static float itd_control_value(const float *x,uint16_t count,uint16_t j)
{
    uint16_t idx = s_itd_ext_idx[j];

    if ((j == 0U) || (j + 1U == count)) {
        return x[idx];
    }

    uint16_t left = s_itd_ext_idx[j - 1U];
    uint16_t right = s_itd_ext_idx[j + 1U];

    float left_span = (float)(idx - left);
    float right_span = (float)(right - idx);
    float total_span = left_span + right_span;

    float wl = right_span / total_span;
    float wr = left_span / total_span;

    float neighbor =
        wl * x[left] + wr * x[right];

    return PPI_ITD_BASE_ALPHA * x[idx] +
           (1.0f - PPI_ITD_BASE_ALPHA) * neighbor;
}

/*Hermite插值斜率*/
static float itd_control_slope(const float *x,uint16_t count,uint16_t j)
{
    if (j == 0U) {
        uint16_t dx =
            s_itd_ext_idx[1] - s_itd_ext_idx[0];

        return (itd_control_value(x, count, 1U) -
                itd_control_value(x, count, 0U)) /
               (float)dx;
    }

    if (j + 1U == count) {
        uint16_t dx =
            s_itd_ext_idx[j] - s_itd_ext_idx[j - 1U];

        return (itd_control_value(x, count, j) -
                itd_control_value(x, count, j - 1U)) /
               (float)dx;
    }

    uint16_t h0 =
        s_itd_ext_idx[j] - s_itd_ext_idx[j - 1U];

    uint16_t h1 =
        s_itd_ext_idx[j + 1U] - s_itd_ext_idx[j];

    float v0 = itd_control_value(x, count, j - 1U);
    float v1 = itd_control_value(x, count, j);
    float v2 = itd_control_value(x, count, j + 1U);

    float d0 = (v1 - v0) / (float)h0;
    float d1 = (v2 - v1) / (float)h1;

    if (d0 * d1 <= 0.0f) {
        return 0.0f;
    }

    float w1 = 2.0f * h1 + h0;
    float w2 = h1 + 2.0f * h0;

    return (w1 + w2) /
           ((w1 / d0) + (w2 / d1));
}
/*构造当前层基线*/
static void itd_build_baseline(const float *x,uint16_t count,float *baseline)
{
    for (uint16_t j = 0U; j + 1U < count; j++) {
        uint16_t x0 = s_itd_ext_idx[j];
        uint16_t x1 = s_itd_ext_idx[j + 1U];
        uint16_t dx = x1 - x0;

        float y0 = itd_control_value(x, count, j);
        float y1 = itd_control_value(x, count, j + 1U);

        float m0 = itd_control_slope(x, count, j);
        float m1 = itd_control_slope(x, count, j + 1U);

        for (uint16_t i = x0; i <= x1; i++) {
            float t = (float)(i - x0) / (float)dx;
            float t2 = t * t;
            float t3 = t2 * t;

            float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            float h10 = t3 - 2.0f * t2 + t;
            float h01 = -2.0f * t3 + 3.0f * t2;
            float h11 = t3 - t2;

            baseline[i] =
                h00 * y0 +
                h10 * dx * m0 +
                h01 * y1 +
                h11 * dx * m1;
        }
    }
}
/*自相关、频率和能量筛选*/
static bool itd_component_valid(const float *signal,
                                const float *baseline,
                                uint16_t n,
                                uint16_t layer,
                                float previous_energy_sum,
                                float *energy_ratio)
{
    uint16_t lag_min =
        (uint16_t)ceilf(APP_PPW_FRE /
                       PPI_ITD_FREQ_HIGH_HZ);

    uint16_t lag_max =
        (uint16_t)floorf(APP_PPW_FRE /
                        PPI_ITD_FREQ_LOW_HZ);

    float detail_energy = 0.0f;
    float signal_energy = 0.0f;

    for (uint16_t i = 0U; i < n; i++) {
        float h = signal[i] - baseline[i];

        detail_energy += h * h;
        signal_energy += signal[i] * signal[i];
    }

    *energy_ratio =
        detail_energy /
        (signal_energy + PPI_ITD_EPS);

    float rho_sum = 0.0f;
    float rho_square_sum = 0.0f;
    float rho_max = -1.0f;
    uint16_t best_lag = lag_min;

    for (uint16_t lag = lag_min;
         lag <= lag_max;
         lag++) {
        float numerator = 0.0f;
        float energy0 = 0.0f;
        float energy1 = 0.0f;

        uint16_t corr_n = n - lag;

        for (uint16_t i = 0U; i < corr_n; i++) {
            float h0 = signal[i] - baseline[i];
            float h1 =
                signal[i + lag] - baseline[i + lag];

            numerator += h0 * h1;
            energy0 += h0 * h0;
            energy1 += h1 * h1;
        }

        float rho =
            numerator /
            (sqrtf(energy0 * energy1) +
             PPI_ITD_EPS);

        rho_sum += rho;
        rho_square_sum += rho * rho;

        if (rho > rho_max) {
            rho_max = rho;
            best_lag = lag;
        }
    }

    uint16_t lag_count =
        lag_max - lag_min + 1U;

    float rho_mean =
        rho_sum / (float)lag_count;

    float rho_var =
        rho_square_sum / (float)lag_count -
        rho_mean * rho_mean;

    if (rho_var < 0.0f) {
        rho_var = 0.0f;
    }

    float rho_threshold =
        rho_mean +
        PPI_ITD_RHO_LAMBDA * sqrtf(rho_var);

    float frequency =
        (float)APP_PPW_FRE / (float)best_lag;

    float energy_threshold =
        PPI_ITD_ENERGY_LAMBDA *
        ((previous_energy_sum + *energy_ratio) /
         (float)layer);

    return
        (frequency >= PPI_ITD_FREQ_LOW_HZ) &&
        (frequency <= PPI_ITD_FREQ_HIGH_HZ) &&
        (rho_max >= rho_threshold) &&
        (*energy_ratio >= energy_threshold);
}
/*PCA-ITD总处理函数*/
static void pca_itd_reconstruct(float *signal,uint16_t n,float *output)
{
    float *current = signal;
    float *baseline = s_itd_baseline;

    float energy_sum = 0.0f;
    uint16_t selected_count = 0U;
    uint16_t decomposition_count = 0U;

    memset(output, 0, sizeof(float) * n);

    for (uint16_t layer = 1U;
         layer <= PPI_ITD_LEVEL_MAX;
         layer++) {
        uint16_t control_count =
            itd_find_extrema(current, n);

        if (control_count < PPI_ITD_MIN_EXTREMA + 2U) {
            break;
        }

        itd_build_baseline(current,
                           control_count,
                           baseline);

        float energy_ratio = 0.0f;

        bool selected =
            itd_component_valid(current,
                                baseline,
                                n,
                                layer,
                                energy_sum,
                                &energy_ratio);

        energy_sum += energy_ratio;
        decomposition_count++;

        if (selected) {
            for (uint16_t i = 0U; i < n; i++) {
                output[i] +=
                    current[i] - baseline[i];
            }

            selected_count++;
        }

        float *temp = current;
        current = baseline;
        baseline = temp;
    }

    if (decomposition_count == 0U) {
        memcpy(output,
               signal,
               sizeof(float) * n);
        return;
    }

    /*
     * 如果没有分量通过筛选，保留第一层细节，
     * 防止输出全零。baseline此时是可复用缓存。
     */
    if (selected_count == 0U) {
        uint16_t count = itd_find_extrema(signal, n);

        if (count >= PPI_ITD_MIN_EXTREMA + 2U) {
            itd_build_baseline(signal,
                               count,
                               baseline);

            for (uint16_t i = 0U; i < n; i++) {
                output[i] =
                    signal[i] - baseline[i];
            }
        }
    }

    /* 残差补偿 */
    for (uint16_t i = 0U; i < n; i++) {
        output[i] +=
            PPI_ITD_RESIDUAL_GAMMA * current[i];
    }

    remove_mean_inplace(output, n);
}


/*===============================================================归一化-统计函数配置=====================================================*/
/*快速选择算法，找数组中第kth小的元素---中位数/分位数工具*/
static float quickselect_inplace(float *a, uint16_t n, uint16_t kth)
{
    uint16_t left = 0U;
    uint16_t right = (uint16_t)(n - 1U);

    while (left < right) {
        uint16_t mid = (uint16_t)(left + ((right - left) >> 1));
        float pivot;
        uint16_t i;
        uint16_t j;

        if (a[mid] < a[left]) {
            swap_f32(&a[mid], &a[left]);
        }
        if (a[right] < a[left]) {
            swap_f32(&a[right], &a[left]);
        }
        if (a[right] < a[mid]) {
            swap_f32(&a[right], &a[mid]);
        }

        pivot = a[mid];
        i = left;
        j = right;

        while (i <= j) {
            while (a[i] < pivot) {          /*左指针向右找不小于pivot的元素*/
                i++;
            }
            while (a[j] > pivot) {          /*右指针向左找不大于pivot的元素*/
                if (j == 0U) {              /*防止 uint16_t j 在 j-- 时从 0 下溢变成 65535*/
                    break;
                }
                j--;                
            }
            if (i <= j) {
                swap_f32(&a[i], &a[j]);
                i++;
                if (j == 0U) {
                    break;
                }
                j--;
            }
        }

        if (kth <= j) {                     /*[left ... j]    kth在 pivot 左侧*/
            right = j;
        } else if (kth >= i) {              /*[i ... right]   kth在 pivot 右侧*/
            left = i;
        } else {                            /*[j+1 ... i-1]   kth在pivot 区域*/
            return a[kth];
        }
    }
    /*left = right 搜索范围只剩一个元素，该元素是目标值*/
    return a[left];
}

/*计算中位数---归一化/峰检测工具*/
static float median_from_copy(const float *x, float *scratch, uint16_t n)
{
    uint16_t mid;
    float hi;

    if ((x == NULL) || (scratch == NULL) || (n == 0U)) {
        return 0.0f;
    }

    if (x != scratch) {
        memcpy(scratch, x, sizeof(float) * n);
    }

    mid = (uint16_t)(n >> 1);
    hi = quickselect_inplace(scratch, n, mid);

    if ((n & 1U) != 0U) {                   /*n是奇数，直接返回hi*/
        return hi;
    }
                                            /*n是偶数，返回0.5*(x[mid-1]/2+hi)*/
    return 0.5f * (quickselect_inplace(scratch, n, (uint16_t)(mid - 1U)) + hi);
}

/*计算75分位数---正峰高度阈值*/
static float percentile75_from_copy(const float *x, float *scratch, uint16_t n)
{
    uint16_t idx;

    if ((x == NULL) || (scratch == NULL) || (n == 0U)) {
        return 0.0f;
    }

    if (x != scratch) {
        memcpy(scratch, x, sizeof(float) * n);
    }
                                            /*3n/4*/
    idx = (uint16_t)(((uint32_t)(n - 1U) * 3U) >> 2);
    return quickselect_inplace(scratch, n, idx);
}
/*稳健归一化---(x-median)/MAD*/
static void robust_normalize_inplace(float *x, float *scratch, uint16_t n)
{
    float med = median_from_copy(x, scratch, n);
    float mad;

    for (uint16_t i = 0; i < n; i++) {
        scratch[i] = f_abs(x[i] - med);
    }                                           /*中位数是mad*/

    mad = median_from_copy(scratch, scratch, n);
    if (mad < PPI_EPS_F) {
        mad = 1.0f;
    }

    for (uint16_t i = 0; i < n; i++) {
        x[i] = (x[i] - med) / mad;
    }
}
/*=======================================================峰值检测与PPI处理===============================================================*/
/*正峰检测---基于局部极大值-高度阈值-prominence阈值-最小峰距筛选*/
static uint16_t detect_positive_peaks_adaptive(const float *x, uint16_t n, uint16_t *locs, float *amps)
{                                                                       /*min_dist = 100   100个采样点---最小峰间距*/
    uint16_t min_dist = (uint16_t)lroundf((float)APP_PPW_FRE * 60.0f / PPI_BPM_HIGH);
    uint16_t span = u16_min(min_dist, 150U);                            /*span = 100  400ms*/
    uint16_t count = 0U;
    int32_t last_keep = -100000;
    float med = median_from_copy(x, s_work, n);
    float prom_base;
    float prom_th;
    float h_base;                                                       /*原数组75分位数*/
    float h_th;

    for (uint16_t i = 0; i < n; i++) {
        float v = x[i] - med;
        s_work[i] = (v > 0.0f) ? v : 0.0f;                              /*高于原数据中位数的正偏差*/
    }

    prom_base = median_from_copy(s_work, s_work, n);
    prom_th = 0.6f * prom_base;                                         /*prominence-峰值突出度阈值*/
    if (prom_th < 0.15f) {
        prom_th = 0.15f;
    }

    h_base = percentile75_from_copy(x, s_work, n);
    h_th = 0.25f * h_base;                                              /*高度阈值-75分位数的0.25阈值*/

    for (uint16_t i = 1U; i < (uint16_t)(n - 1U); i++) {
        float xi = x[i];

        if ((xi >= x[i - 1U]) && (xi > x[i + 1U]) && (xi >= h_th)) {
            float left_min = xi;
            float right_min = xi;
            uint16_t l0 = (i > span) ? (uint16_t)(i - span) : 0U;
            uint16_t r0 = u16_min((uint16_t)(i + span), (uint16_t)(n - 1U));

            for (uint16_t k = l0; k <= i; k++) {
                if (x[k] < left_min) {
                    left_min = x[k];
                }
            }
            for (uint16_t k = i; k <= r0; k++) {
                if (x[k] < right_min) {
                    right_min = x[k];
                }
            }

            {                                                               /*[l0,r0]区间内,x[i]-max(left_min,right_min) > prom_th*/
                float prom = xi - ((left_min > right_min) ? left_min : right_min);  
                if (prom >= prom_th) {                                      /*最小峰间距内的峰值重判*/
                    if ((count > 0U) && ((i - (uint16_t)last_keep) < min_dist)) {
                        if (xi > amps[count - 1U]) {
                            locs[count - 1U] = i;
                            amps[count - 1U] = xi;
                            last_keep = i;
                        }
                    } else if (count < (APP_AFRAME_PPI_MAXS + 1U)) {
                        locs[count] = i;
                        amps[count] = xi;
                        count++;
                        last_keep = i;
                    }
                }
            }
        }
    }

    if (count < 3U) {
        count = 0U;
        last_keep = -100000;

        for (uint16_t i = 1U; i < (uint16_t)(n - 1U); i++) {
            float xi = x[i];

            if ((xi >= x[i - 1U]) && (xi > x[i + 1U])) {
                if ((count == 0U) || ((i - (uint16_t)last_keep) >= min_dist)) {
                    if (count < (APP_AFRAME_PPI_MAXS + 1U)) {
                        locs[count] = i;
                        amps[count] = xi;
                        count++;
                        last_keep = i;
                    }
                }
            }
        }
    }

    return count;
}
/*峰索引排序并去重---去近峰前处理*/
static uint16_t sort_and_unique_u16(uint16_t *locs, uint16_t count)
{
    uint16_t wr;

    for (uint16_t i = 0; i + 1U < count; i++) {                         /*从小到大排序*/
        for (uint16_t j = (uint16_t)(i + 1U); j < count; j++) {
            if (locs[j] < locs[i]) {
                uint16_t t = locs[i];
                locs[i] = locs[j];
                locs[j] = t;
            }
        }
    }

    wr = 0U;
    for (uint16_t i = 0; i < count; i++) {                              /*去重*/
        if ((wr == 0U) || (locs[i] != locs[wr - 1U])) {
            locs[wr++] = locs[i];
        }
    }

    return wr;
}
/*每个粗峰附近±120ms内重新找最大值---峰位置精修*/
static void refine_peaks_on_filtered(const float *sig, uint16_t n, uint16_t *locs, uint16_t count)
{                                                                       /*120ms---30采样点*/
    uint16_t half_win = (uint16_t)lroundf((float)APP_PPW_FRE * ((float)PPI_REFINE_HALF_WIN_MS / 1000.0f));

    for (uint16_t k = 0; k < count; k++) {
        uint16_t c = locs[k];
        uint16_t left = (c > half_win) ? (uint16_t)(c - half_win) : 0U;
        uint16_t right = u16_min((uint16_t)(c + half_win), (uint16_t)(n - 1U));
        uint16_t best = left;
        float best_v = sig[left];

        for (uint16_t i = (uint16_t)(left + 1U); i <= right; i++) {
            if (sig[i] > best_v) {
                best_v = sig[i];
                best = i;
            }
        }

        locs[k] = best;
    }
}
/*对距离过近的峰保留幅值更大的一个*/
static uint16_t remove_too_close_peaks(const float *x, uint16_t *locs, uint16_t count, uint16_t min_dist)
{
    uint16_t wr;

    if (count == 0U) {
        return 0U;
    }

    count = sort_and_unique_u16(locs, count);
    if (count == 0U) {
        return 0U;
    }

    wr = 1U;                                                            /*当前保留下来的峰的数量，下一个写入位置*/
    for (uint16_t i = 1U; i < count; i++) {
        uint16_t cur = locs[i];                                         /*当前正在检查的峰位置*/
        uint16_t last = locs[wr - 1U];                                  /*已经保留的最后一个峰位置*/

        if ((cur - last) < min_dist) {
            if (x[cur] > x[last]) {                 
                locs[wr - 1U] = cur;                                    /*两个太近的峰只保留一个，没有增加数量*/
            }
        } else {
            locs[wr++] = cur;
        }
    }

    return wr;
}

/*=========================================================总控函数配置==================================================================*/
/*生成滤波后的脉搏波信号--ADC转换-基线校正-去均值-去趋势-FIR-PCA-ITD*/
static void build_filtered_signal(const uint16_t *adc_samples, uint16_t n, float *sig)
{
    for (uint16_t i = 0; i < n; i++) {
        sig[i] = (float)adc_samples[i];
    }
    /*四阶多项式拟合去基线---去均值*/
    baseline_poly4_correct_inplace(sig, s_work, n);
    remove_mean_inplace(sig, n);

    /* 0.5～5 Hz FIR预处理 */
    fir_bandpass_stage1(sig, s_work, n);

    /* PCA-ITD分解和重构*/
    pca_itd_reconstruct(s_work, n, sig);
}
/*根据峰位置计算PPI-瞬时脉率-平均脉率-平均PPI*/
static void fill_result_from_peaks(const uint16_t *locs, uint16_t peak_n, PPI_RESULT_T *out)
{
    uint16_t ppi_n = (uint16_t)(peak_n - 1U);
    float sum_ppi = 0.0f;
    uint32_t sum_pr = 0U;

    if (ppi_n > APP_AFRAME_PPI_MAXS) {
        ppi_n = APP_AFRAME_PPI_MAXS;
    }

    for (uint16_t i = 0; i < ppi_n; i++) {
        uint16_t ds = (uint16_t)(locs[i + 1U] - locs[i]);
        out->ppi_ms_aframe[i] = (uint16_t)lroundf(((float)ds * 1000.0f) / (float)APP_PPW_FRE);
    }

    out->valid = 1U;
    for (uint16_t i = 0; i < ppi_n; i++) {
        uint16_t ppi = out->ppi_ms_aframe[i];
        uint16_t pr = (ppi > 0U) ? (uint16_t)lroundf(60000.0f / (float)ppi) : 0U;

        if (pr > UINT8_MAX) {
            pr = UINT8_MAX;
        }

        out->insta_pr[i] = (uint8_t)pr;
        sum_pr += pr;
        sum_ppi += (float)ppi;
    }

    out->ppi_count = ppi_n;
    if (ppi_n > 0U) {
        out->mean_ppi = sum_ppi / (float)ppi_n;
        out->mean_pr = (uint8_t)lroundf((float)sum_pr / (float)ppi_n);
    }
}

/*对外唯一入口，接收一帧数据7500点ADC，输出PPI_RESULT_T，总控流程*/
esp_err_t ppi_process_block(const uint16_t *adc_samples, uint16_t sample_count, PPI_RESULT_T *out)
{
    uint16_t smooth_win;
    uint16_t peak_n;
    uint16_t min_dist;

    if ((adc_samples == NULL) || (out == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (sample_count != APP_PPI_WIN_SAMPLINGS) {
        return ESP_ERR_INVALID_SIZE;
    }

    build_filtered_signal(adc_samples, sample_count, s_buf0);                               /*脉搏波信号去噪处理*/

    smooth_win = win_samples_from_ms(PPI_SMOOTH_WIN_MS);
    moving_average_inplace(s_buf0, s_work, sample_count, smooth_win);                       /*原地-80ms平滑处理*/
    robust_normalize_inplace(s_buf0, s_work, sample_count);                                 /*原地-稳健归一化处理*/

    peak_n = detect_positive_peaks_adaptive(s_buf0, sample_count, s_peak_idx, s_peak_amp);  /*正峰峰值检测*/
    if (peak_n < 2U) {
        return ESP_ERR_NOT_FOUND;
    }

    build_filtered_signal(adc_samples, sample_count, s_buf0);                               /*未平滑归一化的滤波重构脉搏波*/
    refine_peaks_on_filtered(s_buf0, sample_count, s_peak_idx, peak_n);

    min_dist = (uint16_t)lroundf((float)APP_PPW_FRE * 60.0f / PPI_BPM_HIGH);
    peak_n = remove_too_close_peaks(s_buf0, s_peak_idx, peak_n, min_dist);
    if (peak_n < 2U) {
        return ESP_ERR_NOT_FOUND;
    }

    fill_result_from_peaks(s_peak_idx, peak_n, out);
    if (out->ppi_count == 0U) {
        out->valid = 0U;
        return ESP_ERR_NOT_FOUND;
    }

    return out->valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
