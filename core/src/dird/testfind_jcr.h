#ifndef TESTFIND_JCR_H
#define TESTFIND_JCR_H

#include "include/jcr.h"
#include "findlib/find.h"
#include "dird/dird_conf.h"

void SetupTestfindJcr(directordaemon::FilesetResource* jcr_fileset,const char* configfile,int attrs);
void SetOptions(findFOPTS* fo, const char* opts);
bool setupFileset(FindFilesPacket* ff, directordaemon::FilesetResource* jcr_fileset);
void CountFiles(FindFilesPacket* ar);
int PrintFile(JobControlRecord*, FindFilesPacket* ff, bool);
int testfindLogic(JobControlRecord* jcr,
                      FindFilesPacket* ff_pkt,
                      bool top_level);
#endif // TESTFIND_JCR_H
