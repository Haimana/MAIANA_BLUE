/*
  Copyright (c) 2016-2020 Peter Antypas

  This file is part of the MAIANA™ transponder firmware.

  The firmware is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>
*/



#include "bsp.hpp"
#include <stm32l4xx_hal.h>
#include "printf_serial.h"
#include "UartQuiet.hpp"
#include <string.h>
#include <stdio.h>
#include <stddef.h>



#if BOARD_REV == 121

// ST M95040-DRDW8TP/K SPI EEPROM (4 Kbit = 512 bytes, 16-byte page, 9-bit addressing)
#define EEPROM_SIZE                     512U
#define EEPROM_PAGE_SIZE                16U
// Max data bytes per SPI1 critical section (see SpiBusLock). 2 header bytes +
// 8 data bytes = 10 bytes = 16 us of SCK at 5 MHz, so the radio bit-clock
// interrupts (~104 us AIS bit period) stay masked for well under half a bit.
#define EEPROM_XFER_CHUNK               8U
#define EEPROM_WRITE_TIMEOUT_MS         20U
// Largest record handled by eeprom_write_record() (read-back buffer size)
#define EEPROM_RECORD_MAX               64U
#define EEPROM_STATION_ADDRESS          0x000U
#define EEPROM_CONFIG_ADDRESS           0x040U
#define EEPROM_XOTRIM_ADDRESS           0x060U
#define EEPROM_TX_CONFIG_ADDRESS        0x070U

// M95040 SPI instruction set (bit 3 carries address bit A8 for READ/WRITE)
#define M95040_CMD_WRSR                 0x01U
#define M95040_CMD_WRITE                0x02U
#define M95040_CMD_READ                 0x03U
#define M95040_CMD_WRDI                 0x04U
#define M95040_CMD_RDSR                 0x05U
#define M95040_CMD_WREN                 0x06U

// M95040 Status Register: b7..b4 always read as 1, b3 = BP1, b2 = BP0,
// b1 = WEL, b0 = WIP. (The "b6..b4 = 0, b7 = SRWD" layout belongs to the
// larger M95080+ parts, not to the M950x0 family.)
#define M95040_SR_WIP                   0x01U
#define M95040_SR_WEL                   0x02U
#define M95040_SR_BP_MASK               0x0CU
#define M95040_SR_FIXED_ONES_MASK       0xF0U

static_assert(EEPROM_STATION_ADDRESS + sizeof(StationData) <= EEPROM_CONFIG_ADDRESS,
    "StationData overlaps ConfigFlags in EEPROM");
static_assert(EEPROM_CONFIG_ADDRESS + sizeof(ConfigFlags) <= EEPROM_XOTRIM_ADDRESS,
    "ConfigFlags overlaps XOTrim in EEPROM");
static_assert(EEPROM_XOTRIM_ADDRESS + sizeof(XOTrim) <= EEPROM_TX_CONFIG_ADDRESS,
    "XOTrim overlaps TXConfig in EEPROM");
static_assert(EEPROM_TX_CONFIG_ADDRESS + sizeof(TXConfig) <= EEPROM_SIZE,
    "TXConfig exceeds M95040 capacity");
// eeprom_write_record() treats the first 4 bytes of every record as its magic
static_assert(offsetof(StationData, magic) == 0U && sizeof(StationData) <= EEPROM_RECORD_MAX,
    "StationData record layout");
static_assert(offsetof(ConfigFlags, magic) == 0U && sizeof(ConfigFlags) <= EEPROM_RECORD_MAX,
    "ConfigFlags record layout");
static_assert(offsetof(XOTrim, magic) == 0U && sizeof(XOTrim) <= EEPROM_RECORD_MAX,
    "XOTrim record layout");
static_assert(offsetof(TXConfig, magic) == 0U && sizeof(TXConfig) <= EEPROM_RECORD_MAX,
    "TXConfig record layout");

const char *BSP_HW_REV = "12.1.0";

SPI_HandleTypeDef hspi1;
IWDG_HandleTypeDef hiwdg;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart1;
TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_usart2_rx;
DMA_HandleTypeDef hdma_usart2_tx;

#define GNSS_DMA_BUF_SIZE 1024
#define GNSS_TX_BUF_SIZE  128
static volatile uint8_t sGnssDmaBuf[GNSS_DMA_BUF_SIZE];
static uint8_t sGnssTxBuf[GNSS_TX_BUF_SIZE];
static bool sGnssTxStarted = false;
static uint16_t sGnssDmaRead = 0;
static uint32_t sGnssBaud = 0;
static uint32_t sGnssRxErrEvents = 0;
#if NMEA_DIAG
static volatile uint32_t sUsart2OreCount = 0;
static volatile uint32_t sUsart2FeCount = 0;
static volatile uint32_t sUsart2NeCount = 0;
static volatile uint32_t sUsart2PeCount = 0;
#endif

void SystemClock_Config();

char_input_cb gnssInputCallback = nullptr;
char_input_cb terminalInputCallback = nullptr;
irq_callback ppsCallback = nullptr;
irq_callback sotdmaCallback = nullptr;
irq_callback trxClockCallback = nullptr;
irq_callback rxClockCallback = nullptr;
irq_callback tickCallback = nullptr;
irq_callback txWdtCallback = nullptr;

StationData __station = {0};

// Drop anything left in the SPI1 RX FIFO (e.g. after a radio transfer that
// timed out: bsp_tx_spi_byte() ignores HAL errors). A stale byte would
// otherwise shift every byte the next transfer receives. Reading DR and then
// SR also clears a pending OVR flag. SPE must be 0 when this is called.
static inline void spi1_drain_rx_fifo()
{
  while ( (SPI1->SR & SPI_SR_FRLVL) != 0U )
    (void)*(volatile uint8_t *)&SPI1->DR;
  (void)SPI1->SR;
}

static inline void spi1_wait_idle()
{
  while ( (SPI1->SR & SPI_SR_FTLVL) != 0U )
    ;
  while ( (SPI1->SR & SPI_SR_BSY) != 0U )
    ;
}

