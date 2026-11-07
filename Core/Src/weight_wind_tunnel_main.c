/* COMP0219 Logger: raw ADC or calibrated m/s, chosen by PlatformIO environment.
 * The sensor/module still updates at about 10 Hz. Failed/stale reads are silent.
 */
#include "main.h"
#include "gpio.h"
#include "i2c.h"
#include "usart.h"
#include "weight_i2c.h"
#ifdef WEIGHT_WIND_TUNNEL_SPEED
#include "weight_wind_speed.h"
#else
#include "weight_adc_stream.h"
#endif

static void SystemClock_Config(void);
#ifdef WEIGHT_WIND_TUNNEL_SPEED
static volatile WeightWindSpeed speed;
static volatile WeightSpeedStream stream;
static char tx_line[WEIGHT_SPEED_LINE_CAPACITY];
#else
static volatile WeightAdcStream stream;
/* Owned by the SysTick formatter and UART ISR; never changed while TX is busy. */
static char tx_line[WEIGHT_ADC_LINE_CAPACITY];
#endif
static volatile uint8_t stream_enabled;

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart2);
}

void WeightWindTunnel_Tick(uint32_t now)
{
  if (!stream_enabled)
    return;
#ifdef WEIGHT_WIND_TUNNEL_SPEED
  /* Keep filtering every output slot, including while foreground I2C blocks
   * or UART is busy. Only this ISR filters/formats; main publishes raw samples.
   */
  size_t length = WeightWindSpeed_OutputTick(&speed, &stream, now,
                                      huart2.gState == HAL_UART_STATE_READY,
                                      tx_line, sizeof(tx_line));
#else
  size_t length = WeightAdcStream_Tick(&stream, now,
                                      huart2.gState == HAL_UART_STATE_READY,
                                      tx_line, sizeof(tx_line));
#endif
  if (length != 0)
    (void)HAL_UART_Transmit_IT(&huart2, (uint8_t *)tx_line, (uint16_t)length);
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART2_UART_Init(); /* Existing shared configuration: 115200, 8N1. */
  HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  HAL_Delay(500); /* Quiet startup; allow the module/ADC to settle. */

  uint32_t now = HAL_GetTick();
#ifdef WEIGHT_WIND_TUNNEL_SPEED
  WeightWindSpeed_Init(&speed, now);
  WeightSpeedStream_Init(&stream, now);
  uint32_t last_read_ms = now - WEIGHT_SPEED_POLL_MS;
#else
  WeightAdcStream_Init(&stream, now);
  uint32_t last_read_ms = now - WEIGHT_ADC_POLL_MS;
  uint32_t last_blink_ms = now;
#endif
  stream_enabled = 1;
  while (1)
  {
    now = HAL_GetTick();
#ifdef WEIGHT_WIND_TUNNEL_SPEED
    {
      int pressed = HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_RESET;
      uint32_t irq_mask = __get_PRIMASK();
      __disable_irq();
      if (WeightWindSpeed_Button(&speed, now, pressed))
        WeightSpeedStream_Invalidate(&stream);
      __set_PRIMASK(irq_mask);
    }
    if ((uint32_t)(now - last_read_ms) >= WEIGHT_SPEED_POLL_MS)
#else
    if ((uint32_t)(now - last_read_ms) >= WEIGHT_ADC_POLL_MS)
#endif
    {
#ifndef WEIGHT_WIND_TUNNEL_SPEED
      last_read_ms += ((uint32_t)(now - last_read_ms) / WEIGHT_ADC_POLL_MS) * WEIGHT_ADC_POLL_MS;
#endif
      int32_t raw = 0;
      /* Blocking I2C is outside the output ISR; SysTick/UART keep running. */
      int ok = WeightI2C_ReadRaw(&raw);
#ifdef WEIGHT_WIND_TUNNEL_SPEED
      uint32_t read_done_ms = HAL_GetTick();
      /* At least 100 ms between completed reads, including during zeroing.
       * UART output retains its independent 5 ms clock.
       */
      last_read_ms = read_done_ms;
      uint32_t irq_mask = __get_PRIMASK();
      __disable_irq();
      if (!WeightWindSpeed_Read(&speed, read_done_ms, ok, raw))
        WeightSpeedStream_Invalidate(&stream);
      __set_PRIMASK(irq_mask);
#else
      uint32_t irq_mask = __get_PRIMASK();
      __disable_irq();
      WeightAdcStream_Update(&stream, HAL_GetTick(), ok, raw);
      __set_PRIMASK(irq_mask);
#endif
    }
#ifdef WEIGHT_WIND_TUNNEL_SPEED
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
        WeightWindSpeed_LedOn(&speed, HAL_GetTick()) ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    if ((uint32_t)(now - last_blink_ms) >= 500U)
    {
      last_blink_ms = now;
      HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    }
#endif
    HAL_Delay(1);
  }
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef oscillator = {0};
  RCC_ClkInitTypeDef clock = {0};
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  oscillator.PLL.PLLM = 16;
  oscillator.PLL.PLLN = 336;
  oscillator.PLL.PLLP = RCC_PLLP_DIV4;
  oscillator.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
    Error_Handler();
  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                   RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV2;
  clock.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK)
    Error_Handler();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
