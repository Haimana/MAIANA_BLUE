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


#include "Transceiver.hpp"
#include "NoiseFloorDetector.hpp"
#include "EventQueue.hpp"
#include "Events.hpp"
#include "EZRadioPRO.h"
#include "AISChannels.h"
#include "bsp.hpp"
#include "TXErrors.h"
#include <stdio.h>
#include "TXScheduler.hpp"
#include "Configuration.hpp"
#include "config.h"
#include "CircularQueue.hpp"
#include "RadioState.hpp"

Transceiver::Transceiver(GPIO_TypeDef *sdnPort, uint32_t sdnPin, GPIO_TypeDef *csPort,
    uint32_t csPin, GPIO_TypeDef *dataPort, uint32_t dataPin,
    GPIO_TypeDef *clockPort, uint32_t clockPin, int chipId)
: Receiver(sdnPort, sdnPin, csPort, csPin, dataPort, dataPin, clockPort, clockPin, chipId)
{
  mTXPacket = NULL;
  mTXWdtTestMs = 0;
  mTXWdtTestHard = false;
  EventQueue::instance().addObserver(this, CLOCK_EVENT);
  mUTC = 0;
  mLastTXTime = 0;
  mChannel = CH_87;
}

void Transceiver::configure()
{
  Receiver::configure();

#if SI446X_ERRATA_2_12_RAMPDOWN_FIX
  /*
   * Si446x rev C2A errata 2.12 (digital PA ramp-down is an abrupt edge).
   * Silicon Labs workaround after POWER_UP: SPI write F1 47 4B 00, wait for
   * CTS, then SLEEP -> READY. Properties are kept in SLEEP.
   */
  {
    uint8_t patch[3] = { 0x47, 0x4B, 0x00 };
    sendCmd(0xF1, patch, sizeof patch, NULL, 0);
    uint8_t state = 0x01;     // SLEEP
    sendCmd(CHANGE_STATE, &state, 1, NULL, 0);
    state = 0x03;             // READY
    sendCmd(CHANGE_STATE, &state, 1, NULL, 0);
  }
#endif

  // Anything transmitter specific goes here
  SET_PROPERTY_PARAMS p;

  p.Group = 0x20;
  p.NumProperties = 1;
  p.StartProperty = 0x00;
  p.Data[0] = 0x20 | 0x08 | 0x03; // Synchronous direct mode from GPIO 1 with 2GFSK modulation
  sendCmd(SET_PROPERTY, &p, 4, NULL, 0);

  /**
   * Digital PA ramp-up (PA_DIG_PWR_SEQ_CONFIG, 0x2206) is written by setTXPower()
   * together with the level, so the ramp lasts ~PA_RAMP_TARGET_US for any level.
   * (Previously a fixed 0xFF: step 2, 12.7 us/step -> ~254 us at level 40 but
   * ~813 us at level 127.)
   */

  /*
   * AIS uses a slightly different Gaussian shape than default GMSK in the Si4463.
   * This sets the TX filter coefficients for a BT of 0.4 as per ITU specification.
   *
   * The TX shaping filter is a symmetric 17-tap FIR (10 samples/symbol); only 9
   * coefficients are stored: MODEM_TX_FILTER_COEFF_8 (centre tap) .. COEFF_0, at
   * MODEM group 0x20, properties 0x0F..0x17. Chip defaults (0x67..0x01) are BT=0.5.
   * The values below are a BT=0.4 Gaussian normalised to the same DC gain, so the
   * frequency deviation is unchanged. The shaping filter works in TX direct
   * synchronous mode (set above), not in asynchronous mode.
   *
   * Before 4.6.0 this was written to group 0x22 (PA), which has no such properties
   * (only 0x2200-0x2206), so the chip kept its default BT=0.5 filter.
   *
   * https://www.silabs.com/community/wireless/proprietary/forum.topic.html/si4463_gmsk_spectrum-ojkN
   */
  uint8_t data[] = { 0x52, 0x4f, 0x45, 0x37, 0x28, 0x1a, 0x10, 0x09, 0x04 };
  p.Group = 0x20;           // MODEM
  p.NumProperties = 9;
  p.StartProperty = 0x0f;   // MODEM_TX_FILTER_COEFF_8
  memcpy(p.Data, data, sizeof data);
  sendCmd(SET_PROPERTY, &p, 12, NULL, 0);

  // PA_PWR_LVL is 7-bit (0-127). pa_mode / pa_bias_clkduty follow the chip;
  // only pa_level is taken from stored TXConfig when present.
  pa_params pwr;
  paDefaults(mPartNumber, pwr);

  TXConfig cfg;
  if ( Configuration::instance().readTXConfig(cfg) )
    pwr.pa_level = cfg.pa_level;

  setTXPower(pwr);
}

