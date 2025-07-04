## 项目经历

### 1. 项目整体总结

​       本项目成功实现了一个基于STM32F407系列微控制器的、高性能、多通道数据采集系统。该系统能够以**25 ksps/通道**的速率，对**8个通道**的模拟信号进行同步采样，并将采集到的16位精度的实时数据，通过以太网以**UDP协议**高速、稳定地传输至上位机（PC），其理论数据吞吐率高达**400 KB/s**。（25ksps × 8ch × 2b_16bits = 400kb/s）

### 2. 数据采集链路

#### （1）整体总览

- **核心目标**: **准确、高速、无间断**地将模拟信号数字化。

- **核心硬件**: 外部ADC芯片**ADS8688**。这是一款16位、8通道的模数转换器，通过SPI接口与STM32通信。
- **触发机制**: 使用**TIM2定时器**作为系统的心跳。定时器被配置为以极高的频率（`210kHz`）产生中断。每一次中断，都代表着系统需要启动一次对单个通道的采样。
- **执行流程**: 

```
[TIM2 中断] ---> [设置标志位 g_start_acquisition_flag]
     |
     V
[主循环 a'a'a'a'a'a'a'] ---> [ADC_Processing_Task 检测到标志位]
     |
     V
[配置 DMA & SPI] ---> [拉低 CS -> 使能 SPI -> 启动 DMA]
     |
     V
[DMA & SPI 硬件协同工作 (后台自动)] ---> [DMA 传输完成]
     |
     V
[DMA 完成中断] ---> [SPI1_DMA_RX_Callback 执行] ---> [提取数据 -> 存入乒乓缓冲 -> 清除 DMA 忙标志]
```

#### （2）分步解析

##### 第1步：心跳的产生 (TIM2 中断)

- **执行文件**: `stm32f4xx_it.c`
- **触发函数**: `TIM2_IRQHandler(void)`

```c
void TIM2_IRQHandler(void)
{
//  printf("TIM2 Interrupt Triggered!\n");
  // 检查是否是更新中断
  if(LL_TIM_IsActiveFlag_UPDATE(TIM2) == 1)
  {
    // 清除更新中断标志位
    LL_TIM_ClearFlag_UPDATE(TIM2);
    // 定时器中断触发一次采集
    // 检查DMA是否空闲，防止重入
    if (g_dma_busy_flag == 0)
    {
      g_dma_busy_flag = 1;            // 设置DMA忙标志
      g_start_acquisition_flag = 1;   // 请求主循环启动一次DMA传输
    }
    else
    {
        //Log_Debug("IT: DMA Busy! Skipping one sample.");	//错误计数器或日志
    }
  }
}
```

- **原理**: `TIM2`定时器以我们上面计算出的精确频率（例如400kHz）溢出，硬件自动触发Cortex-M4内核的中断机制，CPU暂停当前任务，跳转到`TIM2_IRQHandler`函数。
- **执行内容**:
  1. `LL_TIM_ClearFlag_UPDATE(TIM2)`: 首先清除中断标志，否则中断会无限触发。
  2. `if (g_dma_busy_flag == 0)`: 这是一个至关重要的**“保护锁”**。它检查上一次的DMA采集是否已经完成。如果DMA仍然“忙”，说明采样率可能过高，CPU或DMA处理不过来。本次中断将什么都不做直接退出，这是为了防止因系统处理不过来而导致的数据请求“堆积”和重入错误。
  3. `g_dma_busy_flag = 1;`: 如果DMA空闲，立刻“上锁”，表示我们将要启动一次新的采集。
  4. `g_start_acquisition_flag = 1;`: 设置一个全局标志位。这是中断与主循环之间最高效的通信方式，避免在中断服务程序中执行复杂操作。

##### 第2步：任务的派发 (主循环)

- **执行文件**: `main.c` -> `adc_processing.c`
- **触发函数**: `while(1)` 循环中的 `ADC_Processing_Task()`

```c
while (1)
{
    /* --- 核心任务调度 --- */

    // 1. LwIP网络协议栈处理任务
    //    此函数必须在主循环中被持续调用，以处理网络包的接收、发送和TCP/IP状态机。
    MX_LWIP_Process();

    // 2. 我们的ADC数据处理任务
    //    此函数检查是否有采集请求或已满的数据缓冲区，并执行相应操作。
    ADC_Processing_Task(); 
    
    // ===========        周期性状态监控模块         ===========
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
    Log_Process(); //日志输出帮助DEBUG
}
```

