/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 Leo Osio.
  * SPDX-License-Identifier: BSD-3-Clause
  *
  * This software is licensed under the BSD-3-Clause license.
  * See LICENSE file in the project root for full license information.
  * Third-party notices in THIRD_PARTY_LICENSES/.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "TFT_ILI9341.h"
#include "encoder.h"
#include "menu.h"
#include "sim.h"
#include "usbd_cdc_if.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
    int16_t m;      /* muestra ECG   */
    uint8_t r;      /* flag pico R   */
} MuestraECG_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RING_N                256u   /* potencia de 2: ~0.5 s de margen a 500 Hz */
#define MUESTRAS_POR_PAQUETE  5
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_tx;

TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;

/* USER CODE BEGIN PV */
SimuladorECG_t sim_ecg;
volatile uint8_t sim_activa = 0;

/* Buffer circular: la ISR (TIM3, 500 Hz) escribe, el main lee y envía por USB */
static MuestraECG_t      ring[RING_N];
static volatile uint16_t ring_head = 0;      /* escribe la ISR */
static volatile uint16_t ring_tail = 0;      /* lee el main    */
volatile uint32_t        ring_overruns = 0;  /* muestras descartadas (PC sin leer) */

/* Doble buffer USB: el que está en vuelo no se toca hasta que termina */
static char    usb_buf[2][64];
static uint8_t usb_sel = 0;

extern void Sim_Iniciar(uint32_t);
extern void Sim_Detener(void);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM4_Init(void);
/* USER CODE BEGIN PFP */
static void MX_TIM3_Init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Tick de 500 Hz: encoder + generación de la muestra. No depende de lo que
   esté haciendo el main (dibujar la TFT, USB, etc.). */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM3) return;

    Encoder_Tick();                              /* cada 2 ms exactos */

    if (sim_activa && !sim_ecg.pausa) {
        int16_t m = Sim_Step(&sim_ecg);
        uint16_t h = ring_head;
        if ((uint16_t)(h - ring_tail) < RING_N) {
            ring[h & (RING_N - 1u)].m = m;
            ring[h & (RING_N - 1u)].r = sim_ecg.flag_pico_r;
            __DMB();
            ring_head = (uint16_t)(h + 1u);
        } else {
            ring_overruns++;                     /* buffer lleno: descarta lo nuevo */
        }
    }
}

/* Vacía el buffer circular hacia USB CDC (se llama desde el while principal) */
static void USB_Servicio(void)
{
    uint16_t head = ring_head;
    __DMB();
    uint16_t disp = (uint16_t)(head - ring_tail);
    if (disp == 0u) return;

    /* Simulando: agrupa de 5 en 5. En pausa/detenida: vacía lo que quede */
    if (disp < MUESTRAS_POR_PAQUETE && sim_activa && !sim_ecg.pausa) return;
    if (disp > MUESTRAS_POR_PAQUETE) disp = MUESTRAS_POR_PAQUETE;

    char *buf = usb_buf[usb_sel];
    uint16_t len = 0;
    for (uint16_t i = 0; i < disp; i++) {
        const MuestraECG_t *s = &ring[(uint16_t)(ring_tail + i) & (RING_N - 1u)];
        len += (uint16_t)snprintf(buf + len, sizeof(usb_buf[0]) - len,
                                  "%d,%u\n", (int)s->m, (unsigned)s->r);
    }

    if (CDC_Transmit_FS((uint8_t *)buf, len) == USBD_OK) {
        ring_tail = (uint16_t)(ring_tail + disp);   /* solo se consume si USB lo aceptó */
        usb_sel ^= 1u;                              /* el otro buffer queda libre */
    }
    /* Si fue BUSY, las muestras siguen en el ring y se reintenta en la próxima vuelta */
}

void Sim_Iniciar(uint32_t s)
{
    uint32_t p = __get_PRIMASK();
    __disable_irq();
    sim_activa = 0;                 /* la ISR deja de avanzar la simulación */
    ring_tail  = ring_head;         /* descarta muestras viejas */
    __set_PRIMASK(p);

    sim_ecg.pausa = 0u;
    Sim_SetSemilla(&sim_ecg, s);    /* reinicia el generador con esta semilla */
    sim_activa = 1;
}

void Sim_Detener(void)
{
    uint32_t p = __get_PRIMASK();
    __disable_irq();
    sim_activa = 0;
    ring_tail  = ring_head;
    __set_PRIMASK(p);
}

void Sim_Pausar(uint8_t pausar)
{
    if (!sim_activa) return;
    /* Las muestras que queden en el ring se envían solas desde USB_Servicio() */
    sim_ecg.pausa = pausar ? 1u : 0u;
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
  MX_SPI1_Init();
  MX_TIM4_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */

  TFT_Init();
  Encoder_Init();
  //Encoder_DebugInit();
  Sim_Init(&sim_ecg, 1u);      /* parámetros por defecto una sola vez al arrancar */
  Menu_Init();
  DibujarMenuPrincipal();

  MX_TIM3_Init();
  HAL_TIM_Base_Start_IT(&htim3);   /* a partir de aquí corre el tick de 500 Hz */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      Menu_Update(Encoder_GetDelta(), Encoder_GetBoton());
      USB_Servicio();
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_Encoder_InitTypeDef sConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 0;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 65535;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sConfig.IC1Polarity = TIM_ICPOLARITY_FALLING;
  sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC1Filter = 0;
  sConfig.IC2Polarity = TIM_ICPOLARITY_FALLING;
  sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC2Filter = 0;
  if (HAL_TIM_Encoder_Init(&htim4, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Configure GPIO pin Output Level (Solo pines de la pantalla TFT) */
  HAL_GPIO_WritePin(GPIOB, TFT_CS_Pin|TFT_DC_Pin|TFT_RST_Pin, GPIO_PIN_SET);

  /* Configure GPIO pins : TFT_CS_Pin TFT_DC_Pin TFT_RST_Pin */
  GPIO_InitStruct.Pin = TFT_CS_Pin|TFT_DC_Pin|TFT_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* CORRECCIÓN 1: BOTON_Pin debe ser ENTRADA con PULLUP (no salida) */
  GPIO_InitStruct.Pin = BOTON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BOTON_GPIO_Port, &GPIO_InitStruct);

  /* CORRECCIÓN 2: Inicializar pines CLK y DT del encoder con PULLUP */
  GPIO_InitStruct.Pin = ENCODER_CLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(ENCODER_CLK_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = ENCODER_DT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(ENCODER_DT_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */

/* TIM3: tick de 500 Hz (2 ms).
   Reloj de timers APB1 = 48 MHz -> PSC 47 = 1 MHz -> ARR 1999 = 500 Hz. */
static void MX_TIM3_Init(void)
{
    __HAL_RCC_TIM3_CLK_ENABLE();

    htim3.Instance               = TIM3;
    htim3.Init.Prescaler         = 47;
    htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim3.Init.Period            = 1999;
    htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

/* Si CubeMX ya generó TIM3_IRQHandler en stm32f4xx_it.c, borra una de las dos */
void TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}

/* USER CODE END 4 */

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
