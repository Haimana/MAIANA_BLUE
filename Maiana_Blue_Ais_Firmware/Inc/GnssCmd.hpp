/*
  `gnss <sentence>` CLI passthrough: validation of a configuration sentence
  for the Quectel LC76G (header-only, no HAL dependencies so it can be
  unit-tested on the host, like CliParse.hpp).

  Only an explicit whitelist is forwarded. Anything that could stop the data
  the AIS depends on (RMC/GGA at 1 Hz, 1PPS, the UART rate the MCU can detect,
  GNSS power) is rejected. Source: Quectel LC26G/LC26G-T/LC76G/LC86G GNSS
  Protocol Specification and AGNSS Application Note.

  This file is part of the MAIANA(TM) transponder firmware.
*/

#ifndef GNSSCMD_HPP_
#define GNSSCMD_HPP_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>

// Max length of the sentence typed after "gnss " ("$...", optional "*HH").
// The CLI line buffer is 80 bytes ("gnss " + sentence + LF).
#ifndef GNSS_CMD_MAX_LEN
#define GNSS_CMD_MAX_LEN 72
#endif

namespace gnss_cmd
{

static const char *const kErrFormat   = "invalid GNSS sentence";
static const char *const kErrTooLong  = "GNSS sentence too long";
static const char *const kErrChecksum = "bad checksum";
static const char *const kErrDenied   = "GNSS command not allowed";
static const char *const kErrParams   = "invalid or unsafe GNSS parameters";

inline int hexVal(char c)
{
  if ( c >= '0' && c <= '9' ) return c - '0';
  if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
  if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
  return -1;
}

// XOR of the characters between '$' and '*' (core excludes both).
inline uint8_t checksum(const std::string &core)
{
  uint8_t c = 0;
  for ( size_t i = 0; i < core.size(); ++i )
    c ^= (uint8_t)core[i];
  return c;
}

// Canonical decimal integer: optional '-', 1..6 digits, no leading zeros
// (so "04" cannot slip past a string comparison with "4"), value in [mn, mx].
inline bool isInt(const std::string &s, long mn, long mx)
{
  size_t i = 0;
  bool neg = false;
  if ( i < s.size() && s[i] == '-' )
    {
      neg = true;
      ++i;
    }
  if ( i >= s.size() || s.size() - i > 6 )
    return false;
  if ( s[i] == '0' && (s.size() - i > 1 || neg) )
    return false;
  long v = 0;
  for ( ; i < s.size(); ++i )
    {
      if ( s[i] < '0' || s[i] > '9' )
        return false;
      v = v * 10 + (s[i] - '0');
    }
  if ( neg )
    v = -v;
  return v >= mn && v <= mx;
}

inline bool isOneOf(const std::string &s, const char *const *vals, size_t n)
{
  for ( size_t i = 0; i < n; ++i )
    if ( s == vals[i] )
      return true;
  return false;
}

// Optional '-', digits, optional '.' and digits (at least one digit), value in [mn, mx].
inline bool isDec(const std::string &s, double mn, double mx)
{
  if ( s.empty() || s.size() > 12 )
    return false;
  size_t i = 0;
  bool neg = false;
  if ( s[0] == '-' )
    {
      neg = true;
      ++i;
    }
  double v = 0, scale = 0;
  int digits = 0;
  bool dot = false;
  for ( ; i < s.size(); ++i )
    {
      const char c = s[i];
      if ( c == '.' )
        {
          if ( dot )
            return false;
          dot = true;
          scale = 1;
          continue;
        }
      if ( c < '0' || c > '9' )
        return false;
      ++digits;
      if ( dot )
        {
          scale /= 10;
          v += (c - '0') * scale;
        }
      else
        v = v * 10 + (c - '0');
    }
  if ( digits == 0 )
    return false;
  if ( neg )
    v = -v;
  return v >= mn && v <= mx;
}

typedef std::vector<std::string> Fields;   // f[0] = command name (no '$')

inline bool bit(const std::string &s) { return s == "0" || s == "1"; }

inline bool chkNoArgs(const Fields &f) { return f.size() == 1; }
inline bool chkBit(const Fields &f)    { return f.size() == 2 && bit(f[1]); }

inline bool chkFixRate(const Fields &f)   { return f.size() == 2 && f[1] == "1000"; }
inline bool chkMinSnr(const Fields &f)    { return f.size() == 2 && isInt(f[1], 9, 30); }
inline bool chkStatic(const Fields &f)    { return f.size() == 2 && isInt(f[1], 0, 20); }
inline bool chkElevMask(const Fields &f)  { return f.size() == 2 && isInt(f[1], 0, 45); }
inline bool chkGetRate(const Fields &f)   { return f.size() == 2 && isInt(f[1], -1, 9); }
inline bool chkEpoSys(const Fields &f)    { return f.size() == 2 && isInt(f[1], 0, 3); }

// PAIR062,<type>,<rate>: GGA (0) and RMC (4) must stay at 1.
inline bool chkNmeaRate(const Fields &f)
{
  if ( f.size() != 3 || !isInt(f[1], -1, 9) || !isInt(f[2], 0, 20) )
    return false;
  if ( (f[1] == "0" || f[1] == "4") && f[2] != "1" )
    return false;
  return true;
}

// PAIR066,<GPS>,<GLO>,<GAL>,<BDS>,<QZSS>,<Reserved>: GPS must stay enabled.
inline bool chkSearchMode(const Fields &f)
{
  if ( f.size() != 7 || f[1] != "1" || f[6] != "0" )
    return false;
  for ( size_t i = 2; i <= 5; ++i )
    if ( !bit(f[i]) )
      return false;
  return true;
}

inline bool chkNavMode(const Fields &f)
{
  static const char *const v[] = { "0", "1", "3", "4", "5", "7" };
  return f.size() == 2 && isOneOf(f[1], v, 6);
}

inline bool chkDgps(const Fields &f)
{
  static const char *const v[] = { "0", "2", "3" };
  return f.size() == 2 && isOneOf(f[1], v, 3);
}

inline bool chkEpocCfg(const Fields &f)
{
  static const char *const v[] = { "1", "3", "5" };
  return f.size() == 2 && isOneOf(f[1], v, 3);
}

// PAIR590,YYYY,MM,DD,hh,mm,ss (reference UTC)
inline bool chkRefTime(const Fields &f)
{
  return f.size() == 7 && isInt(f[1], 1980, 2099) && isInt(f[2], 1, 12) && isInt(f[3], 1, 31)
      && isInt(f[4], 0, 23) && isInt(f[5], 0, 59) && isInt(f[6], 0, 59);
}

// PAIR600,lat,lon,height,accMaj,accMin,bearing,accVert (reference position)
inline bool chkRefPos(const Fields &f)
{
  return f.size() == 8 && isDec(f[1], -90, 90) && isDec(f[2], -180, 180) && isDec(f[3], -1000, 20000)
      && isDec(f[4], 0, 1000000) && isDec(f[5], 0, 1000000) && isDec(f[6], 0, 360) && isDec(f[7], 0, 1000000);
}

// PAIR864,0,0,115200 only: the LC76G default that the MCU detects.
inline bool chkBaud(const Fields &f)
{
  return f.size() == 4 && f[1] == "0" && f[2] == "0" && f[3] == "115200";
}

inline bool chkGetBaud(const Fields &f)
{
  return f.size() == 3 && f[1] == "0" && f[2] == "0";
}

// PQTMCFGCNST,R | PQTMCFGCNST,W,<GPS>,<GLO>,<GAL>,<BDS>,<QZSS>,<Reserved> (GPS stays on)
inline bool chkCnst(const Fields &f)
{
  if ( f.size() == 2 && f[1] == "R" )
    return true;
  if ( f.size() != 8 || f[1] != "W" || f[2] != "1" || f[7] != "0" )
    return false;
  for ( size_t i = 3; i <= 6; ++i )
    if ( !bit(f[i]) )
      return false;
  return true;
}

inline bool isMsgName(const std::string &s)
{
  if ( s.empty() || s.size() > 20 )
    return false;
  for ( size_t i = 0; i < s.size(); ++i )
    {
      const char c = s[i];
      if ( !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) )
        return false;
    }
  return true;
}