- **原理**: 主循环在后台不断地轮询。它的速度远快于定时器中断。
- **执行内容**:
  1. `if (g_start_acquisition_flag)`: `ADC_Processing_Task`函数检测到中断设置的标志位。
  2. `g_start_acquisition_flag = 0;`: 清除标志，表示任务已被接收。
  3. **进入核心采集启动流程**: 接下来，它会执行一系列的配置，为硬件的自动数据搬运做好准备。

##### 第3步：硬件的“编程” (DMA & SPI配置)

- **执行文件**: `adc_processing.c`
- **触发函数**: `ADC_Processing_Task()` 内
- **原理**: 在这一步，CPU就像一个指挥官，它不亲自搬运数据，而是对DMA和SPI这两个“自动化机器人”进行编程，告诉它们“待会儿要做什么”。

```c
void ADC_Processing_Task(void)
{
    // --- 任务1: 处理定时器触发的DMA采集请求 ---
    if (g_start_acquisition_flag)
    {
        g_start_acquisition_flag = 0; // 清除标志，表示我们已开始处理
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
```

- **执行内容**:
  1. **复位与配置**: 首先禁用DMA和SPI，确保它们处于一个已知的初始状态。然后配置DMA的传输长度（4字节）、源地址（SPI1->DR）、目标地址（`g_dma_rx_buffer`）等。
  2. **清除旧标志**: **这是至关重要的一步**，如我们之前调试发现的，必须用`LL_DMA_ClearFlag_TCx`清除所有相关DMA流的上一次传输完成标志，否则硬件会认为任务“已经做完了”而拒绝启动。
  3. **使能中断**: `LL_DMA_EnableIT_TC()`告诉DMA控制器：“当你完成任务后，请触发一个中断通知CPU”。
  4. **建立连接**: `LL_SPI_EnableDMAReq_RX()`在SPI和DMA之间建立了一条“硬件握手线”。当SPI接收到新数据时，它会自动通知DMA来取走。
  5. **启动序列**: 最终，按照**CS拉低 -> SPI使能 -> DMA启动**的可靠顺序，正式启动整个硬件采集流程。CPU的工作到此暂告一段落，接下来它会继续执行主循环的其他任务（比如检查网络发送）。

##### 第4步：数据的自动飞行 (硬件协同)

- **执行者**: STM32的**硬件** (DMA控制器, SPI外设, 总线矩阵)
- **原理**: 这是一个完全在后台、无需CPU干预的自动化流程。

```c
//	伪代码：
static uint8_t g_dma_tx_buffer[4] = {0x00, 0x00, 0x00, 0x00};
static uint8_t g_dma_rx_buffer[4] = {0};

    LL_DMA_SetPeriphAddress(DMA2, LL_DMA_STREAM_0, (uint32_t)&(SPI1->DR));
    LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_0, (uint32_t)g_dma_rx_buffer);
    LL_DMA_SetPeriphAddress(DMA2, LL_DMA_STREAM_3, (uint32_t)&(SPI1->DR));
    LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_3, (uint32_t)g_dma_tx_buffer);
// 在SPI和CS都就绪后，启动DMA数据流
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_0); 
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_3); 
```

- **执行内容**:
  1. DMA Stream 3 (TX) 将一个“哑巴”字节从内存送到 `SPI1->DR` 寄存器。
  2. SPI1外设检测到发送数据寄存器非空，开始通过`MOSI`线向外发送数据，同时在`SCK`线上产生时钟信号。
  3. 外部ADC (ADS8688) 在`SCK`的驱动下，通过`MISO`线将它的数据发送给STM32。
  4. SPI1外设接收到数据，存入它的接收数据寄存器 `SPI1->DR`。
  5. SPI1通过硬件握手线通知DMA：“我收到一个字节了，快来取！”
  6. DMA Stream 0 (RX) 响应请求，通过总线矩阵直接将 `SPI1->DR` 中的数据搬运到内存中的 `g_dma_rx_buffer`。
  7. 这个过程会根据DMA配置的传输长度（4字节）自动重复。

##### 第5步：任务的完成与数据的处理 (DMA中断)

