/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 * @author         : (您的姓名)
 * @date           : (当前日期)
 *
 * @details
 * 本文件是整个ADC数据采集与网络传输系统的入口。`main`函数负责：
 * 1. 执行HAL库和MCU的基本初始化 (HAL_Init, SystemClock_Config)。
 * 2. 初始化所有用到的硬件外设，包括GPIO, DMA, SPI, TIM, USART, 以及LwIP网络协议栈。
 * 3. 初始化自定义的ADC处理模块 (ADC_Processing_Init) 和日志模块 (Log_Init)。
 * 4. 启动ADC采集定时器，开始整个数据采集流程 (ADC_Processing_Start)。
 * 5. 进入无限循环，在循环中持续调用LwIP网络处理函数和ADC数据处理任务，
 * 并处理来自调试日志系统的消息打印。
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
#include "adc_processing.h" // 包含我们的ADC处理模块头文件
#include "bsp_led.h"
#include <stdio.h>          // 包含标准输入输出头文件以使用printf
#include "debug_log.h"      // 包含自定义的日志系统头文件
#include "stm32f4xx_hal.h"  // 包含HAL库头文件以使用HAL_Delay
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// 定义ADS8688的片选端口和引脚，方便管理
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

extern struct netif gnetif; // <--- 声明在lwip.c中定义的全局网络接口变量
// --- 【新增】从 adc_processing.c 引用监控所需的全局变量 ---
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
 * @brief  重定向C库函数fputc到USART1。
 * @param  ch   要发送的字符。
 * @param  f    文件指针 (未使用)。
 * @retval int  返回发送的字符。
 * @details
 * 通过重定向fputc，我们可以直接使用标准C库中的printf函数，
 * 其输出将通过USART1发送出去，极大地简化了串口调试信息的打印。
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
   * --- 【最终的、关键的修正】---
   * 在任何其他初始化之前，首先禁用数据缓存（D-Cache）。
   * 这将解决CPU缓存与以太网DMA之间潜在的一致性冲突。
   * 注意：这通常是一个诊断步骤，最终的优化方案是在必要时进行缓存维护。
   * 但对于验证问题根源，这是最有效的方法。
   */
  // SCB_DisableDCache(); // 如果您的头文件中没有直接定义，请使用下面的CMSIS函数
	
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

    // 1. 等待网络接口启动 (Link Up)
    // 这个循环会等待以太网物理连接成功 (网线插好，PHY芯片链接成功)
    uint32_t tickstart = HAL_GetTick();


    // 初始化自定义的日志系统
    Log_Init(); //

    // 初始化ADC处理模块，包括ADS8688芯片的初始化和UDP的设置
    ADC_Processing_Init(); //
    printf("ADS8688 & ADC Processing Initialized.\r\n");

    // 延时一小会，等待网络协议栈和PHY芯片稳定
    //HAL_Delay(1000);

    // 启动ADC数据采集，此函数会启动TIM2定时器
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

			
        /* --- 核心任务调度 --- */

        // 1. LwIP网络协议栈处理任务
        //    此函数必须在主循环中被持续调用，以处理网络包的接收、发送和TCP/IP状态机。
        MX_LWIP_Process();
			
        // 2. 我们的ADC数据处理任务
        //    此函数检查是否有采集请求或已满的数据缓冲区，并执行相应操作。
        ADC_Processing_Task(); 
				// =================================================================
				// ===========        周期性状态监控模块 (保持不变)      ===========
				// =================================================================
				if (HAL_GetTick() - last_status_tick > 5000)
				{
						last_status_tick = HAL_GetTick();
						printf("\n--- Status Update ---\n");
						printf("  Network Link: %s\n", netif_is_link_up(&gnetif) ? "UP" : "DOWN");
						printf("  STM32 IP: %s\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
						// 新增UDP包发送计数器监控
						printf("  UDP Packets Sent: %lu\n", g_udp_packets_sent_count);
						printf("----------------------\n");
				}

        Log_Process(); //

		/* USER CODE END 3 */
		}
}

/**
 * @brief  配置系统时钟
 * @retval None
 * @details
 * 此函数配置了系统的主时钟源、PLL（锁相环）参数以及AHB/APB总线的时钟分频。
 * - 使用内部高速振荡器 (HSI, 16MHz) 作为PLL时钟源。
 * - PLL配置：PLLM=16, PLLN=336, PLLP=2。
 * - 系统时钟 (SYSCLK) = (HSI/PLLM) * PLLN / PLLP = (16/16) * 336 / 2 = 168MHz。
 * - AHB总线时钟 (HCLK) = SYSCLK / 1 = 168MHz。
 * - APB1总线时钟 (PCLK1) = HCLK / 4 = 42MHz (TIM2定时器挂载于此总线)。
 * - APB2总线时钟 (PCLK2) = HCLK / 2 = 84MHz (SPI1挂载于此总线)。
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
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI; // 指定振荡器类型为内部高速振荡器 (HSI)作为时钟源
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;             // 开启HSI
    RCC_OscInitStruct.HSEState            = RCC_HSE_OFF;            // 关闭外部高速振荡器 (HSE)
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;             // 开启锁相环 (PLL)，因为需要将 HSI 的 16MHz 提升到更高的系统频率。
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;      // PLL源为HSI
    RCC_OscInitStruct.PLL.PLLM            = 16;											// 对 HSI (16MHz) 进行 16 分频，得到 1MHz 的中间频率
    RCC_OscInitStruct.PLL.PLLN            = 336;										// 将 1MHz 的中间频率进行 336 倍频，得到 336MHz
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV2;					// 将 336MHz 进行 2 分频，最终得到 168MHz。这是系统的主时钟 (SYSCLK)
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
    /* 用户可以在这里添加自己的实现来报告HAL错误返回状态 */
    __disable_irq();
    while (1)
    {
        // 进入死循环，通常会在这里通过闪烁LED等方式指示错误
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
    /* 用户可以添加自己的实现来报告文件名和行号，
       例如: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

