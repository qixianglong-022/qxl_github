/**
 ******************************************************************************
 * @file    adc_processing.c
 * @brief   ADC数据处理、缓冲与网络发送核心逻辑 (UDP + CPU搬运方案)
 * @author  (您的姓名)
 * @date    (当前日期)
 *
 * @details
 * - **协议**: 使用UDP将数据高速发送至固定PC。
 * - **内存方案**: 采集数据存储在CCMRAM中的大型乒乓缓冲区(`g_adc_ping_pong_buffer`)，
 * 以节约宝贵的SRAM。
 * - **发送策略 (方案B)**:
 * 1. 当一个CCMRAM缓冲区填满后，标记为待处理。
 * 2. 发送任务启动，在主循环中被调用。
 * 3. 发送任务将CCMRAM中的大块数据，通过CPU (`memcpy`) 逐片复制到
 * 一个位于主SRAM的、较小的中转缓冲区(`udp_tx_sram_staging_buf`)。
 * 4. 将SRAM中转缓冲区内的数据打包成UDP包，交给LwIP发送。
 * 5. LwIP从SRAM中获取数据，因此以太网DMA可以正常访问并执行**硬件校验和卸载**。
 * 6. 循环此过程，直到CCMRAM中的整个大缓冲区被发送完毕。
 ******************************************************************************
 */

#include "adc_processing.h"
#include "ads8688.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "debug_log.h"

// 包含所有必需的头文件
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip.h" // 确保在 lwip/udp.h 之后
#include "stm32f4xx_ll_spi.h"
#include "stm32f4xx_ll_dma.h"
#include "stm32f4xx_ll_tim.h"
#include "stm32f4xx_ll_gpio.h"

/* Private defines -----------------------------------------------------------*/
#define CS1_PORT CS1_GPIO_Port
#define CS1_PIN  CS1_Pin

/* Private variables ---------------------------------------------------------*/
// --- 网络相关 ---
static struct udp_pcb *g_upcb;          // 全局UDP控制块
static ip_addr_t g_dest_ip_addr;        // 目标PC的IP地址

// --- 【核心】SRAM中的UDP发送中转缓冲区 ---
// 此缓冲区位于主SRAM，以太网DMA可以访问它。
// CPU负责将数据从CCMRAM拷贝到这里。
static uint8_t udp_tx_sram_staging_buf[UDP_PAYLOAD_SIZE] __attribute__((aligned(4)));

// --- 乒乓数据双缓冲 (位于CCMRAM) ---
// 使用 `__attribute__((section(".ccmram")))` 将其放入CCMRAM
// 注意: 请确保您的链接描述文件(linker script, .ld)正确配置了 .ccmram 段
__attribute__((section(".ccmram")))
__attribute__((aligned(32))) // 确保32字节对齐以优化DMA和CPU访问
static uint16_t g_adc_ping_pong_buffer[2][PING_PONG_BUFFER_SIZE];	//创建了两个这样的大数组，用于实现乒乓缓冲机制。

// --- 状态与计数器 ---
volatile uint8_t  g_start_acquisition_flag = 0;   // 定时器触发的采集请求标志
volatile uint8_t  g_dma_busy_flag = 0;            // DMA忙标志，防止重入
volatile uint32_t g_sample_count = 0;             // 当前缓冲区的采样点计数
volatile uint8_t  g_acquisition_buffer_idx = 0;   // 当前正在被DMA填充的缓冲区索引 (0或1)
volatile int8_t   g_process_buffer_idx = -1;      // 当前需要被发送的缓冲区索引 (-1表示无)
volatile uint32_t g_udp_packets_sent_count = 0;   // UDP数据包发送总数计数器

// --- DMA相关 ---
static uint8_t g_dma_tx_buffer[4] = {0x00, 0x00, 0x00, 0x00};
static uint8_t g_dma_rx_buffer[4] = {0};

/* Private function prototypes -----------------------------------------------*/
static void SendWaveformDataViaUDP(void);

/* Public functions ----------------------------------------------------------*/

/**
 * @brief 初始化ADC处理、UDP网络和相关外设
 */
