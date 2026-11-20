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


#include "GPS.hpp"
#include "config.h"
#include "NMEASentence.hpp"
#include "Utils.hpp"
#include "EventQueue.hpp"
#include "UartQuiet.hpp"
#include "Configuration.hpp"
#include "DataTerminal.hpp"
#include "bsp.hpp"
#include <stm32l4xx_hal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void gnssInputCB(char c);
void gnss1PPSCB();
void gnssSOTDMACB();

// LC76G default output is ~1.5KB/s (1Hz burst). USART1 at 38400 (~3.8KB/s) can
// relay the full stream. Drain complete lines only so AIVDM/$PAI cannot land
// mid-sentence (that was the ESP32 "glue" class).
#define GPS_ECHO_BUF_SIZE           2560
#define GPS_ECHO_DRAIN_LINES        2

static char sEchoBuf[GPS_ECHO_BUF_SIZE];
static volatile uint16_t sEchoHead = 0;
static volatile uint16_t sEchoTail = 0;
#if NMEA_DIAG
static volatile uint32_t sDroppedChecksum = 0;
static volatile uint32_t sEchoOverflow = 0;
#endif
static bool sEchoMidLine = false;

static void echoPush(char c)
{
  uint16_t next = (uint16_t)((sEchoHead + 1) % GPS_ECHO_BUF_SIZE);
  if ( next == sEchoTail )
    {
#if NMEA_DIAG
      sEchoOverflow++;
#endif
      return;
    }

  sEchoBuf[sEchoHead] = c;
  sEchoHead = next;
}

static void echoPushLine(const char *line, uint8_t len)
{
  for ( uint8_t i = 0; i < len; ++i )
    echoPush(line[i]);
}

/*
 * GNSS UART rate candidates, probed in order starting at the stored hint.
 *
 * To support another rate: append { baud, GNSS_BAUD_CODE_x }. Codes are
 * persisted in 2 bits of ConfigFlags (ConfigFlags.h), so never renumber an
 * existing code; 3 is the last free one (widen the field beyond that).
 * Index 0 is the default when no hint is stored.
 */
struct GnssBaudCandidate
{
  uint32_t baud;
  uint8_t code;
};

static const GnssBaudCandidate kGnssBauds[] = {
    { 115200, GNSS_BAUD_CODE_115200 },  // Quectel LC76G default (Board 11.10+)
    { 9600,   GNSS_BAUD_CODE_9600 },    // Quectel L76L-M33 default (Board 11.9.x and older)
};

static const uint8_t kGnssBaudCount = (uint8_t)(sizeof kGnssBauds / sizeof kGnssBauds[0]);

static bool isNavSentence(const char *line)
{
  if ( line[0] != '$' )
    return false;

  if ( line[3] == 'R' && line[4] == 'M' && line[5] == 'C' )
    return true;
  if ( line[3] == 'G' && line[4] == 'G' && line[5] == 'A' )
    return true;

  return false;
}

GPS &
GPS::instance()
{
  static GPS __instance;
  return __instance;
}

GPS::GPS()
: mBuffPos(0), mUTC(0), mLat(0), mLng(0), mStarted(false), mSlotNumber(0), mDelegate(NULL), mCOG(511), mSpeed(0),
  mLinkState(LINK_OFF), mBaudIndex(0), mValidLines(0), mHintPending(false), mWindowStart(0), mWindowMs(0),
  mLastValidTick(0), mErrAtWindowStart(0), mProbeSwitches(0)
{
  memset(&mTime, 0, sizeof(mTime));
  mPeriod = (bsp_get_system_clock() / 37.5) - 1;
  EventQueue::instance().addObserver(this, GPS_NMEA_SENTENCE);
}

GPS::~GPS()
{

}

time_t GPS::UTC()
{
  return mUTC;
}

struct tm &GPS::time()
{
  return mTime;
}

uint32_t GPS::aisSlot()
{
  return mSlotNumber;
}

double GPS::lat()
{
  return mLat;
}

double GPS::lng()
{
  return mLng;
}

void GPS::setDelegate(GPSDelegate *delegate)
{
  mDelegate = delegate;
}

