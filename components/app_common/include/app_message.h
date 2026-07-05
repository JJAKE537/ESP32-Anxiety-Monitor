#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "app_config.h"

/*
s_events[ SENSOR_FAULT,  OVERRUN,  ANXIETY_VALID,  FEATURE_VALID,  PPI_VALID,  WINDOW_READY,  SYS_STOPPED,  SYS_RUN]
多个任务共享的一组状态灯
*/ 
#define APP_EVT_SYS_RUN             (1U<<0)
#define APP_EVT_SYS_STOPPED         (1U<<1)
#define APP_EVT_WIN_READY           (1U<<2)
#define APP_EVT_PPI_VALID           (1U<<3)
#define APP_EVT_FEATURE_VALID       (1U<<4)
#define APP_EVT_ANXIETY_VALID       (1U<<5)
#define APP_EVT_OVERRUN             (1U<<6)
#define APP_EVT_SENSOR_FAULT        (1U<<7)
/*
定义数据队列的大小 
*/
#define APP_QUEUE_DEPTH_SMALL       2U
/*==================================前一级任务算完结果 -> 打包成结构体 -> 丢进队列 -> 后一级任务取出来继续算=================================*/
/*
定义ppi结果结构体
包括一帧ppi数据ms、一帧ppi数量、一帧瞬时脉率、一帧平均脉率、一帧平均ppi
*/
typedef struct{
    uint16_t ppi_ms_aframe[APP_AFRAME_PPI_MAXS];
    uint16_t ppi_count;
    uint8_t insta_pr[APP_AFRAME_PPI_MAXS];
    uint8_t mean_pr;
    float mean_ppi;
    uint8_t valid;
}PPI_RESULT_T;

/*窗口信息结构体*/
typedef struct{
    uint32_t WIN_ID;
    uint16_t sample_count;
    TickType_t tick;
}PPI_WIN_INFO;


/*PPI结果信息结构体*/
typedef struct{
    uint32_t WIN_ID;
    PPI_RESULT_T result;    
}PPI_MSG_T;

/*特征结果消息*/
typedef struct{
    uint32_t WIN_ID;
    uint8_t valid;
    uint8_t PR;
    float RMSSD;
    float SD2;
    float HF;
    float LF;
    float LF_HF;
    float WE;
    float DFA;
    float VAI;
}PPI_Features_T;

/*=======================================================================================================================================*/
/*焦虑指数消息*/
typedef struct{
    uint32_t WIN_ID;
    uint8_t valid;
    float anxiety_idx;
    PPI_Features_T features;
}ANXIETY_MSG_T;

#ifndef APP_ANXIETY_FEATURE_NUM
#define APP_ANXIETY_FEATURE_NUM 9U
#endif

esp_err_t anxiety_topsis_calculate(const uint8_t *positive_idx,
                                   uint8_t positive_count,
                                   const uint8_t *negative_idx,
                                   uint8_t negative_count,
                                   const float sample_features_solo[APP_ANXIETY_FEATURE_NUM],
                                   const float combined_weights[APP_ANXIETY_FEATURE_NUM],
                                   const float feature_pr_matrix[2][APP_ANXIETY_FEATURE_NUM],
                                   float *out_score);

esp_err_t anxiety_index_calculate(const PPI_Features_T *features, float *out_score);