void Transceiver::paDefaults(uint16_t partNumber, pa_params &pwr)
{
  pwr.pa_bias_clkduty = 0x00;
  if ( partNumber == 0x4463 )
    {
      pwr.pa_mode = 0x48;
      pwr.pa_level = 0x28;
    }
  else
    {
      pwr.pa_mode = 0x28;
      pwr.pa_level = 0x42;
    }
}

uint8_t Transceiver::defaultPALevel() const
{
  pa_params pwr;
  paDefaults(mPartNumber, pwr);
  return pwr.pa_level;
}

bool Transceiver::applyPALevel(uint8_t level)
{
  pa_params pwr;
  paDefaults(mPartNumber, pwr);
  pwr.pa_level = level;

  PrimaskGuard lock;
  if ( gRadioState == RADIO_TRANSMITTING )
    return false;
  setTXPower(pwr);
  return true;
}

void Transceiver::processEvent(const Event &e)
{
  switch(e.type)
  {
  case CLOCK_EVENT:
    mUTC = e.clock.utc;
    break;
  default:
    break;
  }
}

void Transceiver::transmitCW(VHFChannel channel)
{
  startReceiving(channel, false);
  configureGPIOsForTX();

  SET_PROPERTY_PARAMS p;
  p.Group = 0x20;
  p.NumProperties = 1;
  p.StartProperty = 0x00;
  p.Data[0] = 0x08;
  sendCmd (SET_PROPERTY, &p, 4, NULL, 0);


  TX_OPTIONS options;
  options.channel = AIS_CHANNELS[channel].ordinal;
  options.condition = 8 << 4;
  options.tx_len = 0;
  options.tx_delay = 0;
  options.repeats = 0;

  sendCmd (START_TX, &options, sizeof options, NULL, 0);
}

/**
 * PA_DIG_PWR_SEQ_CONFIG (0x2206): bit 7 INC_STEP_SIZE (0 = 1 DDAC step,
 * 1 = 2 steps), bits 6:0 DIG_PWR_SEQ_DELAY in 0.1 us (3..127). With
 * PA_MODE.DIG_PWR_SEQ set, the PA steps from 0 up to PA_PWR_LVL, so the
 * ramp-up lasts level x delay x 0.1 us. Single steps, delay per level.
 */
uint8_t Transceiver::digPwrSeqConfig(uint8_t level)
{
  uint32_t delay = 127u;
  if ( level )
    delay = (PA_RAMP_TARGET_US * 10u + level / 2u) / level;
  if ( delay < 3u )
    delay = 3u;
  if ( delay > 127u )
    delay = 127u;
  return (uint8_t)delay;      // INC_STEP_SIZE = 0
}

void Transceiver::setTXPower(const pa_params &pwr)
{
  SET_PROPERTY_PARAMS p;
  p.Group = 0x22;
  p.NumProperties = 3;
  p.StartProperty = 0x00;
  p.Data[0] = pwr.pa_mode;
  p.Data[1] = pwr.pa_level;
  p.Data[2] = pwr.pa_bias_clkduty;
  sendCmd(SET_PROPERTY, &p, 6, NULL, 0);

  p.Group = 0x22;
  p.NumProperties = 1;
  p.StartProperty = 0x06;     // PA_DIG_PWR_SEQ_CONFIG
  p.Data[0] = digPwrSeqConfig(pwr.pa_level);
  sendCmd(SET_PROPERTY, &p, 4, NULL, 0);
}


