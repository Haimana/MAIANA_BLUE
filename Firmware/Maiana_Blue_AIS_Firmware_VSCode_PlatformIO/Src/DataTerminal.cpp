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


#include "DataTerminal.hpp"
#include "Events.hpp"
#include "Utils.hpp"
#include "GPS.hpp"
#include "UartQuiet.hpp"
#include "bsp.hpp"
#include <ctype.h>
#include <stdio.h>

static char __rxbuff[80];
static uint8_t __rxpos = 0;
static volatile uint32_t sCommandDrop = 0;

void termInputCB(char c);

DataTerminal &DataTerminal::instance()
{
  static DataTerminal __instance;
  return __instance;
}

void DataTerminal::init()
{
  bsp_set_terminal_input_callback(termInputCB);
}

DataTerminal::DataTerminal()
: mCmdBuffPos(0)
{
  mCmdTokens.reserve(5);
  EventQueue::instance().addObserver(this, DEBUG_EVENT|PROPR_NMEA_SENTENCE);
}

void DataTerminal::processEvent(const Event &e)
{
  switch (e.type) {
  case DEBUG_EVENT:
#ifdef MULTIPLEXED_OUTPUT
    write("DEBUG", e.debugMessage.buffer);
#endif
    break;
  case PROPR_NMEA_SENTENCE:
#ifdef MULTIPLEXED_OUTPUT
    write("NMEA", e.nmeaBuffer.sentence);
#else
    write(e.nmeaBuffer.sentence);
#endif
    break;
  default:
    break;
  }
}

#ifdef MULTIPLEXED_OUTPUT

void DataTerminal::write(const char *cls, const char* s)
{
  GPS::instance().flushEchoLine();
  char line[160];
  snprintf(line, sizeof line, "[%s]%s", cls ? cls : "", s ? s : "");
  UartQuiet::instance().write(line);
}

#else

void DataTerminal::write(const char* s)
{
  GPS::instance().flushEchoLine();
  UartQuiet::instance().write(s);
}
#endif



uint32_t DataTerminal::commandDropCount() const
{
  return sCommandDrop;
}

void DataTerminal::_write(const char *s)
{
#ifdef MULTIPLEXED_OUTPUT
  write("", s);
#else
  write(s);
#endif
}


void termInputCB(char c)
{
  if ( c == '\r' )
    {
      return;
    }

  // TX EMI on USART1 RX often prefixes a command with junk (0xAA, 'b', …).
  // Every CLI verb starts with a letter; drop leading non-alpha so "nmea?"
  // is not stored as "\xAAnmea?" and then ignored by a prefix match.
  if ( __rxpos == 0 && c != '\n' && !isalpha((unsigned char)c) )
    return;

  __rxbuff[__rxpos++] = c;
  if ( __rxpos >= sizeof __rxbuff )
    {
      __rxpos = 0;
    }
  else if ( c == '\n' )
    {
      __rxbuff[__rxpos++] = 0;
      Event *e = EventPool::instance().newEvent(COMMAND_EVENT);
      if ( e )
        {
          strlcpy(e->command.buffer, __rxbuff, sizeof e->command.buffer);
          EventQueue::instance().push(e);
        }
      else
        sCommandDrop++;
      __rxpos = 0;
    }
}





