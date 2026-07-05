#include "app_config.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/*
输入PPI数据
    ->计算 VAI 指数    
    ->DFA 分析 
        去均值累积  
        多尺度盒子计算 RMS 
        线性拟合 log-log  
    ->小波熵 WE 计算
        样条插值均匀化 
        多级 DWT 分解
        能量归一化计算熵值
*/
static const float s_we_lo_d[APP_WE_DWT_DB_LEN] = {
    -0.0010773010849956f, 0.0047772575110107f, 0.0005538422009938f, -0.0315820393180312f,
     0.0275228655300163f, 0.0975016055870794f, -0.1297668675670956f, -0.2262646939651691f,
     0.3152503517092432f, 0.7511339080215775f, 0.4946238903984f, 0.1115407433500802f,
};

static const float s_we_hi_d[APP_WE_DWT_DB_LEN] = {
    -0.1115407433500802f, 0.4946238903984f, -0.7511339080215775f, 0.3152503517092432f,
     0.2262646939651691f, -0.1297668675670956f, -0.0975016055870794f, 0.0275228655300163f,
     0.0315820393180312f, 0.0005538422009938f, -0.0047772575110107f, -0.0010773010849956f,
};

static float s_dfa_ppi[APP_AFRAME_PPI_MAXS];
static float s_dfa_y_int[APP_AFRAME_PPI_MAXS];

static float s_we_ppi_sec[APP_AFRAME_PPI_MAXS];
static float s_we_t_accum[APP_AFRAME_PPI_MAXS];
static float s_we_y2[APP_AFRAME_PPI_MAXS];
static float s_we_sig_u[APP_WE_MAX_UNIFORM_LEN];
static float s_we_buf_a[APP_WE_MAX_UNIFORM_LEN];
static float s_we_buf_b[APP_WE_MAX_UNIFORM_LEN];
static float s_we_det[APP_WE_MAX_UNIFORM_LEN];

static float s_spline_h[APP_AFRAME_PPI_MAXS];
static float s_spline_a2[APP_AFRAME_PPI_MAXS];
static float s_spline_a1[APP_AFRAME_PPI_MAXS];
static float s_spline_a0[APP_AFRAME_PPI_MAXS];
static float s_spline_b1[APP_AFRAME_PPI_MAXS];
static float s_spline_b2[APP_AFRAME_PPI_MAXS];
static float s_spline_rhs[APP_AFRAME_PPI_MAXS];

static float nl_mean(const float *x, uint32_t n)
{
    float sum = 0.0f;

    if ((x == NULL) || (n == 0U)) {
        return 0.0f;
    }

    for (uint32_t i = 0U; i < n; i++) {
        sum += x[i];
    }

    return sum / (float)n;
}

