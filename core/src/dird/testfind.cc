/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2008 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2022 Bareos GmbH & Co. KG

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
// Kern Sibbald, MM
/**
 * @file
 * Test program for find files
 */


#include "include/bareos.h"
#include "include/ch.h"

#include "dird/dird_conf.h"
#include "dird/dird_globals.h"
#include "dird/jcr_util.h"
#include "dird/testfind_jcr.h"
#include "filed/fileset.h"

#include "lib/mntent_cache.h"
#include "lib/parse_conf.h"
#include "dird/jcr_util.h"
#include "dird/dird_globals.h"
#include "dird/dird_conf.h"
#include "dird/director_jcr_impl.h"
#include "lib/recent_job_results_list.h"
#include "lib/tree.h"
#include "findlib/find.h"
#include "findlib/attribs.h"

#if defined(HAVE_WIN32)
#  define isatty(fd) (fd == 0)
#endif


using namespace directordaemon;

void TestfindFreeJcr(JobControlRecord* jcr)
{
  Dmsg0(200, "Start testfind FreeJcr\n");

  if (jcr->dir_impl) {
    delete jcr->dir_impl;
    jcr->dir_impl = nullptr;
  }

  Dmsg0(200, "End testfind FreeJcr\n");
}


extern bool ParseDirConfig(const char* configfile, int exit_code);

/* Global variables */
static int num_files = 0;
static int max_file_len = 0;
static int max_path_len = 0;
static int trunc_fname = 0;
static int trunc_path = 0;
static int attrs = 0;


static int PrintFile(JobControlRecord* jcr, FindFilesPacket* ff, bool);
static void CountFiles(FindFilesPacket* ff);
static void usage()
{
  fprintf(
      stderr,
      _("\n"
        "Usage: testfind [-d debug_level] [-] [pattern1 ...]\n"
        "       -a          print extended attributes (Win32 debug)\n"
        "       -d <nn>     set debug level to <nn>\n"
        "       -dt         print timestamp in debug output\n"
        "       -c          specify config file containing FileSet resources\n"
        "       -f          specify which FileSet to use\n"
        "       -?          print this message.\n"
        "\n"
        "Patterns are used for file inclusion -- normally directories.\n"
        "Debug level >= 1 prints each file found.\n"
        "Debug level >= 10 prints path/file for catalog.\n"
        "Errors are always printed.\n"
        "Files/paths truncated is the number of files/paths with len > 255.\n"
        "Truncation is only in the catalog.\n"
        "\n"));

  exit(1);
}


