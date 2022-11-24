/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2022-2022 Bareos GmbH & Co. KG

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


#include "filed/filed_utils.h"
#include "filed/filed.h"
#include "filed/filed_jcr_impl.h"
#include "filed/filed_globals.h"
#include "filed/dir_cmd.h"

#include "dird/dird_conf.h"

#include "dird/testfind_jcr.h"

#include "lib/bsock_testfind.h"
#include "lib/parse_conf.h"


using namespace filedaemon;

void SetupTestfindJcr(directordaemon::FilesetResource* jcr_fileset,
                      const char* configfile)
{
  crypto_cipher_t cipher = CRYPTO_CIPHER_NONE;

  my_config = InitFdConfig(configfile, M_ERROR_TERM);
  my_config->ParseConfig();

  me = static_cast<ClientResource*>(my_config->GetNextRes(R_CLIENT, nullptr));
  no_signals = true;
  me->compatible = true;

  if (CheckResources()) {
    BareosSocketTestfind* dird_sock = new BareosSocketTestfind;
    BareosSocketTestfind* stored_sock = new BareosSocketTestfind;
    stored_sock->message_length = 0;
    JobControlRecord* jcr;
    jcr = create_new_director_session(dird_sock);

    setupFileset(jcr->fd_impl->ff, jcr_fileset);

    jcr->store_bsock = stored_sock;

    GetWantedCryptoCipher(jcr, &cipher);

    BlastDataToStorageDaemon(jcr, cipher, DEFAULT_NETWORK_BUFFER_SIZE);

    CleanupFileset(jcr);
    FreeJcr(jcr);
  }
  if (me->secure_erase_cmdline) { FreePoolMemory(me->secure_erase_cmdline); }
}


bool setupFileset(FindFilesPacket* ff,
                  directordaemon::FilesetResource* jcr_fileset)
{
  int num;
  bool include = true;

  findFILESET* fileset;
  findFOPTS* current_opts;

  findFILESET* fileset_allocation = (findFILESET*)malloc(sizeof(findFILESET));
  fileset = new (fileset_allocation)(findFILESET);
  ff->fileset = fileset;

  fileset->state = state_none;
  fileset->include_list.init(1, true);
  fileset->exclude_list.init(1, true);

  for (;;) {
    if (include) {
      num = jcr_fileset->include_items.size();
    } else {
      num = jcr_fileset->exclude_items.size();
    }
    for (int i = 0; i < num; i++) {
      directordaemon::IncludeExcludeItem* ie;
      ;
      int k;

      if (include) {
        ie = jcr_fileset->include_items[i];
        /* New include */
        findIncludeExcludeItem* incexe_allocation
            = (findIncludeExcludeItem*)malloc(sizeof(findIncludeExcludeItem));
        fileset->incexe = new (incexe_allocation)(findIncludeExcludeItem);
        fileset->include_list.append(fileset->incexe);
      } else {
        ie = jcr_fileset->exclude_items[i];

        /* New exclude */
        findIncludeExcludeItem* incexe_allocation
            = (findIncludeExcludeItem*)malloc(sizeof(findIncludeExcludeItem));
        fileset->incexe = new (incexe_allocation)(findIncludeExcludeItem);
        fileset->exclude_list.append(fileset->incexe);
      }

      for (std::size_t j = 0; j < ie->file_options_list.size(); j++) {
        directordaemon::FileOptions* fo = ie->file_options_list[j];

        findFOPTS* current_opts_allocation
            = (findFOPTS*)malloc(sizeof(findFOPTS));
        current_opts = new (current_opts_allocation)(findFOPTS);

        fileset->incexe->current_opts = current_opts;
        fileset->incexe->opts_list.append(current_opts);

        SetOptions(current_opts, fo->opts);

        for (k = 0; k < fo->regex.size(); k++) {
          current_opts->regex.append(StringToRegex(fo->regex.get(k)));
        }
        for (k = 0; k < fo->regexdir.size(); k++) {
          // fd->fsend("RD %s\n", fo->regexdir.get(k));
          current_opts->regexdir.append(StringToRegex(fo->regexdir.get(k)));
        }
        for (k = 0; k < fo->regexfile.size(); k++) {
          // fd->fsend("RF %s\n", fo->regexfile.get(k));
          current_opts->regexfile.append(StringToRegex(fo->regexfile.get(k)));
        }
        for (k = 0; k < fo->wild.size(); k++) {
          current_opts->wild.append(strdup((const char*)fo->wild.get(k)));
        }
        for (k = 0; k < fo->wilddir.size(); k++) {
          current_opts->wilddir.append(strdup((const char*)fo->wilddir.get(k)));
        }
        for (k = 0; k < fo->wildfile.size(); k++) {
          current_opts->wildfile.append(
              strdup((const char*)fo->wildfile.get(k)));
        }
        for (k = 0; k < fo->wildbase.size(); k++) {
          current_opts->wildbase.append(
              strdup((const char*)fo->wildbase.get(k)));
        }
        for (k = 0; k < fo->fstype.size(); k++) {
          current_opts->fstype.append(strdup((const char*)fo->fstype.get(k)));
        }
        for (k = 0; k < fo->Drivetype.size(); k++) {
          current_opts->Drivetype.append(
              strdup((const char*)fo->Drivetype.get(k)));
        }
      }
    }

    if (!include) { /* If we just did excludes */
      break;        /*   all done */
    }

    include = false; /* Now do excludes */
  }

  return true;
}

