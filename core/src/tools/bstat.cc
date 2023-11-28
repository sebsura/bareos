/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2023 Bareos GmbH & Co. KG

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

#include "include/bareos.h"
#include "include/exit_codes.h"
#include "lib/cli.h"

#include "lib/attribs.h"

#include <ctime>

int main(int argc, const char** argv)
{
  setlocale(LC_ALL, "");
  tzset();
  bindtextdomain("bareos", LOCALEDIR);
  textdomain("bareos");


  CLI::App bstat_app;
  InitCLIApp(bstat_app, "The Bareos LStat tool.");

  AddDebugOptions(bstat_app);

  int64_t dev = 0;
  bstat_app.add_option("--dev", dev);
  int64_t ino = 0;
  bstat_app.add_option("--ino", ino);
  int64_t mode = 0;
  bstat_app.add_option("--mode", mode);
  int64_t nlink = 0;
  bstat_app.add_option("--nlink", nlink);
  int64_t uid = 0;
  bstat_app.add_option("--uid", uid);
  int64_t gid = 0;
  bstat_app.add_option("--gid", gid);
  int64_t rdev = 0;
  bstat_app.add_option("--rdev", rdev);
  int64_t size = 0;
  bstat_app.add_option("--size", size);

#ifndef HAVE_MINGW
  int64_t blksize = 0;
  bstat_app.add_option("--blksize", blksize);
  int64_t blocks = 0;
  bstat_app.add_option("--blocks", blocks);
#endif
  int64_t atime = 0;
  bstat_app.add_option("--atime", atime);
  int64_t mtime = 0;
  bstat_app.add_option("--mtime", mtime);
  int64_t ctime = 0;
  bstat_app.add_option("--ctime", ctime);
  int32_t linkfi = 0;
  bstat_app.add_option("--linkfi", linkfi);

#ifdef HAVE_CHFLAGS
  int64_t flags = 0;
  bstat_app.add_option("--flags", flags);
#endif
  int stream = 0;
  bstat_app.add_option("--stream", stream);

  bool decode = false;
  bstat_app.add_flag("--decode", decode);
  bool output_mask = false;
  bstat_app.add_flag("--mask", output_mask);

  std::vector<std::string> rest;

  bstat_app.add_option("parts-to-decode", rest);

  ParseBareosApp(bstat_app, argc, (char**)argv);

  OSDependentInit();

  struct stat st = {};
  st.st_dev = (dev_t)dev;
  st.st_ino = (ino_t)ino;
  st.st_mode = (mode_t)mode;

  st.st_nlink = nlink;
  st.st_uid = uid;
  st.st_gid = gid;
  st.st_rdev = rdev;
  st.st_size = size;

#ifndef HAVE_MINGW
  st.st_blksize = blksize;
  st.st_blocks = blocks;
#endif

  st.st_atime = atime;
  st.st_mtime = mtime;
  st.st_ctime = ctime;

#ifdef HAVE_CHFLAGS
  st.st_flags = flags;
#endif

  if (output_mask) {
    std::cout << "dev ino mode nlink uid gid rdev size blksize blocks atime "
                 "mtime ctime linkfi flags stream"
              << std::endl;
  } else if (!decode) {
    char buf[1024] = {};
    EncodeStat(buf, &st, sizeof(st), linkfi, stream);

    std::cout << buf << std::endl;
  } else {
    std::string to_decode{};

    for (auto& str : rest) {
      if (to_decode.size()) to_decode += " ";
      to_decode += str;
    }

    stream = DecodeStat((char*)to_decode.c_str(), &st, sizeof(st), &linkfi);


    std::tm atime = *std::localtime(&st.st_atime);
    std::tm mtime = *std::localtime(&st.st_mtime);
    std::tm ctime = *std::localtime(&st.st_ctime);

    std::cout << "Decoded:\n"
              << "LinkFI: " << linkfi << "\n"
              << "DataStream: " << stream << "\n"
              << "lstat:\n"
              << "  st_dev: " << st.st_dev << "\n"
              << "  st_ino: " << st.st_ino << "\n"
              << "  st_mode: " << st.st_mode << "\n"
              << "  st_nlink: " << st.st_nlink << "\n"
              << "  st_uid: " << st.st_uid << "\n"
              << "  st_gid: " << st.st_gid << "\n"
              << "  st_rdev: " << st.st_rdev << "\n"
              << "  st_size: " << st.st_size << "\n"
#ifndef HAVE_MINGW
              << "  st_blksize: " << st.st_blksize << "\n"
              << "  st_blocks: " << st.st_blocks << "\n"
#endif
              << "  st_atime: " << std::put_time(&atime, "%Y-%m-%dT%H%M%SZ")
              << "\n"
              << "  st_mtime: " << std::put_time(&mtime, "%Y-%m-%dT%H%M%SZ")
              << "\n"
              << "  st_ctime: " << std::put_time(&ctime, "%Y-%m-%dT%H%M%SZ")
              << "\n"
#ifdef HAVE_CHFLAGS
              << "  st_flags: " << st.st_flags << "\n"
#endif
        ;
  }

  return 0;
}
