/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include <iostream>
#include "logging.h"
#include <fstream>

using namespace std;

int debug_level = 0;
ostream *logfile_trace = 0;
ostream *logfile_info = 0;
ostream *logfile_warning = 0;
ostream *logfile_error = 0;
ofstream logfile_null( "/dev/null" );
ofstream logfile_file;

void setup_debug(int level, const string &filename)
{
    debug_level = level;

    ostream *output = 0;
    if ( filename.length() ) {
        logfile_file.open( filename.c_str() );
        output = &logfile_file;
    } else
        output = &cerr;

    if ( debug_level & Debug )
        logfile_trace = output;
    else
        logfile_trace = &logfile_null;

    if ( debug_level & Info )
        logfile_info = output;
    else
        logfile_info = &logfile_null;

    if ( debug_level & Warning )
        logfile_warning = output;
    else
        logfile_warning = &logfile_null;

    if ( debug_level & Error )
        logfile_error = output;
    else
        logfile_error = &logfile_null;
}