// PQTMCFGMSGRATE,R,<name>[,<ver>] | PQTMCFGMSGRATE,W,<name>,<rate>[,<ver>]
inline bool chkMsgRate(const Fields &f)
{
  if ( f.size() < 2 )
    return false;
  if ( f[1] == "R" )
    return (f.size() == 3 || (f.size() == 4 && isInt(f[3], 1, 9))) && isMsgName(f[2]);
  if ( f[1] != "W" || f.size() < 4 )
    return false;

  const std::string &name = f[2];
  if ( name == "RMC" || name == "GGA" )
    return f.size() == 4 && f[3] == "1";

  static const char *const kStd[] = { "GLL", "GSA", "GSV", "VTG", "ZDA", "GRS", "GST", "GNS" };
  if ( isOneOf(name, kStd, sizeof kStd / sizeof kStd[0]) )
    return f.size() == 4 && bit(f[3]);

  if ( f.size() != 5 || !bit(f[3]) )
    return false;
  if ( name == "PQTMEPE" )
    return f[4] == "2";
  if ( name == "PQTMDOP" || name == "PQTMJAMMINGSTATUS" )
    return f[4] == "1";
  return false;
}

// PQTMCFGxxx,R[,<n>[,<n>]]: read-only access to the other PQTM configs.
inline bool chkCfgRead(const Fields &f)
{
  if ( f.size() < 2 || f.size() > 4 || f[1] != "R" )
    return false;
  for ( size_t i = 2; i < f.size(); ++i )
    if ( !isInt(f[i], 0, 99) )
      return false;
  return true;
}

