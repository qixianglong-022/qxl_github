// Core/Inc/adc_processing.h

#ifndef INC_ADC_PROCESSING_H_
#define INC_ADC_PROCESSING_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

// --- 用户可配置宏定义 ---

// ** 数据采集参数 **
#define CHANNELS_PER_SAMPLE     8       // ADC每次自动扫描的通道数
#define SAMPLES_PER_CHANNEL     1024    // 每个通道采集的样本数
// 每个乒乓缓冲区的总样本数 (注意：类型现在是uint16_t)
#define PING_PONG_BUFFER_SIZE   (CHANNELS_PER_SAMPLE * SAMPLES_PER_CHANNEL)

// ** 网络参数 **
#define DEST_IP_ADDR0           192
#define DEST_IP_ADDR1           168
#define DEST_IP_ADDR2           0
#define DEST_IP_ADDR3           100
#define DEST_PORT               5001

// ** UDP包净荷大小 **
#define UDP_PAYLOAD_SIZE        1440    // bytes

// --- 对外暴露的函数 ---
void ADC_Processing_Init(void);
void ADC_Processing_Start(void);
void ADC_Processing_Task(void);

// --- 中断回调函数 ---
void SPI1_DMA_RX_Callback(void);
void SPI1_DMA_Error_Callback(void);

// --- 全局变量声明 ---
extern volatile uint8_t g_dma_busy_flag;
extern volatile uint8_t g_start_acquisition_flag;
extern volatile uint32_t g_udp_packets_sent_count;

#ifdef __cplusplus
}
#endif

#endif /* INC_ADC_PROCESSING_H_ */
