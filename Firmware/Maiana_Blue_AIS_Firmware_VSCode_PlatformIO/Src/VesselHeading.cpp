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


#include "VesselHeading.hpp"
#include "NMEASentence.hpp"
#include "Utils.hpp"
#include "DataTerminal.hpp"
#include "config.h"
#include <stm32l4xx_hal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace std;

static bool parseDegrees(const string &s, float &deg)
{
  if ( s.empty() )
    return false;

  char *end = 0;
  deg = strtof(s.c_str(), &end);
  if ( end == s.c_str() )
    return false;
  while ( *end == ' ' || *end == '\t' )
    ++end;
  if ( *end != 0 )
    return false;
  if ( deg < 0.f || deg > 360.f )
    return false;
  if ( deg == 360.f )
    deg = 0.f;
  return true;
}

static float wrap360(float deg)
{
  while ( deg < 0.f )
    deg += 360.f;
  while ( deg >= 360.f )
    deg -= 360.f;
  return deg;
}

static bool codeEndsWith(const string &code, const char *suffix)
{
  const size_t n = strlen(suffix);
  return code.size() >= n && code.compare(code.size() - n, n, suffix) == 0;
}

static bool signedOffset(const string &mag, const string &dir, float &out)
{
  if ( mag.empty() )
    {
      out = 0.f;
      return true;
    }

  char *end = 0;
  out = strtof(mag.c_str(), &end);
  if ( end == mag.c_str() )
    return false;
  while ( *end == ' ' || *end == '\t' )
    ++end;
  if ( *end != 0 )
    return false;
  if ( out > 180.f || out < -180.f )
    return false;
  if ( !dir.empty() && (dir[0] == 'W' || dir[0] == 'w') )
    out = -out;
  else if ( !dir.empty() && dir[0] != 'E' && dir[0] != 'e' )
    return false;
  return true;
}

VesselHeading &VesselHeading::instance()
{
  static VesselHeading inst;
  return inst;
}

VesselHeading::VesselHeading()
: mDeg(0.f), mTick(0), mValid(false)
{
}

void VesselHeading::setTrueDegrees(float deg)
{
  if ( deg < 0.f || deg > 360.f )
    return;
  if ( deg == 360.f )
    deg = 0.f;
  mDeg = deg;
  mTick = HAL_GetTick();
  mValid = true;
}

void VesselHeading::clear()
{
  mValid = false;
}

bool VesselHeading::fresh() const
{
  if ( !mValid )
    return false;
  return (HAL_GetTick() - mTick) <= (uint32_t)HEADING_MAX_AGE_MS;
}

uint16_t VesselHeading::aisValue() const
{
  if ( !fresh() )
    return 511;

  int v = (int)(mDeg + 0.5f);
  if ( v >= 360 )
    v = 0;
  if ( v < 0 )
    v = 0;
  return (uint16_t)v;
}

bool VesselHeading::acceptSentence(const char *line)
{
  if ( !line || line[0] != '$' )
    return false;
  if ( !Utils::nmeaChecksumValid(line) )
    return false;

  NMEASentence sentence(line);
  const string &code = sentence.code();
  const vector<string> &f = sentence.fields();

  if ( codeEndsWith(code, "HDT") )
    {
      if ( f.size() < 2 )
        return false;
      if ( f.size() >= 3 && !f[2].empty() && f[2][0] != 'T' && f[2][0] != 't' )
        return false;
      float deg;
      if ( !parseDegrees(f[1], deg) )
        return false;
      setTrueDegrees(deg);
      return true;
    }

  if ( code == "PAIHDG" )
    {
      if ( f.size() < 2 )
        return false;
      float deg;
      if ( !parseDegrees(f[1], deg) )
        return false;
      setTrueDegrees(deg);
      return true;
    }

  // $--HDG,mag,dev,E/W,var,E/W. True = mag + deviation + variation.
  // Variation is required: message 18 carries true heading only.
  if ( codeEndsWith(code, "HDG") )
    {
      if ( f.size() < 6 || f[1].empty() || f[4].empty() )
        return false;
      float mag;
      if ( !parseDegrees(f[1], mag) )
        return false;
      float dev = 0.f;
      float var = 0.f;
      if ( !signedOffset(f[2], f.size() > 3 ? f[3] : "", dev) )
        return false;
      if ( !signedOffset(f[4], f[5], var) )
        return false;
      setTrueDegrees(wrap360(mag + dev + var));
      return true;
    }

  return false;
}

void VesselHeading::report()
{
  char line[80];
  if ( !mValid )
    {
      snprintf(line, sizeof line, "$PAIHDG,,511*");
    }
  else
    {
      const uint32_t age = HAL_GetTick() - mTick;
      snprintf(line, sizeof line, "$PAIHDG,%.1f,%lu,%u*",
          mDeg, (unsigned long)age, (unsigned)aisValue());
    }
  Utils::completeNMEA(line);
  DataTerminal::instance().write(line);
}
