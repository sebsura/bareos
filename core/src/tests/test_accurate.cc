/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2019-2022 Bareos GmbH & Co. KG

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
#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "gtest/gtest.h"
#else
#  include "gtest/gtest.h"
#  include "include/bareos.h"
#endif

#include "lib/parse_conf.h"
#include "filed/filed_globals.h"
#include "filed/filed_conf.h"
#include "findlib/find.h"
#include "filed/accurate.h"
#include "filed/filed.h"
#include "filed/jcr_private.h"

namespace filedaemon {

static JobControlRecord* NewFiledJcr()
{
  JobControlRecord* jcr = new_jcr(nullptr);
  jcr->impl = new JobControlRecordPrivate;
  return jcr;
}


TEST(accurate, accurate_lmdb)
{
  OSDependentInit();

  std::string path_to_config_file = std::string(
      RELATIVE_PROJECT_SOURCE_DIR "/configs/bareos-configparser-tests");
  my_config = InitFdConfig(path_to_config_file.c_str(), M_ERROR_TERM);

  ASSERT_TRUE(my_config->ParseConfig());

  JobControlRecord* jcr = NewFiledJcr();
  uint32_t number_of_previous_files = 100;

  my_config->DumpResources(PrintMessage, NULL);
  BareosAccurateFilelist* my_filelist
      = new BareosAccurateFilelistLmdb(jcr, number_of_previous_files);

  ASSERT_TRUE(my_filelist->init());

  delete my_config;
}

}  // namespace filedaemon
