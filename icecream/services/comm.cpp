/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <iostream>
#include <assert.h>

#include "logging.h"
#include "job.h"
#include "comm.h"

using namespace std;

/* TODO
 * buffered in/output per MsgChannel
    + move read* into MsgChannel, create buffer-fill function
    + add timeouting select() there, handle it in the different
    + read* functions.
    + write* unbuffered / or per message buffer (flush in send_msg)
 * think about error handling
    + saving errno somewhere (in MsgChannel class)
 */
bool
readfull (int fd, void *_buf, size_t count)
{
  char *buf = (char*)_buf;
  while (count)
    {
      ssize_t ret = read (fd, buf, count);
      if (ret < 0 && (errno == EINTR || errno == EAGAIN))
	continue;
      // EOF or some error
      if (ret <= 0)
	break;
      count -= ret;
      buf += ret;
    }
  if (count)
    return false;
  return true;
}

bool
writefull (int fd, const void *_buf, size_t count)
{
  const char *buf = (const char*)_buf;
  while (count)
    {
      ssize_t ret = write (fd, buf, count);
      if (ret < 0 && (errno == EINTR || errno == EAGAIN))
	continue;
      // XXX handle EPIPE ?
      // EOF or some error
      if (ret <= 0)
	break;
      count -= ret;
      buf += ret;
    }
  if (count)
    return false;
  return true;
}

bool
readuint (int fd, unsigned int &buf)
{
  unsigned int b;
  buf = 0;
  if (!readfull (fd, &b, 4))
    return false;
  buf = ntohl (b);
  return true;
}

bool
writeuint (int fd, unsigned int i)
{
  i = htonl (i);
  return writefull (fd, &i, 4);
}

#include <minilzo.h>

bool writecompressed( int fd, const unsigned char *in_buf, lzo_uint in_len, lzo_uint &out_len )
{
  out_len = in_len + in_len / 64 + 16 + 3;
  lzo_byte *out_buf = new lzo_byte[out_len];
  lzo_voidp wrkmem = ( lzo_voidp )malloc(LZO1X_MEM_COMPRESS);
  int ret = lzo1x_1_compress( in_buf, in_len, out_buf, &out_len, wrkmem );
  if ( ret != LZO_E_OK)
    {
      /* this should NEVER happen */
      printf("internal error - compression failed: %d\n", ret);
      free( wrkmem );
      delete [] out_buf;
      return false;
    }
#if 0
  printf( "compress %d bytes to %d bytes\n", in_len, out_len );
#endif
  bool bret = ( writeuint( fd, in_len )
                && writeuint( fd, out_len )
                && writefull( fd, out_buf, out_len ) );

  free( wrkmem );
  delete [] out_buf;
  return bret;
}

bool readcompressed( int fd, unsigned char **uncompressed_buf, lzo_uint &uncompressed_len, lzo_uint &compressed_len )
{
  if ( !readuint( fd, uncompressed_len ) )
    return false;
  if ( !readuint( fd, compressed_len ) )
    return false;
  *uncompressed_buf = new unsigned char[uncompressed_len];
  unsigned char *compressed_buf = new unsigned char[compressed_len];
  lzo_voidp wrkmem = ( lzo_voidp )malloc(LZO1X_MEM_COMPRESS);
  bool bret = readfull( fd, compressed_buf, compressed_len );
  int ret = LZO_E_OK;
  if ( bret )
    ret = lzo1x_decompress( compressed_buf, compressed_len, *uncompressed_buf, &uncompressed_len, wrkmem );
  if ( ret !=  LZO_E_OK)
    {
      /* this should NEVER happen */
      printf("internal error - decompression failed: %d\n", ret);
      bret = false;
  }
  if ( !bret )
    {
      delete [] *uncompressed_buf;
      *uncompressed_buf = 0;
      uncompressed_len = 0;
    }
  free( wrkmem );
  delete [] compressed_buf;
  return bret;
}