- **执行文件**: `stm32f4xx_it.c` -> `adc_processing.c`
- **触发函数**: `DMA2_Stream0_IRQHandler()` -> `SPI1_DMA_RX_Callback()`
- **原理**: 当DMA Stream 0成功搬运了4个字节后，它的硬件任务就完成了，于是触发之前配置好的“传输完成”中断。

```c
void DMA2_Stream0_IRQHandler(void)
{
    // 检查是否是“传输完成”中断
    if (LL_DMA_IsActiveFlag_TC0(DMA2) == 1)
    {
        LL_DMA_ClearFlag_TC0(DMA2); // 手动清除标志位
        SPI1_DMA_RX_Callback();     // 调用我们自己的回调函数
    }
    // 检查是否是“传输错误”中断
    else if (LL_DMA_IsActiveFlag_TE0(DMA2) == 1)
    {
        LL_DMA_ClearFlag_TE0(DMA2); // 手动清除标志位
        SPI1_DMA_Error_Callback();    // 调用错误处理回调
    }
}
```



```c
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
```

- **执行内容**:

  1. **拉高CS**: `LL_GPIO_SetOutputPin(CS1_PORT, CS1_PIN)`，结束本次与ADC的通信。
  2. **数据提取**: `uint16_t adc_16bit_value = ...`，从4字节的`g_dma_rx_buffer`中，**精确地提取出有意义的16位数据**。
  3. **存入缓冲**: 将这个16位的值存入当前活动的乒乓缓冲区 (`g_adc_ping_pong_buffer`)。
  4. **更新计数**: `g_sample_count++`，为填满整个大缓冲区做准备。`volatile uint8_t  g_acquisition_buffer_idx = 0; `   当前正在被DMA填充的缓冲区索引 (0或1)。
  5. **检查与切换**: 检查`g_sample_count`是否达到阈值`PING_PONG_BUFFER_SIZE`，如果达到，则执行乒乓切换逻辑。

  ```c
  // ** 数据采集参数 **
  #define CHANNELS_PER_SAMPLE     8       // ADC每次自动扫描的通道数
  #define SAMPLES_PER_CHANNEL     1024    // 每个通道采集的样本数
  // 每个乒乓缓冲区的总样本数 (注意：类型现在是uint16_t)
  #define PING_PONG_BUFFER_SIZE   (CHANNELS_PER_SAMPLE * SAMPLES_PER_CHANNEL)
  ```

  6. **“开锁”**: `g_dma_busy_flag = 0;`，**这是闭环的关键**。它清除了之前在TIM2中断中设置的“保护锁”，告诉系统：“本次采集已圆满完成，可以接受下一次采集请求了”。

- **SPI与DMA协同工作**: 这是实现高速采集的关键。
  - **软件逻辑**: 在`TIM2_IRQHandler`中，我们并不直接进行耗时的SPI读写，而是仅仅设置一个标志位`g_start_acquisition_flag`。
  - **硬件执行**: 在主循环的`ADC_Processing_Task`中，检测到该标志后，会配置**DMA2**（Stream 0用于RX，Stream 3用于TX）和**SPI1**。
  - **时序修正**: 项目演进中发现，必须采用**先拉低片选(CS) -> 再使能SPI -> 最后启动DMA**的顺序，才能保证DMA在完全就绪的通信总线上开始工作，解决了项目初期的“无法发送任何数据包”的停滞问题。
  - **数据提取**: 在DMA接收完成中断`SPI1_DMA_RX_Callback`中，我们从收到的4字节SPI数据包中，精确地提取出位于第3和第4字节的**有效16位ADC值**。这解决了项目中期的“数据值不正确”问题。

#### （3）细节探析

##### 总线时钟



##### TIM2定时器

- TIM2的**时钟源**：

  - 在`main.c`的`SystemClock_Config`函数中，可以看到APB1总线的时钟配置为：

    ```c
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    ```

  - 这意味着PCLK1（APB1总线时钟）的频率是HCLK/4；HCLK（AHB总线时钟）被设置为168MHz。因此PCLK1 = 168Mhz / 4 = 42Mhz。
  - 此外：STM32的定时器时钟有个特性，如果APB的分频系数不是1（这里是4），那么供给定时器的时钟频率就会自动翻倍。所以TIM2的最终时钟频率是PCLK1*2 = 42MHz * 2 = 84Mhz。 

