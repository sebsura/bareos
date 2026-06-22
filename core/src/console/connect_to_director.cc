/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2026 Bareos GmbH & Co. KG

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
#include "console/console_conf.h"
#include "console/console_globals.h"
#include "console/console_output.h"
#include "console/console_conf.h"
#include "include/jcr.h"
#include "lib/qualified_resource_name_type_converter.h"
#include "lib/bstringlist.h"
#include "lib/bsock_tcp.h"
#include "lib/version.h"

namespace console {

bool ConsoleAuthenticateWithDirector(BareosSocket* dir,
                                     JobControlRecord* jcr,
                                     const char* identity,
                                     s_password& password,
                                     TlsResource* tls_resource,
                                     const std::string& own_qualified_name,
                                     BStringList& response_args,
                                     uint32_t& response_id)
{
  char bashed_name[MAX_NAME_LENGTH];

  bstrncpy(bashed_name, identity, sizeof(bashed_name));
  BashSpaces(bashed_name);

  dir->enable_cram = false;
  dir->StartTimer(60 * 5); /* 5 minutes */
  dir->InitBnetDump(own_qualified_name);
  dir->fsend("Hello %s calling version %s\n", bashed_name,
             kBareosVersionStrings.Full);

  if (!dir->AuthenticateOutboundConnection(jcr, own_qualified_name, identity,
                                           password, tls_resource)) {
    Dmsg0(100, "Authenticate outbound connection failed\n");
    dir->StopTimer();
    return false;
  }
  dir->StopTimer();

  Dmsg1(6, ">dird: %s", dir->msg);

  uint32_t message_id;
  BStringList args;
  if (dir->ReceiveAndEvaluateResponseMessage(message_id, args)) {
    response_id = message_id;
    response_args = args;
    return true;
  }
  Dmsg0(100, "Wrong Message Protocol ID\n");
  return false;
}

BareosSocket* ConnectToDirector(JobControlRecord& jcr,
                                utime_t heart_beat,
                                BStringList& response_args,
                                uint32_t& response_id)
{
  BareosSocketTCP* UA_sock = new BareosSocketTCP;
  if (!UA_sock->connect(NULL, 5, 15, heart_beat, "Director daemon",
                        director_resource->address, NULL,
                        director_resource->DIRport, false)) {
    delete UA_sock;
    UA_sock = nullptr;
    return nullptr;
  }
  jcr.dir_bsock = UA_sock;

  const char* name;
  s_password* password = NULL;

  TlsResource* local_tls_resource;
  if (console_resource) {
    name = console_resource->resource_name_;
    ASSERT(console_resource->password_.encoding == p_encoding_md5);
    password = &console_resource->password_;
    local_tls_resource = console_resource;
  } else { /* default console */
    name = "*UserAgent*";
    ASSERT(director_resource->password_.encoding == p_encoding_md5);
    password = &director_resource->password_;
    local_tls_resource = director_resource;
  }

  if (local_tls_resource->IsTlsConfigured()) {
    std::string qualified_resource_name;
    if (!my_config->GetQualifiedResourceNameTypeConverter()->ResourceToString(
            name, my_config->r_own_, qualified_resource_name)) {
      delete UA_sock;
      UA_sock = nullptr;
      jcr.dir_bsock = nullptr;
      return nullptr;
    }

    if (!UA_sock->DoTlsHandshake(TlsPolicy::kBnetTlsAuto, local_tls_resource,
                                 false, qualified_resource_name.c_str(),
                                 password->value, &jcr)) {
      delete UA_sock;
      UA_sock = nullptr;
      jcr.dir_bsock = nullptr;
      return nullptr;
    }
  } /* IsTlsConfigured */

  std::string own_qualified_name = "R_CONSOLE::";
  own_qualified_name += name;

  if (!ConsoleAuthenticateWithDirector(UA_sock, &jcr, name, *password,
                                       director_resource, own_qualified_name,
                                       response_args, response_id)) {
    delete UA_sock;
    UA_sock = nullptr;
    jcr.dir_bsock = nullptr;
    return nullptr;
  }
  return UA_sock;
}

} /* namespace console */