void Transceiver::configureGPIOsForTX()
{
  bsp_set_tx_mode();

  GPIO_PIN_CFG_PARAMS gpiocfg;
  gpiocfg.GPIO0 = 0x00;       // No change
  gpiocfg.GPIO1 = 0x04;       // RX/TX bit data
  gpiocfg.GPIO2 = 0x1F;       // RX/TX bit clock
#if BOARD_REV < 105
  gpiocfg.GPIO3 = 0x21;       // RX_STATE: high in RX, low otherwise (T/R switch, LNA)
#else
  gpiocfg.GPIO3 = 0x20;       // TX_STATE: high in TX, low otherwise (Q7 LNA supply / U8 CTRL)
#endif
  gpiocfg.NIRQ  = 0x00;       // No change
  gpiocfg.SDO   = 0x00;       // No change
  gpiocfg.GENCFG = 0x00;      // No change
  sendCmd(GPIO_PIN_CFG, &gpiocfg, sizeof gpiocfg, NULL, 0);
}


void Transceiver::assignTXPacket(TXPacket *p)
{
  ASSERT(!mTXPacket);
  mTXPacket = p;
  mTXPacket->setTimestamp(mUTC);
}

TXPacket* Transceiver::assignedTXPacket()
{
  return mTXPacket;
}

uint32_t Transceiver::bitsUntilPossibleTx() const
{
  if ( gRadioState == RADIO_TRANSMITTING )
    return 0;

  // Snapshot ISR-owned fields; do not add work to the bit-clock path.
  PrimaskGuard lock;
  TXPacket *p = mTXPacket;
  if ( !p )
    return 0xffffffff;

  const bool test = p->isTestPacket();
  const time_t ts = p->timestamp();
  const time_t utc = mUTC;
  const time_t last = mLastTXTime;
  int bit = mSlotBitNumber;

  if ( !test )
    {
      if ( utc && ts && utc - ts >= MIN_MSG_18_TX_INTERVAL )
        return 0xffffffff;
      if ( last && utc - last < MIN_TX_INTERVAL )
        return 0xffffffff;
    }

  // TX starts in the bit-clock ISR at CCA_SLOT_BIT (11) of this slot if
  // CCA is clear. After that bit, the next start is bit 11 of the next slot.
  if ( bit < 0 || bit > 255 )
    return 0;

  if ( bit <= CCA_SLOT_BIT )
    return (uint32_t)(CCA_SLOT_BIT - bit);

  return (uint32_t)((256 - bit) + CCA_SLOT_BIT);
}

uint32_t Transceiver::msUntilPossibleTx() const
{
  const uint32_t bits = bitsUntilPossibleTx();
  if ( bits == 0xffffffff )
    return 0xffffffff;
  return (uint32_t)((bits * 1000u + 9599u) / 9600u);
}

/**
 * This method is called in interrupt context
 */