void GPS::init()
{
  bsp_set_gnss_input_callback(gnssInputCB);
  bsp_set_gnss_1pps_callback(gnss1PPSCB);
  bsp_set_gnss_sotdma_timer_callback(gnssSOTDMACB);
}

void GPS::enable()
{
  bsp_gnss_off();
  HAL_Delay(300);
  bsp_gnss_on();
  startProbe(hintIndex(), GNSS_PROBE_BOOT_WINDOW_MS);
}

void GPS::disable()
{
  mLinkState = LINK_OFF;
  bsp_gnss_off();
  bsp_gps_led_off();
}

bool GPS::isEnabled()
{
  return bsp_is_gnss_on();
}

void GPS::onRX(char c)
{
  if ( c == '$' )
    mBuffPos = 0;

  if ( mBuffPos >= sizeof(mBuff) - 1 )
    {
      mBuffPos = 0;
      mBuff[0] = 0;
      if ( c == '$' )
        mBuff[mBuffPos++] = c;
      return;
    }

  mBuff[mBuffPos++] = c;

  if ( c == '\n' )
    {
      mBuff[mBuffPos] = 0;

      if ( mBuff[0] == '$' )
        {
          if ( !Utils::nmeaChecksumValid(mBuff) )
            {
#if NMEA_DIAG
              sDroppedChecksum++;
#endif
            }
          else
            {
              noteValidLine();
              echoPushLine(mBuff, mBuffPos);

              if ( isNavSentence(mBuff) )
                {
                  Event *e = EventPool::instance().newEvent(GPS_NMEA_SENTENCE);
                  if ( e )
                    {
                      strlcpy(e->nmeaBuffer.sentence, mBuff, sizeof e->nmeaBuffer.sentence);
                      EventQueue::instance ().push(e);
                    }
                }
            }
        }

      mBuffPos = 0;
      mBuff[0] = 0;
    }
}

static void echoWriteChar(char c)
{
  UartQuiet::instance().writeCharRaw(c);
  sEchoMidLine = (c != '\n');
}

void GPS::flushEchoLine()
{
  if ( !sEchoMidLine )
    return;

  while ( sEchoTail != sEchoHead )
    {
      char c = sEchoBuf[sEchoTail];
      sEchoTail = (uint16_t)((sEchoTail + 1) % GPS_ECHO_BUF_SIZE);
      echoWriteChar(c);
      if ( c == '\n' )
        return;
    }

  sEchoMidLine = false;
}

void GPS::pollRX()
{
  bsp_poll_gnss_rx();
  serviceLink();
}

/*
 * GNSS UART rate auto-detection.
 *
 * OFF ──enable()──> PROBE(hint, boot window)
 * PROBE: >= GNSS_LOCK_VALID_LINES valid lines        → LOCKED
 *        window over, no valid line                  → PROBE(next candidate)
 *        window over, some valid lines               → same candidate again
 *        >= FAST_MS, many FE/NE polls, no valid line → PROBE(next candidate)
 * LOCKED: no valid line for GNSS_LINK_LOST_MS        → PROBE(same rate, boot window)
 *
 * Runs from pollRX() in the main thread only, like bsp_poll_gnss_rx(), so it
 * never races the DMA read index. It does not touch the 1PPS / SOTDMA timer
 * path: AIS slot timing keeps running from PPS across a re-probe.
 */
void GPS::noteValidLine()
{
  mLastValidTick = HAL_GetTick();
  if ( mValidLines < 0xff )
    ++mValidLines;
}

uint8_t GPS::hintIndex() const
{
  const uint8_t code = Configuration::instance().gnssBaudCode();
  for ( uint8_t i = 0; i < kGnssBaudCount; ++i )
    {
      if ( kGnssBauds[i].code == code )
        return i;
    }
  return 0;
}

void GPS::startProbe(uint8_t index, uint32_t windowMs)
{
  if ( index >= kGnssBaudCount )
    index = 0;

  mBaudIndex = index;
  if ( bsp_gnss_baud() != kGnssBauds[index].baud )
    bsp_gnss_set_baud(kGnssBauds[index].baud);

  // Partial line from the old rate (or from before power-up) is garbage.
  mBuffPos = 0;
  mBuff[0] = 0;

  mValidLines = 0;
  mWindowStart = HAL_GetTick();
  mWindowMs = windowMs;
  mErrAtWindowStart = bsp_gnss_rx_error_events();
  mLinkState = LINK_PROBE;
}

