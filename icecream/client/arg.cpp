/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */


#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <cassert>

#include <sys/stat.h>

#include "distcc.h"
#include "logging.h"
#include "io.h"
#include "util.h"
#include "exitcode.h"
#include "filename.h"
#include "arg.h"

using namespace std;

bool analyse_argv( const char * const *argv,
                   CompileJob::ArgumentsList &local_args,
                   CompileJob::ArgumentsList &remote_args,
                   CompileJob::ArgumentsList &rest_args,
                   string &ofile )
{
    ofile.clear();
    local_args.clear();
    remote_args.clear();
    rest_args.clear();

    trace() << "scanning arguments ";
    for ( int index = 0; argv[index]; index++ )
        trace() << argv[index] << " ";
    trace() << endl;

    bool always_local = false;
    bool seen_c = false;
    bool seen_s = false;

    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];

        if (a[0] == '-') {
            if (!strcmp(a, "-E")) {
                always_local = true;
                local_args.push_back( a );
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) {
                local_args.push_back( a );
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                local_args.push_back( a );
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF") || !strcmp(a, "-MT") ||
                       !strcmp(a, "-MQ")) {
                local_args.push_back( a );
                local_args.push_back( argv[i++] );
                /* as above but with extra argument */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                    produce a list of make-style dependencies on
                    header files, either to stdout or to a local file.
                    It implies -E, so only the preprocessor is run,
                    not the compiler.  There would be no point trying
                    to distribute it even if we could. */
                trace() << a << " implies -E (maybe) and must be local" << endl;
                always_local = true;
                local_args.push_back( a );
            } else if (str_startswith("-Wa,", a)) {
                /* Options passed through to the assembler.  The only one we
                 * need to handle so far is -al=output, which directs the
                 * listing to the named file and cannot be remote.  Parsing
                 * all the options would be complex since you can give several
                 * comma-separated assembler options after -Wa, but looking
                 * for '=' should be safe. */
                if (strchr(a, '=')) {
                    trace() << a
                            << " needs to write out assembly listings and must be local"
                            << endl;
                    always_local = true;
                    local_args.push_back( a );
                } else
                    remote_args.push_back( a );
            } else if (!strcmp(a, "-S")) {
                seen_s = true;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")) {
                log_info() << "compiler will emit profile info; must be local" << endl;
                always_local = true;
                local_args.push_back( a );
            } else if (!strcmp(a, "-x")) {
                log_info() << "gcc's -x handling is complex; running locally" << endl;
                always_local = true;
                local_args.push_back( a );
            } else if (!strcmp(a, "-c")) {
                seen_c = true;
            } else if (str_startswith("-o", a)) {
                assert( ofile.empty() );
                if (!strcmp(a, "-o")) {
                    /* Whatever follows must be the output */
                    ofile = argv[++i];
                } else {
                    a += 2;
                    ofile = a;
                }
            } else
                rest_args.push_back( a );
        } else {
            rest_args.push_back( a );
        }
    }

    /* TODO: ccache has the heuristic of ignoring arguments that are not
     * extant files when looking for the input file; that's possibly
     * worthwile.  Of course we can't do that on the server. */

    if (!seen_c && !seen_s)
        always_local = true;
    else if ( seen_s ) {
        if ( seen_c )
            log_error() << "can't have both -c and -S, ignoring -c" << endl;
        remote_args.push_back( "-S" );
    } else
        remote_args.push_back( "-c" );

    if (ofile == "-" ) {
        /* Different compilers may treat "-o -" as either "write to
         * stdout", or "write to a file called '-'".  We can't know,
         * so we just always run it locally.  Hopefully this is a
         * pretty rare case. */
        log_info() << "output to stdout?  running locally" << endl;
        always_local = true;
    }

    return always_local;
}

