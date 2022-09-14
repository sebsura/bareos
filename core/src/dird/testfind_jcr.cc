#include "include/bareos.h"
#include "filed/filed.h"
#include "filed/jcr_private.h"
#include "filed/dir_cmd.h"
#include "lib/crypto.h"
#include "lib/bsock_tcp.h"


using namespace filedaemon;

JobControlRecord* SetupTestfindJcr(FindFilesPacket* ff)
{
  BareosSocketTCP* sock = new BareosSocketTCP;
  sock->message_length = 0;
  JobControlRecord* jcr;
  jcr = NewFiledJcr();
  jcr->store_bsock = sock;
  jcr->impl->ff = ff;
  return jcr;
}