struct Rule
{
  const char *name;
  bool (*check)(const Fields &);
};

// Everything not listed here is rejected, including (deliberately):
//   PAIR003 power off, PAIR650 RTC sleep, PAIR382 lock sleep, PAIR680/690/730/732 low power,
//   PAIR086 debug log, PAIR75x/76x and PQTMCFGPPS,W (1PPS), PAIR864 other rates,
//   PAIR43x RTCM, PAIR9xx LOCUS, PAIR471/472 EPO data, PQTMCFGNMEADP/NMEATID writes,
//   PQTMPVT output, PAIR080 modes 2/6.
static const Rule kRules[] = {
    // restarts, NVM, identification
    { "PAIR002", chkNoArgs },          // GNSS subsystem power on (harmless)
    { "PAIR004", chkNoArgs },          // hot start
    { "PAIR005", chkNoArgs },          // warm start
    { "PAIR006", chkNoArgs },          // cold start
    { "PAIR511", chkNoArgs },          // save navigation data to NVM
    { "PAIR513", chkNoArgs },          // save PAIR settings to NVM
    { "PAIR514", chkNoArgs },          // restore default settings
    { "PQTMSAVEPAR", chkNoArgs },
    { "PQTMRESTOREPAR", chkNoArgs },
    { "PQTMVERNO", chkNoArgs },
    // queries
    { "PAIR051", chkNoArgs },
    { "PAIR059", chkNoArgs },
    { "PAIR063", chkGetRate },
    { "PAIR067", chkNoArgs },
    { "PAIR071", chkNoArgs },
    { "PAIR073", chkNoArgs },
    { "PAIR075", chkNoArgs },
    { "PAIR081", chkNoArgs },
    { "PAIR163", chkNoArgs },
    { "PAIR401", chkNoArgs },
    { "PAIR411", chkNoArgs },
    { "PAIR470", chkEpoSys },          // EPO status
    { "PAIR491", chkNoArgs },          // EASY status
    { "PAIR865", chkGetBaud },
    // settings (field-checked)
    { "PAIR050", chkFixRate },         // 1000 ms only
    { "PAIR058", chkMinSnr },          // 9..30 dB
    { "PAIR062", chkNmeaRate },        // GGA/RMC stay at 1
    { "PAIR066", chkSearchMode },      // GPS stays on
    { "PAIR070", chkStatic },          // 0..20 dm/s
    { "PAIR072", chkElevMask },        // 0..45 deg
    { "PAIR074", chkBit },             // active interference canceller
    { "PAIR080", chkNavMode },         // 0,1,3,4,5,7
    { "PAIR162", chkBit },             // 2D fix
    { "PAIR391", chkBit },             // jamming detection ($PAIRSPF)
    { "PAIR400", chkDgps },            // 0,2,3
    { "PAIR410", chkBit },             // SBAS
    { "PAIR490", chkBit },             // EASY
    { "PAIR496", chkBit },             // EPOC
    { "PAIR498", chkEpocCfg },         // 1,3,5
    { "PAIR590", chkRefTime },
    { "PAIR600", chkRefPos },
    { "PAIR864", chkBaud },            // 0,0,115200 only
    { "PQTMCFGCNST", chkCnst },
    { "PQTMCFGMSGRATE", chkMsgRate },
    { "PQTMCFGPPS", chkCfgRead },      // read only
    { "PQTMCFGANTENNA", chkCfgRead },
    { "PQTMCFGNMEADP", chkCfgRead },
    { "PQTMCFGNMEATID", chkCfgRead },
    { "PQTMCFGODO", chkCfgRead },
    { "PQTMCFGGEOFENCE", chkCfgRead },
    { "PQTMCFGSLAS", chkCfgRead },
};

