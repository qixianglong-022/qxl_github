/**
 ******************************************************************************
 * @file    ads8688.c
 * @brief   ADS8688 ADC �������� (LL��汾)
 * @author  (��������)
 * @date    (��ǰ����)
 *
 * @details
 * �������ļ��ṩ����������� (TI) ADS8688 16λ��8ͨ��ADC����ͨ��
 * ����ĺ��Ĺ��ܡ�����װ��ͨ��SPI���������/д�ڲ�����Ĵ�����
 * �ײ���������ṩ��һ���߼���ʼ�����������ڽ��豸����Ϊ���
 * ���Զ�ͨ��ɨ��ģʽ���˰汾����ȫǨ����STM32 LL�����������ܡ�
 ******************************************************************************
 */

#include "ads8688.h"
#include "main.h" // ���� LL ��ͷ�ļ�
#include "stm32f4xx_hal.h" // ��Ϊ��ʹ�� HAL_Delay

// --- ˽�и���������ʹ��LL������ѯ��ʽ�շ�һ���ֽ� ---
static uint8_t LL_SPI_TransmitReceive_Polling(SPI_TypeDef* SPIx, uint8_t data)
{
    // �ȴ����ͻ�����Ϊ��
    while (LL_SPI_IsActiveFlag_TXE(SPIx) == 0);
    // ��������
    LL_SPI_TransmitData8(SPIx, data);

    // �ȴ����ջ������ǿ�
    while (LL_SPI_IsActiveFlag_RXNE(SPIx) == 0);
    // ��ȡ�����ؽ��յ�������
    return LL_SPI_ReceiveData8(SPIx);
}

/**
 * @brief  ��ADS8688����һ��16λ�����
 * @param  cs_port ����ADS8688Ƭѡ(CS)���ŵ�GPIO�˿� (���� GPIOA)��
 * @param  cs_pin  ����ADS8688Ƭѡ(CS)���ŵ�GPIO���ź� (���� LL_GPIO_PIN_4)��
 * @param  com     Ҫ���͵�16λ���
 * @retval None
 */
void ADS8688_Write_Command(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint16_t com)
{
    // ����Ƭѡ��ѡ�д��豸
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);

    // ���͸�8λ
    LL_SPI_TransmitReceive_Polling(SPI1, (uint8_t)(com >> 8));
    // ���͵�8λ
    LL_SPI_TransmitReceive_Polling(SPI1, (uint8_t)com);

    // ����Ƭѡ������ͨ��
    LL_GPIO_SetOutputPin(cs_port, cs_pin);
}

/**
 * @brief  ��ADS8688�ĳ���Ĵ���д��һ��8λ��ֵ��
 * @param  cs_port CS���ŵ�GPIO�˿ڡ�
 * @param  cs_pin  CS���ŵ�GPIO���źš�
 * @param  addr    Ҫд���8λ����Ĵ�����ַ��
 * @param  data    Ҫд���8λ���ݡ�
 * @retval None
 */
void ADS8688_Write_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr, uint8_t data)
{
    // ����Ƭѡ��ѡ�д��豸
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);

    // ����д����֡�ĵ�һ���ֽ�: (ADDR << 1) | 0x01
    LL_SPI_TransmitReceive_Polling(SPI1, (addr << 1) | 0x01);
    // ����Ҫд�������
    LL_SPI_TransmitReceive_Polling(SPI1, data);

    // ����Ƭѡ������ͨ��
    LL_GPIO_SetOutputPin(cs_port, cs_pin);
}

/**
 * @brief  ��ADS8688�ĳ���Ĵ�����ȡһ��8λ��ֵ��
 * @param  cs_port CS���ŵ�GPIO�˿ڡ�
 * @param  cs_pin  CS���ŵ�GPIO���źš�
 * @param  addr    Ҫ��ȡ��8λ����Ĵ�����ַ��
 * @retval uint8_t ��ָ����ַ��ȡ����8λ���ݡ�
 */
uint8_t ADS8688_Read_Program(GPIO_TypeDef* cs_port, uint16_t cs_pin, uint8_t addr)
{
    uint8_t received_data;

    // --- ��һ��SPI���䣺���Ͷ���ַ���� ---
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);
    // ���Ͷ�����֡�ĵ�һ���ֽ�: (ADDR << 1)
    LL_SPI_TransmitReceive_Polling(SPI1, (addr << 1));
    // ����һ��Dummy�ֽ�
    LL_SPI_TransmitReceive_Polling(SPI1, 0x00);
    LL_GPIO_SetOutputPin(cs_port, cs_pin);

    // ������һ�����̵���ʱ������CS���Էָ����δ���
    for (volatile int i = 0; i < 10; i++); // ������ʱ

    // --- �ڶ���SPI���䣺����NO_OP���������� ---
    LL_GPIO_ResetOutputPin(cs_port, cs_pin);
    // ����һ��NO_OP���� (0x0000)
    LL_SPI_TransmitReceive_Polling(SPI1, 0x00);
    received_data = LL_SPI_TransmitReceive_Polling(SPI1, 0x00);
    LL_GPIO_SetOutputPin(cs_port, cs_pin);

    return received_data;
}

/**
 * @brief  ��ʼ��ADS8688�豸������������Զ�ɨ��ģʽ��
 * @param  cs_port CS���ŵ�GPIO�˿ڡ�
 * @param  cs_pin  CS���ŵ�GPIO���źš�
 * @retval None
 */
void ADS8688_Device_Init(GPIO_TypeDef* cs_port, uint16_t cs_pin)
{
    // ���� 1: ���������λ����
    ADS8688_Write_Command(cs_port, cs_pin, CMD_RST);

    // ���� 2: ʹ������ͨ��(CH0-CH7)�����Զ�ɨ������
    ADS8688_Write_Program(cs_port, cs_pin, REG_AUTO_SEQ_EN, 0xFF);

    // ���� 3: �����Զ�ɨ��ģʽ
    ADS8688_Write_Command(cs_port, cs_pin, CMD_AUTO_RST);

}
