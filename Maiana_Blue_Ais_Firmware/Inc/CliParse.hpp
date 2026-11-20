/*
  CLI line assembly and argument parsing helpers (header-only, no HAL
  dependencies so they can be unit-tested on the host).

  This file is part of the MAIANA(TM) transponder firmware.
*/

#ifndef CLIPARSE_HPP_
#define CLIPARSE_HPP_

#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>

// Max bytes of junk (TX EMI, partial line) skipped in front of a safe verb.
#ifndef CLI_MAX_JUNK_SKIP
#define CLI_MAX_JUNK_SKIP 4
#endif

namespace cli
{

/**
 * Assembles USART1 bytes into lines.
 * - CR is ignored, LF ends a line.
 * - A line may only start with a letter, '$' or '!' (other leading bytes are TX EMI).
 * - '$' or '!' anywhere restarts the line (NMEA resync), except inside a
 *   "station " line, whose name/callsign may legally contain them.
 * - rxLost (bytes discarded by the TX mute window) drops the partial line,
 *   so two half lines are never glued together.
 */
struct LineAssembler
{
  char    buf[80];
  uint8_t pos;

  LineAssembler() : pos(0) { buf[0] = 0; }

  bool inStationLine() const
  {
    return pos >= 8 && memcmp(buf, "station ", 8) == 0;
  }

  // Returns true when a complete NUL-terminated line is in buf.
  bool feed(char c, bool rxLost)
  {
    if ( rxLost )
      pos = 0;
    if ( c == '\r' )
      return false;
    if ( (c == '$' || c == '!') && pos > 0 && !inStationLine() )
      pos = 0;
    if ( pos == 0 && c != '\n' && c != '$' && c != '!' && !isalpha((unsigned char)c) )
      return false;

    buf[pos++] = c;
    if ( pos >= sizeof buf )
      {
        pos = 0;            // overflow: drop the line
        return false;
      }
    if ( c == '\n' )
      {
        buf[pos] = 0;
        pos = 0;
        return true;
      }
    return false;
  }
};

inline std::string trimmed(const std::string &s)
{
  size_t b = 0, e = s.size();
  while ( b < e && (s[b] == ' ' || s[b] == '\t') )
    ++b;
  while ( e > b && (s[e - 1] == ' ' || s[e - 1] == '\t') )
    --e;
  return s.substr(b, e - b);
}

inline bool isDigits(const std::string &s)
{
  if ( s.empty() )
    return false;
  for ( size_t i = 0; i < s.size(); ++i )
    if ( s[i] < '0' || s[i] > '9' )
      return false;
  return true;
}

// Decimal digits only (surrounding blanks allowed), value 0..maxVal.
inline bool parseUInt(const std::string &in, uint32_t maxVal, uint32_t &out)
{
  const std::string t = trimmed(in);
  if ( !isDigits(t) || t.size() > 10 )
    return false;
  uint64_t v = 0;
  for ( size_t i = 0; i < t.size(); ++i )
    {
      v = v * 10u + (uint64_t)(t[i] - '0');
      if ( v > maxVal )
        return false;
    }
  out = (uint32_t)v;
  return true;
}

// Split on delim, keeping empty fields (including a trailing one).
inline void split(const std::string &s, char delim, std::vector<std::string> &out)
{
  out.clear();
  std::string tok;
  for ( size_t i = 0; i < s.size(); ++i )
    {
      if ( s[i] == delim )
        {
          out.push_back(tok);
          tok.clear();
        }
      else
        tok += s[i];
    }
  out.push_back(tok);
}

struct StationFields
{
  uint32_t    mmsi;
  std::string name;
  std::string callsign;
  uint32_t    type;
  uint32_t    len;
  uint32_t    beam;
  uint32_t    portOffset;
  uint32_t    bowOffset;
};

/**
 * "mmsi,name,callsign,type,len,beam,portoffset,bowoffset": exactly 8 fields,
 * MMSI exactly 9 digits, numeric fields digits only (0-255, they are stored
 * in uint8_t). Name and callsign are taken as given.
 */
inline bool parseStation(const std::string &params, StationFields &f)
{
  std::vector<std::string> t;
  split(params, ',', t);
  if ( t.size() != 8 )
    return false;

  const std::string m = trimmed(t[0]);
  if ( m.size() != 9 || !isDigits(m) )
    return false;
  uint32_t mmsi;
  if ( !parseUInt(m, 999999999u, mmsi) )
    return false;

  StationFields r;
  r.mmsi = mmsi;
  r.name = t[1];
  r.callsign = t[2];
  if ( !parseUInt(t[3], 255u, r.type) || !parseUInt(t[4], 255u, r.len) ||
       !parseUInt(t[5], 255u, r.beam) || !parseUInt(t[6], 255u, r.portOffset) ||
       !parseUInt(t[7], 255u, r.bowOffset) )
    return false;
  f = r;
  return true;
}

// Every CLI verb (prefix form). A line already starting with one is never skipped.
static const char *const kAllVerbs[] = {
    "help", "station ", "station?", "sys?", "nmea?",
    "hourlyreboot on", "hourlyreboot off", "hourlyreboot?",
    "dfu", "erase station", "factory reset",
    "tx test", "tx on", "tx off", "tx?", "stoptx", "reboot", "cli",
    "gps off", "gps on", "gnss?", "heading off", "heading?", "heading ",
    "msg24", "otp?", "otp ", "xotrim ", "xotrim?", "pa_level ", "pa_level?"
};

// Verbs that are read-only or idempotent: junk in front of them may be skipped.
static const char *const kSafeVerbs[] = {
    "help", "station?", "sys?", "nmea?", "hourlyreboot?", "tx?", "gnss?",
    "heading?", "heading off", "heading ", "otp?", "xotrim?", "pa_level?"
};

/**
 * TX EMI used to put a few junk bytes in front of a command. Skip at most
 * CLI_MAX_JUNK_SKIP bytes, and only toward a safe verb. Anything that changes
 * state, transmits or resets must start the line.
 */
inline void skipToSafeCommand(std::string &s)
{
  for ( size_t i = 0; i < sizeof kAllVerbs / sizeof kAllVerbs[0]; ++i )
    if ( s.compare(0, strlen(kAllVerbs[i]), kAllVerbs[i]) == 0 )
      return;

  size_t best = std::string::npos;
  for ( size_t i = 0; i < sizeof kSafeVerbs / sizeof kSafeVerbs[0]; ++i )
    {
      const size_t p = s.find(kSafeVerbs[i]);
      if ( p != std::string::npos && p <= CLI_MAX_JUNK_SKIP && p < best )
        best = p;
    }
  if ( best != std::string::npos && best > 0 )
    s.erase(0, best);
}

} // namespace cli

#endif /* CLIPARSE_HPP_ */