// Mask NVIC priorities >= 1 (radio bit-clock EXTI3/EXTI15_10, SOTDMA TIM2,
// USART1, 1PPS EXTI9_5) while keeping SysTick (priority 0) active so HAL
// timeouts keep advancing. SPI1 is shared between both Si4463 RF ICs (10 MHz)
// and U7 M95040-DR (10 MHz max at 3.3 V; HSI +1% can exceed 10.0 MHz at /8),
// so temporarily switch SPI1 prescaler to /16 (5 MHz) for 2x timing margin.
// Callers keep each locked section to at most EEPROM_XFER_CHUNK data bytes
// (about 16 us of SCK plus HAL overhead) and never wait for a write cycle
// (tW) while holding the lock.
class SpiBusLock
{
public:
  SpiBusLock()
    : mPrevBasepri(__get_BASEPRI()),
      mSavedCr1(0U)
  {
    __set_BASEPRI_MAX(1U << (8U - __NVIC_PRIO_BITS));
    __DSB();
    __ISB();

    spi1_wait_idle();

    mSavedCr1 = SPI1->CR1;
    CLEAR_BIT(SPI1->CR1, SPI_CR1_SPE);
    spi1_drain_rx_fifo();
    MODIFY_REG(SPI1->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_16);
    SET_BIT(SPI1->CR1, SPI_CR1_SPE);
  }

  ~SpiBusLock()
  {
    spi1_wait_idle();

    CLEAR_BIT(SPI1->CR1, SPI_CR1_SPE);
    spi1_drain_rx_fifo();
    SPI1->CR1 = mSavedCr1 & ~SPI_CR1_SPE;
    if ( (mSavedCr1 & SPI_CR1_SPE) != 0U )
      SET_BIT(SPI1->CR1, SPI_CR1_SPE);
    __DSB();
    __ISB();

    __set_BASEPRI(mPrevBasepri);
    __DSB();
    __ISB();
  }

private:
  uint32_t mPrevBasepri;
  uint32_t mSavedCr1;
  SpiBusLock(const SpiBusLock &);
  SpiBusLock &operator=(const SpiBusLock &);
};

typedef struct
{
  GPIO_TypeDef *port;
  GPIO_InitTypeDef gpio;
  GPIO_PinState init;
} GPIO;