- 分析`MX_TIM2_Init`中的参数

  - ```c
    void MX_TIM2_Init(void)
    {
      LL_TIM_InitTypeDef TIM_InitStruct = {0};
      LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
      NVIC_SetPriority(TIM2_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),0, 0));
      NVIC_EnableIRQ(TIM2_IRQn);
        // 预分频器的值是0。这意味着定时器时钟不进行分频，计数器的时钟频率就是 84MHz。
      TIM_InitStruct.Prescaler = 0;
      TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
        // 自动重装载寄存器（ARR）的值是399。
      TIM_InitStruct.Autoreload = 399;
      TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
      LL_TIM_Init(TIM2, &TIM_InitStruct);
      LL_TIM_DisableARRPreload(TIM2);
      LL_TIM_SetClockSource(TIM2, LL_TIM_CLOCKSOURCE_INTERNAL);
      LL_TIM_SetTriggerOutput(TIM2, LL_TIM_TRGO_RESET);
      LL_TIM_DisableMasterSlaveMode(TIM2);
      LL_TIM_EnableIT_UPDATE(TIM2);
    }
    ```

  - 自动重装载寄存器（ARR）的值是399。意味着定时器的计数器会从0开始，每经过一个时钟周期（1/84Mhz秒）就加1，直到计数到399，在下一个时钟 周期到400时，每触发一次“更新事件”并产生中断，同时计数器清零，开始下一轮计数。

  - 所以，触发一次中断所需要的时钟周期数时`Autoreload + 1 = 399 + 1 = 400`个周期。

- 计算最终中断频率：

  - 中断频率 = 定时器时钟源频率 / 预分频 +1 / 自动重装载值 +1 

  - 中断频率 = 84Mhz / (0 + 1) / (399 +1 ) = 210Khz

- `疑问：为什么不能div2，div8等。只要ARR对应减小呢？`

### 3.  数据缓冲策略

#### （1）整体总览

数据缓冲链路是连接**“刚性”的数据采集**和**“弹性”的网络发送**之间的桥梁和“蓄水池”。它的设计目标是**在不丢失任何一个采样点的前提下，平滑高速数据流，应对网络延迟和抖动**。

**执行流程图:**

```
[采集链路] ---> [数据写入 g_adc_ping_pong_buffer[A]]
     |
     +-----> [缓冲区 A 填满] ---> [触发“乒乓切换”]
     |                                      |
     V                                      V
[采集链路] ---> [数据写入 g_adc_ping_pong_buffer[B]]  [网络发送链路] <--- [读取 g_adc_ping_pong_buffer[A]]
```

#### （2）分步解析

##### 第1步：核心组件 - 乒乓双缓冲 (Ping-Pong Buffer)

- **执行文件**: `adc_processing.c`
- **核心变量**: `static uint16_t g_adc_ping_pong_buffer[2][PING_PONG_BUFFER_SIZE];`
- **原理**:
  1. **结构**: 在内存中开辟了两个完全相同、且连续的大数组。我们称它们为**缓冲区0 (Ping)和缓冲区1 (Pong)**。
  2. **位置 (CCMRAM)**: 这个巨大的缓冲区（`8通道 * 1024点 * 2字节 = 16KB` * 2 = 32KB）被明确地放置在CCMRAM中。这是一个战略性的选择，因为它**避免了消耗宝贵的主SRAM**，将主SRAM留给了对所有总线主设备（特别是以太网DMA）都必须可见的LwIP协议栈。
  3. **状态指针**: 整个机制靠三个全局变量来驱动：
     - `g_acquisition_buffer_idx`: 指示DMA当前应该向哪个缓冲区（0或1）写入数据。
     - `g_sample_count`: 记录当前正在写入的缓冲区已经填充了多少个样本。
     - `g_process_buffer_idx`: 指示网络任务当前应该从哪个缓冲区（0或1）读取数据并发送。如果值为-1，则表示没有需要发送的数据。

```c
// ** 数据采集参数 **
#define CHANNELS_PER_SAMPLE     8       // ADC每次自动扫描的通道数
#define SAMPLES_PER_CHANNEL     1024    // 每个通道采集的样本数
// 每个乒乓缓冲区的总样本数 (注意：类型现在是uint16_t)
#define PING_PONG_BUFFER_SIZE   (CHANNELS_PER_SAMPLE * SAMPLES_PER_CHANNEL)
```

##### 第2步：数据填充与“满载”检测