Service::Service (struct sockaddr *_a, socklen_t _l)
{
  c = 0;
  len = _l;
  if (len && _a)
    {
      addr = (struct sockaddr *)malloc (len);
      memcpy (addr, _a, len);
      name = inet_ntoa (((struct sockaddr_in *) addr)->sin_addr);
      port = ntohs (((struct sockaddr_in *)addr)->sin_port);
    }
  else
    {
      addr = 0;
      name = "";
      port = 0;
    }
}

Service::Service (const string &hostname, unsigned short p)
{
  int remote_fd;
  struct sockaddr_in remote_addr;
  c = 0;
  addr = 0;
  port = 0;
  name = "";
  if ((remote_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      return;
    }
  struct hostent *host = gethostbyname (hostname.c_str());
  if (!host)
    {
      fprintf (stderr, "Unknown host\n");
      close (remote_fd);
      return;
    }
  if (host->h_length != 4)
    {
      fprintf (stderr, "Invalid address length\n");
      close (remote_fd);
      return;
    }
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons (p);
  memcpy (&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
  if (connect (remote_fd, (struct sockaddr *) &remote_addr, sizeof (remote_addr)) < 0)
    {
      perror ("connect()");
      close (remote_fd);
      return;
    }
  len = sizeof (remote_addr);
  addr = (struct sockaddr *)malloc (len);
  memcpy (addr, &remote_addr, len);
  name = hostname;
  port = p;
  new MsgChannel (remote_fd, this); // sets c
}

Service::~Service ()
{
  if (addr)
    free (addr);
  if ( c )
    {
      assert( c->other_end == this );
      c->other_end = 0;
    }
  delete c;
}

bool
Service::eq_ip (const Service &s)
{
  struct sockaddr_in *s1, *s2;
  s1 = (struct sockaddr_in *) addr;
  s2 = (struct sockaddr_in *) s.addr;
  return (len == s.len
          && memcmp (&s1->sin_addr, &s2->sin_addr, sizeof (s1->sin_addr)) == 0);
}

MsgChannel::MsgChannel (int _fd)
  : other_end(0), fd(_fd)
{
    abort(); // currently not tested
}

MsgChannel::MsgChannel (int _fd, Service *serv)
  : other_end(serv), fd(_fd)
{
    assert( !serv->c );
    serv->c = this;
}

MsgChannel::~MsgChannel()
{
  close (fd);
  fd = 0;
  if ( other_end )
    {
      assert( !other_end->c || other_end->c == this );
      other_end->c = 0;
    }
  delete other_end;
}

Msg *
MsgChannel::get_msg(void)
{
  Msg *m;
  enum MsgType type;
  unsigned int t;
  if (!readuint (fd, t))
    return 0;
  type = (enum MsgType) t;
  switch (type)
    {
    case M_UNKNOWN: return 0;
    case M_PING: m = new PingMsg; break;
    case M_END:  m = new EndMsg; break;
    case M_TIMEOUT: m = new TimeoutMsg; break;
    case M_GET_CS: m = new GetCSMsg; break;
    case M_USE_CS: m = new UseCSMsg; break;
    case M_COMPILE_FILE: m = new CompileFileMsg (new CompileJob, true); break;
    case M_FILE_CHUNK: m = new FileChunkMsg; break;
    case M_COMPILE_RESULT: m = new CompileResultMsg; break;
    case M_JOB_BEGIN: m = new JobBeginMsg; break;
    case M_JOB_DONE: m = new JobDoneMsg; break;
    case M_LOGIN: m = new LoginMsg; break;
    case M_STATS: m = new StatsMsg; break;
    case M_GET_SCHEDULER: m = new GetSchedulerMsg; break;
    case M_USE_SCHEDULER: m = new UseSchedulerMsg; break;
    case M_MON_LOGIN: m = new MonLoginMsg; break;
    case M_MON_GET_CS: m = new MonGetCSMsg; break;
    case M_MON_JOB_BEGIN: m = new MonJobBeginMsg; break;
    case M_MON_JOB_DONE: m = new MonJobDoneMsg; break;
    default:
      abort();
      return 0; break;
    }
  if (!m->fill_from_fd (fd))
    {
      delete m;
      return 0;
    }
  return m;
}

bool
MsgChannel::send_msg (const Msg &m)
{
  return m.send_to_fd (fd);
}

MsgChannel *Service::createChannel( int remote_fd )
{
  if ( channel() )
    {
      assert( remote_fd == c->fd );
      return channel();
    }
  new MsgChannel( remote_fd, this ); // sets c
  assert( channel() );
  return channel();
}

#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>

MsgChannel *
connect_scheduler (const char *netname)
{
  const char *get = getenv( "USE_SCHEDULER" );
  string hostname;
  unsigned int sport = 8765;
  char buf2[16];

  if (!netname)
    netname = "ICECREAM";

  if (get)
    {
      hostname = get;
      netname = "";
    }
  else
    {
      int ask_fd;
      struct sockaddr_in remote_addr;
      socklen_t remote_len;
      if ((ask_fd = socket (PF_INET, SOCK_DGRAM, 0)) < 0)
        {
          perror ("socket()");
          return 0;
        }

      int optval = 1;
      if (setsockopt (ask_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0)
        {
          perror ("setsockopt()");
          close (ask_fd);
          return 0;
        }

      struct ifaddrs *addrs;
      int ret = getifaddrs(&addrs);

      if ( ret < 0 )
        return 0;

      char buf = 42;
      for (struct ifaddrs *addr = addrs; addr != NULL; addr = addr->ifa_next)
        {
          /*
           * See if this interface address is IPv4...
           */

          if (addr->ifa_addr == NULL || addr->ifa_addr->sa_family != AF_INET ||
              addr->ifa_netmask == NULL || addr->ifa_name == NULL)
            continue;

          if ( ntohl( ( ( struct sockaddr_in* ) addr->ifa_addr )->sin_addr.s_addr ) == 0x7f000001 )
            {
              trace() << "ignoring localhost " << addr->ifa_name << endl;
              continue;
            }

          if ( ( addr->ifa_flags & IFF_POINTOPOINT ) || !( addr->ifa_flags & IFF_BROADCAST) )
            {
              log_info() << "ignoring tunnels " << addr->ifa_name << endl;
              continue;
            }

          if ( addr->ifa_broadaddr )
            {
              log_info() << "broadcast "
                         << addr->ifa_name << " "
                         << inet_ntoa( ( ( sockaddr_in* )addr->ifa_broadaddr )->sin_addr )
                         << endl;

              remote_addr.sin_family = AF_INET;
              remote_addr.sin_port = htons (8765);
              remote_addr.sin_addr = ( ( sockaddr_in* )addr->ifa_broadaddr )->sin_addr ;

              if (sendto (ask_fd, &buf, 1, 0, (struct sockaddr*)&remote_addr,
                        sizeof (remote_addr)) != 1)
                {
                  perror ("sendto()");
                }
            }
        }
      freeifaddrs (addrs);

      time_t time0 = time (0);
      bool found = false;
      while (time (0) - time0 < 4)
        {
	  fd_set read_set;
	  FD_ZERO (&read_set);
	  FD_SET (ask_fd, &read_set);
	  struct timeval tv;
	  tv.tv_sec = 2;
	  tv.tv_usec = 0;
	  errno = 0;
	  if (select (ask_fd + 1, &read_set, NULL, NULL, &tv) <= 0)
	    {
	      /* Normally this is a timeout, i.e. no scheduler there.  */
	      if (errno)
		perror ("waiting for scheduler");
	      continue;
	    }
	  remote_len = sizeof (remote_addr);
	  if (recvfrom (ask_fd, &buf2, sizeof (buf2), 0, (struct sockaddr*) &remote_addr,
			&remote_len) != sizeof (buf2))
	    {
	      perror ("recvfrom()");
	      continue;
	    }
	  if (buf + 1 != buf2[0])
	    {
	      fprintf (stderr, "wrong answer\n");
	      continue;
	    }
	  buf2[sizeof (buf2) - 1] = 0;
	  if (strcasecmp (netname, buf2 + 1) == 0)
	    {
	      found = true;
	      break;
	    }
	}
      close (ask_fd);
      if (!found)
        return 0;
      hostname = inet_ntoa (remote_addr.sin_addr);
      sport = ntohs (remote_addr.sin_port);
      netname = buf2 + 1;
    }

  printf ("scheduler is on %s:%d (net %s)\n", hostname.c_str(), sport,
	  netname);
  Service *sched = new Service (hostname, sport);
  return sched->channel();
}

bool
read_string (int fd, string &s)
{
  char *buf;
  // len is including the (also saved) 0 Byte
  unsigned int len;
  if (!readuint (fd, len))
    return false;
  buf = new char[len];
  if (!readfull (fd, buf, len))
    {
      s = "";
      delete [] buf;
      return false;
    }
  s = buf;
  delete [] buf;
  return true;
}

bool
write_string (int fd, const string &s)
{
  unsigned int len = 1 + s.length();
  if (!writeuint (fd, len))
    return false;
  return writefull (fd, s.c_str(), len);
}

bool
read_strlist (int fd, list<string> &l)
{
  unsigned int len;
  l.clear();
  if (!readuint (fd, len))
    return false;
  while (len--)
    {
      string s;
      if (!read_string (fd, s))
        return false;
      l.push_back (s);
    }
  return true;
}

bool
write_strlist (int fd, const list<string> &l)
{
  if (!writeuint (fd, (unsigned int) l.size()))
    return false;
  for (list<string>::const_iterator it = l.begin();
       it != l.end(); ++it )
    {
      if (!write_string (fd, *it))
        return false;
    }
  return true;
}

bool
Msg::fill_from_fd (int fd)
{
  assert( fd != 0 );
  return true;
}

bool
Msg::send_to_fd (int fd) const
{
  assert( fd != 0 );
  return writeuint (fd, (unsigned int) type);
}

bool
GetCSMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  unsigned int _lang;
  if (!read_string (fd, version)
      || !read_string (fd, filename)
      || !readuint (fd, _lang))
    return false;
  lang = static_cast<CompileJob::Language>( _lang );
  return true;
}

bool
GetCSMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  unsigned int _lang = lang;
  if (!write_string (fd, version)
      || !write_string (fd, filename)
      || !writeuint (fd, _lang))
    return false;
  return true;
}

bool
UseCSMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  bool ret = (readuint (fd, job_id)
              && readuint (fd, port)
              && read_string (fd, hostname)
              && read_string( fd, environment ) );
  return ret;
}

