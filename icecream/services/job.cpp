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

#include "job.h"
#include "logging.h"
#include "exitcode.h"
#include <sys/utsname.h>
#include <stdio.h>

using namespace std;

list<string> CompileJob::flags( Argument_Type argumentType ) const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        if ( it->second == argumentType )
            args.push_back( it->first );
    return args;
}

list<string> CompileJob::localFlags() const
{
    return flags( Arg_Local );
}

list<string> CompileJob::remoteFlags() const
{
    return flags( Arg_Remote );
}

list<string> CompileJob::restFlags() const
{
    return flags( Arg_Rest );
}

list<string> CompileJob::allFlags() const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        args.push_back( it->first );
    return args;
}

void CompileJob::__setTargetPlatform()
{
    struct utsname buf;
    if ( uname(&buf) ) {
        perror( "uname failed" );
        return;
    } else
        m_target_platform = buf.machine;
}