void GPS::lockLink()
{
  mLinkState = LINK_LOCKED;
  mHintPending = Configuration::instance().gnssBaudCode() != kGnssBauds[mBaudIndex].code;
  configureModule();
}

void GPS::configureModule()
{
#if BSP_GNSS_HAS_TX
  /*
   * TODO(BSP_GNSS_HAS_TX): extension point for boards where MCU TX reaches
   * GNSS RXD. Called once per lock, at the detected rate (kGnssBauds[mBaudIndex]).
   * Not implemented: no current board can send to the module.
   *
   * Possible use:
   *  - Identify the module from its own output: LC76G (Airoha) emits $PAIR /
   *    $PQTM replies, L76L (MediaTek) answers $PMTK605 with $PMTK705.
   *  - Move it to a preferred rate, then startProbe() there:
   *      LC76G: "$PAIR864,0,0,<baud>*HH" then a module reset (GNSS_EN power
   *             cycle). PQTMSAVEPAR does not cover it; verify that the
   *             module keeps it across power cycles before relying on it.
   *      L76L:  "$PMTK251,<baud>*HH" (kept only while V_BCKP is supplied).
   *  - Trim the sentence set so it fits the rate (an LC76G at 9600 cannot
   *    carry its default ~1.5 KB/s): $PAIR062 / $PMTK314.
   * Send with bsp_gnss_send() between output bursts, keep probing as the
   * fallback, and never block the main loop.
   */
#endif
}

void GPS::serviceLink()
{
  const uint32_t now = HAL_GetTick();

  switch ( mLinkState )
    {
    case LINK_PROBE:
      {
        if ( mValidLines >= GNSS_LOCK_VALID_LINES )
          {
            lockLink();
            break;
          }

        const uint32_t elapsed = now - mWindowStart;
        const uint32_t errors = bsp_gnss_rx_error_events() - mErrAtWindowStart;
        const bool garbage = mValidLines == 0 && elapsed >= GNSS_PROBE_FAST_MS
            && errors >= GNSS_PROBE_FAST_ERR_EVENTS;

        if ( garbage || elapsed >= mWindowMs )
          {
            if ( mValidLines == 0 )
              {
                ++mProbeSwitches;
                startProbe((uint8_t)((mBaudIndex + 1) % kGnssBaudCount), GNSS_PROBE_WINDOW_MS);
              }
            else
              {
                // Valid NMEA but not enough yet (window ended mid-burst): stay.
                mWindowStart = now;
                mWindowMs = GNSS_PROBE_WINDOW_MS;
                mErrAtWindowStart = bsp_gnss_rx_error_events();
              }
          }
      }
      break;

    case LINK_LOCKED:
      if ( mHintPending && now - mLastValidTick >= GNSS_HINT_IDLE_MS )
        {
          // In the gap after a 1 Hz burst, so even a one-time full flags
          // write cannot overrun the GNSS DMA ring.
          mHintPending = false;
          Configuration::instance().setGnssBaudCode(kGnssBauds[mBaudIndex].code);
        }

      if ( now - mLastValidTick >= GNSS_LINK_LOST_MS )
        {
          bsp_gps_led_off();
          startProbe(mBaudIndex, GNSS_PROBE_BOOT_WINDOW_MS);
        }
      break;

    default:
      break;
    }
}

uint8_t GPS::linkState() const
{
  return mLinkState;
}

uint32_t GPS::gnssBaud() const
{
  return bsp_gnss_baud();
}

