/*
 * ads8688.h
 *
 * Created on: Jun 18, 2025
 * Author: Your Name
 */

#ifndef INC_ADS8688_H_
#define INC_ADS8688_H_

#include "main.h"

//================================================================
// 宏定义
//================================================================
// 命令寄存器 (16-bit commands)
#define CMD_NO_OP				0x0000	// 继续操作
#define CMD_RST					0x8500	// 复位 (更正了原始代码中的错误注释)
#define CMD_AUTO_RST			0xA000	// 重启后启动自动模式

// 程序寄存器地址 (8-bit addresses)
#define REG_AUTO_SEQ_EN			0x01	// 自动扫描排序控制寄存器
// ... 其他宏定义保持不变 ...

//================================================================
// 外部函数声明 (修改后)
//================================================================
// 不再需要传递 SPI_HandleTypeDef*，因为我们将直接使用 SPI1
void ADS8688_Device_Init(GPIO_TypeDef* cs_port, uint16_t cs_pin);
void ADS8688_Write_Command(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint16_t com);
void ADS8688_Write_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr, uint8_t data);
uint8_t ADS8688_Read_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr);


#endif /* INC_ADS8688_H_ */
