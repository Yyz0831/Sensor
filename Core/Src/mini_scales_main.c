/* Standalone Mini Scales test. Selected only by the mini_scales environment. */
#include "main.h"
#include "gpio.h"
#include "i2c.h"
#include "usart.h"
#include "mini_scales.h"
#include <stdio.h>
#include <string.h>

static void SystemClock_Config(void);

static void SendText(const char *text)
{
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)text, (uint16_t)strlen(text), 100);
}

static const char *StatusText(MiniScalesStatus status)
{
  switch (status)
  {
    case MINI_SCALES_OK: return "正常";
    case MINI_SCALES_IO_ERROR: return "I2C通信失败";
    case MINI_SCALES_INVALID_DATA: return "数据无效";
    case MINI_SCALES_OUT_OF_RANGE: return "超出量程";
    default: return "未知状态";
  }
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART2_UART_Init();
  HAL_Delay(300);

  SendText("# M5Stack Mini Scales U177 | I2C地址=0x26 | 重量单位=g（克）\r\n");
  SendText("# 空载稳定后，按模块自身按钮去皮。\r\n");
  uint8_t version;
  char line[256];
  if (MiniScales_GetFirmwareVersion(&version) == MINI_SCALES_OK)
  {
    (void)snprintf(line, sizeof(line), "# 模块固件版本=%u\r\n", version);
    SendText(line);
  }
  else
  {
    SendText("# 模块未响应，请检查5V/GND/SDA/SCL；程序会自动重试读取。\r\n");
  }

  uint32_t last_sample_ms = HAL_GetTick();
  uint32_t last_blink_ms = last_sample_ms;
  uint8_t previous_button = 1;
  while (1)
  {
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_blink_ms) >= 500U)
    {
      last_blink_ms = now;
      HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    }
    if ((uint32_t)(now - last_sample_ms) >= 250U)
    {
      last_sample_ms = now;
      MiniScalesSample sample;
      MiniScalesStatus status = MiniScales_Read(&sample);
      int length;
      if (sample.button <= 1)
      {
        length = snprintf(line, sizeof(line),
                          "时间=%lu ms | 重量=%.2f g | 原始ADC=%ld | 按钮=%s | 状态=%s(%u)\r\n",
                          (unsigned long)now, (double)sample.weight_g,
                          (long)sample.raw_adc, sample.button == 0 ? "按下" : "松开",
                          StatusText(status), (unsigned int)status);
      }
      else
      {
        length = snprintf(line, sizeof(line),
                          "时间=%lu ms | 重量=nan g | 原始ADC=不可用 | 按钮=不可用 | 状态=%s(%u)\r\n",
                          (unsigned long)now, StatusText(status), (unsigned int)status);
      }
      if (length > 0 && length < (int)sizeof(line))
      {
        SendText(line);
      }
      if (sample.button == 0 && previous_button == 1)
      {
        SendText(MiniScales_Tare() == MINI_SCALES_OK
                     ? "# 去皮指令已接受，请松开按钮并等待读数稳定。\r\n"
                     : "# 去皮指令失败，请检查连接后再次按下按钮。\r\n");
      }
      previous_button = sample.button <= 1 ? sample.button : 1;
    }
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
  {
    Error_Handler();
  }
  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                   RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV2;
  clock.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