static const GPIO __gpios[] = {
    // Initialize all SPI chip-select and RF shutdown lines HIGH first (before
    // SPI1 AF pins) so no slave sees a glitch; PB6 has no external pull-up.
    {EEPROM_CS_PORT, {EEPROM_CS_PIN, GPIO_MODE_OUTPUT_PP, GPIO_PULLUP, GPIO_SPEED_HIGH, 0}, GPIO_PIN_SET},
    {CS1_PORT, {CS1_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, 0}, GPIO_PIN_SET},
    {CS2_PORT, {CS2_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, 0}, GPIO_PIN_SET},
    {SDN1_PORT, {SDN1_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {SDN2_PORT, {SDN2_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {GNSS_EN_PORT, {GNSS_EN_PIN, GPIO_MODE_OUTPUT_OD, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {TRX_IC_CLK_PORT, {TRX_IC_CLK_PIN, GPIO_MODE_IT_RISING, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {TX_DISABLE_PORT, {TX_DISABLE_PIN, GPIO_MODE_INPUT, GPIO_PULLUP, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {RX_EVT_PORT, {RX_EVT_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    // GNSS starts powered off (GNSS_EN = SET); keep PA2/PA3 in analog Hi-Z
    // until bsp_gnss_on() so USART2_TX idle HIGH cannot back-power U4.
    {GNSS_NMEA_TX_PORT, {GNSS_NMEA_TX_PIN, GPIO_MODE_ANALOG, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {GNSS_NMEA_RX_PORT, {GNSS_NMEA_RX_PIN, GPIO_MODE_ANALOG, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {SCK_PORT, {SCK_PIN, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, GPIO_AF5_SPI1}, GPIO_PIN_SET},
    {MISO_PORT, {MISO_PIN, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, GPIO_AF5_SPI1}, GPIO_PIN_SET},
    {MOSI_PORT, {MOSI_PIN, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, GPIO_AF5_SPI1}, GPIO_PIN_SET},
    {TRX_IC_DATA_PORT, {TRX_IC_DATA_PIN, GPIO_MODE_INPUT, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {TX_EVT_PORT, {TX_EVT_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {UART_TX_PORT, {UART_TX_PIN, GPIO_MODE_AF_PP, GPIO_PULLUP, GPIO_SPEED_LOW, GPIO_AF7_USART1}, GPIO_PIN_RESET},
    {UART_RX_PORT, {UART_RX_PIN, GPIO_MODE_AF_PP, GPIO_PULLUP, GPIO_SPEED_LOW, GPIO_AF7_USART1}, GPIO_PIN_RESET},
    {GNSS_STATE_PORT, {GNSS_STATE_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {RX_IC_CLK_PORT, {RX_IC_CLK_PIN, GPIO_MODE_IT_RISING, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {RX_IC_DATA_PORT, {RX_IC_DATA_PIN, GPIO_MODE_INPUT, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {PA_BIAS_PORT, {PA_BIAS_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {GNSS_1PPS_PORT, {GNSS_1PPS_PIN, GPIO_MODE_IT_RISING, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
};



void gpio_pin_init();
static void eeprom_init();
static void tx_wdt_init();

static void gnss_tx_dma_abort()
{
  CLEAR_BIT(USART2->CR3, USART_CR3_DMAT);
  CLEAR_BIT(DMA1_Channel7->CCR, DMA_CCR_EN);
  DMA1->IFCR = DMA_IFCR_CGIF7;
  sGnssTxStarted = false;
}

static bool gnss_tx_dma_busy()
{
  if ( !sGnssTxStarted )
    return false;

  if ( (DMA1_Channel7->CCR & DMA_CCR_EN) != 0U )
    {
      if ( DMA1_Channel7->CNDTR != 0U || (USART2->ISR & USART_ISR_TC) == 0U )
        return true;

      CLEAR_BIT(USART2->CR3, USART_CR3_DMAT);
      CLEAR_BIT(DMA1_Channel7->CCR, DMA_CCR_EN);
      DMA1->IFCR = DMA_IFCR_CGIF7;
    }

  sGnssTxStarted = false;
  return false;
}

static void gnss_uart_pins_enable()
{
  GPIO_InitTypeDef gpio = {0};

  gpio.Pin = GNSS_NMEA_TX_PIN;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_LOW;
  gpio.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GNSS_NMEA_TX_PORT, &gpio);

  gpio.Pin = GNSS_NMEA_RX_PIN;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GNSS_NMEA_RX_PORT, &gpio);

  SET_BIT(USART2->CR1, USART_CR1_TE | USART_CR1_RE);
}

static void gnss_uart_pins_disable()
{
  gnss_tx_dma_abort();

  // Disable USART2 RX/TX before switching pins to analog Hi-Z so an analog
  // input reading 0 does not generate continuous break / framing errors.
  CLEAR_BIT(USART2->CR1, USART_CR1_TE | USART_CR1_RE);

  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GNSS_NMEA_TX_PIN | GNSS_NMEA_RX_PIN;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_LOW;
  HAL_GPIO_Init(GNSS_NMEA_TX_PORT, &gpio);
}

extern "C"
{
  void Error_Handler(uint8_t i)
  {
    asm("BKPT 0");
  }
}


void bsp_hw_init()
{
  HAL_Init();
  SystemClock_Config();

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_SPI1_CLK_ENABLE();
  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  // Configure UART1 pins first so early logging is available
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = UART_TX_PIN | UART_RX_PIN;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_LOW;
  gpio.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(UART_TX_PORT, &gpio);

  gpio_pin_init();

  // USART1 (host/terminal UART)
  huart1.Instance                     = USART1;
  huart1.Init.BaudRate                = 38400;
  huart1.Init.WordLength              = UART_WORDLENGTH_8B;
  huart1.Init.StopBits                = UART_STOPBITS_1;
  huart1.Init.Parity                  = UART_PARITY_NONE;
  huart1.Init.Mode                    = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl               = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling            = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling          = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit  = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart1);

  HAL_NVIC_SetPriority(USART1_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);

  // SPI1 (shared between U1/U2 Si4463 RF ICs and U7 M95040 EEPROM; Mode 0, 10 MHz)
  hspi1.Instance                = SPI1;
  hspi1.Init.Mode               = SPI_MODE_MASTER;
  hspi1.Init.Direction          = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize           = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity        = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase           = SPI_PHASE_1EDGE;
  hspi1.Init.NSS                = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler  = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit           = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode             = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation     = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial      = 7;
  hspi1.Init.CRCLength          = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode           = SPI_NSS_PULSE_DISABLE;

  if (HAL_SPI_Init(&hspi1) != HAL_OK)
    {
      Error_Handler(0);
    }

  __HAL_SPI_ENABLE(&hspi1);

  // Satisfy M95040 power-up write inhibit time (tPUW <= 10 ms), then check
  // presence and block protection (see eeprom_init()).
  HAL_Delay(10);
  eeprom_init();

  // USART2 (bidirectional GNSS link: PA2 = TX, PA3 = RX)
  huart2.Instance                     = USART2;
  huart2.Init.BaudRate                = 115200;
  huart2.Init.WordLength              = UART_WORDLENGTH_8B;
  huart2.Init.StopBits                = UART_STOPBITS_1;
  huart2.Init.Parity                  = UART_PARITY_NONE;
  huart2.Init.Mode                    = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl               = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling            = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling          = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit  = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart2);
  // Keep TE/RE disabled while GNSS is powered off; bsp_gnss_on() enables them.
  CLEAR_BIT(USART2->CR1, USART_CR1_TE | USART_CR1_RE);
  // Initial rate only; GPS::enable() switches to the stored or probed rate.
  sGnssBaud = huart2.Init.BaudRate;

  // Polled circular DMA for LC76G RX. Radio bit-clock ISRs (prio 1) plus
  // reportRSSI/SPI waits can exceed the 87 us USART2 character time and
  // overrun a byte-IRQ receiver. DMA1 Ch6 / request 2 = USART2_RX on L432.
  // No DMA or USART2 IRQ: the radio priorities stay untouched.
  hdma_usart2_rx.Instance                 = DMA1_Channel6;
  hdma_usart2_rx.Init.Request             = DMA_REQUEST_2;
  hdma_usart2_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_usart2_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart2_rx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart2_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart2_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart2_rx.Init.Mode                = DMA_CIRCULAR;
  hdma_usart2_rx.Init.Priority            = DMA_PRIORITY_HIGH;
  if ( HAL_DMA_Init(&hdma_usart2_rx) != HAL_OK )
    Error_Handler(0);

  HAL_DMA_Start(&hdma_usart2_rx, (uint32_t)&USART2->RDR, (uint32_t)sGnssDmaBuf, GNSS_DMA_BUF_SIZE);
  SET_BIT(USART2->CR3, USART_CR3_DMAR);

  // Polled normal-mode DMA for non-blocking GNSS TX (bsp_gnss_send()).
  // DMA1 Ch7 / request 2 = USART2_TX on L432. No DMA IRQ: radio priorities stay untouched.
  hdma_usart2_tx.Instance                 = DMA1_Channel7;
  hdma_usart2_tx.Init.Request             = DMA_REQUEST_2;
  hdma_usart2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_usart2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart2_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart2_tx.Init.Mode                = DMA_NORMAL;
  hdma_usart2_tx.Init.Priority            = DMA_PRIORITY_LOW;
  if ( HAL_DMA_Init(&hdma_usart2_tx) != HAL_OK )
    Error_Handler(0);

  CLEAR_BIT(USART2->CR3, USART_CR3_DMAT);
  __HAL_UART_DISABLE_IT(&huart2, UART_IT_RXNE);
  HAL_NVIC_DisableIRQ(USART2_IRQn);

  // TIM2 for SOTDMA (37.5 Hz)
  uint32_t period = (SystemCoreClock / 37.5) - 1;

  htim2.Instance               = TIM2;
  htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.Prescaler         = 0;
  htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim2.Init.Period            = period;
  htim2.Init.RepetitionCounter = 0;

  HAL_TIM_Base_Init(&htim2);

  // 1PPS signal on PB7 (EXTI7 -> EXTI9_5_IRQn)
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

  // SOTDMA + AIS bit clocks at priority 1 (same as USART1; above 1PPS)
  HAL_NVIC_SetPriority(TIM2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);

  // RF IC clock interrupts (PC15 -> EXTI15_10, PB3 -> EXTI3)
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  HAL_NVIC_SetPriority(EXTI3_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);

  tx_wdt_init();

  memset(&__station, 0, sizeof(StationData));
}



void SystemClock_Config()
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  // Initialize CPU, AHB and APB bus clocks from HSI + PLL (80 MHz)
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 10;  // 80 MHz
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
      Error_Handler(0);
    }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
      |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
      Error_Handler(0);
    }

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1;
  PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler(0);
    }

  // Configure main internal regulator output voltage
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
    {
      Error_Handler(0);
    }

  // Configure SysTick for 1 ms period
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq()/1000);
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  // SysTick_IRQn interrupt configuration
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

void gpio_pin_init()
{
  for ( unsigned i = 0; i < sizeof __gpios / sizeof(GPIO); ++i )
    {
      const GPIO* io = &__gpios[i];
      if ( io->gpio.Mode == GPIO_MODE_OUTPUT_PP || io->gpio.Mode == GPIO_MODE_OUTPUT_OD )
        {
          HAL_GPIO_WritePin(io->port, io->gpio.Pin, io->init);
        }
      HAL_GPIO_Init(io->port, (GPIO_InitTypeDef*)&io->gpio);
    }
}

void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

  // System interrupt priorities
  HAL_NVIC_SetPriority(MemoryManagement_IRQn, 0, 0);
  HAL_NVIC_SetPriority(BusFault_IRQn, 0, 0);
  HAL_NVIC_SetPriority(UsageFault_IRQn, 0, 0);
  HAL_NVIC_SetPriority(SVCall_IRQn, 10, 0);
  HAL_NVIC_SetPriority(DebugMonitor_IRQn, 0, 0);
  HAL_NVIC_SetPriority(PendSV_IRQn, 10, 0);
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

bool bsp_is_tx_hardwired()
{
  return true;
}

/*
 * M95040-DR SPI EEPROM helpers (shared SPI1 bus with PB6 active-low CS).
 */

static uint8_t sEepromInitStatus = BSP_NVM_NOT_RESPONDING;
static uint8_t sEepromInitSr = 0U;

static inline void eeprom_cs_low()
{
  HAL_GPIO_WritePin(EEPROM_CS_PORT, EEPROM_CS_PIN, GPIO_PIN_RESET);
}

static inline void eeprom_cs_high()
{
  HAL_GPIO_WritePin(EEPROM_CS_PORT, EEPROM_CS_PIN, GPIO_PIN_SET);
}

static inline void eeprom_cs_deselect_delay()
{
  // Satisfy M95040 tSHSL (>= 40..90 ns): 16 NOPs is ~200 ns at 80 MHz
  for ( uint8_t i = 0; i < 16U; ++i )
    __NOP();
}

static inline uint8_t eeprom_cmd_with_a8(uint8_t baseCmd, uint16_t addr)
{
  return (uint8_t)(baseCmd | ((addr & 0x0100U) ? 0x08U : 0x00U));
}

// b7..b4 of the M95040 status register always read as 1. A status byte that
// does not carry them means MISO is stuck low or no device answered. A
// floating-high MISO (0xFF) passes this test but then shows WIP stuck at 1
// (eeprom_wait_ready() times out) and fails the WEL toggle in eeprom_ready().
static inline bool eeprom_sr_valid(uint8_t sr)
{
  return (sr & M95040_SR_FIXED_ONES_MASK) == M95040_SR_FIXED_ONES_MASK;
}

static bool eeprom_read_status_locked(uint8_t *status)
{
  alignas(4) uint8_t tx[2] = { M95040_CMD_RDSR, 0x00U };
  alignas(4) uint8_t rx[2] = { 0U, 0U };

  eeprom_cs_low();
  HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2U, 5U);
  eeprom_cs_high();

  if ( st != HAL_OK )
    return false;

  *status = rx[1];
  return true;
}

static bool eeprom_send_simple_cmd_locked(uint8_t cmd)
{
  alignas(4) uint8_t tx[1] = { cmd };
  alignas(4) uint8_t rx[1] = { 0U };

  eeprom_cs_low();
  HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 1U, 5U);
  eeprom_cs_high();
  return st == HAL_OK;
}

static bool eeprom_read_status(uint8_t *status)
{
  SpiBusLock lock;
  return eeprom_read_status_locked(status);
}

// Best effort: never leave the Write Enable Latch set after a failed write
static void eeprom_write_disable()
{
  SpiBusLock lock;
  eeprom_send_simple_cmd_locked(M95040_CMD_WRDI);
}

static bool eeprom_wait_ready(uint32_t timeoutMs)
{
  const uint32_t start = HAL_GetTick();
  for ( ;; )
    {
      uint8_t sr = 0x00U;
      if ( !eeprom_read_status(&sr) )
        return false;

      if ( !eeprom_sr_valid(sr) )
        return false;

      if ( (sr & M95040_SR_WIP) == 0U )
        return true;

      if ( (HAL_GetTick() - start) >= timeoutMs )
        return false;

      HAL_Delay(1);
    }
}

static bool eeprom_ready(void)
{
  if ( !eeprom_wait_ready(EEPROM_WRITE_TIMEOUT_MS) )
    return false;

  // Non-destructive presence check via volatile WEL latch (0 NV write cycles):
  // detects both floating-high (0xFF) and stuck-low (0x00) MISO lines.
  uint8_t sr1 = 0U;
  uint8_t sr2 = 0U;
  bool ok = false;

  {
    SpiBusLock lock;
    bool wrenOk = eeprom_send_simple_cmd_locked(M95040_CMD_WREN);
    eeprom_cs_deselect_delay();
    bool rdsr1Ok = wrenOk && eeprom_read_status_locked(&sr1);
    eeprom_cs_deselect_delay();
    // Always clear WEL before leaving so the EEPROM is never left write-enabled
    bool wrdiOk = eeprom_send_simple_cmd_locked(M95040_CMD_WRDI);
    eeprom_cs_deselect_delay();
    bool rdsr2Ok = wrdiOk && eeprom_read_status_locked(&sr2);
    ok = wrenOk && rdsr1Ok && wrdiOk && rdsr2Ok;
  }

  if ( !ok )
    return false;

  if ( !eeprom_sr_valid(sr1) || (sr1 & M95040_SR_WEL) == 0U )
    return false;
  if ( !eeprom_sr_valid(sr2) || (sr2 & M95040_SR_WEL) != 0U )
    return false;

  return true;
}

static bool eeprom_read_bytes(uint16_t addr, uint8_t *dst, uint16_t len)
{
  if ( !dst || len == 0U || (uint32_t)addr + len > EEPROM_SIZE )
    return false;

  if ( !eeprom_wait_ready(EEPROM_WRITE_TIMEOUT_MS) )
    return false;

  // One READ instruction per EEPROM_XFER_CHUNK bytes keeps every SPI1
  // critical section short (see SpiBusLock).
  alignas(4) uint8_t tx[2U + EEPROM_XFER_CHUNK];
  alignas(4) uint8_t rx[2U + EEPROM_XFER_CHUNK];

  uint16_t remaining = len;
  while ( remaining > 0U )
    {
      const uint16_t chunk = (remaining > EEPROM_XFER_CHUNK) ? EEPROM_XFER_CHUNK : remaining;
      tx[0] = eeprom_cmd_with_a8(M95040_CMD_READ, addr);
      tx[1] = (uint8_t)(addr & 0xFFU);
      memset(&tx[2], 0x00, chunk);

      HAL_StatusTypeDef st;
      {
        SpiBusLock lock;
        eeprom_cs_low();
        st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, (uint16_t)(2U + chunk), 5U);
        eeprom_cs_high();
      }

      if ( st != HAL_OK )
        return false;

      memcpy(dst, &rx[2], chunk);
      addr += chunk;
      dst += chunk;
      remaining -= chunk;
    }

  return true;
}

// One WRITE instruction: at most EEPROM_XFER_CHUNK bytes, inside one page.
static bool eeprom_write_chunk(uint16_t addr, const uint8_t *src, uint16_t len)
{
  if ( !src || len == 0U || len > EEPROM_XFER_CHUNK )
    return false;
  if ( (addr / EEPROM_PAGE_SIZE) != ((addr + len - 1U) / EEPROM_PAGE_SIZE) )
    return false;
  if ( !eeprom_wait_ready(EEPROM_WRITE_TIMEOUT_MS) )
    return false;

  alignas(4) uint8_t wrenTx[1] = { M95040_CMD_WREN };
  alignas(4) uint8_t wrenRx[1] = { 0U };
  alignas(4) uint8_t tx[2U + EEPROM_XFER_CHUNK];
  alignas(4) uint8_t rx[2U + EEPROM_XFER_CHUNK];

  tx[0] = eeprom_cmd_with_a8(M95040_CMD_WRITE, addr);
  tx[1] = (uint8_t)(addr & 0xFFU);
  memcpy(&tx[2], src, len);

  // WREN + WRITE atomically on SPI1; wait for WIP outside the lock
  HAL_StatusTypeDef st;
  {
    SpiBusLock lock;
    eeprom_cs_low();
    st = HAL_SPI_TransmitReceive(&hspi1, wrenTx, wrenRx, 1U, 5U);
    eeprom_cs_high();

    if ( st == HAL_OK )
      {
        eeprom_cs_deselect_delay();
        eeprom_cs_low();
        st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, (uint16_t)(2U + len), 5U);
        eeprom_cs_high();
      }
  }

  if ( st != HAL_OK )
    {
      // WEL may still be set (WREN went out, WRITE did not complete)
      eeprom_write_disable();
      return false;
    }

  if ( !eeprom_wait_ready(EEPROM_WRITE_TIMEOUT_MS) )
    {
      eeprom_write_disable();
      return false;
    }

  return true;
}

static bool eeprom_write_bytes(uint16_t addr, const uint8_t *src, uint16_t len)
{
  if ( !src || len == 0U || (uint32_t)addr + len > EEPROM_SIZE )
    return false;

  uint16_t remaining = len;
  while ( remaining > 0U )
    {
      uint16_t chunk = (uint16_t)(EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE));
      if ( chunk > EEPROM_XFER_CHUNK )
        chunk = EEPROM_XFER_CHUNK;
      if ( chunk > remaining )
        chunk = remaining;
      if ( !eeprom_write_chunk(addr, src, chunk) )
        return false;

      addr += chunk;
      src += chunk;
      remaining -= chunk;
    }

  return true;
}

static bool eeprom_matches(uint16_t addr, const uint8_t *src, uint16_t len)
{
  alignas(4) uint8_t buf[EEPROM_RECORD_MAX];
  if ( len > sizeof buf )
    return false;
  return eeprom_read_bytes(addr, buf, len) && memcmp(buf, src, len) == 0;
}

/*
 * Power-loss-safe record write. Every record starts with a 4-byte magic:
 * invalidate the magic, write the payload, then write the new magic, and
 * finally read the whole record back. An interrupted write leaves an invalid
 * magic (record treated as absent), never a valid magic over a partial
 * payload. A record that already holds the requested bytes is not rewritten.
 */
static bool eeprom_write_record(uint16_t addr, const uint8_t *src, uint16_t len)
{
  static const uint8_t invalidMagic[4] = { 0U, 0U, 0U, 0U };

  if ( !src || len <= 4U || len > EEPROM_RECORD_MAX )
    return false;

  if ( eeprom_matches(addr, src, len) )
    return true;

  if ( !eeprom_write_bytes(addr, invalidMagic, 4U) )
    return false;
  if ( !eeprom_write_bytes((uint16_t)(addr + 4U), src + 4U, (uint16_t)(len - 4U)) )
    return false;
  if ( !eeprom_write_bytes(addr, src, 4U) )
    return false;

  return eeprom_matches(addr, src, len);
}

/*
 * Called once from bsp_hw_init(): check that the EEPROM answers and that no
 * block is write-protected. BP1/BP0 are non-volatile and the board ties W
 * high, so a part that arrives with BP bits set would silently ignore writes
 * to the protected area; clear them with WRSR. Result: bsp_nvm_status().
 */
static void eeprom_init()
{
  sEepromInitSr = 0U;
  if ( !eeprom_ready() )
    {
      sEepromInitStatus = BSP_NVM_NOT_RESPONDING;
      return;
    }

  uint8_t sr = 0U;
  if ( !eeprom_read_status(&sr) || !eeprom_sr_valid(sr) )
    {
      sEepromInitStatus = BSP_NVM_NOT_RESPONDING;
      return;
    }

  sEepromInitSr = sr;
  if ( (sr & M95040_SR_BP_MASK) == 0U )
    {
      sEepromInitStatus = BSP_NVM_OK;
      return;
    }

  alignas(4) uint8_t tx[2] = { M95040_CMD_WRSR, 0x00U };
  alignas(4) uint8_t rx[2] = { 0U, 0U };
  bool ok;
  {
    SpiBusLock lock;
    ok = eeprom_send_simple_cmd_locked(M95040_CMD_WREN);
    if ( ok )
      {
        eeprom_cs_deselect_delay();
        eeprom_cs_low();
        ok = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2U, 5U) == HAL_OK;
        eeprom_cs_high();
      }
  }

  if ( !ok )
    eeprom_write_disable();

  ok = ok && eeprom_wait_ready(EEPROM_WRITE_TIMEOUT_MS) && eeprom_read_status(&sr)
      && eeprom_sr_valid(sr) && (sr & M95040_SR_BP_MASK) == 0U;

  sEepromInitSr = sr;
  sEepromInitStatus = ok ? BSP_NVM_BP_CLEARED : BSP_NVM_BP_CLEAR_FAILED;
}

uint8_t bsp_nvm_status(uint8_t *statusReg)
{
  if ( statusReg )
    *statusReg = sEepromInitSr;
  return sEepromInitStatus;
}


void bsp_read_station_data(StationData *data)
{
  if ( !data )
    return;

  if ( !eeprom_read_bytes(EEPROM_STATION_ADDRESS, (uint8_t*)data, sizeof(StationData)) )
    {
      memset(data, 0, sizeof(StationData));
    }

  __station = *data;
}

bool bsp_write_station_data(const StationData &data)
{
  const StationData d = data;
  if ( eeprom_write_record(EEPROM_STATION_ADDRESS, (const uint8_t*)&d, sizeof(StationData)) )
    {
      __station = d;
      return true;
    }

  // Re-sync the RAM copy with what the EEPROM really holds now
  StationData current;
  bsp_read_station_data(&current);
  return false;
}

bool bsp_erase_station_data()
{
  StationData empty;
  memset(&empty, 0, sizeof empty);
  return bsp_write_station_data(empty);
}

bool bsp_is_station_data_provisioned()
{
  return __station.magic == STATION_DATA_MAGIC;
}

void bsp_read_config_flags(ConfigFlags *flags)
{
  if ( !flags )
    return;

  if ( !eeprom_read_bytes(EEPROM_CONFIG_ADDRESS, (uint8_t*)flags, sizeof(ConfigFlags)) )
    {
      memset(flags, 0, sizeof(ConfigFlags));
    }
}

bool bsp_write_config_flags(const ConfigFlags &flags)
{
  const ConfigFlags f = flags;
  return eeprom_write_record(EEPROM_CONFIG_ADDRESS, (const uint8_t*)&f, sizeof(ConfigFlags));
}

bool bsp_erase_config_flags()
{
  ConfigFlags empty;
  memset(&empty, 0, sizeof empty);
  return bsp_write_config_flags(empty);
}

void bsp_read_xo_trim(XOTrim *t)
{
  if ( !t )
    return;

  if ( !eeprom_read_bytes(EEPROM_XOTRIM_ADDRESS, (uint8_t*)t, sizeof(XOTrim)) )
    {
      memset(t, 0, sizeof(XOTrim));
    }
}

bool bsp_write_xo_trim(const XOTrim &t)
{
  const XOTrim x = t;
  return eeprom_write_record(EEPROM_XOTRIM_ADDRESS, (const uint8_t*)&x, sizeof(XOTrim));
}

bool bsp_erase_xo_trim()
{
  XOTrim t;
  memset(&t, 0, sizeof t);
  return bsp_write_xo_trim(t);
}

bool bsp_read_tx_config(TXConfig *c)
{
  if ( !c )
    return false;
  if ( !eeprom_ready() )
    return false;
  return eeprom_read_bytes(EEPROM_TX_CONFIG_ADDRESS, (uint8_t*)c, sizeof(TXConfig));
}

bool bsp_write_tx_config(const TXConfig &c)
{
  if ( !eeprom_ready() )
    return false;

  const TXConfig t = c;
  return eeprom_write_record(EEPROM_TX_CONFIG_ADDRESS, (const uint8_t*)&t, sizeof(TXConfig));
}

bool bsp_erase_tx_config()
{
  TXConfig c;
  memset(&c, 0, sizeof c);
  return bsp_write_tx_config(c);
}

bool bsp_write_config_flags_byte(uint8_t offset, uint8_t value)
{
  if ( offset >= sizeof(ConfigFlags) )
    return false;
  if ( !eeprom_ready() )
    return false;

  const uint16_t addr = (uint16_t)(EEPROM_CONFIG_ADDRESS + offset);
  if ( !eeprom_write_bytes(addr, &value, 1U) )
    return false;

  uint8_t rb = 0;
  return eeprom_read_bytes(addr, &rb, 1U) && rb == value;
}

void bsp_set_rx_mode()
{
  HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_RESET);       // Kill the RF MOSFET bias voltage
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = TRX_IC_DATA_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TRX_IC_DATA_PORT, &gpio);
}

void bsp_rx_led_on()
{
  HAL_GPIO_WritePin(RX_EVT_PORT, RX_EVT_PIN, GPIO_PIN_SET);
}

void bsp_rx_led_off()
{
  HAL_GPIO_WritePin(RX_EVT_PORT, RX_EVT_PIN, GPIO_PIN_RESET);
}

void bsp_tx_led_on()
{
#if LEGACY_BREAKOUTS
  HAL_GPIO_WritePin(TX_EVT_PORT, TX_EVT_PIN, GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(TX_EVT_PORT, TX_EVT_PIN, GPIO_PIN_SET);
#endif
}

void bsp_tx_led_off()
{
#if LEGACY_BREAKOUTS
  HAL_GPIO_WritePin(TX_EVT_PORT, TX_EVT_PIN, GPIO_PIN_SET);
#else
  HAL_GPIO_WritePin(TX_EVT_PORT, TX_EVT_PIN, GPIO_PIN_RESET);
#endif
}

void bsp_gps_led_on()
{
  HAL_GPIO_WritePin(GNSS_STATE_PORT, GNSS_STATE_PIN, GPIO_PIN_SET);
}

void bsp_gps_led_off()
{
  HAL_GPIO_WritePin(GNSS_STATE_PORT, GNSS_STATE_PIN, GPIO_PIN_RESET);
}

void bsp_set_tx_mode()
{
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = TRX_IC_DATA_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TRX_IC_DATA_PORT, &gpio);

  HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_SET);       // RF MOSFET bias voltage
}

void bsp_gnss_on()
{
  HAL_GPIO_WritePin(GNSS_EN_PORT, GNSS_EN_PIN, GPIO_PIN_RESET);
  // Let Q2 bring U4 VCC up before driving PA2 (USART2_TX) high
  HAL_Delay(2);
  gnss_uart_pins_enable();

  // Discard any glitches captured while GNSS power was ramping up
  USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
  uint16_t write = (uint16_t)(GNSS_DMA_BUF_SIZE - (uint16_t)hdma_usart2_rx.Instance->CNDTR);
  if ( write >= GNSS_DMA_BUF_SIZE )
    write = 0;
  sGnssDmaRead = write;
}

void bsp_gnss_off()
{
  // Put PA2/PA3 into analog Hi-Z before turning Q2 off to prevent back-powering U4
  gnss_uart_pins_disable();
  HAL_GPIO_WritePin(GNSS_EN_PORT, GNSS_EN_PIN, GPIO_PIN_SET);
}

bool bsp_is_gnss_on()
{
  // GNSS_EN is active low (bsp_gnss_on() drives it RESET).
  return HAL_GPIO_ReadPin(GNSS_EN_PORT, GNSS_EN_PIN) == GPIO_PIN_RESET;
}

bool bsp_gnss_send(const char *sentence)
{
  if ( !sentence || !bsp_is_gnss_on() )
    return false;

  const size_t len = strlen(sentence);
  if ( len == 0U || len > GNSS_TX_BUF_SIZE )
    return false;

  if ( gnss_tx_dma_busy() )
    return false;

  memcpy(sGnssTxBuf, sentence, len);

  CLEAR_BIT(DMA1_Channel7->CCR, DMA_CCR_EN);
  DMA1->IFCR = DMA_IFCR_CGIF7;
  USART2->ICR = USART_ICR_TCCF;

  DMA1_Channel7->CPAR = (uint32_t)&USART2->TDR;
  DMA1_Channel7->CMAR = (uint32_t)sGnssTxBuf;
  DMA1_Channel7->CNDTR = (uint32_t)len;

  SET_BIT(DMA1_Channel7->CCR, DMA_CCR_EN);
  SET_BIT(USART2->CR3, USART_CR3_DMAT);
  sGnssTxStarted = true;
  return true;
}

void USART_putc(USART_TypeDef* USARTx, char c)
{
  while ( !(USARTx->ISR & USART_ISR_TXE) )
    ;

  USARTx->TDR = c;
}

bool bsp_uart1_can_write()
{
  return (USART1->ISR & USART_ISR_TXE) != 0;
}

#if NMEA_DIAG
static volatile uint32_t sUsart1OreCount = 0;
static volatile uint32_t sUsart1FeCount = 0;
static volatile uint32_t sUsart1NeCount = 0;
static volatile uint32_t sUsart1PeCount = 0;

uint32_t bsp_uart1_ore_count()
{
  return sUsart1OreCount;
}

uint32_t bsp_uart1_fe_count()
{
  return sUsart1FeCount;
}

uint32_t bsp_uart1_ne_count()
{
  return sUsart1NeCount;
}

uint32_t bsp_uart1_pe_count()
{
  return sUsart1PeCount;
}

uint32_t bsp_gnss_ore_count()
{
  return sUsart2OreCount;
}

uint32_t bsp_gnss_fe_count()
{
  return sUsart2FeCount;
}

uint32_t bsp_gnss_ne_count()
{
  return sUsart2NeCount;
}

uint32_t bsp_gnss_pe_count()
{
  return sUsart2PeCount;
}

static void countUsart2Errors(uint32_t isr)
{
  if ( isr & USART_ISR_ORE )
    sUsart2OreCount++;
  if ( isr & USART_ISR_FE )
    sUsart2FeCount++;
  if ( isr & USART_ISR_NE )
    sUsart2NeCount++;
  if ( isr & USART_ISR_PE )
    sUsart2PeCount++;
}
#endif

void bsp_poll_gnss_rx()
{
  // Reap completed TX DMA channel if finished
  gnss_tx_dma_busy();

  const uint32_t isr = USART2->ISR;
  if ( isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE) )
    {
      // Sticky flags: this counts polls that saw errors, not bytes. That is
      // enough to tell "wrong rate" (continuous) from a stray glitch.
      if ( isr & (USART_ISR_FE | USART_ISR_NE) )
        sGnssRxErrEvents++;
#if NMEA_DIAG
      countUsart2Errors(isr);
#endif
      USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
    }

  const uint16_t write = (uint16_t)(GNSS_DMA_BUF_SIZE - (uint16_t)hdma_usart2_rx.Instance->CNDTR);
  uint16_t n = 0;
  while ( sGnssDmaRead != write && n < GNSS_DMA_BUF_SIZE )
    {
      char c = (char)sGnssDmaBuf[sGnssDmaRead];
      sGnssDmaRead++;
      if ( sGnssDmaRead >= GNSS_DMA_BUF_SIZE )
        sGnssDmaRead = 0;
      if ( gnssInputCallback )
        gnssInputCallback(c);
      ++n;
    }
}

void bsp_gnss_set_baud(uint32_t baud)
{
  if ( baud == 0 )
    return;

  // Abort any in-flight TX DMA before changing USART2 BRR
  gnss_tx_dma_abort();

  // USART2 kernel clock is PCLK1 (CCIPR USART2SEL left at reset value, as
  // HAL_UART_Init() assumed). BRR may only be written while UE = 0. RE/TE and
  // DMAR keep their state: circular RX DMA simply gets no requests while UE=0.
  const uint32_t pclk = HAL_RCC_GetPCLK1Freq();
  const uint32_t brr = (pclk + baud / 2u) / baud;

  CLEAR_BIT(USART2->CR1, USART_CR1_UE);
  USART2->BRR = brr;
  USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
  SET_BIT(USART2->CR1, USART_CR1_UE);

  huart2.Init.BaudRate = baud;
  sGnssBaud = baud;

  // Discard whatever was received at the old rate: move the read index to
  // the DMA write position.
  uint16_t write = (uint16_t)(GNSS_DMA_BUF_SIZE - (uint16_t)hdma_usart2_rx.Instance->CNDTR);
  if ( write >= GNSS_DMA_BUF_SIZE )
    write = 0;
  sGnssDmaRead = write;
}

uint32_t bsp_gnss_baud()
{
  return sGnssBaud;
}

uint32_t bsp_gnss_rx_error_events()
{
  return sGnssRxErrEvents;
}

void bsp_write_char(char c)
{
  USART_putc(USART1, c);
}

void bsp_write_string(const char *s)
{
  for ( int i = 0; s[i] != 0; ++i )
    USART_putc(USART1, s[i]);
}

void bsp_start_wdt()
{
  IWDG_InitTypeDef iwdg;
  iwdg.Prescaler = IWDG_PRESCALER_16;
  iwdg.Reload = 0x0fff;
  iwdg.Window = 0x0fff;

  hiwdg.Instance = IWDG;
  hiwdg.Init = iwdg;

  HAL_IWDG_Init(&hiwdg);
}

void bsp_refresh_wdt()
{
  HAL_IWDG_Refresh(&hiwdg);
}

void bsp_set_gnss_input_callback(char_input_cb cb)
{
  gnssInputCallback = cb;
}

void bsp_set_terminal_input_callback(char_input_cb cb)
{
  terminalInputCallback = cb;
}

void bsp_set_tick_callback(irq_callback cb)
{
  tickCallback = cb;
}

void bsp_start_sotdma_timer()
{
  HAL_TIM_Base_Start_IT(&htim2);
}

void bsp_stop_sotdma_timer()
{
  HAL_TIM_Base_Stop_IT(&htim2);
}

void bsp_set_gnss_1pps_callback(irq_callback cb)
{
  ppsCallback = cb;
}

/*
 * TX watchdog on TIM7 (basic timer, unused elsewhere): 10 kHz tick (0.1 ms),
 * one-pulse mode, update interrupt at NVIC priority 1 like the radio bit
 * clocks and TIM2, so it never preempts them (and vice versa).
 */
static void tx_wdt_init()
{
  __HAL_RCC_TIM7_CLK_ENABLE();

  uint32_t clk = HAL_RCC_GetPCLK1Freq();
  if ( (RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1 )
    clk *= 2U;                                // APB1 timer clock is x2 when APB1 is divided

  TIM7->CR1  = 0;
  TIM7->DIER = 0;
  TIM7->PSC  = (clk / 10000U) - 1U;
  TIM7->ARR  = 0xFFFFU;
  TIM7->CR1  = TIM_CR1_OPM | TIM_CR1_URS;     // one-shot; a software UG does not set UIF
  TIM7->EGR  = TIM_EGR_UG;                    // load PSC now
  TIM7->SR   = 0;

  HAL_NVIC_SetPriority(TIM7_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM7_IRQn);
}

void bsp_set_tx_wdt_callback(irq_callback cb)
{
  txWdtCallback = cb;
}

void bsp_tx_wdt_arm(uint32_t ms)
{
  if ( ms == 0 )
    ms = 1;
  if ( ms > 6500U )
    ms = 6500U;                               // 16-bit counter at 10 kHz

  TIM7->CR1 &= ~TIM_CR1_CEN;
  TIM7->DIER = 0;
  TIM7->CNT  = 0;
  TIM7->ARR  = ms * 10U - 1U;
  TIM7->SR   = 0;
  TIM7->DIER = TIM_DIER_UIE;
  TIM7->CR1 |= TIM_CR1_CEN;
}

void bsp_tx_wdt_disarm()
{
  TIM7->CR1 &= ~TIM_CR1_CEN;
  TIM7->DIER = 0;
  TIM7->SR   = 0;
  NVIC_ClearPendingIRQ(TIM7_IRQn);
}

void bsp_rf_safe_shutdown()
{
  // Stop every source of radio SPI traffic first (bit clocks, SOTDMA, TX watchdog)
  HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
  HAL_NVIC_DisableIRQ(EXTI3_IRQn);
  HAL_NVIC_DisableIRQ(TIM2_IRQn);
  HAL_NVIC_DisableIRQ(TIM7_IRQn);
  TIM7->CR1 &= ~TIM_CR1_CEN;
  __DSB();
  __ISB();

  HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_RESET);   // Q1 gate bias off
  HAL_GPIO_WritePin(SDN1_PORT, SDN1_PIN, GPIO_PIN_SET);           // U1 (transceiver) shut down
}

void bsp_set_trx_clk_callback(irq_callback cb)
{
  trxClockCallback = cb;
}

void bsp_set_rx_clk_callback(irq_callback cb)
{
  rxClockCallback = cb;
}

void bsp_set_gnss_sotdma_timer_callback(irq_callback cb)
{
  sotdmaCallback = cb;
}

uint32_t bsp_get_sotdma_timer_value()
{
  return TIM2->CNT;
}

void bsp_set_sotdma_timer_value(uint32_t v)
{
  TIM2->CNT = v;
}

uint32_t bsp_get_system_clock()
{
  return SystemCoreClock;
}

uint8_t bsp_tx_spi_byte(uint8_t data)
{
  uint8_t result = 0;
  HAL_SPI_TransmitReceive(&hspi1, &data, &result, 1, 2);
  return result;
}

void bsp_reboot()
{
  bsp_rf_safe_shutdown();
  bsp_gnss_off();
  HAL_Delay(50);
  NVIC_SystemReset();
}

bool bsp_is_tx_disabled()
{
  return HAL_GPIO_ReadPin(TX_DISABLE_PORT, TX_DISABLE_PIN) == GPIO_PIN_RESET;
}

void bsp_enter_dfu()
{
  // No RF while waiting for the reset: PA bias off, U1 in shutdown
  bsp_rf_safe_shutdown();

  // Cut off the GPS signals immediately to prevent its UART from transmitting and hijacking the bootloader upon reset
  bsp_gnss_off();

  HAL_Delay(1000);

  // This flag simply tells main() to jump to the ROM bootloader immediately upon reset, before initializing anything
  *(uint32_t*)BOOTMODE_ADDRESS = DFU_FLAG_MAGIC;

  bsp_reboot();
}

extern "C"
{

  void USART1_IRQHandler(void)
  {
    const uint32_t isr = USART1->ISR;

    // ORE stays set after a RDR read and will retrigger RXNEIE. Clear error
    // flags first, but do not discard RDR: on overrun the unread byte is still
    // valid and RXNE is set. FE/NE/PE are cleared the same way so a bad CLI
    // byte cannot lock the IRQ.
    if ( isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE) )
      {
#if NMEA_DIAG
        if ( isr & USART_ISR_ORE )
          sUsart1OreCount++;
        if ( isr & USART_ISR_FE )
          sUsart1FeCount++;
        if ( isr & USART_ISR_NE )
          sUsart1NeCount++;
        if ( isr & USART_ISR_PE )
          sUsart1PeCount++;
#endif
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
      }

    if ( isr & USART_ISR_RXNE )
      {
        char c = (char)USART1->RDR;
        if ( gUart1RxMute )
          {
            gUart1RxLost = 1;
#if NMEA_DIAG
            gUart1RxDiscard++;
#endif
          }
        else if ( terminalInputCallback )
          terminalInputCallback(c);
      }
  }

  void EXTI9_5_IRQHandler(void)
  {
    if ( __HAL_GPIO_EXTI_GET_IT(GNSS_1PPS_PIN) != RESET )
      {
        __HAL_GPIO_EXTI_CLEAR_IT(GNSS_1PPS_PIN);
        if ( ppsCallback )
          ppsCallback();
      }
  }

  void USART2_IRQHandler()
  {
    // USART2 NVIC is disabled; this is only a safety net. DMA owns RDR, so
    // never read it here (that used to drop a valid byte on ORE).
    const uint32_t isr = USART2->ISR;
    if ( isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE) )
      {
#if NMEA_DIAG
        countUsart2Errors(isr);
#endif
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
      }
  }


  void TIM7_IRQHandler(void)
  {
    if ( TIM7->SR & TIM_SR_UIF )
      {
        TIM7->SR = 0;
        TIM7->DIER = 0;
        if ( txWdtCallback )
          txWdtCallback();
      }
  }

  void TIM2_IRQHandler(void)
  {
    if(__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET)
      {
        if(__HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE) !=RESET)
          {
            __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
            if ( sotdmaCallback )
              sotdmaCallback();
          }
      }
  }

  void EXTI3_IRQHandler(void)
  {
    if ( __HAL_GPIO_EXTI_GET_IT(RX_IC_CLK_PIN) != RESET )
      {
        __HAL_GPIO_EXTI_CLEAR_IT(RX_IC_CLK_PIN);
        if ( rxClockCallback )
          rxClockCallback();
      }
  }

  void EXTI15_10_IRQHandler(void)
  {
    if ( __HAL_GPIO_EXTI_GET_IT(TRX_IC_CLK_PIN) != RESET )
      {
        __HAL_GPIO_EXTI_CLEAR_IT(TRX_IC_CLK_PIN);
        if ( trxClockCallback )
          trxClockCallback();
      }
  }

  void HAL_SYSTICK_Callback()
  {
    if ( tickCallback )
      tickCallback();
  }

}

#else
#error "bsp_12_1.cpp is the BSP for BOARD_REV 121 (MAIANA Blue v12.1.0)"
#endif
