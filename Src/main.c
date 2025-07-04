/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 * @author         : (��������)
 * @date           : (��ǰ����)
 *
 * @details
 * ���ļ�������ADC���ݲɼ������紫��ϵͳ����ڡ�`main`��������
 * 1. ִ��HAL���MCU�Ļ�����ʼ�� (HAL_Init, SystemClock_Config)��
 * 2. ��ʼ�������õ���Ӳ�����裬����GPIO, DMA, SPI, TIM, USART, �Լ�LwIP����Э��ջ��
 * 3. ��ʼ���Զ����ADC����ģ�� (ADC_Processing_Init) ����־ģ�� (Log_Init)��
 * 4. ����ADC�ɼ���ʱ������ʼ�������ݲɼ����� (ADC_Processing_Start)��
 * 5. ��������ѭ������ѭ���г�������LwIP���紦������ADC���ݴ�������
 * ���������Ե�����־ϵͳ����Ϣ��ӡ��
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "lwip.h"
#include "lwip/netif.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "adc_processing.h" // �������ǵ�ADC����ģ��ͷ�ļ�
#include "bsp_led.h"
#include <stdio.h>          // ������׼�������ͷ�ļ���ʹ��printf
#include "debug_log.h"      // �����Զ������־ϵͳͷ�ļ�
#include "stm32f4xx_hal.h"  // ����HAL��ͷ�ļ���ʹ��HAL_Delay
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// ����ADS8688��Ƭѡ�˿ں����ţ��������
#define CS1_PORT GPIOA
#define CS1_PIN  GPIO_PIN_4
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
extern volatile uint16_t g_debug_last_adc_value;
extern volatile uint8_t  g_debug_new_value_flag;

extern struct netif gnetif; // <--- ������lwip.c�ж����ȫ������ӿڱ���
// --- ���������� adc_processing.c ���ü�������ȫ�ֱ��� ---
extern volatile int8_t g_process_buffer_idx;
extern volatile uint32_t g_sample_count;
extern volatile uint32_t g_udp_packets_sent;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
extern volatile uint8_t g_pc_ready_for_data;
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
 * @brief  �ض���C�⺯��fputc��USART1��
 * @param  ch   Ҫ���͵��ַ���
 * @param  f    �ļ�ָ�� (δʹ��)��
 * @retval int  ���ط��͵��ַ���
 * @details
 * ͨ���ض���fputc�����ǿ���ֱ��ʹ�ñ�׼C���е�printf������
 * �������ͨ��USART1���ͳ�ȥ������ؼ��˴��ڵ�����Ϣ�Ĵ�ӡ��
 */
