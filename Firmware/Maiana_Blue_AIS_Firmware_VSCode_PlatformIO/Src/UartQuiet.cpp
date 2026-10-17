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
  : mHolding(false), mSawTx(false), mMidLine(false),
    mHoldStart(0), mPostUntil(0), mDeferStart(0), mOpenSince(0), mHoldLen(0),
    mWindows(0), mHoldBytes(0), mHoldMax(0),
    mRxDiscard(0), mOutDrop(0), mHoldMaxMs(0), mCapCloses(0),
    mSlotSent(0), mSlotDefer(0), mMaxOutLatMs(0), mSlotNofit(0)
{
}

uint32_t UartQuiet::aisBitsForChars(uint16_t n)
{
  // 8N1 at 38400 vs AIS bit clock 9600: ceil(n * 10 / 4) = ceil(n * 2.5)
  return ((uint32_t)n * 5u + 1u) / 2u;
}

uint32_t UartQuiet::slotMarginBits()
{
  // SLOT_MARGIN_MS before the next bit 11, plus one UART char of TC after TXE.
  return (uint32_t)UART_QUIET_SLOT_MARGIN_MS * 9600u / 1000u + 3u;
}

uint32_t UartQuiet::bitsUntilTx() const
{
  Transceiver *trx = RadioManager::instance().getTransceiver();
  if ( !trx )
    return 0xffffffff;
  return trx->bitsUntilPossibleTx();
}

bool UartQuiet::wontFitFullSlot(uint16_t n) const
{
  // Longest free window is just after bit 11 with no TX: 255 AIS bits.
  return aisBitsForChars(n) + slotMarginBits() > 255u;
}

bool UartQuiet::canStartBytesNoPoll(uint16_t n)
{
  if ( n == 0 )
    return false;

  if ( mHolding || gRadioState == RADIO_TRANSMITTING )
    return false;

  if ( mPostUntil != 0 )
    return false;

  const uint32_t bits = bitsUntilTx();
  if ( bits == 0xffffffff )
    return true;

  if ( bits == 0 )
    return false;

  return bits >= aisBitsForChars(n) + slotMarginBits();
}

bool UartQuiet::canStartBytes(uint16_t n)
{
  poll();
  return canStartBytesNoPoll(n);
}

bool UartQuiet::canStartLine()
{
  return canStartBytes(82);
}

bool UartQuiet::slotGated()
{
  poll();
  if ( mHolding )
    return true;
  return bitsUntilTx() != 0xffffffff;
}

void UartQuiet::noteDefer(uint16_t n)
{
  if ( mDeferStart == 0 )
    {
      mDeferStart = HAL_GetTick();
      if ( wontFitFullSlot(n) )
        mSlotNofit++;
    }
  mSlotDefer++;
}

void UartQuiet::noteStart()
{
  if ( mDeferStart != 0 )
    {
      const uint32_t dur = HAL_GetTick() - mDeferStart;
      if ( dur > mMaxOutLatMs )
        mMaxOutLatMs = dur;
      mDeferStart = 0;
    }

  if ( bitsUntilTx() != 0xffffffff )
    mSlotSent++;
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

  return false;
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
    mCapCloses++;
  flushHoldFitting();
}

void UartQuiet::poll()
{
  const uint32_t now = HAL_GetTick();
  const bool tx = (gRadioState == RADIO_TRANSMITTING);

  if ( tx )
    mSawTx = true;
  else if ( mSawTx )
    {
      mSawTx = false;
      mPostUntil = now + UART_QUIET_POST_TX_MS;
    }

  const bool hold = wantHold(now);
  if ( hold )
    {
      if ( !mHolding )
        enterHold(now);

      noteDuration(now);

      // Safety cap: TX+post is already short. Never unmute during PA-on.
      if ( !tx && mPostUntil == 0 &&
           (now - mHoldStart) >= (uint32_t)UART_QUIET_MAX_MS )
        closeHold(now, true);
    }
  else if ( mHolding )
    {
      closeHold(now, false);
    }
  else if ( mHoldLen && !mMidLine )
    {
      if ( holdOpen() )
        {
          if ( mOpenSince == 0 )
            mOpenSince = now;
          else if ( (now - mOpenSince) >= (uint32_t)UART_QUIET_OPEN_HOLD_MS )
            {
              holdPushSentence("\r\n");
              mOpenSince = 0;
            }
        }
      else
        mOpenSince = 0;

      flushHoldFitting();
    }

  gUart1RxMute = (mHolding || tx) ? 1 : 0;
}

