/*
  Copyright (c) 2016-2020 Peter Antypas

  This file is part of the MAIANA™ transponder firmware.
*/

#include "UartQuiet.hpp"
#include "RadioManager.hpp"
#include "Transceiver.hpp"
#include "RadioState.hpp"
#include "bsp.hpp"
#include <stm32l4xx_hal.h>

volatile uint8_t gUart1RxMute = 0;
volatile uint32_t gUart1RxDiscard = 0;

UartQuiet &UartQuiet::instance()
{
  static UartQuiet __instance;
  return __instance;
}

UartQuiet::UartQuiet()
  : mHolding(false), mSawTx(false), mMidLine(false), mPostUntil(0),
    mHoldLen(0), mWindows(0), mHoldBytes(0), mHoldMax(0),
    mRxDiscard(0), mOutDrop(0)
{
}

bool UartQuiet::computeHold()
{
  if ( gRadioState == RADIO_TRANSMITTING )
    return true;

  if ( mPostUntil != 0 )
    {
      if ( (int32_t)(HAL_GetTick() - mPostUntil) < 0 )
        return true;
      mPostUntil = 0;
    }

  Transceiver *trx = RadioManager::instance().getTransceiver();
  if ( !trx )
    return false;

  const uint32_t ms = trx->msUntilPossibleTx();
  return ms <= (uint32_t)UART_QUIET_PRE_TX_MS;
}

void UartQuiet::enterHold()
{
  if ( mHolding )
    return;
  mHolding = true;
  gUart1RxMute = 1;
  mWindows++;
}

void UartQuiet::poll()
{
  const bool tx = (gRadioState == RADIO_TRANSMITTING);
  if ( tx )
    mSawTx = true;
  else if ( mSawTx )
    {
      mSawTx = false;
      mPostUntil = HAL_GetTick() + UART_QUIET_POST_TX_MS;
    }

  const bool hold = computeHold();
  if ( hold )
    enterHold();
  else if ( mHolding )
    {
      mHolding = false;
      gUart1RxMute = 0;
      flushHold();
    }

  gUart1RxMute = (mHolding || tx) ? 1 : 0;
}

bool UartQuiet::canStartLine()
{
  poll();
  return !mHolding;
}

bool UartQuiet::rxMute()
{
  return mHolding || (gRadioState == RADIO_TRANSMITTING);
}

void UartQuiet::countRxDiscard()
{
  mRxDiscard++;
}

void UartQuiet::writeCharRaw(char c)
{
  bsp_write_char(c);
  mMidLine = (c != '\n');
}

void UartQuiet::holdPush(char c)
{
  if ( mHoldLen >= UART_QUIET_HOLD_BUF )
    {
      mOutDrop++;
      return;
    }
  mHold[mHoldLen++] = c;
  mHoldBytes++;
  if ( mHoldLen > mHoldMax )
    mHoldMax = mHoldLen;
}

void UartQuiet::flushHold()
{
  for ( uint16_t i = 0; i < mHoldLen; ++i )
    writeCharRaw(mHold[i]);
  mHoldLen = 0;
}

void UartQuiet::write(const char *s)
{
  poll();

  if ( mHolding && !mMidLine )
    {
      for ( ; s && *s; ++s )
        holdPush(*s);
      return;
    }

  for ( ; s && *s; ++s )
    writeCharRaw(*s);
}

uint32_t UartQuiet::quietWindows() const { return mWindows; }
uint32_t UartQuiet::holdBytes() const { return mHoldBytes; }
uint32_t UartQuiet::holdMax() const { return mHoldMax; }
uint32_t UartQuiet::rxDiscarded() const { return mRxDiscard + gUart1RxDiscard; }
uint32_t UartQuiet::outputDrops() const { return mOutDrop; }