static float nl_energy(const float *x, uint32_t n)
{
    float sum = 0.0f;

    for (uint32_t i = 0U; i < n; i++) {
        sum += x[i] * x[i];
    }

    return sum;
}
/*
symmetric 边界取样：把 idx 映射到 [0, N-1] 的对称反射 
-8 -7 -6 -5 -4 -3 -2 -1 ||   0 1 2 3 4   || 5 6 7 8 9 10 11 12
*/
static int32_t nl_sym_idx(int32_t idx, int32_t n)
{
    while ((idx < 0) || (idx >= n)) {
        if (idx < 0) {
            idx = -idx - 1;
        } else {
            idx = 2 * n - idx - 1;
        }
    }

    return idx;
}
/*=========================================================计算VAI指数==================================================================*/
static void calc_vai(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_NL_FEATURES_T *out)
{
    float sum = 0.0f;

    if ((ppi_ms == NULL) || (out == NULL) || (ppi_len < 2U)) {
        return;
    }

    for (uint16_t i = 0; i < (uint16_t)(ppi_len - 1U); i++) {
        float x = (float)ppi_ms[i];
        float y = (float)ppi_ms[i + 1U];
        float angle_deg;

        if (fabsf(x - 400.0f) < 1.0e-6f) {
            angle_deg = (y > 0.0f) ? 90.0f : 0.0f;
        } else {
            angle_deg = atanf((y - 400.0f) / (x - 400.0f)) * 180.0f / APP_PI_F;
        }

        if (angle_deg < 0.0f) {
            angle_deg = 0.0f;
        } else if (angle_deg > 90.0f) {
            angle_deg = 90.0f;
        }
        /* 以身份轴 45° 为基准，计算偏离程度 */
        sum += fabsf(angle_deg - 45.0f);
    }

    out->VAI = sum / (float)(ppi_len - 1U);
}
/*=========================================================计算DFA指数==================================================================*/
static void dfa_line_fit_segment(const float *seg, uint16_t s, float *a, float *b)
{
    float sx = 0.0f;
    float sy = 0.0f;
    float sxx = 0.0f;
    float sxy = 0.0f;

    for (uint16_t i = 1U; i <= s; i++) {
        float x = (float)i;
        float y = seg[i - 1U];

        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }

    {
        float denom = (float)s * sxx - sx * sx;
        if (fabsf(denom) < 1.0e-12f) {/*s太小或数值异常时兜底，趋势设为常数*/
            *a = 0.0f;
            *b = sy / (float)s;
        } else {
            *a = ((float)s * sxy - sx * sy) / denom;
            *b = (sy - (*a) * sx) / (float)s;
        }
    }
}

static void dfa_line_fit_log(const float *log_f, const float *log_s, uint16_t n, float *a, float *b)
{
    float sx = 0.0f;
    float sy = 0.0f;
    float sxx = 0.0f;
    float sxy = 0.0f;

    for (uint16_t i = 0U; i < n; i++) {
        float x = log_s[i];
        float y = log_f[i];

        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }

    {
        float denom = (float)n * sxx - sx * sx;
        if (fabsf(denom) < 1.0e-12f) {/*s太小或数值异常时兜底，趋势设为常数*/
            *a = 0.0f;
            *b = sy / (float)n;
        } else {
            *a = ((float)n * sxy - sx * sy) / denom;
            *b = (sy - (*a) * sx) / (float)n;
        }
    }
}