bool
UseCSMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return (writeuint (fd, job_id)
  	  && writeuint (fd, port)
          && write_string (fd, hostname)
          && write_string( fd, environment ) );
}

bool
CompileFileMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  unsigned int id, lang;
  list<string> l1, l2;
  string version;
  if (!readuint (fd, lang)
      || !readuint (fd, id)
      || !read_strlist (fd, l1)
      || !read_strlist (fd, l2)
      || !read_string( fd, version ) )
    return false;
  job->setLanguage ((CompileJob::Language) lang);
  job->setJobID (id);
  job->setRemoteFlags (l1);
  job->setRestFlags (l2);
  job->setEnvironmentVersion( version );
  return true;
}

bool
CompileFileMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return (writeuint (fd, (unsigned int) job->language())
  	  && writeuint (fd, job->jobID())
  	  && write_strlist (fd, job->remoteFlags())
          && write_strlist (fd, job->restFlags())
          && write_string( fd, job->environmentVersion() ) );
}

CompileJob *CompileFileMsg::takeJob() {
    assert( deleteit );
    deleteit = false;
    return job;
}

bool
FileChunkMsg::fill_from_fd (int fd)
{
  if (del_buf)
    delete [] buffer;
  buffer = 0;
  del_buf = true;

  if (!Msg::fill_from_fd (fd))
    return false;
  lzo_uint _len = 0;
  lzo_uint _compressed = 0;
  if ( !readcompressed( fd, &buffer, _len, _compressed ) )
      return false;

  len = _len;
  compressed = _compressed;

  return true;
}

