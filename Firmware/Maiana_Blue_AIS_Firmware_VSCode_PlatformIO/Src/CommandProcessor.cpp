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


#include "CommandProcessor.hpp"
#include "EventQueue.hpp"
#include "DataTerminal.hpp"
#include "Utils.hpp"
#include "TXScheduler.hpp"
#include "config.h"
#include "StationData.h"
#include "TXScheduler.hpp"
#include "bsp.hpp"
#include "GPS.hpp"
#include "VesselHeading.hpp"
#if NMEA_DIAG
#include "UartQuiet.hpp"
#endif
#include "RadioManager.hpp"
#include "EZRadioPRO.h"
#include <stdlib.h>
#include "RadioState.hpp"
#include "TXPacket.hpp"
#include "Configuration.hpp"
#include "AISChannels.h"
#include <stm32l4xx_hal.h>
#include <stdio.h>

CommandProcessor &CommandProcessor::instance()
{
  static CommandProcessor __instance;
  return __instance;
}

void CommandProcessor::init()
{

}

CommandProcessor::CommandProcessor()
{
  EventQueue::instance().addObserver(this, COMMAND_EVENT);
}

void CommandProcessor::processEvent(const Event &e)
{
  switch(e.type)
  {
  case COMMAND_EVENT:
    processCommand(e.command.buffer);
    break;
  default:
    break;
  }
}

void fireTestPacket()
{
  VHFChannel channel = CH_87;

  if ( rand() % 2 == 0 )
    channel = CH_88;

  TXPacket *p = TXPacketPool::instance().newTXPacket(channel);
  if ( !p ) {
      return;
  }

  /**
   * Define a dummy packet of 9600 random bits, so it will take 1 second to transmit.
   * This is long enough for most spectrum analyzers to capture details using "max hold",
   * even at very low resolution bandwidths. Great way to measure power and look for
   * spurious emissions as well as harmonics.
   */
  p->configureForTesting(channel, 9600);

  RadioManager::instance().sendTestPacketNow(p);
}

static void skipToKnownCommand(string &s)
{
  static const char *const cmds[] = {
      "help",
      "station ",
      "station?",
      "sys?",
#if NMEA_DIAG
      "nmea?",
#endif
      "hourlyreboot on",
      "hourlyreboot off",
      "hourlyreboot?",
      "dfu",
      "erase station",
      "factory reset",
      "tx test",
      "tx on",
      "tx off",
      "tx?",
      "stoptx",
      "reboot",
      "cli",
      "gps off",
      "gps on",
      "gnss?",
      "heading off",
      "heading?",
      "heading ",
      "msg24",
      "otp?",
      "otp ",
      "xotrim ",
      "xotrim?",
      "pa_level ",
      "pa_level?"
  };

  size_t best = string::npos;
  for ( unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; ++i )
    {
      size_t p = s.find(cmds[i]);
      if ( p != string::npos && (best == string::npos || p < best) )
        best = p;
    }

  if ( best != string::npos && best > 0 )
    s.erase(0, best);
}

