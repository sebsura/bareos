/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2009-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2016 Planets Communications B.V.
   Copyright (C) 2016-2025 Bareos GmbH & Co. KG

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
#ifndef BAREOS_CATS_POSTGRESQL_H_
#define BAREOS_CATS_POSTGRESQL_H_

#include "include/bareos.h"


#ifdef HAVE_POSTGRESQL

#  include "cats/column_data.h"
#  include "cats.h"
#  include "libpq-fe.h"

#  include <string>
#  include <vector>

struct AttributesDbRecord;
class JobControlRecord;

namespace postgresql {
db_conn* connect(JobControlRecord* jcr, const connection_parameter& params);
};
#endif  /* HAVE_POSTGRESQL */
#endif  // BAREOS_CATS_POSTGRESQL_H_
