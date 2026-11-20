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
#include "CliParse.hpp"
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

/**
 * `tx test` sends 1 s of modulated carrier through Q1. Refuse it while a TX is
 * pending or running, and allow at most one per TX_TEST_MIN_INTERVAL_MS.
 */
static bool testPacketAllowed()
{
  static bool fired = false;
  static uint32_t lastMs = 0;

  if ( RadioManager::instance().isTXBusy() )
    {
      DataTerminal::instance().write("ERROR: TX busy, test packet not sent\r\n");
      return false;
    }

  const uint32_t now = HAL_GetTick();
  if ( fired && now - lastMs < TX_TEST_MIN_INTERVAL_MS )
    {
      char line[64];
      snprintf(line, sizeof line, "ERROR: tx test rate limited, retry in %lu s\r\n",
          (unsigned long)((TX_TEST_MIN_INTERVAL_MS - (now - lastMs) + 999u) / 1000u));
      DataTerminal::instance().write(line);
      return false;
    }

  fired = true;
  lastMs = now;
  return true;
}

void fireTestPacket(uint32_t wdtMs = 0, bool simulateUnresponsive = false)
{
  if ( !testPacketAllowed() )
    return;

  VHFChannel channel = CH_87;

  if ( rand() % 2 == 0 )
    channel = CH_88;

  TXPacket *p = TXPacketPool::instance().newTXPacket(channel);
  if ( !p ) {
      DataTerminal::instance().write("ERROR: no TX packet available\r\n");
      return;
  }

  /**
   * Define a dummy packet of 9600 random bits, so it will take 1 second to transmit.
   * This is long enough for most spectrum analyzers to capture details using "max hold",
   * even at very low resolution bandwidths. Great way to measure power and look for
   * spurious emissions as well as harmonics.
   */
  p->configureForTesting(channel, 9600);

  RadioManager::instance().sendTestPacketNow(p, wdtMs, simulateUnresponsive);
  DataTerminal::instance().write(channel == CH_87 ? "OK, test packet on channel A\r\n"
                                                  : "OK, test packet on channel B\r\n");
}

/**
 * Commands that transmit, reset, or write configuration must match the whole
 * (trimmed) line; a prefix match would turn e.g. "tx test xyz", "reboot now"
 * or an echoed help line ("reboot   - Reboot the device") into a real action.
 */
static bool isExactly(const string &s, const char *cmd)
{
  return s == cmd;
}

