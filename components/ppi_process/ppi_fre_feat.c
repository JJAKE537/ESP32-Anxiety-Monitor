#include "app_config.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
LF      = LF 相对小波频带能量，范围大致 0~1
HF      = HF 相对小波频带能量，范围大致 0~1
LF_HF   = LF / HF
TP      = LF + HF 的原始小波频带能量
K       = 平均小波有效支撑比例，用来观察边界影响

原始 PPI 序列 → 构造非均匀时间轴 → PCHIP 插值重采样 → 得到等间隔 PPI 序列 → 去均值 → 复 Morlet 小波频域分析 → 计算 LF、HF、LF/HF、TP、K 等频域特征。
*/


typedef struct {
    float re;
    float im;
} complex32_t;

static float s_fre_ppi_ms[APP_AFRAME_PPI_MAXS];
static float s_fre_t_irregular[APP_AFRAME_PPI_MAXS];
static float s_fre_pchip_d[APP_AFRAME_PPI_MAXS];
static float s_fre_pchip_h[APP_AFRAME_PPI_MAXS];
static float s_fre_pchip_delta[APP_AFRAME_PPI_MAXS];

static float s_fre_x_uniform[APP_FRE_MAX_UNIFORM_LEN];
static float s_fre_freq[APP_FRE_NUM_SCALES];
static float s_fre_power[APP_FRE_NUM_SCALES];
/*计算等间隔PPI序列均值*/
static float fre_mean(const float *x, uint32_t n)
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
/*计算零均值序列方差*/
static float fre_var_zero_mean(const float *x, uint32_t n)
{
    float sum2 = 0.0f;

    if ((x == NULL) || (n < 2U)) {
        return 0.0f;
    }

    for (uint32_t i = 0U; i < n; i++) {
        sum2 += x[i] * x[i];
    }

    return sum2 / (float)(n - 1U);
}
/*生成对数分布的频率点---生成小波分析的频率序列      0.04-0.5hz之间生成频率点，对数分布*/
static float fre_logspace_at(uint32_t idx, uint32_t n, float log10_min, float log10_max)
{
    if (n <= 1U) {
        return powf(10.0f, log10_min);
    }

    return powf(10.0f,
                log10_min + ((float)idx / (float)(n - 1U)) * (log10_max - log10_min));
}
/*对指定频带内的功率曲线做梯形积分*/
static float fre_trapz_band(const float *f, const float *y, uint32_t n,
                            float f_low, float f_high)
{
    float area = 0.0f;
    bool started = false;
    float prev_f = 0.0f;
    float prev_y = 0.0f;

    if ((f == NULL) || (y == NULL) || (n < 2U)) {
        return 0.0f;
    }

    for (uint32_t i = 0U; i < n; i++) {
        if ((f[i] >= f_low) && (f[i] <= f_high)) {
            if (!started) {
                prev_f = f[i];
                prev_y = y[i];
                started = true;
            } else {
                float dx = f[i] - prev_f;
                area += 0.5f * dx * (prev_y + y[i]);
                prev_f = f[i];
                prev_y = y[i];
            }
        }
    }

    return area;
}
/*计算PCHIP插值所需的节点导数*/
static void pchip_slopes(const float *x, const float *y, uint32_t n, float *d)
{
    if (n == 2U) {
        float s = (y[1] - y[0]) / (x[1] - x[0]);
        d[0] = s;
        d[1] = s;
        return;
    }

    for (uint32_t i = 0U; i < n - 1U; i++) {
        s_fre_pchip_h[i] = x[i + 1U] - x[i];                                            /*非等间隔时间轴---相邻时间间隔*/
        s_fre_pchip_delta[i] = (y[i + 1U] - y[i]) / s_fre_pchip_h[i];                   /*相邻PPI变化斜率*/
    }

    {
        float d0 = ((2.0f * s_fre_pchip_h[0] + s_fre_pchip_h[1]) * s_fre_pchip_delta[0]
                    - s_fre_pchip_h[0] * s_fre_pchip_delta[1])
                   / (s_fre_pchip_h[0] + s_fre_pchip_h[1]);

        if ((d0 * s_fre_pchip_delta[0]) <= 0.0f) {
            d0 = 0.0f;
        } else if (((s_fre_pchip_delta[0] * s_fre_pchip_delta[1]) < 0.0f)
                   && (fabsf(d0) > fabsf(3.0f * s_fre_pchip_delta[0]))) {
            d0 = 3.0f * s_fre_pchip_delta[0];
        }

        d[0] = d0;
    }

    for (uint32_t k = 1U; k < n - 1U; k++) {
        if ((s_fre_pchip_delta[k - 1U] == 0.0f)
            || (s_fre_pchip_delta[k] == 0.0f)
            || ((s_fre_pchip_delta[k - 1U] > 0.0f) !=
                (s_fre_pchip_delta[k] > 0.0f))) {
            d[k] = 0.0f;
        } else {
            float w1 = 2.0f * s_fre_pchip_h[k] + s_fre_pchip_h[k - 1U];
            float w2 = s_fre_pchip_h[k] + 2.0f * s_fre_pchip_h[k - 1U];

            d[k] = (w1 + w2) /
                   (w1 / s_fre_pchip_delta[k - 1U] + w2 / s_fre_pchip_delta[k]);
        }
    }

    {
        uint32_t k = n - 1U;
        float dn = ((2.0f * s_fre_pchip_h[k - 1U] + s_fre_pchip_h[k - 2U])
                    * s_fre_pchip_delta[k - 1U]
                    - s_fre_pchip_h[k - 1U] * s_fre_pchip_delta[k - 2U])
                   / (s_fre_pchip_h[k - 1U] + s_fre_pchip_h[k - 2U]);

        if ((dn * s_fre_pchip_delta[k - 1U]) <= 0.0f) {
            dn = 0.0f;
        } else if (((s_fre_pchip_delta[k - 1U] * s_fre_pchip_delta[k - 2U]) < 0.0f)
                   && (fabsf(dn) > fabsf(3.0f * s_fre_pchip_delta[k - 1U]))) {
            dn = 3.0f * s_fre_pchip_delta[k - 1U];
        }

        d[k] = dn;
    }
}
/*x:非等间隔时间轴---根据PCHIP插值公式，计算任一个重采样时间点xq的PPI值【yq】---生成等间隔的PPI序列*/
static float pchip_eval(const float *x, const float *y, const float *d,
                        uint32_t n, float xq, uint32_t *hint_idx)
{
    uint32_t k; 

    if (xq <= x[0]) {
        return y[0];
    }

    if (xq >= x[n - 1U]) {
        return y[n - 1U];
    }

    k = ((hint_idx != NULL) && (*hint_idx < (n - 1U))) ? *hint_idx : 0U;
    while (((k + 1U) < n) && (x[k + 1U] < xq)) {
        k++;
    }

    if (hint_idx != NULL) {
        *hint_idx = k;
    }

    {
        float h = x[k + 1U] - x[k];
        float t = (xq - x[k]) / h;
        float t2 = t * t;
        float t3 = t2 * t;
        float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        float h10 = t3 - 2.0f * t2 + t;
        float h01 = -2.0f * t3 + 3.0f * t2;
        float h11 = t3 - t2;

        return h00 * y[k] + h10 * h * d[k]
               + h01 * y[k + 1U] + h11 * h * d[k + 1U];
    }
}
/*计算某一个频率点上的复Morlet小波平均功率*/
static float cmor_power_at_freq(const float *x, uint32_t n, float dt,
                                float freq_hz, float *mean_support_out)
{/*在一个指定频率上，用复 Morlet 小波估计 PPI 波动序列的能量强度*/
    float scale_sec;
    int32_t radius;
    float norm0;
    float scale_sqrt;
    float full_energy = 0.0f;
    float power_sum = 0.0f;
    float support_sum = 0.0f;
    uint32_t valid_count = 0U;

    if ((x == NULL) || (n == 0U) || (freq_hz <= 0.0f)) {
        if (mean_support_out != NULL) {
            *mean_support_out = 0.0f;
        }
        return 0.0f;
    }
    /*频率换算小波尺度---频率越低，小波尺度越大---频率越高，小波尺度越小*/
    scale_sec = APP_FRE_CMOR_C / freq_hz;
    radius = (int32_t)ceilf(APP_FRE_WAVELET_HALF_WIDTH * scale_sec / dt);  /*计算小波窗口半径*/

    if (radius < 1) {
        radius = 1;
    }

    norm0 = 1.0f / sqrtf(APP_PI_F * APP_FRE_CMOR_B);                       /*1/(（pai*B）^ (1/2))*/
    scale_sqrt = sqrtf(scale_sec);                                         /*(a^(1/2))*/

    for (int32_t m = -radius; m <= radius; m++) {
        float tau = ((float)m * dt) / scale_sec;
        float env = norm0 * expf(-(tau * tau) / APP_FRE_CMOR_B) / scale_sqrt;
        full_energy += env * env * dt;/*计算完整小波能量*/
    }

    if (full_energy <= 1.0e-20f) {
        if (mean_support_out != NULL) {
            *mean_support_out = 0.0f;
        }
        return 0.0f;
    }

    for (uint32_t center = 0U; center < n; center++) {
        complex32_t c = {0.0f, 0.0f};
        float local_energy = 0.0f;

        for (int32_t m = -radius; m <= radius; m++) {
            int32_t idx = (int32_t)center + m;

            if ((idx < 0) || (idx >= (int32_t)n)) {
                continue;
            }

            {
                float tau = ((float)m * dt) / scale_sec;
                float env = norm0 * expf(-(tau * tau) / APP_FRE_CMOR_B) / scale_sqrt;
                float ang = 2.0f * APP_PI_F * APP_FRE_CMOR_C * tau;
                float wr = env * cosf(ang);                                 /*小波实部权重*/
                float wi = env * sinf(ang);                                 /*小波虚部权重*/
                /*对每一个中心点做小波卷积*/
                c.re += x[(uint32_t)idx] * wr * dt;                         /*小波复系数*/
                c.im -= x[(uint32_t)idx] * wi * dt;
                local_energy += (wr * wr + wi * wi) * dt;                   /*小波局部能量*/
            }
        }

        if (local_energy > 1.0e-20f) {
            float support = local_energy / full_energy;/*局部有效支撑比例---靠近序列边界时，小波窗口会被截断，有效能量变小。这个比例用于衡量当前中心点的小波计算是否可靠。*/

            if (support >= APP_FRE_CWT_MIN_SUPPORT) {/*剔除支撑比例太低的点---如果边界影响太大，该点不参与平均*/
                power_sum += (c.re * c.re + c.im * c.im) / local_energy;/*该频率点的局部归一化功率*/
                support_sum += support;
                valid_count++;
            }
        }
    }

    if (valid_count == 0U) {
        if (mean_support_out != NULL) {
            *mean_support_out = 0.0f;
        }
        return 0.0f;
    }

    if (mean_support_out != NULL) {
        *mean_support_out = support_sum / (float)valid_count;
    }
    /*计算当前频率的平均功率---最终输出的是该频率下的平均小波功率*/
    return power_sum / (float)valid_count;
}
/*频域特征主函数计算*/
esp_err_t ppi_frequency_features_calculate(const uint16_t *ppi_ms,
                                           uint16_t ppi_len,
                                           PPI_FRE_FEATURES_T *out)
{
    uint32_t n = (uint32_t)ppi_len;
    uint32_t nu = 0U;
    uint32_t hint = 0U;
    float start_time;
    float end_time;
    float tq;
    float mean_uniform;
    float time_domain_power;
    float lf_abs;
    float hf_abs;
    float total_abs;
    float support_sum = 0.0f;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    if ((out == NULL) || (ppi_ms == NULL) ||
        (ppi_len < APP_PPI_FEATURE_MIN_COUNT) ||
        (ppi_len > APP_AFRAME_PPI_MAXS)) {
        return ESP_ERR_INVALID_ARG;
    }

    {/*PPI数据转换和非等间隔时间轴构造*/
        float acc_ms = 0.0f;

        for (uint32_t i = 0U; i < n; i++) {
            if (ppi_ms[i] == 0U) {
                return ESP_ERR_INVALID_ARG;
            }

            s_fre_ppi_ms[i] = (float)ppi_ms[i];
            acc_ms += s_fre_ppi_ms[i];
            s_fre_t_irregular[i] = acc_ms * 0.001f;/*非等间隔时间轴*/
        }
    }

    start_time = s_fre_t_irregular[0];
    end_time = s_fre_t_irregular[n - 1U];

    if (end_time <= start_time) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    /*根据非等间隔PPI序列计算PCHIP插值所需的分段斜率*/
    pchip_slopes(s_fre_t_irregular, s_fre_ppi_ms, n, s_fre_pchip_d);
    /*生成等间隔PPI序列*/
    tq = start_time;
    while (tq <= (end_time + 1.0e-7f)) {
        if (nu >= APP_FRE_MAX_UNIFORM_LEN) {
            return ESP_ERR_NO_MEM;
        }

        s_fre_x_uniform[nu] =
            pchip_eval(s_fre_t_irregular, s_fre_ppi_ms, s_fre_pchip_d, n, tq, &hint);

        nu++;
        tq += APP_FRE_DT;
    }

    if (nu < 16U) {
        return ESP_ERR_INVALID_SIZE;
    }
    /*去除均值*/
    mean_uniform = fre_mean(s_fre_x_uniform, nu);
    for (uint32_t i = 0U; i < nu; i++) {
        s_fre_x_uniform[i] -= mean_uniform;
    }

    {/*生成频率点并计算小波功率*/
        float log10_min = log10f(APP_FRE_F_MIN_HZ);
        float log10_max = log10f(APP_FRE_F_MAX_HZ);

        for (uint32_t si = 0U; si < APP_FRE_NUM_SCALES; si++) {
            float support = 0.0f;
            float f_desired = fre_logspace_at(si,
                                               APP_FRE_NUM_SCALES,
                                               log10_min,
                                               log10_max);

            s_fre_freq[si] = f_desired;
            s_fre_power[si] =
                cmor_power_at_freq(s_fre_x_uniform, nu, APP_FRE_DT, f_desired, &support);

            support_sum += support;
        }
    }

    lf_abs = fre_trapz_band(s_fre_freq,
                            s_fre_power,
                            APP_FRE_NUM_SCALES,
                            APP_FRE_LF_LOW_HZ,
                            APP_FRE_LF_HIGH_HZ);

    hf_abs = fre_trapz_band(s_fre_freq,
                            s_fre_power,
                            APP_FRE_NUM_SCALES,
                            APP_FRE_HF_LOW_HZ,
                            APP_FRE_HF_HIGH_HZ);

    total_abs = lf_abs + hf_abs;
    time_domain_power = fre_var_zero_mean(s_fre_x_uniform, nu);

    out->VLF = 0.0f;
    out->TP = total_abs;

    if (total_abs > 1.0e-20f) {
        out->LF = lf_abs / total_abs;
        out->HF = hf_abs / total_abs;
    } else {
        out->LF = 0.0f;
        out->HF = 0.0f;
    }

    out->LF_HF = (out->HF > 1.0e-20f) ? (out->LF / out->HF) : 0.0f;
    out->K = support_sum / (float)APP_FRE_NUM_SCALES;/*平均小波有效支撑比例，反映边界影响*/
    out->time_domain_power = time_domain_power;/*去均值PPI序列的时域方差*/
    out->freq_domain_integral = total_abs;
    out->uniform_len = (uint16_t)nu;/*重采样后的等间隔序列长度*/
    out->num_scales = (uint16_t)APP_FRE_NUM_SCALES;/*小波分析使用的频率点数量*/

    return ESP_OK;
}