void Transceiver::onBitClock()
{
  if ( gRadioState == RADIO_RECEIVING )
    {
      Receiver::onBitClock();
      /*
          We start transmitting a packet if:
            - We have a TX packet assigned
            - Transmission is enabled
            - We are at bit CCA_SLOT_BIT+1 (after obtaining an RSSI level)
            - The TX packet's transmission channel is our current listening channel
            - The RSSI is within 6dB of the noise floor for this channel
            - It's been at least MIN_TX_INTERVAL seconds since our last transmission
       */

      if ( !mTXPacket )
        return;


      if ( mTXPacket->isTestPacket() )
        {
          // Test packets are sent immediately. Presumably, we're firing into a dummy load ;)
          startTransmitting();
        }
      else if ( !TXScheduler::instance().isTXAllowed() )
        {
          // Transmission has been disabled since scheduling, so don't transmit!
          TXPacketPool::instance().deleteTXPacket(mTXPacket);
          mTXPacket = NULL;
        }
      else if ( mUTC && mUTC - mTXPacket->timestamp() >= MIN_MSG_18_TX_INTERVAL )
        {
          // The packet is way too old. Discard it.
#if REPORT_TX_SCHEDULING
          Event *e = EventPool::instance().newEvent(PROPR_NMEA_SENTENCE);
          if ( e )
            {
              sprintf(e->nmeaBuffer.sentence, "$PAISCHTX,%s,%d*", mTXPacket->messageType(), TX_PACKET_TOO_OLD);
              Utils::completeNMEA(e->nmeaBuffer.sentence);
              EventQueue::instance().push(e);
            }
#endif
          TXPacketPool::instance().deleteTXPacket(mTXPacket);
          mTXPacket = NULL;
        }
      else if ( mUTC - mLastTXTime < MIN_TX_INTERVAL )
        {
          // It's not time to transmit yet
          return;
        }
      else if ( mUTC && mSlotBitNumber == CCA_SLOT_BIT && mTXPacket->channel() == mChannel )
        {
          // It has already been sampled during Receiver::onBitClock();
          int rssi = mRXPacket->rssi();
          int nf = NoiseFloorDetector::instance().getNoiseFloor(AIS_CHANNELS[mChannel].designation);
          if ( rssi <= nf + TX_CCA_HEADROOM )
            {
              startTransmitting();
            }
        }
    }
  else if ( mTXPacket )
    {
      if ( mTXPacket->eof() )
        {
          bsp_tx_wdt_disarm();
          mTXWdtTestMs = 0;
          mTXWdtTestHard = false;
          mLastTXTime = mUTC;
          startReceiving(mChannel, true);
          gRadioState = RADIO_RECEIVING;
          reportTXEvent();
          TXPacketPool::instance().deleteTXPacket(mTXPacket);
          mTXPacket = NULL;
        }
      else
        {
          uint8_t bit = mTXPacket->nextBit();
          if ( bit )
            HAL_GPIO_WritePin(mDataPort, mDataPin, GPIO_PIN_SET);
          else
            HAL_GPIO_WritePin(mDataPort, mDataPin, GPIO_PIN_RESET);

          /**
           * As of September 2020, the digital ramp-down of the Si4463 is broken and not
           * in the latest firmware patch either, so this is a good alternative.
           *
           * The packet has 2-3 ramp down bits, so when it tells us it's reached them,
           * we ramp the bias voltage down by means of the RC delay circuit.
           * This really helped clean up spurs.
           *
           */
          if ( mTXPacket->canRampDown() )
            HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_RESET);
        }
    }
  else
    {
      // We're transmitting carrier wave, do nothing. This is only done during XO trimming.
    }
}


void Transceiver::timeSlotStarted(uint32_t slot)
{
  Receiver::timeSlotStarted(slot);

  // Switch channel if we have a transmission scheduled and we're not on the right channel
  if ( gRadioState == RADIO_RECEIVING && mTXPacket && mTXPacket->channel() != mChannel )
    startReceiving(mTXPacket->channel(), false);
}

void Transceiver::startTransmitting()
{
  // Configure the RFIC GPIOs for transmission
  // Configure the pin for GPIO 1 as output
  // Set TX power level
  // Start transmitting
  gRadioState = RADIO_TRANSMITTING;
  // Armed before the PA bias goes on; disarmed at end of packet or abort
  bsp_tx_wdt_arm(txWatchdogMs());
  configureGPIOsForTX();

  //ASSERT(false);


  TX_OPTIONS options;
  options.channel     = AIS_CHANNELS[mTXPacket->channel()].ordinal;
  options.condition   = 0;
  options.tx_len      = 0;
  options.tx_delay    = 0;
  options.repeats     = 0;

  sendCmd(START_TX, &options, sizeof options, NULL, 0);

  // Ensure all data changes in the function have completed, otherwise gRadioState may not actually be modified
  __DSB();

}

