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
  bool rxMute();
  void write(const char *s);
  void writeCharRaw(char c);
  void countRxDiscard();

  uint32_t quietWindows() const;
  uint32_t holdBytes() const;
  uint32_t holdMax() const;
  uint32_t rxDiscarded() const;
  uint32_t outputDrops() const;

private:
  UartQuiet();
  bool computeHold();
  void enterHold();
  void flushHold();
  void holdPush(char c);

  bool mHolding;
  bool mSawTx;
  bool mMidLine;
  uint32_t mPostUntil;
  uint16_t mHoldLen;
  uint32_t mWindows;
  uint32_t mHoldBytes;
  uint32_t mHoldMax;
  uint32_t mRxDiscard;
  uint32_t mOutDrop;
  char mHold[UART_QUIET_HOLD_BUF];
};

#endif
