#include "include/bareos.h"
#include "filed/filed.h"
#include "filed/jcr_private.h"
#include "filed/dir_cmd.h"
#include "lib/crypto.h"


using namespace filedaemon;

JobControlRecord* SetupTestfindJcr(FindFilesPacket* ff)
{
  JobControlRecord* jcr;
  jcr = NewFiledJcr();
  jcr->impl->ff = ff;

  return jcr;
}