int main(int argc, char* const* argv)
{
  const char* configfile = ConfigurationParser::GetDefaultConfigDir();
  const char* fileset_name = "SelfTest";
  int ch, hard_links;

  OSDependentInit();

  setlocale(LC_ALL, "");
  tzset();
  bindtextdomain("bareos", LOCALEDIR);
  textdomain("bareos");

  while ((ch = getopt(argc, argv, "ac:d:f:?")) != -1) {
    switch (ch) {
      case 'a': /* print extended attributes *debug* */
        attrs = 1;
        break;

      case 'c': /* set debug level */
        configfile = optarg;
        break;

      case 'd': /* set debug level */
        if (*optarg == 't') {
          dbg_timestamp = true;
        } else {
          debug_level = atoi(optarg);
          if (debug_level <= 0) { debug_level = 1; }
        }
        break;

      case 'f': /* exclude patterns */
        fileset_name = optarg;
        break;

      case '?':
      default:
        usage();
    }
  }

  argc -= optind;
  argv += optind;

  directordaemon::my_config = InitDirConfig(configfile, M_ERROR_TERM);
  directordaemon::my_config->ParseConfig();


  MessagesResource* msg;

  foreach_res (msg, R_MSGS) {
    InitMsg(NULL, msg);
  }

  JobControlRecord* jcr;
  jcr = NewDirectorJcr(TestfindFreeJcr);

  FilesetResource* dir_fileset
      = (FilesetResource*)my_config->GetResWithName(R_FILESET, fileset_name);


  if (dir_fileset == NULL) {
    fprintf(stderr, "%s: Fileset not found\n", fileset_name);

    FilesetResource* var;

    fprintf(stderr, "Valid FileSets:\n");

    foreach_res (var, R_FILESET) {
      fprintf(stderr, "    %s\n", var->resource_name_);
    }

    exit(1);
  }

  FindFilesPacket* ff;
  ff = init_find_files();

  setupFileset(ff, dir_fileset);

  //  const char* filename = dir_fileset->include_items[0]->name_list.get(0);

  //  filedaemon::AddFileToFileset(jcr, filename, true, ff->fileset);

  SetupTestfindJcr(dir_fileset, configfile);

  FindFiles(jcr, ff, PrintFile, NULL);

  FreeJcr(jcr);
  if (my_config) {
    delete my_config;
    my_config = NULL;
  }

  RecentJobResultsList::Cleanup();
  CleanupJcrChain();

  /* Clean up fileset */
  findFILESET* fileset = ff->fileset;

  if (fileset) {
    int i, j, k;
    /* Delete FileSet Include lists */
    for (i = 0; i < fileset->include_list.size(); i++) {
      findIncludeExcludeItem* incexe
          = (findIncludeExcludeItem*)fileset->include_list.get(i);
      for (j = 0; j < incexe->opts_list.size(); j++) {
        findFOPTS* fo = (findFOPTS*)incexe->opts_list.get(j);
        for (k = 0; k < fo->regex.size(); k++) {
          regfree((regex_t*)fo->regex.get(k));
        }
        fo->regex.destroy();
        fo->regexdir.destroy();
        fo->regexfile.destroy();
        fo->wild.destroy();
        fo->wilddir.destroy();
        fo->wildfile.destroy();
        fo->wildbase.destroy();
        fo->fstype.destroy();
        fo->Drivetype.destroy();
      }
      incexe->opts_list.destroy();
      incexe->name_list.destroy();
    }
    fileset->include_list.destroy();

    /* Delete FileSet Exclude lists */
    for (i = 0; i < fileset->exclude_list.size(); i++) {
      findIncludeExcludeItem* incexe
          = (findIncludeExcludeItem*)fileset->exclude_list.get(i);
      for (j = 0; j < incexe->opts_list.size(); j++) {
        findFOPTS* fo = (findFOPTS*)incexe->opts_list.get(j);
        fo->regex.destroy();
        fo->regexdir.destroy();
        fo->regexfile.destroy();
        fo->wild.destroy();
        fo->wilddir.destroy();
        fo->wildfile.destroy();
        fo->wildbase.destroy();
        fo->fstype.destroy();
        fo->Drivetype.destroy();
      }
      incexe->opts_list.destroy();
      incexe->name_list.destroy();
    }
    fileset->exclude_list.destroy();
    free(fileset);
  }
  ff->fileset = NULL;
  hard_links = TermFindFiles(ff);

  printf(_("\n"
           "Total files    : %d\n"
           "Max file length: %d\n"
           "Max path length: %d\n"
           "Files truncated: %d\n"
           "Paths truncated: %d\n"
           "Hard links     : %d\n"),
         num_files, max_file_len, max_path_len, trunc_fname, trunc_path,
         hard_links);

  FlushMntentCache();

  TermMsg();

  exit(0);
}