void SetOptions(findFOPTS* fo, const char* opts)
{
  int j;
  const char* p;

  for (p = opts; *p; p++) {
    switch (*p) {
      case 'a': /* alway replace */
      case '0': /* no option */
        break;
      case 'e':
        SetBit(FO_EXCLUDE, fo->flags);
        break;
      case 'f':
        SetBit(FO_MULTIFS, fo->flags);
        break;
      case 'h': /* no recursion */
        SetBit(FO_NO_RECURSION, fo->flags);
        break;
      case 'H': /* no hard link handling */
        SetBit(FO_NO_HARDLINK, fo->flags);
        break;
      case 'i':
        SetBit(FO_IGNORECASE, fo->flags);
        break;
      case 'M': /* MD5 */
        SetBit(FO_MD5, fo->flags);
        break;
      case 'n':
        SetBit(FO_NOREPLACE, fo->flags);
        break;
      case 'p': /* use portable data format */
        SetBit(FO_PORTABLE, fo->flags);
        break;
      case 'R': /* Resource forks and Finder Info */
        SetBit(FO_HFSPLUS, fo->flags);
        [[fallthrough]];
      case 'r': /* read fifo */
        SetBit(FO_READFIFO, fo->flags);
        break;
      case 'S':
        switch (*(p + 1)) {
          case ' ':
            /* Old director did not specify SHA variant */
            SetBit(FO_SHA1, fo->flags);
            break;
          case '1':
            SetBit(FO_SHA1, fo->flags);
            p++;
            break;
#ifdef HAVE_SHA2
          case '2':
            SetBit(FO_SHA256, fo->flags);
            p++;
            break;
          case '3':
            SetBit(FO_SHA512, fo->flags);
            p++;
            break;
#endif
          default:
            /* Automatically downgrade to SHA-1 if an unsupported
             * SHA variant is specified */
            SetBit(FO_SHA1, fo->flags);
            p++;
            break;
        }
        break;
      case 's':
        SetBit(FO_SPARSE, fo->flags);
        break;
      case 'm':
        SetBit(FO_MTIMEONLY, fo->flags);
        break;
      case 'k':
        SetBit(FO_KEEPATIME, fo->flags);
        break;
      case 'A':
        SetBit(FO_ACL, fo->flags);
        break;
      case 'V': /* verify options */
        /* Copy Verify Options */
        for (j = 0; *p && *p != ':'; p++) {
          fo->VerifyOpts[j] = *p;
          if (j < (int)sizeof(fo->VerifyOpts) - 1) { j++; }
        }
        fo->VerifyOpts[j] = 0;
        break;
      case 'w':
        SetBit(FO_IF_NEWER, fo->flags);
        break;
      case 'W':
        SetBit(FO_ENHANCEDWILD, fo->flags);
        break;
      case 'Z': /* compression */
        p++;    /* skip Z */
        if (*p >= '0' && *p <= '9') {
          SetBit(FO_COMPRESS, fo->flags);
          fo->Compress_algo = COMPRESS_GZIP;
          fo->Compress_level = *p - '0';
        } else if (*p == 'o') {
          SetBit(FO_COMPRESS, fo->flags);
          fo->Compress_algo = COMPRESS_LZO1X;
          fo->Compress_level = 1; /* not used with LZO */
        }
        Dmsg2(200, "Compression alg=%d level=%d\n", fo->Compress_algo,
              fo->Compress_level);
        break;
      case 'x':
        SetBit(FO_NO_AUTOEXCL, fo->flags);
        break;
      case 'X':
        SetBit(FO_XATTR, fo->flags);
        break;
      default:
        Emsg1(M_ERROR, 0, _("Unknown include/exclude option: %c\n"), *p);
        break;
    }
  }
}
