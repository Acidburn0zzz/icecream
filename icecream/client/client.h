#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <job.h>

#include "exitcode.h"
#include "logging.h"
#include "util.h"

class MsgChannel;

/* in remote.cpp */
std::string get_absfilename( const std::string &_file );

/* In arg.cpp.  */
bool analyse_argv (const char * const *argv,
                   CompileJob &job);

/* In cpp.cpp.  */
pid_t call_cpp (CompileJob &job, int fd);

/* In local.cpp.  */
int build_local (CompileJob& job, MsgChannel *scheduler);

/* In remote.cpp - permill is the probability it will be compiled three times */
int build_remote (CompileJob &job, MsgChannel *scheduler, int permill);

#endif