void GPS::reportLink()
{
  // $PAIGNSS,<baud>,<state 0=off 1=probe 2=locked>,<stored hint baud, 0=none>,
  //          <probe switches>,<FE/NE poll events>,<has_tx>*HH
  uint32_t hintBaud = 0;
  const uint8_t code = Configuration::instance().gnssBaudCode();
  for ( uint8_t i = 0; i < kGnssBaudCount; ++i )
    {
      if ( kGnssBauds[i].code == code )
        hintBaud = kGnssBauds[i].baud;
    }

  char line[80];
  snprintf(line, sizeof line, "$PAIGNSS,%lu,%u,%lu,%lu,%lu,%u*",
      (unsigned long)bsp_gnss_baud(),
      (unsigned)mLinkState,
      (unsigned long)hintBaud,
      (unsigned long)mProbeSwitches,
      (unsigned long)bsp_gnss_rx_error_events(),
      (unsigned)BSP_GNSS_HAS_TX);
  Utils::completeNMEA(line);
  DataTerminal::instance().write(line);
}

static uint16_t echoPeekLineLen()
{
  if ( sEchoTail == sEchoHead )
    return 0;

  uint16_t i = sEchoTail;
  uint16_t n = 0;
  while ( i != sEchoHead )
    {
      n++;
      if ( sEchoBuf[i] == '\n' )
        return n;
      i = (uint16_t)((i + 1) % GPS_ECHO_BUF_SIZE);
    }

  return 0;
}

void GPS::drainEcho()
{
  // Yield between sentences, never in the middle of one. A long GSV line is
  // ~21 ms at 38400; dispatch() can emit AIVDM only after '\n'.
  // While a TX packet is pending, pack as many whole sentences as fit in
  // the current free-slot window (then stop until the next CCA-no-TX).
  static uint16_t sDeferTail = 0xffff;
  const int maxLines = UartQuiet::instance().slotGated() ? 8 : GPS_ECHO_DRAIN_LINES;

  for ( int n = 0; n < maxLines; ++n )
    {
      if ( sEchoTail == sEchoHead )
        return;

      if ( !sEchoMidLine )
        {
          const uint16_t len = echoPeekLineLen();
          if ( len == 0 )
            return;

          if ( !bsp_uart1_can_write() )
            return;

          if ( !UartQuiet::instance().canStartBytes(len) )
            {
              if ( sDeferTail != sEchoTail )
                {
                  sDeferTail = sEchoTail;
                  UartQuiet::instance().noteDefer(len);
                }
              return;
            }

          sDeferTail = 0xffff;
          UartQuiet::instance().noteStart();
        }

      while ( sEchoTail != sEchoHead )
        {
          char c = sEchoBuf[sEchoTail];
          sEchoTail = (uint16_t)((sEchoTail + 1) % GPS_ECHO_BUF_SIZE);
          echoWriteChar(c);
          if ( c == '\n' )
            break;
        }
    }
}

void GPS::onPPS()
{
  if ( mUTC == 0 )
    return;

  ++mUTC;
  localtime_r (&mUTC, &mTime);
  if (!mStarted)
    {
      mSlotNumber = (mTime.tm_sec % 60) * 2250;
      if (!(mTime.tm_sec & 0x00000001))
        startTimer ();
    }
  else
    {
      if (mTime.tm_sec & 1)
        {
          uint32_t nominalTimerValue = mPeriod / 2 - 1;
          bsp_set_sotdma_timer_value(nominalTimerValue);
        }
      else
        {
          uint32_t currentTimerValue = bsp_get_sotdma_timer_value();
          if (currentTimerValue >= mPeriod / 2 - 1)
            bsp_set_sotdma_timer_value(mPeriod - 1);
          else
            bsp_set_sotdma_timer_value(0);
        }
    }

  Event *e = EventPool::instance().newEvent(CLOCK_EVENT);
  if ( e )
    {
      e->clock.utc = mUTC;
      EventQueue::instance ().push(e);
    }
}

void GPS::processEvent(const Event &event)
{
  processLine(event.nmeaBuffer.sentence);
  ASSERT(event.rxPacket == nullptr);
}

void GPS::processLine(const char* buff)
{
  if ( buff[0] == '$' && buff[1] != '$' && Utils::nmeaChecksumValid(buff) )
    parseSentence(buff);
}

#if NMEA_DIAG
uint32_t GPS::droppedChecksumCount() const
{
  return sDroppedChecksum;
}

uint32_t GPS::echoOverflowCount() const
{
  return sEchoOverflow;
}
#endif

bool GPS::echoPending() const
{
  return sEchoTail != sEchoHead;
}

