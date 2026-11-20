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


#ifndef VESSELHEADING_HPP_
#define VESSELHEADING_HPP_

#include <inttypes.h>

// True heading for AIS message 18.
// ITU-R M.1371: 0-359 degrees, 511 = not available.
// Sources, all reduced to true degrees:
//   $--HDT,<deg>,T
//   $--HDG,<mag>,<dev>,<E/W>,<var>,<E/W>  (needs variation)
//   $PAIHDG,<deg>                          (from the N2K adapter, PGN 127250)
//   CLI: heading <deg> | heading off | heading?
// A sample older than HEADING_MAX_AGE_MS is reported as 511.
class VesselHeading
{
public:
  static VesselHeading &instance();

  void setTrueDegrees(float deg);
  void clear();
  bool acceptSentence(const char *line);
  uint16_t aisValue() const;
  void report();

private:
  VesselHeading();
  bool fresh() const;

  float mDeg;
  uint32_t mTick;
  bool mValid;
};

#endif
