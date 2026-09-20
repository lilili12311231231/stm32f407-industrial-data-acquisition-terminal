/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "modbus_slave.h"
#include "lcd.h"
#include <stdio.h>
#include "log.h"
#include "../BSP/SDIO/sdio_sdcard.h"
#include "../../Middlewares/FATFS/ff.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define MED_WINDOW  5   /* 中值滤波窗口（奇数） */
#define MA_WINDOW   8   /* 滑动平均窗口 */

typedef struct {
    uint32_t med_buf[MED_WINDOW];   /* 中值滤波缓冲区 */
    uint32_t ma_buf[MA_WINDOW];     /* 滑动平均缓冲区 */
    uint8_t med_idx;
    uint8_t ma_idx;
} FilterChain;                      /* 中值+滑动平均 两级滤波器 */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc3;
DMA_HandleTypeDef hdma_adc1;

I2C_HandleTypeDef hi2c1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
osThreadId_t logTaskHandle;
const osThreadAttr_t logTask_attributes = {
  .name = "logTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
uint32_t result[100];
osMutexId_t mbRegsMutex;
FilterChain filt_vol, filt_temp, filt_light;   /* 三路滤波器 */
osThreadId_t collectTaskHandle;
const osThreadAttr_t collectTask_attributes = {
  .name = "collectTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
osThreadId_t displayTaskHandle;
const osThreadAttr_t displayTask_attributes = {
  .name = "displayTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
osThreadId_t modbusTaskHandle;
const osThreadAttr_t modbusTask_attributes = {
  .name = "modbusTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
osThreadId_t watchdogTaskHandle;
const osThreadAttr_t watchdogTask_attributes = {
  .name = "watchdogTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC3_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */

void LogTask(void *argument);
void CollectTask(void *argument);
void DisplayTask(void *argument);
void ModbusTask(void *argument);
void WatchdogTask(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* 中值滤波：冒泡排序取中间值，窗口奇数（防 ADC 偶发跳变） */
static uint32_t Median(uint32_t *buf)
{
    uint32_t tmp[MED_WINDOW];
    uint8_t i, j;
    for (i = 0; i < MED_WINDOW; i++) tmp[i] = buf[i];
    for (i = 0; i < MED_WINDOW - 1; i++)
        for (j = 0; j < MED_WINDOW - 1 - i; j++)
            if (tmp[j] > tmp[j + 1]) {
                uint32_t t = tmp[j]; tmp[j] = tmp[j + 1]; tmp[j + 1] = t;
            }
    return tmp[MED_WINDOW / 2];
}

/* 两级滤波：中值去尖峰 -> 滑动平均平滑 */
uint32_t Filter_Process(FilterChain *f, uint32_t raw)
{
    uint32_t sum = 0;
    uint8_t i;
    f->med_buf[f->med_idx] = raw;
    f->med_idx = (f->med_idx + 1) % MED_WINDOW;
    f->ma_buf[f->ma_idx] = Median(f->med_buf);
    f->ma_idx = (f->ma_idx + 1) % MA_WINDOW;
    for (i = 0; i < MA_WINDOW; i++) sum += f->ma_buf[i];
    return sum / MA_WINDOW;
}

/* 滤波初始化：用初值填满窗口，避免平滑从 0 开始爬升 */
void Filter_Init(FilterChain *f, uint32_t v)
{
    uint8_t i;
    for (i = 0; i < MED_WINDOW; i++) f->med_buf[i] = v;
    for (i = 0; i < MA_WINDOW; i++) f->ma_buf[i] = v;
    f->med_idx = 0;
    f->ma_idx = 0;
}

/* 初始化看门狗 IWDG：直接操作寄存器（不启用 HAL 模块），
   KR=0x5555 解锁写保护 -> 配置PR/重载RLR -> KR=0xCCCC 使能看门狗 -> KR=0xAAAA 喂狗 */
void IWDG_Init(void)
{
    RCC->CSR |= RCC_CSR_LSION;                  /* 使能 LSI(约32kHz) */
    while ((RCC->CSR & RCC_CSR_LSIRDY) == 0) {} /* 等 LSI 就绪 */
    IWDG->KR  = 0x5555;                         /* 解锁写保护 */
    IWDG->PR  = 0x04;                           /* 64分频 -> 500Hz */
    IWDG->RLR = 1250;                           /* 重载 -> 约2.5s超时 */
    IWDG->KR  = 0xCCCC;                         /* 启动看门狗 */
    IWDG->KR  = 0xAAAA;                         /* 先喂一次 */
}

void IWDG_Feed(void)
{
    IWDG->KR = 0xAAAA;   /* 喂狗：写0xAAAA刷新计数器 */
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

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
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_ADC1_Init();
  MX_ADC3_Init();
  MX_USART2_UART_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  Modbus_Init();
  HAL_ADC_Start_DMA(&hadc1, (uint32_t*)&result, 100);
  lcd_init();
  lcd_clear(WHITE);
  LOG_INFO("System started\n");
  LOG_DEBUG("Start collecting data\n");

  /* SD card init */
  if (sd_init() == 0)
  {
    LOG_INFO("SD Card OK: %d MB\n", (uint32_t)SD_TOTAL_SIZE_MB(&g_sdcard_handler));
  }
  else
  {
    LOG_ERROR("SD Card Error!\n");
  }

  /* Init log-to-SD: mount SD + open log.txt, after this every log is persisted */
  if (log_fs_init() == 0)
  {
    LOG_INFO("Log to SD card ready\n");
  }
  else
  {
    LOG_ERROR("Log FS init fail\n");
  }

  /* LED test: configure DS0 (PF9) as push-pull output */
  GPIO_InitTypeDef led = {0};
  led.Pin   = GPIO_PIN_9;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &led);
  /* IWDG_Init();   启动看门狗 IWDG */
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  mbRegsMutex = osMutexNew(NULL);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  log_init();
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  collectTaskHandle = osThreadNew(CollectTask, NULL, &collectTask_attributes);
  displayTaskHandle = osThreadNew(DisplayTask, NULL, &displayTask_attributes);
  modbusTaskHandle = osThreadNew(ModbusTask, NULL, &modbusTask_attributes);
  watchdogTaskHandle = osThreadNew(WatchdogTask, NULL, &watchdogTask_attributes);
  logTaskHandle = osThreadNew(LogTask, NULL, &logTask_attributes);
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 2;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC3_Init(void)
{

  /* USER CODE BEGIN ADC3_Init 0 */

  /* USER CODE END ADC3_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC3_Init 1 */

  /* USER CODE END ADC3_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc3.Instance = ADC3;
  hadc3.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc3.Init.Resolution = ADC_RESOLUTION_12B;
  hadc3.Init.ScanConvMode = DISABLE;
  hadc3.Init.ContinuousConvMode = DISABLE;
  hadc3.Init.DiscontinuousConvMode = DISABLE;
  hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc3.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc3.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc3.Init.NbrOfConversion = 1;
  hadc3.Init.DMAContinuousRequests = DISABLE;
  hadc3.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc3) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC3_Init 2 */

  /* USER CODE END ADC3_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, GPIO_PIN_8, GPIO_PIN_RESET);

  /*Configure GPIO pin : PG8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
int fputc(int ch, FILE *f)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
  return ch;
}
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
  {
    Modbus_RxCpltCallback(huart, Size);
  }
void CollectTask(void *argument)
{
  static uint8_t first = 1;       /* first-sample flag for filter window init */
  uint32_t value = 0, value2 = 0; /* voltage/temp 50-point average */
  float voltage, temperature;
  uint32_t light_raw;
  int i;


  for(;;)
  {
    /* voltage/temp: average ADC1 DMA result[] (even index = volt, odd = temp) */
    value = 0; value2 = 0;
    for (i = 0; i < 100; )
    {
      value  += result[i++];
      value2 += result[i++];
    }
    value  /= 50;
    value2 /= 50;
    voltage     = value  * 3.3f / 4096.0f;
    temperature = (value2 * 3.3f / 4096.0f - 0.76f) / 0.0025f + 25.0f;

    /* light: single conversion, trigger + poll + read each loop */
    HAL_ADC_Start(&hadc3);
    HAL_ADC_PollForConversion(&hadc3, 10);
    light_raw = 100 - HAL_ADC_GetValue(&hadc3) * 100 / 4096;
    HAL_ADC_Stop(&hadc3);

    /* fill filter window with first real sample to avoid ramping from 0 */
    if (first)
    {
      Filter_Init(&filt_vol,   (uint32_t)(voltage * 100.0f));
      Filter_Init(&filt_temp,  (uint32_t)(temperature * 10.0f));
      Filter_Init(&filt_light, light_raw);
      first = 0;
    }

    /* two-stage filter then write shared regs under lock */
    osMutexAcquire(mbRegsMutex, osWaitForever);
    mb_holding_regs[0] = (uint16_t)Filter_Process(&filt_vol,   (uint32_t)(voltage * 100.0f));
    mb_holding_regs[1] = (uint16_t)Filter_Process(&filt_temp,  (uint32_t)(temperature * 10.0f));
    mb_holding_regs[2] = (uint16_t)Filter_Process(&filt_light, light_raw);
    osMutexRelease(mbRegsMutex);

    osDelay(100);
  }
}

void DisplayTask(void *argument)
{
  char buf[64];

  for(;;)
  {
    osMutexAcquire(mbRegsMutex, osWaitForever);

    sprintf(buf, "Vol  :%d.%02dV ", mb_holding_regs[0] / 100, mb_holding_regs[0] % 100);
    lcd_show_string(30, 40, 240, 24, 24, buf, BLUE);

    sprintf(buf, "Temp :%d.%01dC ", mb_holding_regs[1] / 10, mb_holding_regs[1] % 10);
    lcd_show_string(30, 90, 240, 24, 24, buf, BLUE);

    sprintf(buf, "Light:%d%%    ", mb_holding_regs[2]);
    lcd_show_string(30, 140, 240, 24, 24, buf, BLUE);

    osMutexRelease(mbRegsMutex);
    osDelay(100);
  }
}

void ModbusTask(void *argument)
{
  for(;;)
  {
    osMutexAcquire(mbRegsMutex, osWaitForever);
    Modbus_Process();
    osMutexRelease(mbRegsMutex);
    osDelay(1);
  }
}

void WatchdogTask(void *argument)
{
  /* IWDG_Init();   uncomment for release; keep disabled while debugging */
  for(;;)
  {
    IWDG_Feed();
    osDelay(50);
  }
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9);  /* 翻转灯：系统心跳指示 */
    osDelay(500);
  }
  /* USER CODE END 5 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
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
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
