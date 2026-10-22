/* Standalone TAL220B + M5Stack Unit Weight-I2C (U180) application. */
#include "main.h"
#include "gpio.h"
#include "i2c.h"
#include "usart.h"
#include "weight_i2c.h"
#include "weight_calibration.h"
#include "weight_command.h"
#include "weight_profile.h"
#include <stdio.h>
#include <string.h>

#define SAMPLE_PERIOD_MS 250U
#define RX_CAPACITY 128U

static void SystemClock_Config(void);
static WeightCalibration calibration;
static uint8_t rx_byte;
static uint8_t rx_queue[RX_CAPACITY];
static volatile unsigned rx_head;
static volatile unsigned rx_tail;
static volatile uint8_t rx_lost;

static void SendText(const char *text)
{
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)text, (uint16_t)strlen(text), 100);
}

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart2);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
  if (uart != &huart2)
    return;
  unsigned next = (rx_head + 1U) % RX_CAPACITY;
  if (next == rx_tail)
    rx_lost = 1;
  else
  {
    rx_queue[rx_head] = rx_byte;
    rx_head = next;
  }
  if (HAL_UART_Receive_IT(&huart2, &rx_byte, 1) != HAL_OK)
    rx_lost = 1;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  if (uart != &huart2)
    return;
  rx_lost = 1;
  (void)HAL_UART_AbortReceive(&huart2);
  (void)HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
}

static void PrintHelp(void)
{
  SendText("# tare：空载稳定后输入，采集16次设为零点（约4秒）。\r\n");
  SendText("# cal 500：放好已知500 g物品并稳定后输入；请替换为实际质量。\r\n");
  SendText("# help：显示帮助。命令以回车结束，采集期间请保持载荷不变。\r\n");
  SendText("# 固件已记录默认系数；每次上电先空载tare，有系数时无需重复cal。\r\n");
  SendText("# 手动cal只更新本次RAM系数，复位后恢复固件默认值；量程尚未确认。\r\n");
  SendText("# 协议=weight_i2c_v1\r\n");
}

static void ReportResult(WeightResult result)
{
  char text[192];
  switch (result)
  {
    case WEIGHT_PENDING: break;
    case WEIGHT_TARE_DONE:
      (void)snprintf(text, sizeof(text), "# 去皮完成 | 零点ADC=%.3f | 校准=%s\r\n",
                     calibration.zero, calibration.calibrated ? "已有系数" : "尚未校准");
      SendText(text);
      break;
    case WEIGHT_CALIBRATION_DONE:
      (void)snprintf(text, sizeof(text), "# 校准完成 | 参考质量=%.6g g | 系数=%.9g ADC/g\r\n",
                     calibration.reference_g, calibration.counts_per_gram);
      SendText(text);
      break;
    case WEIGHT_BUSY:
      SendText("# 正在采集，请等待完成；新命令未执行。\r\n"); break;
    case WEIGHT_NEEDS_TARE:
      SendText("# 请先空载执行tare，再放已知质量执行cal。\r\n"); break;
    case WEIGHT_INVALID_MASS:
      SendText("# 参考质量必须是有限的正数，单位g。\r\n"); break;
    case WEIGHT_NO_CHANGE:
      SendText("# 校准失败：ADC无变化或系数无效；原有校准保留。\r\n"); break;
    case WEIGHT_READ_FAILED:
      SendText("# 采集因I2C失败中止；原有零点和校准保留，请重新发送命令。\r\n"); break;
  }
}

static void ExecuteCommand(const char *line)
{
  double mass;
  WeightResult result;
  switch (WeightCommand_Parse(line, &mass))
  {
    case WEIGHT_COMMAND_HELP: PrintHelp(); return;
    case WEIGHT_COMMAND_TARE:
      result = WeightCalibration_StartTare(&calibration); break;
    case WEIGHT_COMMAND_CAL:
      result = WeightCalibration_StartCal(&calibration, mass); break;
    default:
      SendText("# 无效命令；使用tare、cal 正数质量g 或help。\r\n"); return;
  }
  if (result == WEIGHT_PENDING)
    SendText("# 开始采集16次（约4秒），请保持载荷稳定。\r\n");
  else
    ReportResult(result);
}