bool
FileChunkMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  lzo_uint _compressed;
  bool ret = writecompressed( fd, buffer, len, _compressed );
  compressed = _compressed;
  return ret;
}

FileChunkMsg::~FileChunkMsg()
{
  if (del_buf)
    delete [] buffer;
}

bool
CompileResultMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  unsigned int _status = 0;
  if ( !read_string( fd, err )
       || !read_string( fd, out )
       || !readuint( fd, _status ) )
      return false;
  status = _status;
  return true;
}

bool
CompileResultMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return ( write_string( fd, err )
           && write_string( fd, out )
           && writeuint( fd, status ) );
}

bool
JobBeginMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return readuint (fd, job_id)
  	 && readuint (fd, stime);
}

bool
JobBeginMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint (fd, job_id)
  	 && writeuint (fd, stime);
}

JobDoneMsg::JobDoneMsg (int id, int exit)
  : Msg(M_JOB_DONE),  exitcode( exit ), job_id( id )
{
  real_msec = 0;
  user_msec = 0;
  sys_msec = 0;
  maxrss = 0;
  idrss = 0;
  majflt = 0;
  nswap = 0;
  exitcode = 0;
  in_compressed = 0;
  in_uncompressed = 0;
  out_compressed = 0;
  out_uncompressed = 0;
}

