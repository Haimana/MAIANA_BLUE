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
#include <string.h>

volatile uint8_t gUart1RxMute = 0;
volatile uint32_t gUart1RxDiscard = 0;

UartQuiet &UartQuiet::instance()
{
  static UartQuiet __instance;
  return __instance;
}

UartQuiet::UartQuiet()
  : mHolding(false), mBlocked(false), mSawTx(false), mMidLine(false),
    mHoldStart(0), mPostUntil(0), mHoldLen(0),
    mWindows(0), mHoldBytes(0), mHoldMax(0),
    mRxDiscard(0), mOutDrop(0), mHoldMaxMs(0), mCapCloses(0)
{
}

uint32_t UartQuiet::msUntilTx() const
{
  Transceiver *trx = RadioManager::instance().getTransceiver();
  if ( !trx )
    return 0xffffffff;
  return trx->msUntilPossibleTx();
}

void UartQuiet::noteDuration(uint32_t now)
{
  const uint32_t dur = now - mHoldStart;
  if ( dur > mHoldMaxMs )
    mHoldMaxMs = dur;
}

bool UartQuiet::wantHold(uint32_t now)
{
  if ( gRadioState == RADIO_TRANSMITTING )
    return true;

  if ( mPostUntil != 0 )
    {
      if ( (int32_t)(now - mPostUntil) < 0 )
        return true;
      mPostUntil = 0;
    }

  if ( mBlocked )
    return false;

  return msUntilTx() <= (uint32_t)UART_QUIET_PRE_TX_MS;
}

void UartQuiet::enterHold(uint32_t now)
{
  mHolding = true;
  mHoldStart = now;
  gUart1RxMute = 1;
  mWindows++;
}

void UartQuiet::closeHold(uint32_t now, bool byCap)
{
  noteDuration(now);
  mHolding = false;
  gUart1RxMute = 0;
  if ( byCap )
    {
      mBlocked = true;
      mCapCloses++;
    }
  flushHold();
}

void UartQuiet::poll()
{
  const uint32_t now = HAL_GetTick();
  const bool tx = (gRadioState == RADIO_TRANSMITTING);

  if ( tx )
    {
      mSawTx = true;
      mBlocked = false;
    }
  else if ( mSawTx )
    {
      mSawTx = false;
      mPostUntil = now + UART_QUIET_POST_TX_MS;
    }

  if ( mBlocked && !tx && msUntilTx() == 0xffffffff )
    mBlocked = false;

  const bool hold = wantHold(now);
  if ( hold )
    {
      if ( !mHolding )
        enterHold(now);

      noteDuration(now);

      // Cap the PRE wait only. Do not cut an in-progress PA burst or the
      // short post-TX tail (those are already bounded by the airtime).
      if ( !tx && mPostUntil == 0 &&
           (now - mHoldStart) >= (uint32_t)UART_QUIET_MAX_MS )
        closeHold(now, true);
    }
  else if ( mHolding )
    {
      closeHold(now, false);
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

void UartQuiet::holdPushSentence(const char *s)
{
  if ( !s )
    return;

  const size_t n = strlen(s);
  if ( n == 0 )
    return;

  if ( (uint32_t)mHoldLen + n > (uint32_t)UART_QUIET_HOLD_BUF )
    {
      mOutDrop++;
      return;
    }

  memcpy(mHold + mHoldLen, s, n);
  mHoldLen = (uint16_t)(mHoldLen + n);
  mHoldBytes += (uint32_t)n;
  if ( mHoldLen > mHoldMax )
    mHoldMax = mHoldLen;
}

void UartQuiet::flushHold()
{
  uint16_t end = mHoldLen;
  while ( end > 0 && mHold[end - 1] != '\n' )
    --end;

  if ( end < mHoldLen )
    mOutDrop++;

  for ( uint16_t i = 0; i < end; ++i )
    writeCharRaw(mHold[i]);
  mHoldLen = 0;
}

void UartQuiet::write(const char *s)
{
  poll();

  if ( !s )
    return;

  if ( mHolding )
    {
      if ( mMidLine )
        {
          // Finish the in-flight sentence on the wire, then hold the rest.
          // Never start a new sentence during a quiet window.
          while ( *s )
            {
              const char c = *s++;
              writeCharRaw(c);
              if ( c == '\n' )
                break;
            }
        }
      if ( *s )
        holdPushSentence(s);
      return;
    }

  for ( ; *s; ++s )
    writeCharRaw(*s);
}

uint32_t UartQuiet::quietWindows() const { return mWindows; }
uint32_t UartQuiet::holdBytes() const { return mHoldBytes; }
uint32_t UartQuiet::holdMax() const { return mHoldMax; }
uint32_t UartQuiet::rxDiscarded() const { return mRxDiscard + gUart1RxDiscard; }
uint32_t UartQuiet::outputDrops() const { return mOutDrop; }
uint32_t UartQuiet::holdMaxMs() const { return mHoldMaxMs; }
uint32_t UartQuiet::capCloses() const { return mCapCloses; }