bool GPS::isValidNmeaDate(const char *ddmmyy)
{
  if ( !ddmmyy || strlen(ddmmyy) < 6 )
    return false;

  // Two-digit NMEA year. Receivers without a valid time report a default
  // date such as 060180 (1980); without the upper bound that became 2080.
  int year = Utils::toInt(std::string(ddmmyy + 4, 2));
  return year >= 24 && year < 80;
}

void GPS::applyFix(float lat, float lng, float speed, float cog)
{
  bsp_gps_led_on();
  mLat = lat;
  mLng = lng;
  mSpeed = speed;
  mCOG = cog;

  Event *e = EventPool::instance().newEvent(GPS_FIX_EVENT);
  if ( e )
    {
      e->gpsFix.utc = mUTC;
      e->gpsFix.lat = mLat;
      e->gpsFix.lng = mLng;
      e->gpsFix.speed = mSpeed;
      e->gpsFix.cog = mCOG;
      EventQueue::instance().push (e);
    }
}

void GPS::parseRmc(const NMEASentence &sentence)
{
  const vector<string> &fields = sentence.fields ();

  if (fields.size () < 10)
    return;

  // RMC arrives every second even without time or fix: no time means no
  // fix, so drop the GPS LED (master behaviour).
  if (fields[1].length () < 6 || fields[9].length () < 6)
    {
      bsp_gps_led_off();
      return;
    }

  if ( !isValidNmeaDate(fields[9].c_str()) )
    {
      bsp_gps_led_off();
      return;
    }

  const string &timestr = fields[1].substr (0, 6);
  const string &datestr = fields[9].substr (0, 6);
  mTime.tm_hour = Utils::toInt (timestr.substr (0, 2));
  mTime.tm_min = Utils::toInt (timestr.substr (2, 2));
  mTime.tm_sec = Utils::toInt (timestr.substr (4, 2));
  mTime.tm_mday = Utils::toInt (datestr.substr (0, 2));
  mTime.tm_mon = Utils::toInt (datestr.substr (2, 2)) - 1;
  mTime.tm_year = 100 + Utils::toInt (datestr.substr (4, 2));
  mUTC = mktime (&mTime);

  const bool positionValid = fields[2].length() > 0 && fields[2][0] == 'A'
      && fields[3].length() > 0 && fields[5].length() > 0;

  if (mUTC && positionValid)
    {
      applyFix(
          Utils::latitudeFromNMEA (fields[3], fields[4]),
          Utils::longitudeFromNMEA (fields[5], fields[6]),
          atof(fields[7].c_str()),
          atof(fields[8].c_str()));
    }
  else
    {
      bsp_gps_led_off();
    }
}

void GPS::parseGga(const NMEASentence &sentence)
{
  const vector<string> &fields = sentence.fields ();

  if (fields.size () < 7)
    return;

  if (fields[6].empty() || Utils::toInt(fields[6]) < 1)
    return;

  if (fields[2].empty() || fields[4].empty())
    return;

  applyFix(
      Utils::latitudeFromNMEA (fields[2], fields[3]),
      Utils::longitudeFromNMEA (fields[4], fields[5]),
      mSpeed,
      mCOG);
}

void GPS::parseSentence(const char *buff)
{
  NMEASentence sentence (buff);

  if (sentence.code ().find ("RMC") == 2)
    parseRmc(sentence);
  else if (sentence.code ().find ("GGA") == 2)
    parseGga(sentence);
}

void GPS::startTimer()
{
  bsp_start_sotdma_timer();
  mStarted = true;
}

void GPS::stopTimer()
{
  mStarted = false;
  bsp_stop_sotdma_timer();
}

void GPS::onTimerIRQ()
{
  if ( mStarted )
    {
      ++mSlotNumber;
      if ( mSlotNumber == 2250 )
        mSlotNumber = 0;

      if ( mDelegate )
        mDelegate->timeSlotStarted(mSlotNumber);
    }
}

void gnssInputCB(char c)
{
  GPS::instance().onRX(c);
}

void gnss1PPSCB()
{
  GPS::instance().onPPS();
}

void gnssSOTDMACB()
{
  GPS::instance().onTimerIRQ();
}

