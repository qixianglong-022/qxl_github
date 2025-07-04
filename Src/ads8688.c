/**
 ******************************************************************************
 * @file    ads8688.c
 * @brief   ADS8688 ADC 驱动程序 (LL库版本)
 * @author  (您的姓名)
 * @date    (当前日期)
 *
 * @details
 * 本驱动文件提供了与德州仪器 (TI) ADS8688 16位、8通道ADC进行通信
 * 所需的核心功能。它封装了通过SPI发送命令、读/写内部程序寄存器的
 * 底层操作，并提供了一个高级初始化函数，用于将设备配置为最常用
 * 的自动通道扫描模式。此版本已完全迁移至STM32 LL库以提升性能。
 ******************************************************************************
 */

#include "ads8688.h"
#include "main.h" // 包含 LL 库头文件
#include "stm32f4xx_hal.h" // 仅为了使用 HAL_Delay

// --- 私有辅助函数：使用LL库以轮询方式收发一个字节 ---
static uint8_t LL_SPI_TransmitReceive_Polling(SPI_TypeDef* SPIx, uint8_t data)
{
    // 等待发送缓冲区为空
    while (LL_SPI_IsActiveFlag_TXE(SPIx) == 0);
    // 发送数据
    LL_SPI_TransmitData8(SPIx, data);

    // 等待接收缓冲区非空
    while (LL_SPI_IsActiveFlag_RXNE(SPIx) == 0);
    // 读取并返回接收到的数据
    return LL_SPI_ReceiveData8(SPIx);
}

/**
 * @brief  向ADS8688发送一个16位的命令。
 * @param  cs_port 控制ADS8688片选(CS)引脚的GPIO端口 (例如 GPIOA)。
 * @param  cs_pin  控制ADS8688片选(CS)引脚的GPIO引脚号 (例如 LL_GPIO_PIN_4)。
 * @param  com     要发送的16位命令。
 * @retval None
 */
void ADS8688_Write_Command(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint16_t com)
{
    // 拉低片选，选中从设备
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);

    // 发送高8位
    LL_SPI_TransmitReceive_Polling(SPI1, (uint8_t)(com >> 8));
    // 发送低8位
    LL_SPI_TransmitReceive_Polling(SPI1, (uint8_t)com);

    // 拉高片选，结束通信
    LL_GPIO_SetOutputPin(cs_port, cs_pin);
}

/**
 * @brief  向ADS8688的程序寄存器写入一个8位的值。
 * @param  cs_port CS引脚的GPIO端口。
 * @param  cs_pin  CS引脚的GPIO引脚号。
 * @param  addr    要写入的8位程序寄存器地址。
 * @param  data    要写入的8位数据。
 * @retval None
 */
void ADS8688_Write_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr, uint8_t data)
{
    // 拉低片选，选中从设备
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);

    // 发送写命令帧的第一个字节: (ADDR << 1) | 0x01
    LL_SPI_TransmitReceive_Polling(SPI1, (addr << 1) | 0x01);
    // 发送要写入的数据
    LL_SPI_TransmitReceive_Polling(SPI1, data);

    // 拉高片选，结束通信
    LL_GPIO_SetOutputPin(cs_port, cs_pin);
}

/**
 * @brief  从ADS8688的程序寄存器读取一个8位的值。
 * @param  cs_port CS引脚的GPIO端口。
 * @param  cs_pin  CS引脚的GPIO引脚号。
 * @param  addr    要读取的8位程序寄存器地址。
 * @retval uint8_t 从指定地址读取到的8位数据。
 */
uint8_t ADS8688_Read_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr)
{
    uint8_t received_data;

    // --- 第一次SPI传输：发送读地址命令 ---
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);
    // 发送读命令帧的第一个字节: (ADDR << 1)
    LL_SPI_TransmitReceive_Polling(SPI1, (addr << 1));
    // 发送一个Dummy字节
    LL_SPI_TransmitReceive_Polling(SPI1, 0x00);
    LL_GPIO_SetOutputPin(cs_port, cs_pin);

    // 必须有一个极短的延时来拉高CS，以分隔两次传输
    for (volatile int i = 0; i < 10; i++); // 短暂延时

    // --- 第二次SPI传输：发送NO_OP并接收数据 ---
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);
    // 发送一个NO_OP命令 (0x0000)
    LL_SPI_TransmitReceive_Polling(SPI1, 0x00);
    received_data = LL_SPI_TransmitReceive_Polling(SPI1, 0x00);
    LL_GPIO_SetOutputPin(cs_port, cs_pin);

    return received_data;
}

/**
 * @brief  初始化ADS8688设备，配置其进入自动扫描模式。
 * @param  cs_port CS引脚的GPIO端口。
 * @param  cs_pin  CS引脚的GPIO引脚号。
 * @retval None
 */
void ADS8688_Device_Init(GPIO_TypeDef* cs_port, uint16_t cs_pin)
{
    // 步骤 1: 发送软件复位命令
    ADS8688_Write_Command(cs_port, cs_pin, CMD_RST);

    // 步骤 2: 使能所有通道(CH0-CH7)进入自动扫描序列
    ADS8688_Write_Program(cs_port, cs_pin, REG_AUTO_SEQ_EN, 0xFF);

    // 步骤 3: 启动自动扫描模式
    ADS8688_Write_Command(cs_port, cs_pin, CMD_AUTO_RST);

}
