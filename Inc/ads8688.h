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
// �궨��
//================================================================
// ����Ĵ��� (16-bit commands)
#define CMD_NO_OP				0x0000	// ��������
#define CMD_RST					0x8500	// ��λ (������ԭʼ�����еĴ���ע��)
#define CMD_AUTO_RST			0xA000	// �����������Զ�ģʽ

// ����Ĵ�����ַ (8-bit addresses)
#define REG_AUTO_SEQ_EN			0x01	// �Զ�ɨ��������ƼĴ���
// ... �����궨�屣�ֲ��� ...

//================================================================
// �ⲿ�������� (�޸ĺ�)
//================================================================
// ������Ҫ���� SPI_HandleTypeDef*����Ϊ���ǽ�ֱ��ʹ�� SPI1
void ADS8688_Device_Init(GPIO_TypeDef* cs_port, uint16_t cs_pin);
void ADS8688_Write_Command(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint16_t com);
void ADS8688_Write_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr, uint8_t data);
uint8_t ADS8688_Read_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr);


#endif /* INC_ADS8688_H_ */
