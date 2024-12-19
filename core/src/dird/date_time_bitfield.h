/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_DIRD_DATE_TIME_BITFIELD_H_
#define BAREOS_DIRD_DATE_TIME_BITFIELD_H_

#include "lib/bits.h"

#include <bitset>

namespace directordaemon {

struct DateTimeBitfield {
  std::bitset<24> hour;
  std::bitset<31> mday;
  std::bitset<12> month;
  std::bitset<7> wday;
  std::bitset<5> wom;
  std::bitset<54> woy;

  bool last_week_of_month{false};
};

}  // namespace directordaemon

#endif  // BAREOS_DIRD_DATE_TIME_BITFIELD_H_