bool
JobDoneMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  unsigned int _exitcode = 255;
  bool ret = readuint (fd, job_id)
    && readuint( fd, _exitcode )
    && readuint( fd, real_msec )
    && readuint( fd, user_msec )
    && readuint( fd, sys_msec )
    && readuint( fd, maxrss )
    && readuint( fd, idrss )
    && readuint( fd, majflt )
    && readuint( fd, nswap )
    && readuint( fd, in_compressed )
    && readuint( fd, in_uncompressed )
    && readuint( fd, out_compressed )
    && readuint( fd, out_uncompressed );
  exitcode = ( int )_exitcode;
  return ret;
}

bool
JobDoneMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint (fd, job_id)
    && writeuint( fd, ( unsigned int )exitcode )
    && writeuint( fd, real_msec )
    && writeuint( fd, user_msec )
    && writeuint( fd, sys_msec )
    && writeuint( fd, maxrss )
    && writeuint( fd, idrss )
    && writeuint( fd, majflt )
    && writeuint( fd, nswap )
    && writeuint( fd, in_compressed )
    && writeuint( fd, in_uncompressed )
    && writeuint( fd, out_compressed )
    && writeuint( fd, out_uncompressed );
}

bool
LoginMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return ( readuint( fd, port )
           && readuint( fd, max_kids )
           && read_strlist( fd, envs ) );
}

bool
LoginMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return ( writeuint( fd, port )
           && writeuint( fd, max_kids )
           && write_strlist( fd, envs ) );
}

bool
StatsMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;

  return readuint( fd, load );
}

bool
StatsMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint( fd, load );
}

bool
GetSchedulerMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
GetSchedulerMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}

bool
UseSchedulerMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  bool ret = ( readuint (fd, port)
               && read_string (fd, hostname));
  return ret;
}

bool
UseSchedulerMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return ( writeuint (fd, port)
           && write_string (fd, hostname));
}

bool
MonGetCSMsg::fill_from_fd (int fd)
{
  if (!GetCSMsg::fill_from_fd (fd))
    return false;
  return readuint( fd, job_id )
    && read_string( fd, client );
}

bool
MonGetCSMsg::send_to_fd (int fd) const
{
  if (!GetCSMsg::send_to_fd (fd))
    return false;
  return writeuint( fd, job_id )
    && write_string( fd, client );
}

bool
MonJobBeginMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return readuint( fd, job_id )
    && readuint( fd, stime )
    && read_string( fd, host );
}

bool
MonJobBeginMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint( fd, job_id )
    && writeuint( fd, stime )
    && write_string( fd, host );
}