void ADC_Processing_Init(void)
{
    // 1. 初始化ADC芯片
    ADS8688_Device_Init(CS1_PORT, CS1_PIN);
    Log_Debug("OK: ADS8688 Initialized.");

    // 2. 初始化UDP
    Log_Debug("INFO: Initializing UDP...");

    // 创建一个新的UDP控制块(PCB)
    g_upcb = udp_new();
    if (g_upcb == NULL)
    {
        Log_Debug("!!! ERROR: udp_new() failed. System halted.");
        while(1); // 严重错误，停机
    }

    // 设置目标PC的IP地址
    IP4_ADDR(&g_dest_ip_addr, DEST_IP_ADDR0, DEST_IP_ADDR1, DEST_IP_ADDR2, DEST_IP_ADDR3);

    // 将UDP PCB“连接”到远程IP和端口。
    // 这比每次发送都调用`udp_sendto`更高效，因为LwIP会缓存地址信息。
    err_t err = udp_connect(g_upcb, &g_dest_ip_addr, DEST_PORT);
    if (err != ERR_OK)
    {
        Log_Debug1("!!! ERROR: udp_connect() failed with err=%d. System halted.", err);
        while(1); // 严重错误，停机
    }

    Log_Debug1("OK: UDP configured. Target: %s:%d", ip4addr_ntoa(&g_dest_ip_addr), DEST_PORT);
    Log_Debug("------------------------------------");
}

/**
 * @brief 启动ADC采集定时器，开始整个数据采集流程
 */
void ADC_Processing_Start(void)
{
    Log_Debug("INFO: Starting ADC acquisition timer (TIM2)...");
    LL_TIM_EnableCounter(TIM2);
}

/**
 * @brief ADC和网络处理的主循环任务函数
 * @details 此函数应在main的while(1)中被持续调用
 */
void ADC_Processing_Task(void)
{
    // --- 任务1: 处理定时器触发的DMA采集请求 ---
    if (g_start_acquisition_flag)
    {
        g_start_acquisition_flag = 0; // 清除标志，表示我们已开始处理

        // --- 【关键修正】调整了外设启动顺序以提高稳定性 ---

        // 1. 确保外设处于已知状态 (禁用)
        LL_SPI_Disable(SPI1);
        LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_0); // RX
        LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_3); // TX

        // 2. 配置DMA传输参数 (长度、地址等)
        LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_0, 4); // 4字节样本
        LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_3, 4);
			
        LL_DMA_SetPeriphAddress(DMA2, LL_DMA_STREAM_0, (uint32_t)&(SPI1->DR));
        LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_0, (uint32_t)g_dma_rx_buffer);
        LL_DMA_SetPeriphAddress(DMA2, LL_DMA_STREAM_3, (uint32_t)&(SPI1->DR));
        LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_3, (uint32_t)g_dma_tx_buffer);

        // 3. 清除可能存在的旧中断标志位
        LL_DMA_ClearFlag_TC0(DMA2);
        LL_DMA_ClearFlag_TE0(DMA2);
				LL_DMA_ClearFlag_TC3(DMA2);
        LL_DMA_ClearFlag_TE3(DMA2);
				

        // 4. 使能DMA中断
        LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_0);
        LL_DMA_EnableIT_TE(DMA2, LL_DMA_STREAM_0);

        // 5. 配置并使能SPI的DMA请求
        LL_SPI_EnableDMAReq_TX(SPI1);
        LL_SPI_EnableDMAReq_RX(SPI1);

        // 6. 首先拉低片选(CS)，选中从设备
        LL_GPIO_ResetOutputPin(CS1_PORT, CS1_PIN);

        // 7. 然后使能SPI主设备
        LL_SPI_Enable(SPI1);

        // 8. 最后，在SPI和CS都就绪后，启动DMA数据流
        LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_0); // RX
        LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_3); // TX
        
        // Log_Debug("DEBUG: Started one DMA acquisition."); // 可选的调试输出
    }

    // --- 任务2: 检查是否有已满的缓冲区需要通过UDP发送 ---
    if (g_process_buffer_idx != -1)
    {
        SendWaveformDataViaUDP();
    }
}

/**
 * @brief SPI DMA接收完成回调函数 (在stm32f4xx_it.c中被调用)
 */
