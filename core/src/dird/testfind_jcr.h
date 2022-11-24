#ifndef TESTFIND_JCR_H
#define TESTFIND_JCR_H

#include "include/jcr.h"
#include "findlib/find.h"
#include "dird/dird_conf.h"

void SetupTestfindJcr(directordaemon::FilesetResource* jcr_fileset,const char* configfile);
void SetOptions(findFOPTS* fo, const char* opts);
bool setupFileset(FindFilesPacket* ff, directordaemon::FilesetResource* jcr_fileset);
#endif // TESTFIND_JCR_H