- **执行文件**: `adc_processing.c`
- **触发函数**: `SPI1_DMA_RX_Callback()`
- **原理**: 每当数据采集链路成功获取一个16位的`adc_16bit_value`时，此回调函数被执行。
- **执行内容**:
  1. **写入**: `g_adc_ping_pong_buffer[g_acquisition_buffer_idx][g_sample_count] = adc_16bit_value;` CPU将刚刚收到的样本点，精准地放入由`g_acquisition_buffer_idx`指定的活动缓冲区中，位置由`g_sample_count`决定。
  2. **计数**: `g_sample_count++`。
  3. **满载检测**: `if (g_sample_count >= PING_PONG_BUFFER_SIZE)`。当`g_sample_count`达到最大值（例如8192）时，意味着当前缓冲区已被完全填满。

```c
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
```

##### 第3步：“乒乓切换” - 核心调度逻辑

- **执行文件**: `adc_processing.c`
- **触发函数**: `SPI1_DMA_RX_Callback()` 内的 `if` 语句块
- **原理**: 这是整个缓冲机制的“大脑”，它在纳秒级别完成缓冲区的角色切换。
- **执行内容**:
  1. **背压检查 (Backpressure Check)**: `if (g_process_buffer_idx == -1)`。这是最关键的保护机制。它检查网络任务是否已经完成了上一个缓冲区的发送任务（即`g_process_buffer_idx`是否为-1）。
  2. **执行切换 (如果网络空闲)**:
     - `g_process_buffer_idx = g_acquisition_buffer_idx;` 将刚刚填满的缓冲区（例如缓冲区0）的索引，赋值给“待处理”指针。这相当于**对主循环发出一个信号：“嘿，缓冲区0已经准备好了，快来拿去发送！”**
     - `g_acquisition_buffer_idx = !g_acquisition_buffer_idx;` 将活动采集缓冲区指针翻转到另一个缓冲区（例如从0翻转到1）。从这一刻起，**所有新进的ADC样本都将开始写入缓冲区1**。
     - `g_sample_count = 0;` 重置样本计数器，为填充新的缓冲区做准备。
  3. **处理拥堵 (如果网络繁忙)**: 如果背压检查失败（`g_process_buffer_idx != -1`），意味着网络发送速度跟不上采集速度。此时，系统会**丢弃当前刚刚采集满的整个缓冲区的数据**，并直接在当前缓冲区上从头开始采集。这是一个健壮的设计，它以“丢弃最新数据”为代价，保证了系统不会因内存溢出或数据踩踏而崩溃。

##### 第4步：数据的消耗与缓冲区的释放

- **执行文件**: `adc_processing.c`
- **触发函数**: `ADC_Processing_Task()` -> `SendWaveformDataViaUDP()`
- **原理**: 主循环的`ADC_Processing_Task`不断轮询`g_process_buffer_idx`的值。

```c
**
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

```

- **执行内容**:
  1. **检测任务**: `if (g_process_buffer_idx != -1)`。一旦检测到有待处理的缓冲区，就调用`SendWaveformDataViaUDP`。
  2. **数据读取**: `SendWaveformDataViaUDP`函数会从`g_adc_ping_pong_buffer[g_process_buffer_idx]`中读取数据。它**只关心**`g_process_buffer_idx`指向的那个缓冲区，完全不影响另一个正在被DMA填充的缓冲区。
  3. **缓冲区释放**: 当`SendWaveformDataViaUDP`成功将一个完整的缓冲区数据全部发送完毕后，它做的最后一件事就是： `g_process_buffer_idx = -1;` 这个动作相当于**“释放了锁”**，告诉`SPI1_DMA_RX_Callback`中的切换逻辑：“我已经处理完这个缓冲区了，你可以把下一个准备好的缓冲区交给我了。”

### 4.  网路发送策略

#### （1）整体总览

网络发送链路是整个系统的“出口”，它负责将位于CCMRAM中的、已准备好的数据缓冲区，高效、可靠地发送到以太网。这个过程是“CPU数据搬运”策略的核心体现。

**执行流程图:**

```
[主循环检测到 g_process_buffer_idx != -1]
     |
     V
[调用 SendWaveformDataViaUDP()]
     |
     V
[进入 while 循环，直到整个大缓冲区发送完毕]
     |
     +-----> [1. pbuf_alloc: 从SRAM中分配一个LwIP数据包]
     |
     +-----> [2. memcpy: CPU将一小块数据从CCMRAM复制到SRAM中转区]
     |
     +-----> [3. pbuf_take: 将SRAM中转区的数据再次复制到LwIP数据包中]
     |
     +-----> [4. udp_send: 将LwIP数据包交给协议栈] ---> [硬件DMA从SRAM读取并发送]
     |
     +-----> [5. pbuf_free: 释放LwIP数据包]
     |
     +-----> [循环，直到所有数据块发送完毕]
     |
     V
[重置 g_process_buffer_idx = -1，释放大缓冲区]
```