void CommandProcessor::processCommand(const char *buff)
{
  string s(buff);
  Utils::trim(s);

  if ( !s.empty() && s[0] == '$' )
    {
      VesselHeading::instance().acceptSentence(s.c_str());
      return;
    }

  skipToKnownCommand(s);

  if ( s.find("help") == 0 )
    {
      DataTerminal::instance().write("Available Commands:\r\n");
      DataTerminal::instance().write("-------------------\r\n");
      DataTerminal::instance().write("  help                                - Display this list of commands\r\n");
      DataTerminal::instance().write("  station <mmsi>,<name>,<callsign>,<type>,<len>,<beam>,<portoffset>,<bowoffset> - Configure vessel static data\r\n");
      DataTerminal::instance().write("  station?                            - Report current station data\r\n");
      DataTerminal::instance().write("  erase station                       - Erase configured station data\r\n");
      DataTerminal::instance().write("  factory reset                       - Reset all settings to factory defaults\r\n");
      DataTerminal::instance().write("  sys?                                - Report system data (FW/HW versions, uptime)\r\n");
#if NMEA_DIAG
      DataTerminal::instance().write("  nmea?                               - Report NMEA path diagnostics ($PAINMEA)\r\n");
#endif
      DataTerminal::instance().write("  hourlyreboot on|off                 - Enable/disable hourly self-reboot ($PAIHRB)\r\n");
      DataTerminal::instance().write("  hourlyreboot?                       - Report hourly-reboot setting and uptime ($PAIHRB)\r\n");
      DataTerminal::instance().write("  reboot                              - Reboot the device\r\n");
      DataTerminal::instance().write("  dfu                                 - Jump to bootloader mode\r\n");
      DataTerminal::instance().write("  cli                                 - Reboot into dedicated CLI mode\r\n");
      DataTerminal::instance().write("  tx on                               - Enable AIS transmission\r\n");
      DataTerminal::instance().write("  tx off                              - Disable AIS transmission (silent mode)\r\n");
      DataTerminal::instance().write("  tx?                                 - Report current TX status\r\n");
      DataTerminal::instance().write("  tx test                             - Send a test packet\r\n");
      DataTerminal::instance().write("  stoptx                              - Stop any ongoing test transmission\r\n");
      DataTerminal::instance().write("  msg24                               - Queue Message 24 for transmission\r\n");
      DataTerminal::instance().write("  gps on                              - Enable GNSS receiver\r\n");
      DataTerminal::instance().write("  gps off                             - Disable GNSS receiver\r\n");
      DataTerminal::instance().write("  gnss?                               - Report GNSS UART rate / detection state ($PAIGNSS)\r\n");
      DataTerminal::instance().write("  heading <deg>                      - Set true heading for message 18 (0-360)\r\n");
      DataTerminal::instance().write("  heading off                         - Clear true heading (message 18 sends 511)\r\n");
      DataTerminal::instance().write("  heading?                            - Report true heading ($PAIHDG)\r\n");
      // DataTerminal::instance().write("  xotrim <value>                      - Set XO trim value\r\n");
      // DataTerminal::instance().write("  xotrim?                             - Report current XO trim value\r\n");
      // DataTerminal::instance().write("  pa_level <value>                    - Set PA level (0-63)\r\n");
      // DataTerminal::instance().write("  pa_level?                           - Report current PA level\r\n");
      // DataTerminal::instance().write("  otp <hw_version> [serial]           - Write One-Time-Programmable data\r\n");
      // DataTerminal::instance().write("  otp?                                - Report OTP data\r\n");
    }
  else if ( s.find("station ") == 0 )
    {
      /*
       * The station command format is:
       * station mmsi,name,callsign,type,len,beam,portoffset,bowoffset
       */

      StationData station;

      string params = s.substr(8);
      if (params.empty())
        return;

      vector<string> tokens;
      Utils::tokenize(params, ',', tokens);
      if ( tokens.size() < 8 )
        return;

      memset(&station, 0, sizeof station);
      station.mmsi = Utils::toInt(tokens[0]);
      strlcpy(station.name, tokens[1].c_str(), sizeof station.name);
      strlcpy(station.callsign, tokens[2].c_str(), sizeof station.callsign);
      int type = (VesselType)Utils::toInt(tokens[3]);
      if ( type == 30 || type == 34 || type == 36 || type == 37 )
        station.type = (VesselType)type;
      station.len = Utils::toInt(tokens[4]);
      station.beam = Utils::toInt(tokens[5]);
      station.portOffset = Utils::toInt(tokens[6]);
      station.bowOffset = Utils::toInt(tokens[7]);
      station.magic = STATION_DATA_MAGIC;

      Configuration::instance().writeStationData(station);
      TXScheduler::instance().reloadStationData();
    }
  else if ( s.find("station?") == 0 )
    {
      Configuration::instance().reportStationData();
    }
  else if ( s.find("sys?") == 0 )
    {
      Configuration::instance().reportSystemData();
    }
  else if ( s.find("hourlyreboot on") == 0 )
    {
      Configuration::instance().setHourlyReboot(true);
      Configuration::instance().reportHourlyReboot();
    }
  else if ( s.find("hourlyreboot off") == 0 )
    {
      Configuration::instance().setHourlyReboot(false);
      Configuration::instance().reportHourlyReboot();
    }
  else if ( s.find("hourlyreboot?") == 0 )
    {
      Configuration::instance().reportHourlyReboot();
    }
#if NMEA_DIAG
  else if ( s.find("nmea?") == 0 )
    {
      // Write directly. Queuing PROPR_NMEA_SENTENCE through the 10-deep thread
      // pool silently dropped replies under AIS load (~40% miss in the field).
      char line[320];
      // 1 usart2_ore 2 usart1_ore 3 csum_drop 4 echo_ovf 5 queue_drop
      // 6-8 usart2 fe/ne/pe 9-11 usart1 fe/ne/pe 12 cmd_drop 13 pool_drop
      // 14 quiet_windows 15 hold_bytes 16 hold_max 17 rx_discarded 18 out_drop
      // 19 hold_max_ms 20 cap_closes
      // 21 slot_sent 22 slot_defer 23 max_out_lat_ms 24 slot_nofit
      snprintf(line, sizeof line,
          "$PAINMEA,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu*",
          (unsigned long)bsp_gnss_ore_count(),
          (unsigned long)bsp_uart1_ore_count(),
          (unsigned long)GPS::instance().droppedChecksumCount(),
          (unsigned long)GPS::instance().echoOverflowCount(),
          (unsigned long)EventQueue::instance().droppedCount(),
          (unsigned long)bsp_gnss_fe_count(),
          (unsigned long)bsp_gnss_ne_count(),
          (unsigned long)bsp_gnss_pe_count(),
          (unsigned long)bsp_uart1_fe_count(),
          (unsigned long)bsp_uart1_ne_count(),
          (unsigned long)bsp_uart1_pe_count(),
          (unsigned long)DataTerminal::instance().commandDropCount(),
          (unsigned long)EventPool::instance().allocFailCount(),
          (unsigned long)UartQuiet::instance().quietWindows(),
          (unsigned long)UartQuiet::instance().holdBytes(),
          (unsigned long)UartQuiet::instance().holdMax(),
          (unsigned long)UartQuiet::instance().rxDiscarded(),
          (unsigned long)UartQuiet::instance().outputDrops(),
          (unsigned long)UartQuiet::instance().holdMaxMs(),
          (unsigned long)UartQuiet::instance().capCloses(),
          (unsigned long)UartQuiet::instance().slotSent(),
          (unsigned long)UartQuiet::instance().slotDefer(),
          (unsigned long)UartQuiet::instance().maxOutLatMs(),
          (unsigned long)UartQuiet::instance().slotNofit());
      Utils::completeNMEA(line);
      DataTerminal::instance().write(line);
    }
#endif
  else if ( s.find("dfu") == 0 )
    {
      jumpToBootloader();
    }
  else if ( s.find("erase station") == 0 )
    {
      Configuration::instance().eraseStationData();
      TXScheduler::instance().reloadStationData();
      Configuration::instance().reportStationData();
    }
  else if ( s.find("factory reset") == 0 )
    {
      Configuration::instance().factoryReset();
      TXScheduler::instance().reloadStationData();
      Configuration::instance().reportStationData();
      Configuration::instance().reportHourlyReboot();
    }
  else if ( s.find("tx test") == 0 )
    {
      fireTestPacket();
    }
  else if ( s.find("tx on") == 0 )
    {
      Configuration::instance().enableTX();
      TXScheduler::instance().reportTXStatus();
    }
  else if ( s.find("tx off") == 0 )
    {
      Configuration::instance().disableTX();
      TXScheduler::instance().reportTXStatus();
    }
  else if ( s.find("tx?") == 0 )
    {
      TXScheduler::instance().reportTXStatus();
    }
  /*
  else if ( s.find("txcw a") == 0 )
    {
      RadioManager::instance().transmitCW(CH_87);
    }
  else if ( s.find("txcw b") == 0 )
    {
      RadioManager::instance().transmitCW(CH_88);
    }
  */
  else if ( s.find("stoptx") == 0 )
    {
      RadioManager::instance().stopTX();
    }
  else if (s.find("reboot") == 0 )
    {
      bsp_reboot();
    }
  else if ( s.find("cli") == 0 )
    {
      enterCLIMode();
    }
  else if ( s.find("gps off") == 0 )
    {
      GPS::instance().disable();
    }
  else if ( s.find("gps on") == 0 )
    {
      // Power-cycles the module and restarts GNSS rate detection.
      GPS::instance().enable();
    }
  else if ( s.find("gnss?") == 0 )
    {
      GPS::instance().reportLink();
    }
  else if ( s.find("heading off") == 0 )
    {
      VesselHeading::instance().clear();
      VesselHeading::instance().report();
    }
  else if ( s.find("heading?") == 0 )
    {
      VesselHeading::instance().report();
    }
  else if ( s.find("heading ") == 0 )
    {
      float deg = 0.f;
      char *end = 0;
      const string p = s.substr(8);
      deg = strtof(p.c_str(), &end);
      while ( end && (*end == ' ' || *end == '\t') )
        ++end;
      if ( !end || end == p.c_str() || *end != 0 || deg < 0.f || deg > 360.f )
        DataTerminal::instance().write("Invalid heading (0-360)\r\n");
      else
        {
          VesselHeading::instance().setTrueDegrees(deg);
          VesselHeading::instance().report();
        }
    }
  else if ( s.find("msg24") == 0 )
    {
      TXScheduler::instance().queueMessage24(CH_87);
    }
#if OTP_DATA
  else if ( s.find("otp?") == 0 )
    {
      dumpOTPData();
    }
  else if ( s.find("otp ") == 0 )
    {
      writeOTPData(s);
    }
#endif
  else if ( s.find("xotrim ") == 0 )
    {
      uint8_t value = Utils::toInt(s.substr(7));
      RadioManager::instance().setXOTrimValue(value);
      Configuration::instance().setXOTrimValue(value);
    }
  else if ( s.find("xotrim?") == 0 )
    {
      Configuration::instance().reportXOTrimValue();
    }
  else if ( s.find("pa_level ") == 0 )
    {
       string p = s.substr(9);
       size_t i = 0;
       while ( i < p.size() && (p[i] == ' ' || p[i] == '\t') )
         ++i;
       bool ok = (i < p.size() && p[i] >= '0' && p[i] <= '9');
       unsigned val = 0;
       while ( ok && i < p.size() && p[i] >= '0' && p[i] <= '9' )
         {
           val = val * 10u + (unsigned)(p[i] - '0');
           if ( val > 127u )
             ok = false;
           else
             ++i;
         }
       while ( ok && i < p.size() && (p[i] == ' ' || p[i] == '\t') )
         ++i;
       if ( !ok || i != p.size() )
         {
           DataTerminal::instance().write("Invalid level (0-127)\r\n");
         }
       else if ( !Configuration::instance().setPALevel((uint8_t)val) )
         {
           DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
         }
       else if ( RadioManager::instance().applyPALevel((uint8_t)val) )
         {
           DataTerminal::instance().write("OK, saved and applied.\r\n");
         }
       else
         {
           DataTerminal::instance().write("OK, saved, applying after current TX.\r\n");
         }
    }
  else if ( s.find("pa_level?") == 0 )
    {
      Configuration::instance().reportTXConfig();
    }
}

void CommandProcessor::enterCLIMode()
{
  *(uint32_t*)BOOTMODE_ADDRESS = CLI_FLAG_MAGIC;
  bsp_reboot();
}

void CommandProcessor::jumpToBootloader()
{
  bsp_enter_dfu();
}

#if OTP_DATA
void CommandProcessor::dumpOTPData()
{
  Configuration::instance().reportOTPData();
}

void CommandProcessor::writeOTPData(const std::string &s)
{
  string params = s.substr(4);
  if (params.empty())
    return;

  vector<string> tokens;
  Utils::tokenize(params, ' ', tokens);

  OTPData data;
  memset(&data, 0, sizeof data);

  data.magic  = OTP_MAGIC;
  data.rev    = OTP_REV;
  strlcpy(data.hwrev, tokens[0].c_str(), sizeof data.hwrev);

  if ( tokens.size() > 2 )
    strlcpy(data.serialnum, tokens[1].c_str(), sizeof data.serialnum);

  bool result = Configuration::instance().writeOTP(data);
  if ( result )
    dumpOTPData();
}
#endif