int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

	  /*
   * --- �����յġ��ؼ���������---
   * ���κ�������ʼ��֮ǰ�����Ƚ������ݻ��棨D-Cache����
   * �⽫���CPU��������̫��DMA֮��Ǳ�ڵ�һ���Գ�ͻ��
   * ע�⣺��ͨ����һ����ϲ��裬���յ��Ż��������ڱ�Ҫʱ���л���ά����
   * ��������֤�����Դ����������Ч�ķ�����
   */
  // SCB_DisableDCache(); // �������ͷ�ļ���û��ֱ�Ӷ��壬��ʹ�������CMSIS����
	
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
	
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LWIP_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  MX_SPI3_Init();
  MX_USART1_UART_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
    printf("\r\n\r\n--- System Start ---\r\n");
    printf("Waiting for network interface to be up...\r\n");

    // 1. �ȴ�����ӿ����� (Link Up)
    // ���ѭ����ȴ���̫���������ӳɹ� (���߲�ã�PHYоƬ���ӳɹ�)
    uint32_t tickstart = HAL_GetTick();


    // ��ʼ���Զ������־ϵͳ
    Log_Init(); //

    // ��ʼ��ADC����ģ�飬����ADS8688оƬ�ĳ�ʼ����UDP������
    ADC_Processing_Init(); //
    printf("ADS8688 & ADC Processing Initialized.\r\n");

    // ��ʱһС�ᣬ�ȴ�����Э��ջ��PHYоƬ�ȶ�
    //HAL_Delay(1000);

    // ����ADC���ݲɼ����˺���������TIM2��ʱ��
    ADC_Processing_Start(); //
    printf("ADC Acquisition Started. Waiting for data...\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
		uint32_t last_status_tick = HAL_GetTick();
	
    while (1)
    {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

			
        /* --- ����������� --- */

        // 1. LwIP����Э��ջ��������
        //    �˺�����������ѭ���б��������ã��Դ���������Ľ��ա����ͺ�TCP/IP״̬����
        MX_LWIP_Process();
			
        // 2. ���ǵ�ADC���ݴ�������
        //    �˺�������Ƿ��вɼ���������������ݻ���������ִ����Ӧ������
        ADC_Processing_Task(); 
				// =================================================================
				// ===========        ������״̬���ģ�� (���ֲ���)      ===========
				// =================================================================
				if (HAL_GetTick() - last_status_tick > 5000)
				{
						last_status_tick = HAL_GetTick();
						printf("\n--- Status Update ---\n");
						printf("  Network Link: %s\n", netif_is_link_up(&gnetif) ? "UP" : "DOWN");
						printf("  STM32 IP: %s\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
						// ����UDP�����ͼ��������
						printf("  UDP Packets Sent: %lu\n", g_udp_packets_sent_count);
						printf("----------------------\n");
				}

        Log_Process(); //

		/* USER CODE END 3 */
		}
}

/**
 * @brief  ����ϵͳʱ��
 * @retval None
 * @details
 * �˺���������ϵͳ����ʱ��Դ��PLL�����໷�������Լ�AHB/APB���ߵ�ʱ�ӷ�Ƶ��
 * - ʹ���ڲ��������� (HSI, 16MHz) ��ΪPLLʱ��Դ��
 * - PLL���ã�PLLM=16, PLLN=336, PLLP=2��
 * - ϵͳʱ�� (SYSCLK) = (HSI/PLLM) * PLLN / PLLP = (16/16) * 336 / 2 = 168MHz��
 * - AHB����ʱ�� (HCLK) = SYSCLK / 1 = 168MHz��
 * - APB1����ʱ�� (PCLK1) = HCLK / 4 = 42MHz (TIM2��ʱ�������ڴ�����)��
 * - APB2����ʱ�� (PCLK2) = HCLK / 2 = 84MHz (SPI1�����ڴ�����)��
 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI; // ָ����������Ϊ�ڲ��������� (HSI)��Ϊʱ��Դ
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;             // ����HSI
    RCC_OscInitStruct.HSEState            = RCC_HSE_OFF;            // �ر��ⲿ�������� (HSE)
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;             // �������໷ (PLL)����Ϊ��Ҫ�� HSI �� 16MHz ���������ߵ�ϵͳƵ�ʡ�
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;      // PLLԴΪHSI
    RCC_OscInitStruct.PLL.PLLM            = 16;											// �� HSI (16MHz) ���� 16 ��Ƶ���õ� 1MHz ���м�Ƶ��
    RCC_OscInitStruct.PLL.PLLN            = 336;										// �� 1MHz ���м�Ƶ�ʽ��� 336 ��Ƶ���õ� 336MHz
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV2;					// �� 336MHz ���� 2 ��Ƶ�����յõ� 168MHz������ϵͳ����ʱ�� (SYSCLK)
    RCC_OscInitStruct.PLL.PLLQ            = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
    /* �û���������������Լ���ʵ��������HAL���󷵻�״̬ */
    __disable_irq();
    while (1)
    {
        // ������ѭ����ͨ����������ͨ����˸LED�ȷ�ʽָʾ����
    }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
    /* �û���������Լ���ʵ���������ļ������кţ�
       ����: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