#### （2）分步解析

##### 第1步：任务的触发与数据源定位

- **执行文件**: `adc_processing.c`

- **触发函数**: `ADC_Processing_Task()`

- **原理**: 主循环在`ADC_Processing_Task`中不断轮询`g_process_buffer_idx`。一旦它不为-1，就意味着有一个填满的乒乓缓冲区（例如缓冲区0）正等待发送。

- **执行内容**:

  - 调用`SendWaveformDataViaUDP()`。
  - 函数内部，首先通过 `uint8_t* ccm_buffer_ptr = (uint8_t*)g_adc_ping_pong_buffer[g_process_buffer_idx];` 获取到这个位于**CCMRAM**的、巨大的16KB数据块的**起始地址**。

  ```
  将 uint16_t* 强制转换为 uint8_t*，是为了统一操作单位。因为我们后续的所有内存偏移计算和 memcpy 操作都是基于字节的，所以我们必须将指针的“世界观”也临时切换到“字节视角”，以确保指针的移动和我们的计算逻辑完全匹配，从而实现对大块内存的精确、逐字节的操作。
  ```

  - 进入一个`while`循环，准备将这个大缓冲区“切片”成多个小的UDP包进行发送。

##### 第2步：为“远征”准备行囊 (`pbuf_alloc`)

- **执行函数**: `struct pbuf *p = pbuf_alloc(...)`
- **原理**: `pbuf`（Packet Buffer）是LwIP管理网络数据的核心结构。它不仅仅是一块内存，还包含了长度、下一个pbuf指针等管理信息。

```c
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
```

- **执行内容**:

  - 调用`pbuf_alloc`，请求LwIP从它位于**主SRAM**的内存堆中，分配一个大小为`UDP_PAYLOAD_SIZE`（1440字节）的`pbuf`。

  - ```c
    // ** UDP包净荷大小 **
    #define UDP_PAYLOAD_SIZE        1440    // bytes
    ```

  - **这是关键的第一步**：我们获得了一块以太网DMA**可以访问**的内存空间，由LwIP代为管理。

  - 如果返回`NULL`，说明LwIP的内存池暂时耗尽（通常是网络拥堵，发出的包来不及被确认和释放），函数会直接返回，等待下次轮询再试。

##### 第3步：CPU的搬运 (`memcpy` & `pbuf_take`)

- **执行函数**: `memcpy()` 和 `pbuf_take()`

- **原理**: 这是连接“不可访问的CCMRAM”和“可访问的SRAM”之间的桥梁，由CPU亲自完成。

- **执行内容**:

  1. **`memcpy(...)`**: CPU执行一条高效的内存复制指令，将`ccm_buffer_ptr + bytes_sent`地址开始的一小块（1440字节）数据，从**CCMRAM**快速复制到我们在SRAM中定义的临时中转缓冲区`udp_tx_sram_staging_buf`。
  2. **`pbuf_take(...)`**: 再次调用一个内存复制函数，将`udp_tx_sram_staging_buf`中的数据，复制到上一步`pbuf_alloc`所分配的`pbuf`的载荷（payload）区域。

  - **为什么需要两步复制？** 这是一个为了代码清晰和模块化的设计。`pbuf_take`是LwIP的标准API，它能确保数据被正确地放入`pbuf`结构中。先`memcpy`到SRAM中转区，再`pbuf_take`，虽然有两次复制，但由于CPU速度极快且数据量小，这点开销完全可以接受，并换来了与LwIP协议栈的清晰接口。



#### **第4步：启航！数据的交付与硬件接力 (`udp_send`)**

