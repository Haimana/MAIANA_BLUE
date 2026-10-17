/*
  Copyright (c) 2016-2020 Peter Antypas

  This file is part of the MAIANA™ transponder firmware.
*/

#ifndef UARTQUIET_HPP_
#define UARTQUIET_HPP_

#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint8_t gUart1RxMute;
extern volatile uint32_t gUart1RxDiscard;
#ifdef __cplusplus
}
#endif

class UartQuiet
{
public:
  static UartQuiet &instance();

  void poll();
  bool canStartLine();
  bool canStartBytes(uint16_t n);
  bool slotGated();
  void noteDefer(uint16_t n);
  void noteStart();
  bool rxMute();
  void write(const char *s);
  void writeCharRaw(char c);
  void countRxDiscard();

  uint32_t quietWindows() const;
  uint32_t holdBytes() const;
  uint32_t holdMax() const;
  uint32_t rxDiscarded() const;
  uint32_t outputDrops() const;
  uint32_t holdMaxMs() const;
  uint32_t capCloses() const;
  uint32_t slotSent() const;
  uint32_t slotDefer() const;
  uint32_t maxOutLatMs() const;
  uint32_t slotNofit() const;

private:
  UartQuiet();
  bool wantHold(uint32_t now);
  void enterHold(uint32_t now);
  void closeHold(uint32_t now, bool byCap);
  void flushHoldFitting();
  void holdPushSentence(const char *s);
  void emitChars(const char *s, uint16_t n);
  void noteDuration(uint32_t now);
  uint32_t bitsUntilTx() const;
  bool canStartBytesNoPoll(uint16_t n);
  bool wontFitFullSlot(uint16_t n) const;
  static uint32_t aisBitsForChars(uint16_t n);
  static uint32_t slotMarginBits();

  bool mHolding;
  bool mSawTx;
  bool mMidLine;
  uint32_t mHoldStart;
  uint32_t mPostUntil;
  uint32_t mDeferStart;
  uint16_t mHoldLen;
  uint32_t mWindows;
  uint32_t mHoldBytes;
  uint32_t mHoldMax;
  uint32_t mRxDiscard;
  uint32_t mOutDrop;
  uint32_t mHoldMaxMs;
  uint32_t mCapCloses;
  uint32_t mSlotSent;
  uint32_t mSlotDefer;
  uint32_t mMaxOutLatMs;
  uint32_t mSlotNofit;
  char mHold[UART_QUIET_HOLD_BUF];
};

#endif