/**
 * Validate the sentence typed after "gnss " and build the line to send.
 * in:  "$NAME[,f1,...][*HH]" (blanks around it are ignored by the caller's trim)
 * out: "$NAME[,f1,...]*HH\r\n" (NUL-terminated), outSize >= GNSS_CMD_MAX_LEN + 7.
 * Returns nullptr on success, otherwise a short error text.
 */
inline const char *build(const std::string &in, char *out, size_t outSize)
{
  if ( !out || outSize < GNSS_CMD_MAX_LEN + 7 )
    return kErrTooLong;
  out[0] = 0;
  if ( in.size() > GNSS_CMD_MAX_LEN )
    return kErrTooLong;
  if ( in.size() < 2 || in[0] != '$' )
    return kErrFormat;

  std::string core;
  const size_t star = in.find('*');
  if ( star == std::string::npos )
    core = in.substr(1);
  else
    {
      if ( star + 3 != in.size() )
        return kErrFormat;
      const int h = hexVal(in[star + 1]), l = hexVal(in[star + 2]);
      if ( h < 0 || l < 0 )
        return kErrFormat;
      core = in.substr(1, star - 1);
      if ( checksum(core) != (uint8_t)(h * 16 + l) )
        return kErrChecksum;
    }

  if ( core.empty() )
    return kErrFormat;
  for ( size_t i = 0; i < core.size(); ++i )
    {
      const char c = core[i];
      if ( c <= 0x20 || c >= 0x7F || c == '$' || c == '!' || c == '*' || c == '\\' || c == '^' )
        return kErrFormat;
    }

  Fields f;
  std::string tok;
  for ( size_t i = 0; i < core.size(); ++i )
    {
      if ( core[i] == ',' )
        {
          f.push_back(tok);
          tok.clear();
        }
      else
        tok += core[i];
    }
  f.push_back(tok);

  const Rule *rule = nullptr;
  for ( size_t i = 0; i < sizeof kRules / sizeof kRules[0]; ++i )
    if ( f[0] == kRules[i].name )
      {
        rule = &kRules[i];
        break;
      }
  if ( !rule )
    return kErrDenied;
  if ( !rule->check(f) )
    return kErrParams;

  static const char hex[] = "0123456789ABCDEF";
  const uint8_t cs = checksum(core);
  const size_t n = core.size();
  out[0] = '$';
  memcpy(out + 1, core.data(), n);
  out[n + 1] = '*';
  out[n + 2] = hex[cs >> 4];
  out[n + 3] = hex[cs & 0x0F];
  out[n + 4] = '\r';
  out[n + 5] = '\n';
  out[n + 6] = 0;
  return nullptr;
}

} // namespace gnss_cmd

#endif /* GNSSCMD_HPP_ */