- **执行函数**: `err_t err = udp_send(g_upcb, p);`
- **原理**: 这是将数据正式交给LwIP协议栈的时刻。
- **执行内容**:
  1. CPU将包含SRAM数据的`pbuf`指针，传递给`udp_send`函数。
  2. LwIP协议栈会在`pbuf`的前面“加上”UDP头和IP头。
  3. LwIP将这个完整的包（`pbuf`链）传递给底层的以太网驱动（`ethernetif.c`）。
  4. 以太网驱动配置**以太网DMA**的描述符，让DMA的源地址指向`pbuf`的载荷地址（位于**主SRAM**）。
  5. **硬件接力**: 以太网DMA从SRAM中读取数据，同时硬件校验和模块在后台计算校验和，最终将完整的以太网帧通过PHY芯片发送出去。
  6. 如果`udp_send`返回错误（例如`ERR_MEM`），说明LwIP的内部发送队列已满，函数会释放`pbuf`并返回，等待下次轮询。



#### **第5步：卸下行囊，准备下一次旅程 (`pbuf_free` & 循环)**



- **执行函数**: `pbuf_free(p);`
- **原理**: `pbuf`是宝贵的内存资源，一旦交付给`udp_send`，应用层就可以认为它“没用了”，必须立即释放，以便LwIP可以回收并用于下一个数据包。
- **执行内容**:
  1. 调用`pbuf_free`将`pbuf`归还给LwIP的内存池。
  2. 更新`bytes_sent_from_current_buffer`计数。
  3. `while`循环继续，处理下一个1440字节的数据块，重复步骤2-5。
  4. 当整个16KB的缓冲区全部发送完毕后，循环结束。函数做的最后一件事就是`g_process_buffer_idx = -1;`，释放这个乒乓缓冲区，让它可以被采集任务再次使用。



### 5.  网络接收策略

#### （1）整体总览

PC端数据接收链路：从网络包到CSV文件的旅程。整条链路可以被看作一个三级流水线，由三个并行的线程和一条数据“传送带”（队列）组成。

**执行流程图:**

```
[网络接口 (NIC)] <--- UDP包
     |
     V
[1. 主线程 (Network Receiver)]
     | - recvfrom(): 接收UDP包
     | - 拼接UDP包 -> 重组为16KB数据帧
     | - 将(完整数据帧, 到达时间)放入队列
     V
[数据传送带: data_queue (Queue)]
     |
     V
[2. 文件写入线程 (Data Processor & Writer)]
     | - get(): 从队列取出数据帧
     | - 解包二进制数据 -> 16位整数
     | - 转换为电压值(mV)
     | - 插值计算时间戳
     | - 累积65536个样本
     | - 写入CSV文件
     V
[硬盘] ---> [voltage_data_xxxx.csv]

[3. 监控线程 (Performance Monitor)] <--- [主线程定期更新样本数]
     | - 每2秒计算一次采样率
     V
[控制台] ---> [打印实时性能]
```

##### 第一站：网络接收与数据帧重组链路 (主线程 `main`)

这个线程是整个系统的“门户”，它直接与网络硬件交互，负责接收最原始的数据包。

- **原理**: 它扮演一个**UDP服务器**的角色，持续监听一个固定的网络端口，等待STM32作为客户端将数据“扔”过来。其核心任务是**将STM32发送的、离散的、小块的UDP包，重新拼接成一个逻辑上完整的、巨大的数据帧**。
- **执行流程**:
  1. **初始化与绑定**:
     - `server_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)`: 创建一个UDP套接字。
     - `server_sock.bind((LISTEN_IP, UDP_PORT))`: 将套接字绑定到本地所有网络接口的5001端口上。这是服务器的“开门营业”动作。
     - `server_sock.settimeout(1.0)`: 设置一个1秒的接收超时。这是一个非常重要的健壮性设计，它防止`recvfrom`在没有数据时无限期地阻塞，使得主循环可以周期性地检查退出标志`stop_event`。
  2. **接收与拼接循环**:
     - `packet, addr = server_sock.recvfrom(RECV_BUFFER_SIZE)`: 这是核心的接收动作。程序会在这里等待，直到一个UDP包到达，或者1秒超时。`packet`是收到的原始二进制数据（例如1440字节），`addr`是发送方（STM32）的IP和端口。
     - `frame_buffer += packet`: **这是UDP包重组的关键**。它将新收到的`packet`像拼接积木一样，附加到`frame_buffer`这个字节串的末尾。
     - `while len(frame_buffer) >= FRAME_SIZE:`: 每次拼接后，都检查`frame_buffer`的长度是否达到了一个完整数据帧的大小（例如16384字节）。
     - `complete_frame = frame_buffer[:FRAME_SIZE]`: 如果长度足够，就从`frame_buffer`的头部精确地切下一个完整的数据帧。
     - `frame_buffer = frame_buffer[FRAME_SIZE:]`: 然后从`frame_buffer`中移除刚刚切走的部分，留下剩余不完整的下一个数据帧的开头。
  3. **任务交付**:
     - `frame_arrival_time = datetime.now()`: 在一个数据帧刚刚重组完成的瞬间，立刻捕获当前的系统时间。这个时间戳非常重要，它是后续所有时间计算的基准。
     - `data_queue.put_nowait((complete_frame, frame_arrival_time))`: 将这个**完整的16KB二进制数据帧**和它的**到达时间戳**打包成一个元组，放入`data_queue`这个线程安全的队列中。这相当于把处理好的“包裹”放上传送带，交给下一个工人。