void CommandProcessor::processCommand(const char *buff)
{
  string s(buff);
  Utils::trim(s);

  // '!' lines (e.g. our own !AIVDM/!AIVDO echoed back by a multiplexer) are never commands
  if ( !s.empty() && s[0] == '!' )
    return;

  if ( !s.empty() && s[0] == '$' )
    {
      VesselHeading::instance().acceptSentence(s.c_str());
      return;
    }

  cli::skipToSafeCommand(s);

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
      DataTerminal::instance().write("  tx test                             - Send a 1 s test packet (max one per 10 s)\r\n");
#if TX_WDT_TEST_CMD
      DataTerminal::instance().write("  tx test wdt [hard]                  - Bench: TX watchdog cuts a test packet after 100 ms\r\n");
#endif
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
      cli::StationFields f;
      if ( !cli::parseStation(s.substr(8), f) )
        {
          DataTerminal::instance().write("ERROR: invalid station data (mmsi[9 digits],name,callsign,type,len,beam,portoffset,bowoffset)\r\n");
          return;
        }

      memset(&station, 0, sizeof station);
      station.mmsi = f.mmsi;
      strlcpy(station.name, f.name.c_str(), sizeof station.name);
      strlcpy(station.callsign, f.callsign.c_str(), sizeof station.callsign);
      int type = (int)f.type;
      if ( type == 30 || type == 34 || type == 36 || type == 37 )
        station.type = (VesselType)type;
      station.len = (uint8_t)f.len;
      station.beam = (uint8_t)f.beam;
      station.portOffset = (uint8_t)f.portOffset;
      station.bowOffset = (uint8_t)f.bowOffset;
      station.magic = STATION_DATA_MAGIC;

      if ( !Configuration::instance().writeStationData(station) )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
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
  else if ( isExactly(s, "hourlyreboot on") )
    {
      if ( !Configuration::instance().setHourlyReboot(true) )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
      Configuration::instance().reportHourlyReboot();
    }
  else if ( isExactly(s, "hourlyreboot off") )
    {
      if ( !Configuration::instance().setHourlyReboot(false) )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
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
  else if ( isExactly(s, "dfu") )
    {
      jumpToBootloader();
    }
  else if ( isExactly(s, "erase station") )
    {
      if ( !Configuration::instance().eraseStationData() )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
      TXScheduler::instance().reloadStationData();
      Configuration::instance().reportStationData();
    }
  else if ( isExactly(s, "factory reset") )
    {
      if ( !Configuration::instance().factoryReset() )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
      TXScheduler::instance().reloadStationData();
      Configuration::instance().reportStationData();
      Configuration::instance().reportHourlyReboot();
    }
#if TX_WDT_TEST_CMD
  else if ( isExactly(s, "tx test wdt hard") )
    {
      // Bench hook: watchdog fires mid-packet and U1 is treated as unresponsive
      fireTestPacket(TX_WDT_TEST_MS, true);
    }
  else if ( isExactly(s, "tx test wdt") )
    {
      // Bench hook: watchdog fires mid-packet, normal recovery to RX
      fireTestPacket(TX_WDT_TEST_MS, false);
    }
#endif
  else if ( isExactly(s, "tx test") )
    {
      fireTestPacket();
    }
  else if ( s.find("tx test") == 0 )
    {
      DataTerminal::instance().write("ERROR: unknown tx test option\r\n");
    }
  else if ( isExactly(s, "tx on") )
    {
      if ( !Configuration::instance().enableTX() )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
      TXScheduler::instance().reportTXStatus();
    }
  else if ( isExactly(s, "tx off") )
    {
      if ( !Configuration::instance().disableTX() )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
      TXScheduler::instance().reportTXStatus();
    }
  else if ( s.find("tx?") == 0 )
    {
      TXScheduler::instance().reportTXStatus();
    }
  else if ( s.find("tx on") == 0 || s.find("tx off") == 0 )
    {
      DataTerminal::instance().write("ERROR: unknown tx option\r\n");
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
  else if ( isExactly(s, "stoptx") )
    {
      RadioManager::instance().stopTX();
    }
  else if ( isExactly(s, "reboot") )
    {
      bsp_reboot();
    }
  else if ( isExactly(s, "cli") )
    {
      enterCLIMode();
    }
  else if ( isExactly(s, "gps off") )
    {
      GPS::instance().disable();
    }
  else if ( isExactly(s, "gps on") )
    {
      // Power-cycles the module and restarts GNSS rate detection.
      GPS::instance().enable();
    }
  else if ( s.find("gnss?") == 0 )
    {
      GPS::instance().reportLink();
    }
  else if ( isExactly(s, "heading off") )
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
  else if ( isExactly(s, "msg24") )
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
      uint32_t v = 0;
      if ( !cli::parseUInt(s.substr(7), 255u, v) )
        {
          DataTerminal::instance().write("ERROR: invalid xotrim (0-255)\r\n");
          return;
        }
      uint8_t value = (uint8_t)v;
      RadioManager::instance().setXOTrimValue(value);
      if ( !Configuration::instance().setXOTrimValue(value) )
        DataTerminal::instance().write("ERROR: EEPROM write failed\r\n");
      Configuration::instance().reportXOTrimValue();
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
  if ( tokens.empty() || tokens[0].empty() )
    {
      DataTerminal::instance().write("ERROR: invalid otp (otp <hw_version> [serial])\r\n");
      return;
    }

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