bool UartQuiet::shouldSpin()
{
  if ( mHoldLen && !mHolding )
    return true;
  if ( mDeferStart != 0 && !mHolding )
    return true;
  return false;
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

void UartQuiet::emitChars(const char *s, uint16_t n)
{
  for ( uint16_t i = 0; i < n; ++i )
    writeCharRaw(s[i]);
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
  if ( holdOpen() && mOpenSince == 0 )
    mOpenSince = HAL_GetTick();
  else if ( !holdOpen() )
    mOpenSince = 0;
}

bool UartQuiet::holdOpen() const
{
  return mHoldLen > 0 && mHold[mHoldLen - 1] != '\n';
}

bool UartQuiet::holdHasComplete() const
{
  for ( uint16_t i = 0; i < mHoldLen; ++i )
    {
      if ( mHold[i] == '\n' )
        return true;
    }
  return false;
}

uint16_t UartQuiet::startNeed(const char *s, uint16_t n)
{
  // A write without LF is an open sentence; reserve CRLF so we do not
  // start a body that cannot finish before the next CCA bit.
  for ( uint16_t i = 0; i < n; ++i )
    {
      if ( s[i] == '\n' )
        return n;
    }
  return (uint16_t)(n + 2);
}

void UartQuiet::flushHoldFitting()
{
  if ( mMidLine )
    return;

  uint16_t pos = 0;
  while ( pos < mHoldLen )
    {
      uint16_t end = pos;
      while ( end < mHoldLen && mHold[end] != '\n' )
        ++end;

      if ( end >= mHoldLen )
        break;

      const uint16_t n = (uint16_t)(end - pos + 1);
      if ( !canStartBytesNoPoll(n) )
        {
          if ( mDeferStart == 0 )
            mDeferStart = HAL_GetTick();
          break;
        }

      noteStart();
      emitChars(mHold + pos, n);
      pos = (uint16_t)(end + 1);
    }

  if ( pos == 0 )
    return;

  const uint16_t rest = (uint16_t)(mHoldLen - pos);
  if ( rest )
    memmove(mHold, mHold + pos, rest);
  mHoldLen = rest;
}

void UartQuiet::write(const char *s)
{
  poll();

  if ( !s || !*s )
    return;

  const bool newNmea = (s[0] == '$' || s[0] == '!');

  // Never glue a new NMEA sentence onto an in-flight line. Close the
  // open line first; the original CRLF (if it arrives later) is then
  // just an empty line.
  if ( mMidLine && newNmea )
    {
      writeCharRaw('\r');
      writeCharRaw('\n');
    }
  else if ( mMidLine )
    {
      while ( *s )
        {
          const char c = *s++;
          writeCharRaw(c);
          if ( c == '\n' )
            break;
        }
      if ( !*s )
        return;
    }

  if ( holdOpen() && newNmea )
    holdPushSentence("\r\n");
  else if ( holdOpen() )
    {
      holdPushSentence(s);
      if ( !mHolding )
        flushHoldFitting();
      return;
    }

  const uint16_t n = (uint16_t)strlen(s);
  const uint16_t need = startNeed(s, n);

  // Preserve order: do not start a new sentence while a complete one
  // is already waiting in the hold (window can open between flush and emit).
  if ( mHolding || holdHasComplete() || !canStartBytesNoPoll(need) )
    {
      noteDefer(need);
      holdPushSentence(s);
      if ( !mHolding && !mMidLine )
        flushHoldFitting();
      return;
    }

  noteStart();
  emitChars(s, n);
}

uint32_t UartQuiet::quietWindows() const { return mWindows; }
uint32_t UartQuiet::holdBytes() const { return mHoldBytes; }
uint32_t UartQuiet::holdMax() const { return mHoldMax; }
uint32_t UartQuiet::rxDiscarded() const { return mRxDiscard + gUart1RxDiscard; }
uint32_t UartQuiet::outputDrops() const { return mOutDrop; }
uint32_t UartQuiet::holdMaxMs() const { return mHoldMaxMs; }
uint32_t UartQuiet::capCloses() const { return mCapCloses; }
uint32_t UartQuiet::slotSent() const { return mSlotSent; }
uint32_t UartQuiet::slotDefer() const { return mSlotDefer; }
uint32_t UartQuiet::maxOutLatMs() const { return mMaxOutLatMs; }
uint32_t UartQuiet::slotNofit() const { return mSlotNofit; }