static void ProcessSerial(void)
{
  static char line[64];
  static unsigned length;
  static uint8_t discard;
  while (rx_tail != rx_head || rx_lost)
  {
    /* Snapshot/clear the ISR error flag without losing a concurrent update. */
    __disable_irq();
    uint8_t lost = rx_lost;
    rx_lost = 0;
    if (lost)
      rx_tail = rx_head;
    __enable_irq();
    if (lost)
    {
      length = 0;
      discard = 1;
      SendText("# 串口接收溢出或错误；请按回车清除后重新输入命令。\r\n");
    }
    if (rx_tail == rx_head)
      break;
    uint8_t byte = rx_queue[rx_tail];
    rx_tail = (rx_tail + 1U) % RX_CAPACITY;
    if (byte == '\r' || byte == '\n')
    {
      if (!discard && length != 0)
      {
        line[length] = '\0';
        ExecuteCommand(line);
      }
      length = 0;
      discard = 0;
    }
    else if (!discard && (byte == '\b' || byte == 127))
    {
      if (length != 0)
        --length;
    }
    else if (!discard)
    {
      if ((byte < 32 && byte != '\t') || byte > 126 || length == sizeof(line) - 1)
      {
        length = 0;
        discard = 1;
        SendText("# 命令过长或含无效字符，已丢弃；请回车后重新输入英文命令。\r\n");
      }
      else
        line[length++] = (char)byte;
    }
  }
}

int main(void)
{
  WeightCalibration_Init(&calibration, WEIGHT_PROFILE_COUNTS_PER_GRAM);
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART2_UART_Init();
  HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  if (HAL_UART_Receive_IT(&huart2, &rx_byte, 1) != HAL_OK)
    Error_Handler();
  HAL_Delay(300);

  SendText("# TAL220B + Unit Weight-I2C U180 | I2C地址=0x26 | 串口=115200\r\n");
  uint8_t version;
  char text[256];
  if (calibration.calibrated)
  {
    (void)snprintf(text, sizeof(text),
                   "# 已加载固件默认系数=%.9g ADC/g | 校准参考质量=%.6g g | 请空载执行tare\r\n",
                   calibration.counts_per_gram, WEIGHT_PROFILE_REFERENCE_G);
    SendText(text);
  }
  else
    SendText("# 未设置有效默认系数，请空载tare后使用已知质量cal。\r\n");
  if (WeightI2C_GetFirmwareVersion(&version))
  {
    (void)snprintf(text, sizeof(text), "# 模块固件版本=%u\r\n", version);
    SendText(text);
  }
  else
    SendText("# 模块未响应，请检查5V/GND/SDA/SCL；读取会自动重试。\r\n");
  PrintHelp();

  uint32_t last_sample_ms = HAL_GetTick();
  uint32_t last_blink_ms = last_sample_ms;
  while (1)
  {
    ProcessSerial();
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_blink_ms) >= 500U)
    {
      last_blink_ms = now;
      HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    }
    if ((uint32_t)(now - last_sample_ms) >= SAMPLE_PERIOD_MS)
    {
      last_sample_ms = now;
      int32_t raw;
      int ok = WeightI2C_ReadRaw(&raw);
      ReportResult(WeightCalibration_Sample(&calibration, ok, raw));
      double grams;
      if (!ok)
        (void)snprintf(text, sizeof(text),
                       "时间=%lu ms | 原始ADC=不可用 | 重量=nan g | 状态=I2C通信失败\r\n",
                       (unsigned long)now);
      else if (WeightCalibration_Grams(&calibration, raw, &grams))
        (void)snprintf(text, sizeof(text),
                       "时间=%lu ms | 原始ADC=%ld | 重量=%.2f g | 状态=正常\r\n",
                       (unsigned long)now, (long)raw, grams);
      else
        (void)snprintf(text, sizeof(text),
                       "时间=%lu ms | 原始ADC=%ld | 重量=%s | 状态=正常\r\n",
                       (unsigned long)now, (long)raw,
                       calibration.calibrated && !calibration.zero_valid ? "待去皮" : "未校准");
      SendText(text);
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