static esp_err_t calc_dfa_r2(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_NL_FEATURES_T *out)
{
    float sum = 0.0f;
    float mean_ppi;
    uint16_t valid_len = 0U;
    uint16_t box_size[25] = {0};
    float f_wf[25] = {0.0f};
    float log_s[25] = {0.0f};
    float log_f[25] = {0.0f};
    uint16_t box_count = 0U;
    uint16_t fit_count = 0U;

    if ((ppi_ms == NULL) || (out == NULL) || (ppi_len < 8U)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t i = 0U; i < ppi_len; i++) {
        s_dfa_ppi[i] = (float)ppi_ms[i];
        sum += s_dfa_ppi[i];
    }

    mean_ppi = sum / (float)ppi_len;
    /*ppi异常值剔除 300-2000ms*/
    for (uint16_t i = 0U; i < ppi_len; i++) {
        float v = s_dfa_ppi[i];
        if ((v >= 300.0f) && (v <= 2000.0f)) {
            s_dfa_ppi[valid_len++] = v;
        }
    }

    if (valid_len < 8U) {
        return ESP_ERR_INVALID_SIZE;
    }
    /*全局DFA计算*/
    for (uint16_t i = 0U; i < valid_len; i++) {                                             /*时间累积序列s_dfa_y_int计算*/
        float y_detrend = s_dfa_ppi[i] - mean_ppi;
        s_dfa_y_int[i] = (i == 0U) ? y_detrend : (s_dfa_y_int[i - 1U] + y_detrend);
    }

    {                                                                                       /*尺度设定*/
        const uint16_t min_box = 4U;                                                        /*beat 为单位*/
        uint16_t max_box = valid_len / min_box; 
        float log_min;
        float log_max;
        const uint16_t scale_num = 20U;                                                     /*20个均匀分布的对数空间尺度*/

        if (max_box <= min_box) {
            max_box = min_box + 1U;
        }

        log_min = log10f((float)min_box);
        log_max = log10f((float)max_box);

        for (uint16_t j = 0U; j < scale_num; j++) {
            float t = (j == 0U) ? 0.0f : ((float)j / (float)(scale_num - 1U));              /*单调递增*/
            uint16_t s = (uint16_t)powf(10.0f, log_min + t * (log_max - log_min));          /*等价于floor向下取整*/

            if ((s > 2U) && (box_count == 0U || s > box_size[box_count - 1U])) {            /*去重*/
                box_size[box_count++] = s;
            }
        }
    }
    /*
        一系列盒子尺寸的序列：遍历在每一个盒子尺寸下，对整个PPI数据进行基于该盒子尺寸（相当于一个窗口）的去除趋势，只保留波动的处理
        DFA :分析非平稳时间序列（如脉率变异性 PPI 数据、EEG 信号等）的长程相关性的方法。计算DFA过程中的波动函数F_wf
    */
    for (uint16_t j = 0U; j < box_count; j++) {
        uint16_t s = box_size[j];
        uint16_t num_boxes = valid_len / s;
        float rms_sum = 0.0f;

        if (num_boxes == 0U) {
            continue;
        }

        for (uint16_t k = 0U; k < num_boxes; k++) {
            const float *seg = &s_dfa_y_int[(uint16_t)(k * s)];                             /*0---s-1      s---2s-1  2s---3s-1*/
            float a;
            float b;

            dfa_line_fit_segment(seg, s, &a, &b);

            for (uint16_t ki = 1U; ki <= s; ki++) {
                float trend = a * (float)ki + b;
                float err = seg[ki - 1U] - trend;
                rms_sum += err * err;
            }
        }

        f_wf[j] = sqrtf(rms_sum / ((float)num_boxes * (float)s));                           /*计算标准差（RMS 波动）。这一步计算出了当前尺度s下的平均波动幅度 F(n)。*/
    }

    for (uint16_t i = 0U; i < box_count; i++) {
        if (f_wf[i] > 0.0f) {
            log_s[fit_count] = log10f((float)box_size[i]);
            log_f[fit_count] = log10f(f_wf[i]);
            fit_count++;
        }
    }

    if (fit_count < 2U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    {
        float a2;
        float b2;
        float sum_log_f = 0.0f;
        float mean_log_f;
        float diff_fit = 0.0f;
        float diff_total = 0.0f;

        dfa_line_fit_log(log_f, log_s, fit_count, &a2, &b2);
        out->DFA = a2;                                                                      /*计算得DAF指数（斜率）*/

        for (uint16_t i = 0U; i < fit_count; i++) {
            sum_log_f += log_f[i];
        }
        mean_log_f = sum_log_f / (float)fit_count;

        for (uint16_t i = 0U; i < fit_count; i++) {
            float y_fit = a2 * log_s[i] + b2;
            diff_fit += (log_f[i] - y_fit) * (log_f[i] - y_fit);
            diff_total += (log_f[i] - mean_log_f) * (log_f[i] - mean_log_f);
        }

        out->DFA_R2 = (diff_total < 1.0e-12f) ? 1.0f : (1.0f - diff_fit / diff_total);      /*计算得到DAF的线性拟合指数R2*/
    }

    return ESP_OK;
}
/*=========================================================计算WE指数==================================================================*/
/*
基于滤波器长度约束估算max_level     maxL=log2[(N/(F-1))] N信号长度  F滤波器长度  F-1有效边界影响长度
可调参数设置（重采样频率、重采样步长、分解目标层数）
db6滤波器系数导入LO_D、HI_D->12
工具函数（求均值、求能量、SPLINE插值、边界对称反射保护、1层DWT、基于滤波器长度约束估算max_level）
*/
static uint32_t we_max_level_est(uint32_t n, uint32_t filter_len)
{
    if ((filter_len <= 1U) || (n < (filter_len - 1U))) {
        return 0U;
    }

    return (uint32_t)floorf(logf((float)n / (float)(filter_len - 1U)) / logf(2.0f));
}
/*单层 DWT，对称边界*/
static void we_dwt_level_sym(const float *y, uint32_t n, const float *lo_d, const float *hi_d,
                             uint32_t filter_len, float *a, float *d, uint32_t *out_n)
{
    uint32_t m_out = (n + filter_len - 1U) >> 1;                                            /*DWT长度修正*/

    for (uint32_t j = 0U; j < m_out; j++) {
        float sa = 0.0f;
        float sd = 0.0f;
        int32_t center = (int32_t)(2U * j + 1U);                                            /*相位修正*/

        for (uint32_t k = 0U; k < filter_len; k++) {
            int32_t idx = nl_sym_idx(center - (int32_t)k, (int32_t)n);
            float v = y[(uint32_t)idx];

            sa += lo_d[k] * v;
            sd += hi_d[k] * v;
        }

        a[j] = sa;
        d[j] = sd;
    }

    if (out_n != NULL) {
        *out_n = m_out;
    }
}
/*三次样条插值*/
static int spline_prepare_notaknot(const float *x, const float *y, uint32_t n, float *y2)
{
    if (n < 4U) {
        return -1;
    }
    if (n > APP_AFRAME_PPI_MAXS) {
        return -2;
    }

    for (uint32_t i = 0U; i < n - 1U; i++) {
        s_spline_h[i] = x[i + 1U] - x[i];
        if (s_spline_h[i] <= 0.0f) {
            return -3;
        }
    }

    for (uint32_t i = 0U; i < n; i++) {
        s_spline_a2[i] = 0.0f;                                                                 /* col i-2 */
        s_spline_a1[i] = 0.0f;                                                                 /* col i-1 */
        s_spline_a0[i] = 0.0f;                                                                 /* col i   */
        s_spline_b1[i] = 0.0f;                                                                 /* col i+1 */
        s_spline_b2[i] = 0.0f;                                                                 /* col i+2 */
        s_spline_rhs[i] = 0.0f;
    }
    /* row 0: not-a-knot at x1
       -h1*M0 + (h0+h1)*M1 - h0*M2 = 0
    */
    s_spline_a0[0] = -s_spline_h[1];
    s_spline_b1[0] = s_spline_h[0] + s_spline_h[1];
    s_spline_b2[0] = -s_spline_h[0];
    /* interior rows: i = 1 .. n-2
       h(i-1)*M(i-1) + 2(h(i-1)+h(i))*M(i) + h(i)*M(i+1) = rhs
    */
    for (uint32_t i = 1U; i <= n - 2U; i++) {
        float h_prev = s_spline_h[i - 1U];
        float h_cur = s_spline_h[i];

        s_spline_a1[i] = h_prev;
        s_spline_a0[i] = 2.0f * (h_prev + h_cur);
        s_spline_b1[i] = h_cur;
        s_spline_rhs[i] = 6.0f * ((y[i + 1U] - y[i]) / h_cur - (y[i] - y[i - 1U]) / h_prev);
    }
    /* row n-1: not-a-knot at x(n-2)
       -h(n-2)*M(n-3) + (h(n-3)+h(n-2))*M(n-2) - h(n-3)*M(n-1) = 0
    */
    s_spline_a2[n - 1U] = -s_spline_h[n - 2U];
    s_spline_a1[n - 1U] = s_spline_h[n - 3U] + s_spline_h[n - 2U];
    s_spline_a0[n - 1U] = -s_spline_h[n - 3U];

    /* banded Gaussian elimination, bandwidth = 2 */
    for (uint32_t i = 0U; i < n; i++) {
        if (fabsf(s_spline_a0[i]) < 1.0e-12f) {
            return -4;
        }

        if (i + 1U < n) {
            float f1 = s_spline_a1[i + 1U] / s_spline_a0[i];
            s_spline_a1[i + 1U] = 0.0f;
            s_spline_a0[i + 1U] -= f1 * s_spline_b1[i];
            if (i + 2U < n) {
                s_spline_b1[i + 1U] -= f1 * s_spline_b2[i];
            }
            s_spline_rhs[i + 1U] -= f1 * s_spline_rhs[i];
        }

        if (i + 2U < n) {
            float f2 = s_spline_a2[i + 2U] / s_spline_a0[i];
            s_spline_a2[i + 2U] = 0.0f;
            s_spline_a1[i + 2U] -= f2 * s_spline_b1[i];
            s_spline_a0[i + 2U] -= f2 * s_spline_b2[i];
            s_spline_rhs[i + 2U] -= f2 * s_spline_rhs[i];
        }
    }
    /* back substitution */
    y2[n - 1U] = s_spline_rhs[n - 1U] / s_spline_a0[n - 1U];
    y2[n - 2U] = (s_spline_rhs[n - 2U] - s_spline_b1[n - 2U] * y2[n - 1U]) / s_spline_a0[n - 2U];

    for (int32_t i = (int32_t)n - 3; i >= 0; i--) {
        y2[i] = (s_spline_rhs[i] - s_spline_b1[i] * y2[i + 1] - s_spline_b2[i] * y2[i + 2])
                / s_spline_a0[i];
    }

    return 0;
}
/*公式是标准三次样条求值*/
static float spline_eval(const float *x, const float *y, const float *y2, uint32_t n,
                         float xq, uint32_t *hint_idx)
{
    uint32_t klo;
    uint32_t khi;
    float h;
    float a;
    float b;

    if (xq <= x[0]) {
        return y[0];
    }
    if (xq >= x[n - 1U]) {
        return y[n - 1U];
    }

    klo = ((hint_idx != NULL) && (*hint_idx < n - 1U)) ? *hint_idx : 0U;
    while (((klo + 1U) < n) && (x[klo + 1U] < xq)) {
        klo++;
    }

    khi = klo + 1U;
    if (hint_idx != NULL) {
        *hint_idx = klo;
    }

    h = x[khi] - x[klo];
    if (h <= 0.0f) {
        return y[klo];
    }

    a = (x[khi] - xq) / h;
    b = (xq - x[klo]) / h;

    return a * y[klo] + b * y[khi]
           + ((a * a * a - a) * y2[klo] + (b * b * b - b) * y2[khi]) * (h * h) / 6.0f;
}
/*  PPI -> WE */
static esp_err_t calc_we(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_NL_FEATURES_T *out)
{
    uint32_t n = (uint32_t)ppi_len;
    float t0;
    float t1;
    uint32_t nu;
    uint32_t hint = 0U;
    uint32_t max_level;
    uint32_t level;
    uint32_t cur_n;
    float energy[APP_WE_LEVEL_TARGET + 1U] = {0.0f};
    float energy_total = 0.0f;
    float wavelet_entropy = 0.0f;

    if ((ppi_ms == NULL) || (out == NULL) || (ppi_len < APP_PPI_FEATURE_MIN_COUNT)
        || (ppi_len > APP_AFRAME_PPI_MAXS)) {
        return ESP_ERR_INVALID_ARG;
    }
    /*单位统一，ppi单位是秒             累积时间轴s_we_t_accum = cumsum(PPI)*/
    for (uint32_t i = 0U; i < n; i++) {
        if (ppi_ms[i] == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        s_we_ppi_sec[i] = (float)ppi_ms[i] * 0.001f;
        s_we_t_accum[i] = (i == 0U) ? s_we_ppi_sec[0] : (s_we_t_accum[i - 1U] + s_we_ppi_sec[i]);
    }

    t0 = s_we_t_accum[0];
    t1 = s_we_t_accum[n - 1U];
    if (t1 <= t0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    /*均匀时间轴：t_uniform = t_accum(1):dt:t_accum(end)*/
    nu = (uint32_t)floorf((t1 - t0) / APP_WE_DT) + 1U;
    if (nu < 16U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (nu > APP_WE_MAX_UNIFORM_LEN) {
        return ESP_ERR_NO_MEM;
    }
    /*三次样条插值*/
    if (spline_prepare_notaknot(s_we_t_accum, s_we_ppi_sec, n, s_we_y2) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint32_t i = 0U; i < nu; i++) {
        float tq = t0 + APP_WE_DT * (float)i;
        s_we_sig_u[i] = spline_eval(s_we_t_accum, s_we_ppi_sec, s_we_y2, n, tq, &hint);
    }
    /*去直流分量*/
    {
        float mu = nl_mean(s_we_sig_u, nu);
        for (uint32_t i = 0U; i < nu; i++) {
            s_we_sig_u[i] -= mu;
        }
    }
    /*小波分解层数*/
    max_level = we_max_level_est(nu, APP_WE_DWT_DB_LEN);
    level = APP_WE_LEVEL_TARGET;
    if (level > max_level) {
        level = max_level;
    }
    if (level < 1U) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(s_we_buf_a, s_we_sig_u, nu * sizeof(float));
    cur_n = nu;

    for (uint32_t lv = 1U; lv <= level; lv++) {
        uint32_t out_n = 0U;

        we_dwt_level_sym(s_we_buf_a, cur_n, s_we_lo_d, s_we_hi_d, APP_WE_DWT_DB_LEN,
                         s_we_buf_b, s_we_det, &out_n);
        energy[lv - 1U] = nl_energy(s_we_det, out_n);/*MATLAB detcoef 第 i 层能量*/
        memcpy(s_we_buf_a, s_we_buf_b, out_n * sizeof(float));
        cur_n = out_n;
    }
    /* 最后一层近似系数能量 */
    energy[level] = nl_energy(s_we_buf_a, cur_n);
    for (uint32_t i = 0U; i < level + 1U; i++) {
        energy_total += energy[i];
    }

    if (energy_total <= 0.0f) {
        out->WE = 0.0f;
        return ESP_OK;
    }
    /*Shannon wavelet entropy*/
    for (uint32_t i = 0U; i < level + 1U; i++) {
        float p = energy[i] / energy_total;
        if (p > 0.0f) {
            wavelet_entropy += -p * logf(p);
        }
    }

    out->WE = wavelet_entropy;

    return ESP_OK;
}

esp_err_t ppi_nonlinear_features_calculate(const uint16_t *ppi_ms, uint16_t ppi_len, PPI_NL_FEATURES_T *out)
{
    esp_err_t dfa_ret;
    esp_err_t we_ret;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    if ((out == NULL) || (ppi_ms == NULL) || (ppi_len < APP_PPI_FEATURE_MIN_COUNT)
        || (ppi_len > APP_AFRAME_PPI_MAXS)) {
        return ESP_ERR_INVALID_ARG;
    }

    calc_vai(ppi_ms, ppi_len, out);
    dfa_ret = calc_dfa_r2(ppi_ms, ppi_len, out);
    we_ret = calc_we(ppi_ms, ppi_len, out);

    if ((dfa_ret != ESP_OK) && (we_ret != ESP_OK)) {
        return dfa_ret;
    }
    if (dfa_ret != ESP_OK) {
        return dfa_ret;
    }
    if (we_ret != ESP_OK) {
        return we_ret;
    }

    return ESP_OK;
}