void SPI1_DMA_RX_Callback(void)
{
    LL_GPIO_SetOutputPin(CS1_PORT, CS1_PIN); // 结束本次SPI通信

    // 从DMA缓冲区中提取16位ADC原始值
    uint16_t adc_raw_value = ((uint16_t)g_dma_rx_buffer[2] << 8) | (g_dma_rx_buffer[3]);

    // 将采集到的数据存入当前活动的乒乓缓冲区
    g_adc_ping_pong_buffer[g_acquisition_buffer_idx][g_sample_count] = adc_raw_value;

    g_sample_count++;

    // 检查当前缓冲区是否已满
    if (g_sample_count >= PING_PONG_BUFFER_SIZE)
    {
        // 如果另一个缓冲区当前空闲（即上次的数据已发送完毕）
        if (g_process_buffer_idx == -1)
        {
            // --- 乒乓切换 ---
            g_process_buffer_idx = g_acquisition_buffer_idx;  // 将刚填满的缓冲区标记为“待处理”
            g_acquisition_buffer_idx = !g_acquisition_buffer_idx; // 切换到另一个缓冲区进行下一次采集
            g_sample_count = 0; // 重置新缓冲区的采样计数器

            Log_Debug1("INFO: Buffer %d full. Swapping to buffer %d. Ready to send.", g_process_buffer_idx, g_acquisition_buffer_idx);
        }
        else
        {
            // 网络拥堵或处理速度跟不上采集速度，一个缓冲区的数据被丢弃
            // 这种背压机制可以防止系统崩溃
            Log_Debug("!!! WARNING: Network backpressure! Dropping one full buffer.");
            g_sample_count = 0; // 丢弃数据，直接在当前缓冲区重新开始采集
        }
    }

    g_dma_busy_flag = 0; // 清除DMA忙标志，允许下一次定时器中断触发采集
}


/**
 * @brief SPI DMA错误回调函数
 */
void SPI1_DMA_Error_Callback(void)
{
    Log_Debug("!!! FATAL: SPI/DMA Transfer Error Occurred!");
    LL_GPIO_SetOutputPin(CS1_PORT, CS1_PIN);
    g_dma_busy_flag = 0;
    // 这里可以加入更复杂的错误恢复机制
}


/**
 * @brief 将一个完整的数据缓冲区通过UDP分片发送出去
 * @details 采用 CPU搬运(memcpy) + LwIP标准pbuf发送 的模式
 */
static void SendWaveformDataViaUDP(void)
{
    uint8_t* ccm_buffer_ptr = (uint8_t*)g_adc_ping_pong_buffer[g_process_buffer_idx];
    // **关键修改**: 计算总字节数时使用sizeof(uint32_t)
    const uint32_t total_bytes_to_send = PING_PONG_BUFFER_SIZE * sizeof(uint16_t);
    static uint32_t bytes_sent_from_current_buffer = 0; // 跟踪当前大缓冲区的发送进度

    // 检查是否是新的发送任务
    if (bytes_sent_from_current_buffer == 0) {
         Log_Debug1("INFO: Starting to send buffer %d (%u bytes) via UDP...", g_process_buffer_idx, total_bytes_to_send);
    }

    // 在一次函数调用中，尝试尽可能多地发送数据，直到LwIP的缓冲区满
    while(bytes_sent_from_current_buffer < total_bytes_to_send)
    {
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, UDP_PAYLOAD_SIZE, PBUF_RAM);
        if (p == NULL) {
            Log_Debug("DEBUG: LwIP PBUF pool temporarily empty. Will retry.");
            return; // pbuf耗尽，退出函数，等待下次轮询
        }

        uint32_t chunk_size = total_bytes_to_send - bytes_sent_from_current_buffer;
        if (chunk_size > UDP_PAYLOAD_SIZE) {
            chunk_size = UDP_PAYLOAD_SIZE;
        }

        memcpy(udp_tx_sram_staging_buf, ccm_buffer_ptr + bytes_sent_from_current_buffer, chunk_size);
        pbuf_take(p, udp_tx_sram_staging_buf, chunk_size);

        err_t err = udp_send(g_upcb, p);
        pbuf_free(p); // 无论成功与否都要释放pbuf

        if (err == ERR_OK) {
            bytes_sent_from_current_buffer += chunk_size;
            g_udp_packets_sent_count++;
        } else {
            Log_Debug1("DEBUG: udp_send failed with err=%d (likely queue full). Will retry.", err);
            return; // 发送队列满，退出函数，等待下次轮询
        }
    }

    // 如果代码执行到这里，说明整个大缓冲区都已成功发送
    Log_Debug1("OK: Finished sending buffer %d. Total packets sent so far: %u.", g_process_buffer_idx, g_udp_packets_sent_count);
    g_process_buffer_idx = -1; // 标记缓冲区为空闲
    bytes_sent_from_current_buffer = 0; // 为下一个缓冲区重置发送计数器
}