##### 第二站：数据处理与存储链路 (文件写入线程 `file_writer_worker`)**

这个线程是系统的“加工车间”，它从传送带上取下包裹，进行拆包、换算、累积，最后打包成最终产品（CSV文件）。它与网络完全解耦，只关心处理数据。

- **原理**: 它扮演一个**消费者**的角色，不断地从`data_queue`中获取数据。其核心任务是**将二进制数据转化为有物理意义的、带时间戳的、人类可读的格式，并进行累积和存储**。
- **执行流程**:
  1. **获取任务**:
     - `raw_frame, frame_start_time = data_queue.get(timeout=1)`: 从队列中取出一个包裹。`get()`是一个阻塞操作，如果队列为空，它会等待，直到有新数据或超时。
  2. **数据解包与转换**:
     - `interleaved_raw_samples = struct.unpack(...)`: 使用`struct`库，按照STM32发送的格式（小端序 `<`，8192个 `H` 无符号短整型），将16KB的二进制`raw_frame`一次性解码成一个包含8192个整数的Python列表。
     - `voltage_value = convert_raw_to_mv(raw_value)`: 遍历解包后的每一个原始ADC值，调用`convert_raw_to_mv`函数，将其转换为浮点型的毫伏电压值。
  3. **高精度时间戳插值**:
     - `sample_time_delta = timedelta(...)`: 这是一个精妙的设计。我们知道STM32的理论总采样率是`25000 * 8 = 200,000 sps`。那么每个样本之间的时间间隔就是`1 / 200,000`秒。
     - `sample_timestamp = frame_start_time + (i * sample_time_delta)`: 以整个数据帧的到达时间为基准，通过这个精确的时间间隔，为**每一个独立**的样本点“插值”计算出一个理论上的高精度时间戳。这远比只给一整行数据一个时间戳要精确得多。
  4. **数据累积与写入**:
     - `accumulated_data.append((sample_timestamp, voltage_value))`: 将处理好的（时间戳，电压值）对，存入一个大的累积列表`accumulated_data`中。
     - `if len(accumulated_data) >= SAMPLES_PER_CSV * TOTAL_CHANNELS:`: 每次累积后，检查列表中的样本总数是否达到了写入CSV的阈值（65536个样本点，对应 `65536 * 8` 个数据项）。
     - **写入CSV**: 如果达到阈值，就执行文件写入操作。它会重组数据，将时间戳和8个通道的电压值整理成一行，并使用`csv.writer`高效地写入磁盘。
     - `accumulated_data = accumulated_data[...]`: 写入完成后，从累积列表中移除已写入的部分，保留剩余的，为下一个文件做准备。

##### 第三站：性能监控链路 (`SamplingRateMonitor` 线程)

这个线程是系统的“仪表盘”，独立于数据流，只向上帝视角观察系统的性能。

- **原理**: 它通过定期（每2秒）比较“当前接收到的总样本数”和“2秒前接收到的总样本数”，来计算出这段时间内的平均采样率。
- **执行流程**:
  1. **数据更新**: 主线程在每次成功重组一个16KB的数据帧后，会调用`rate_monitor.add_samples(TOTAL_SAMPLES_PER_FRAME)`，这个调用是线程安全的，因为它内部有锁保护。
  2. **周期性计算**: `display_rate`函数在一个`while`循环中，每隔2秒执行一次计算。它用（样本增量 / 时间增量）得到总采样率，然后除以通道数和1000，得到用户友好的`kSPS/通道`单位。
  3. **打印输出**: 将计算结果打印到控制台，为用户提供实时的性能反馈。