static int PrintFile(JobControlRecord*, FindFilesPacket* ff, bool)
{
  switch (ff->type) {
    case FT_LNKSAVED:
      if (debug_level == 1) {
        printf("%s\n", ff->fname);
      } else if (debug_level > 1) {
        printf("Lnka: %s -> %s\n", ff->fname, ff->link);
      }
      break;
    case FT_REGE:
      if (debug_level == 1) {
        printf("%s\n", ff->fname);
      } else if (debug_level > 1) {
        printf("Empty: %s\n", ff->fname);
      }
      CountFiles(ff);
      break;
    case FT_REG:
      if (debug_level == 1) {
        printf("%s\n", ff->fname);
      } else if (debug_level > 1) {
        printf(_("Reg: %s\n"), ff->fname);
      }
      CountFiles(ff);
      break;
    case FT_LNK:
      if (debug_level == 1) {
        printf("%s\n", ff->fname);
      } else if (debug_level > 1) {
        printf("Lnk: %s -> %s\n", ff->fname, ff->link);
      }
      CountFiles(ff);
      break;
    case FT_DIRBEGIN:
      return 1;
    case FT_NORECURSE:
    case FT_NOFSCHG:
    case FT_INVALIDFS:
    case FT_INVALIDDT:
    case FT_DIREND:
      if (debug_level) {
        char errmsg[100] = "";
        if (ff->type == FT_NORECURSE) {
          bstrncpy(errmsg, _("\t[will not descend: recursion turned off]"),
                   sizeof(errmsg));
        } else if (ff->type == FT_NOFSCHG) {
          bstrncpy(errmsg,
                   _("\t[will not descend: file system change not allowed]"),
                   sizeof(errmsg));
        } else if (ff->type == FT_INVALIDFS) {
          bstrncpy(errmsg, _("\t[will not descend: disallowed file system]"),
                   sizeof(errmsg));
        } else if (ff->type == FT_INVALIDDT) {
          bstrncpy(errmsg, _("\t[will not descend: disallowed drive type]"),
                   sizeof(errmsg));
        }
        printf("%s%s%s\n", (debug_level > 1 ? "Dir: " : ""), ff->fname, errmsg);
      }
      ff->type = FT_DIREND;
      CountFiles(ff);
      break;
    case FT_SPEC:
      if (debug_level == 1) {
        printf("%s\n", ff->fname);
      } else if (debug_level > 1) {
        printf("Spec: %s\n", ff->fname);
      }
      CountFiles(ff);
      break;
    case FT_NOACCESS:
      printf(_("Err: Could not access %s: %s\n"), ff->fname, strerror(errno));
      break;
    case FT_NOFOLLOW:
      printf(_("Err: Could not follow ff->link %s: %s\n"), ff->fname,
             strerror(errno));
      break;
    case FT_NOSTAT:
      printf(_("Err: Could not stat %s: %s\n"), ff->fname, strerror(errno));
      break;
    case FT_NOCHG:
      printf(_("Skip: File not saved. No change. %s\n"), ff->fname);
      break;
    case FT_ISARCH:
      printf(_("Err: Attempt to backup archive. Not saved. %s\n"), ff->fname);
      break;
    case FT_NOOPEN:
      printf(_("Err: Could not open directory %s: %s\n"), ff->fname,
             strerror(errno));
      break;
    default:
      printf(_("Err: Unknown file ff->type %d: %s\n"), ff->type, ff->fname);
      break;
  }
  if (attrs) {
    char attr[200];
    encode_attribsEx(NULL, attr, ff);
    if (*attr != 0) { printf("AttrEx=%s\n", attr); }
    //    set_attribsEx(NULL, ff->fname, NULL, NULL, ff->type, attr);
  }
  return 1;
}

static void CountFiles(FindFilesPacket* ar)
{
  int fnl, pnl;
  char *l, *p;
  PoolMem file(PM_FNAME);
  PoolMem spath(PM_FNAME);

  num_files++;

  /* Find path without the filename.
   * I.e. everything after the last / is a "filename".
   * OK, maybe it is a directory name, but we treat it like
   * a filename. If we don't find a / then the whole name
   * must be a path name (e.g. c:).
   */
  for (p = l = ar->fname; *p; p++) {
    if (IsPathSeparator(*p)) { l = p; /* set pos of last slash */ }
  }
  if (IsPathSeparator(*l)) { /* did we find a slash? */
    l++;                     /* yes, point to filename */
  } else {                   /* no, whole thing must be path name */
    l = p;
  }

  /* If filename doesn't exist (i.e. root directory), we
   * simply create a blank name consisting of a single
   * space. This makes handling zero length filenames
   * easier.
   */
  fnl = p - l;
  if (fnl > max_file_len) { max_file_len = fnl; }
  if (fnl > 255) {
    printf(_("===== Filename truncated to 255 chars: %s\n"), l);
    fnl = 255;
    trunc_fname++;
  }

  if (fnl > 0) {
    PmStrcpy(file, l); /* copy filename */
  } else {
    PmStrcpy(file, " "); /* blank filename */
  }

  pnl = l - ar->fname;
  if (pnl > max_path_len) { max_path_len = pnl; }
  if (pnl > 255) {
    printf(_("========== Path name truncated to 255 chars: %s\n"), ar->fname);
    pnl = 255;
    trunc_path++;
  }

  PmStrcpy(spath, ar->fname);
  if (pnl == 0) {
    PmStrcpy(spath, " ");
    printf(_("========== Path length is zero. File=%s\n"), ar->fname);
  }
  if (debug_level >= 10) {
    printf(_("Path: %s\n"), spath.c_str());
    printf(_("File: %s\n"), file.c_str());
  }
}
