/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_INCLUDE_TRANSLATE_H_
#define BAREOS_INCLUDE_TRANSLATE_H_

#ifdef ENABLE_NLS
#  include <libintl.h>
#  include <locale.h>
#  ifndef T_
#    define T_(s) gettext((s))
#  endif /* T_ */
#else    /* !ENABLE_NLS */
#  undef T_
#  undef textdomain
#  undef bindtextdomain
#  undef setlocale

#  define T_(s) (s)
#  define textdomain(d)
#  define bindtextdomain(p, d)
#  define setlocale(p, d)
#endif /* ENABLE_NLS */


/* Use the following for strings not to be translated */
#define NT_(s) (s)

#endif  // BAREOS_INCLUDE_TRANSLATE_H_