/**
 * Watchdog time for the packet about to be sent: its air time at 9600 bps
 * plus TX_WDT_MARGIN_MS. Normal packets are capped at TX_WDT_MAX_SLOTS slots.
 */
uint32_t Transceiver::txWatchdogMs() const
{
#if TX_WDT_TEST_CMD
  if ( mTXWdtTestMs )
    return mTXWdtTestMs;
#endif
  uint32_t bits = mTXPacket ? mTXPacket->size() : 0u;
  if ( mTXPacket && !mTXPacket->isTestPacket() && bits > TX_WDT_MAX_SLOTS * 256u )
    bits = TX_WDT_MAX_SLOTS * 256u;
  return (bits * 1000u + 9599u) / 9600u + TX_WDT_MARGIN_MS;
}

void Transceiver::setTXWatchdogTest(uint32_t ms, bool simulateUnresponsive)
{
  PrimaskGuard lock;
  mTXWdtTestMs = ms;
  mTXWdtTestHard = simulateUnresponsive;
}

/**
 * Stop a transmission now. Called from the TX watchdog interrupt (same NVIC
 * priority as the bit clocks) or from main context with interrupts masked.
 * PA bias goes off first. If U1 answers CTS within a bounded poll it is put
 * back into RX on the current channel; otherwise it is shut down (SDN high)
 * and the caller must reboot to recover it.
 * Returns true if U1 is back in RX.
 */
bool Transceiver::abortTransmission()
{
  bsp_tx_wdt_disarm();
  HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_RESET);

  bool responsive = !mTXWdtTestHard && pollCTS(TX_ABORT_CTS_POLLS);
  if ( responsive )
    startReceiving(mChannel, true);
  else
    HAL_GPIO_WritePin(mSDNP, mSDNPin, GPIO_PIN_SET);

  if ( mTXPacket )
    {
      TXPacketPool::instance().deleteTXPacket(mTXPacket);
      mTXPacket = NULL;
    }
  mTXWdtTestMs = 0;
  mTXWdtTestHard = false;
  gRadioState = RADIO_RECEIVING;
  __DSB();
  return responsive;
}

void Transceiver::startReceiving(VHFChannel channel, bool reconfigGPIOs)
{
  Receiver::startReceiving(channel, reconfigGPIOs);
}

void Transceiver::configureGPIOsForRX()
{
  bsp_set_rx_mode();

  GPIO_PIN_CFG_PARAMS gpiocfg;
  gpiocfg.GPIO0 = 0x00;       // No change
  gpiocfg.GPIO1 = 0x14;       // RX data bits
  gpiocfg.GPIO2 = 0x1F;       // RX/TX data clock
#if BOARD_REV < 105
  gpiocfg.GPIO3 = 0x21;       // RX_STATE: high in RX, low otherwise (T/R switch, LNA)
#else
  gpiocfg.GPIO3 = 0x20;       // TX_STATE: high in TX, low otherwise (Q7 LNA supply / U8 CTRL)
#endif
  gpiocfg.NIRQ  = 0x00;       // No change
  gpiocfg.SDO   = 0x00;       // No change
  gpiocfg.GENCFG = 0x00;      // No change
  sendCmd(GPIO_PIN_CFG, &gpiocfg, sizeof gpiocfg, NULL, 0);
}

void Transceiver::reportTXEvent()
{
  Event *e = EventPool::instance().newEvent(PROPR_NMEA_SENTENCE);
  if ( !e )
    return;

  snprintf(e->nmeaBuffer.sentence, sizeof e->nmeaBuffer.sentence, "$PAITX,%c,%s*", AIS_CHANNELS[mTXPacket->channel()].designation, mTXPacket->messageType());
  Utils::completeNMEA(e->nmeaBuffer.sentence);
  EventQueue::instance().push(e);
  bsp_tx_led_off();
}
