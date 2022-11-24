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

#include "include/bareos.h"
#include "filed/filed_utils.h"
#include "filed/filed.h"
#include "filed/jcr_private.h"
#include "filed/dir_cmd.h"
#include "lib/crypto.h"
#include "lib/bsock_testfind.h"
#include "lib/mem_pool.h"
#include "filed/filed_conf.h"
#include "lib/parse_conf.h"
#include "filed/filed_globals.h"
#include "filed/dir_cmd.h"
#include "lib/crypto_openssl.h"


using namespace filedaemon;

void SetupTestfindJcr(FindFilesPacket* ff, const char* configfile)
{
  crypto_cipher_t cipher = CRYPTO_CIPHER_NONE;

  filedaemon::my_config = InitFdConfig(configfile, M_ERROR_TERM);
  filedaemon::my_config->ParseConfig();
  ClientResource* cl
      = static_cast<ClientResource*>(my_config->GetNextRes(R_CLIENT, nullptr));
  filedaemon::me
      = static_cast<ClientResource*>(my_config->GetNextRes(R_CLIENT, nullptr));


  if (CheckResources()) {
    BareosSocketTestfind* sock = new BareosSocketTestfind;
    sock->message_length = 0;
    JobControlRecord* jcr;
    jcr = create_new_director_session(sock);

    jcr->store_bsock = sock;
    jcr->impl->ff = ff;
    jcr->impl->last_fname = GetPoolMemory(PM_FNAME);

    GetWantedCryptoCipher(jcr, &cipher);

    BlastDataToStorageDaemon(jcr, NULL, cipher, DEFAULT_NETWORK_BUFFER_SIZE);
  }
  if (cl->secure_erase_cmdline) { FreePoolMemory(cl->secure_erase_cmdline); }
}
