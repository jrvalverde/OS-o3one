//+++2004-08-31
//    Copyright (C) 2004  Mike Rieker, Beverly, MA USA
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//---2004-08-31

/************************************************************************/
/*									*/
/*  Unix-style network I/O routines					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_fd.h"
#include "oz_io_ip.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_event.h"
#include "oz_sys_io.h"
#include "oz_sys_logname.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef struct { OZ_Handle iochan;		// I/O channel
                 uLong status;			// I/O status
                 struct sockaddr_in cliaddr;	// client's address
               } Lisctx;

typedef struct { int socktype;			// from socket call's type parameter
                 struct sockaddr_in bindaddr;	// address supplied by 'bind' call
                 int backlog;			// backlog from 'listen' call
                 int nextlisten;		// next element in lisctxs to check for completed
                 Lisctx *lisctxs;		// array (of backlog elements)
               } Ctx;

static ssize_t netio_read (int fd, __ptr_t buf, size_t nbytes);
static ssize_t netio_write (int fd, const __ptr_t buf, size_t nbytes);
static int netio_vfcntl (int fd, int cmd, va_list ap);
static int netio_close (int fd);

static const OZ_Crtl_fd_driver netio_driver = { NULL, NULL, netio_read, netio_write, NULL, netio_vfcntl, netio_close };

static void startlisten (int fd, Lisctx *lisctx);

int socket (int domain, int type, int protocol)

{
  Ctx *ctx;
  int fd;
  uLong sts;

  /* We only do internet TCP and UDP sockets for now */

  if (domain != PF_INET) {
    errno = EPROTONOSUPPORT;
    return (-1);
  }

  if ((type != SOCK_STREAM) && (type != SOCK_DGRAM)) {
    errno = EPROTONOSUPPORT;
    return (-1);
  }

  /* Allocate an 'fd' */

  fd = oz_crtl_fd_alloc ();
  if (fd < 0) return (fd);

  /* Assign channel to 'ip' device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(oz_crtl_fd_array[fd].h_iochan), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_crtl_fd_free (fd);
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  /* Create an event flag to use for IO on the socket */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, OZ_IO_IP_DEV, &(oz_crtl_fd_array[fd].h_event));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Malloc a Ctx struct and fill it in */

  ctx = malloc (sizeof *ctx);
  memset (ctx, 0, sizeof *ctx);
  oz_crtl_fd_array[fd].driver = &netio_driver;
  oz_crtl_fd_array[fd].flags1 = type;
  oz_crtl_fd_array[fd].point1 = ctx;

  return (fd);
}

int connect (int sockfd, const struct sockaddr *serv_addr, socklen_t addrlen)

{
  uLong sts;

  /* Make sure we were given an internet address */

  if (serv_addr -> sa_family != AF_INET) {
    errno = EAFNOSUPPORT;
    return (-1);
  }

  /* Make sure the sockfd given is valid and that it was created by 'socket' call (not an 'open' call) */

  if (!oz_crtl_fd_check (sockfd)) return (-1);			// make sure sockfd is in range and allocated
  if (oz_crtl_fd_array[sockfd].driver != &netio_driver) {	// make sure it was created with 'socket' call
    errno = ENOTSOCK;
    return (-1);
  }

  /* Perform connection */

  switch (oz_crtl_fd_array[sockfd].flags1) {

    /* TCP connection */

    case SOCK_STREAM: {
      OZ_IO_ip_tcpconnect ip_tcpconnect;

      memset (&ip_tcpconnect, 0, sizeof ip_tcpconnect);
      ip_tcpconnect.addrsize  = sizeof ((struct sockaddr_in *)serv_addr) -> sin_addr;
      ip_tcpconnect.portsize  = sizeof ((struct sockaddr_in *)serv_addr) -> sin_port;
      ip_tcpconnect.dstipaddr = (uByte *)&(((struct sockaddr_in *)serv_addr) -> sin_addr);
      ip_tcpconnect.dstportno = (uByte *)&(((struct sockaddr_in *)serv_addr) -> sin_port);
      ip_tcpconnect.timeout   = 30000;	// timeout in 30 seconds

      sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[sockfd].h_iochan, oz_crtl_fd_array[sockfd].h_event, 
                       OZ_IO_IP_TCPCONNECT, sizeof ip_tcpconnect, &ip_tcpconnect);

      switch (sts) {
        case         OZ_SUCCESS: oz_crtl_fd_array[sockfd].flags2 = -1; return (0);
        case    OZ_CHANALRBOUND: errno = EISCONN;      break;
        case  OZ_CONNECTREFUSED: errno = ECONNREFUSED; break;
        case     OZ_CONNECTFAIL: errno = ETIMEDOUT;    break;
        case   OZ_NOROUTETODEST: errno = ENETUNREACH;  break;
        case OZ_NOMOREPHEMPORTS: errno = EAGAIN;       break;
        case     OZ_SOCKNOINUSE: errno = EADDRINUSE;   break;
        default: errno = EOZSYSERR; errno_ozsts = sts; break;
      }
      return (-1);
    }

    /* We don't do any other type of connection */

    //case SOCK_DGRAM: {
    //}

    default: {
      errno = EPROTONOSUPPORT;
      return (-1);
    }
  }
}

int bind (int sockfd, const struct sockaddr *my_addr, socklen_t addrlen)

{
  Ctx *ctx;

  /* Make sure we were given an internet address */

  if (my_addr -> sa_family != AF_INET) {
    errno = EAFNOSUPPORT;
    return (-1);
  }

  /* Make sure the sockfd given is valid and that it was created by 'socket' call (not an 'open' call) */

  if (!oz_crtl_fd_check (sockfd)) return (-1);			// make sure sockfd is in range and allocated
  if (oz_crtl_fd_array[sockfd].driver != &netio_driver) {	// make sure it was created with 'socket' call
    errno = ENOTSOCK;
    return (-1);
  }

  /* Perform bind */

  switch (oz_crtl_fd_array[sockfd].flags1) {

    /* TCP connection */

    case SOCK_STREAM: {
      ctx = oz_crtl_fd_array[sockfd].point1;
      movc4 (addrlen, my_addr, sizeof ctx -> bindaddr, &(ctx -> bindaddr));
      return (0);
    }

    /* We don't do any other type of connection */

    //case SOCK_DGRAM: {
    //}

    default: {
      errno = EPROTONOSUPPORT;
      return (-1);
    }
  }
}

int listen (int sockfd, int backlog)

{
  char name[32];
  Ctx *ctx;
  int i;
  Lisctx *lisctxs;
  uLong sts;

  /* Make sure the sockfd given is valid and that it was created by 'socket' call (not an 'open' call) */

  if (!oz_crtl_fd_check (sockfd)) return (-1);			// make sure sockfd is in range and allocated
  if (oz_crtl_fd_array[sockfd].driver != &netio_driver) {	// make sure it was created with 'socket' call
    errno = ENOTSOCK;
    return (-1);
  }
  ctx = oz_crtl_fd_array[sockfd].point1;

  /* Make sure we are bound to something */

  if (oz_crtl_fd_array[sockfd].point1 == NULL) {
    errno = EOPNOTSUPP;
    return (-1);
  }

  /* Make event flag to listen with */

  oz_sys_sprintf (sizeof name, name, "listen %u.%u.%u.%u.%u", 
						(ctx -> bindaddr.sin_addr.s_addr      ) & 0xFF, 
						(ctx -> bindaddr.sin_addr.s_addr >>  8) & 0xFF, 
						(ctx -> bindaddr.sin_addr.s_addr >> 16) & 0xFF, 
						(ctx -> bindaddr.sin_addr.s_addr >> 24) & 0xFF, 
						ntohs (ctx -> bindaddr.sin_port));
  sts = oz_sys_event_create (OZ_PROCMODE_KNL, name, &(oz_crtl_fd_array[sockfd].h_event));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Set up backlog array */

  for (i = 0; i < ctx -> backlog; i ++) {
    oz_sys_handle_release (OZ_PROCMODE_KNL, ctx -> lisctxs[i].iochan);
  }

  lisctxs = realloc (ctx -> lisctxs, backlog * sizeof *(ctx -> lisctxs));
  memset (lisctxs, 0, backlog * sizeof *(ctx -> lisctxs));
  ctx -> backlog = backlog;
  ctx -> lisctxs = lisctxs;

  /* Start listening by posting listen I/O for each lisctxs element */

  for (i = 0; i < backlog; i ++) startlisten (sockfd, lisctxs + i);

  ctx -> nextlisten = 0;

  return (0);
}

int accept (int sockfd, struct sockaddr *addr, socklen_t *addrlen)

{
  char name[32];
  Ctx *ctx;
  int fd, i;
  Lisctx *lisctx;
  uLong sts;

  /* Make sure the sockfd given is valid and that it was created by 'socket' call (not an 'open' call) */

  if (!oz_crtl_fd_check (sockfd)) return (-1);			// make sure sockfd is in range and allocated
  if (oz_crtl_fd_array[sockfd].driver != &netio_driver) {	// make sure it was created with 'socket' call
    errno = ENOTSOCK;
    return (-1);
  }

  /* Scan for a completed listen */

  while (1) {
    for (i = 0; i < ctx -> backlog; i ++) {
      lisctx = ctx -> lisctxs + ((i + ctx -> nextlisten) % ctx -> backlog);
      sts = lisctx -> status;
      switch (sts) {
        case OZ_PENDING: break;
        case OZ_SUCCESS: {
          fd = oz_crtl_fd_alloc ();
          if (fd >= 0) {
            movc4 (sizeof lisctx -> cliaddr, &(lisctx -> cliaddr), *addrlen, addr);
            *addrlen = sizeof lisctx -> cliaddr;
            oz_sys_sprintf (sizeof name, name, "client %u.%u.%u.%u.%u", 
						(lisctx -> cliaddr.sin_addr.s_addr      ) & 0xFF, 
						(lisctx -> cliaddr.sin_addr.s_addr >>  8) & 0xFF, 
						(lisctx -> cliaddr.sin_addr.s_addr >> 16) & 0xFF, 
						(lisctx -> cliaddr.sin_addr.s_addr >> 24) & 0xFF, 
						ntohs (lisctx -> cliaddr.sin_port));
            sts = oz_sys_event_create (OZ_PROCMODE_KNL, name, &(oz_crtl_fd_array[fd].h_event));
            if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
            oz_crtl_fd_array[fd].h_iochan = lisctx -> iochan;
            ctx -> nextlisten = (++ i + ctx -> nextlisten) % ctx -> backlog;
          }
          startlisten (fd, lisctx);
          return (fd);
        }
        default: {
          errno = EOZSYSERR;
          errno_ozsts = sts;
          startlisten (fd, lisctx);
          return (-1);
        }
      }
    }

    /* No listening has completed, wait for something */

    oz_sys_event_wait (OZ_PROCMODE_KNL, oz_crtl_fd_array[sockfd].h_event, 0);		// wait for a connect
    oz_sys_event_set (OZ_PROCMODE_KNL, oz_crtl_fd_array[sockfd].h_event, 0, NULL);	// clear flag in case of wait again
  }
}

static void startlisten (int fd, Lisctx *lisctx)

{
  Ctx *ctx;
  OZ_IO_ip_tcplisten ip_tcplisten;
  uLong sts;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(lisctx -> iochan), OZ_IO_IP_DEV, OZ_LOCKMODE_CW); // assign an I/O channel
  if (sts == OZ_SUCCESS) {
#if OZ_PENDING != 0
    lisctx -> status = OZ_PENDING;					// haven't been connected to yet
#endif

    ctx = oz_crtl_fd_array[fd].point1;

    memset (&ip_tcplisten, 0, sizeof ip_tcplisten);
    ip_tcplisten.addrsize  = sizeof ctx -> bindaddr.sin_addr;		// size we think an ip address is
    ip_tcplisten.portsize  = sizeof ctx -> bindaddr.sin_port;		// size we think n port number is
    ip_tcplisten.lclipaddr = (void *)&(ctx -> bindaddr.sin_addr);	// this is local end (server) ip address
    ip_tcplisten.lclportno = (void *)&(ctx -> bindaddr.sin_port);	// this is local end (server) port number
    ip_tcplisten.srcipaddr = (void *)&(lisctx -> cliaddr.sin_addr);	// tell it where to put client's info
    ip_tcplisten.srcportno = (void *)&(lisctx -> cliaddr.sin_port);

    sts = oz_sys_io_start (OZ_PROCMODE_KNL, lisctx -> iochan, &(lisctx -> status), // start listening
                           oz_crtl_fd_array[fd].h_event, NULL, NULL, 
                           OZ_IO_IP_TCPLISTEN, sizeof ip_tcplisten, &ip_tcplisten);
  }
  if (sts != OZ_STARTED) lisctx -> status = sts;
}

/* Get local address */

int getsockname (int sockfd, struct sockaddr *name, socklen_t *namelen)

{
  uLong sts;

  /* Make sure the sockfd given is valid and that it was created by 'socket' call (not an 'open' call) */

  if (!oz_crtl_fd_check (sockfd)) return (-1);			// make sure sockfd is in range and allocated
  if (oz_crtl_fd_array[sockfd].driver != &netio_driver) {	// make sure it was created with 'socket' call
    errno = ENOTSOCK;
    return (-1);
  }

  /* Retrieve local address */

  switch (oz_crtl_fd_array[sockfd].flags1) {

    /* TCP connection */

    case SOCK_STREAM: {
      if (oz_crtl_fd_array[sockfd].h_iochan != 0) {
        OZ_IO_ip_tcpgetinfo1 ip_tcpgetinfo1;
        struct sockaddr_in myaddr;

        memset (&ip_tcpgetinfo1, 0, sizeof ip_tcpgetinfo1);
        memset (&myaddr, 0, sizeof myaddr);
        ip_tcpgetinfo1.addrsize  = sizeof myaddr.sin_addr;
        ip_tcpgetinfo1.portsize  = sizeof myaddr.sin_port;
        ip_tcpgetinfo1.lclipaddr = (void *)&myaddr.sin_addr;
        ip_tcpgetinfo1.lclportno = (void *)&myaddr.sin_port;
        sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[sockfd].h_iochan, oz_crtl_fd_array[sockfd].h_event, 
                         OZ_IO_IP_TCPGETINFO1, sizeof ip_tcpgetinfo1, &ip_tcpgetinfo1);
        if (sts != OZ_SUCCESS) goto errrtn;
        movc4 (sizeof myaddr, &myaddr, *namelen, name);
        *namelen = sizeof myaddr;
      } else {
        Ctx *ctx;

        ctx = oz_crtl_fd_array[sockfd].point1;
        movc4 (sizeof ctx -> bindaddr, &(ctx -> bindaddr), *namelen, name);
        *namelen = sizeof ctx -> bindaddr;
      }
      return (0);
    }

    /* We don't do any other type of connection */

    //case SOCK_DGRAM: {
    //}

    default: {
      errno = EPROTONOSUPPORT;
      return (-1);
    }
  }

errrtn:
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

/* Get remote address */

int getpeername (int sockfd, struct sockaddr *name, socklen_t *namelen)

{
  uLong sts;

  /* Make sure the sockfd given is valid and that it was created by 'socket' call (not an 'open' call) */

  if (!oz_crtl_fd_check (sockfd)) return (-1);			// make sure sockfd is in range and allocated
  if (oz_crtl_fd_array[sockfd].driver != &netio_driver) {	// make sure it was created with 'socket' call
    errno = ENOTSOCK;
    return (-1);
  }

  /* Retrieve remote address */

  switch (oz_crtl_fd_array[sockfd].flags1) {

    /* TCP connection */

    case SOCK_STREAM: {
      if (oz_crtl_fd_array[sockfd].h_iochan != 0) {
        OZ_IO_ip_tcpgetinfo1 ip_tcpgetinfo1;
        struct sockaddr_in itsaddr;

        memset (&ip_tcpgetinfo1, 0, sizeof ip_tcpgetinfo1);
        memset (&itsaddr, 0, sizeof itsaddr);
        ip_tcpgetinfo1.addrsize  = sizeof itsaddr.sin_addr;
        ip_tcpgetinfo1.portsize  = sizeof itsaddr.sin_port;
        ip_tcpgetinfo1.remipaddr = (void *)&itsaddr.sin_addr;
        ip_tcpgetinfo1.remportno = (void *)&itsaddr.sin_port;
        sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[sockfd].h_iochan, oz_crtl_fd_array[sockfd].h_event, 
                         OZ_IO_IP_TCPGETINFO1, sizeof ip_tcpgetinfo1, &ip_tcpgetinfo1);
        if (sts != OZ_SUCCESS) goto errrtn;
        movc4 (sizeof itsaddr, &itsaddr, *namelen, name);
        *namelen = sizeof itsaddr;
      } else {
        memset (name, 0, *namelen);
        *namelen = sizeof (struct sockaddr_in);
      }
      return (0);
    }

    /* We don't do any other type of connection */

    //case SOCK_DGRAM: {
    //}

    default: {
      errno = EPROTONOSUPPORT;
      return (-1);
    }
  }

errrtn:
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

int recvmsg (int s, struct msghdr *msg, int flags)

{
  if (flags != 0) {
    errno = EINVAL;
    return (-1);
  }
  if (msg -> msg_iovlen != 1) {
    errno = EINVAL;
    return (-1);
  }

  return (recv (s, msg -> msg_iov[0].iov_base, msg -> msg_iov[0].iov_len, flags));
}

int recv (int s, void *buf, size_t len, int flags)

{
  if (!oz_crtl_fd_check (s)) return (-1);
  if (oz_crtl_fd_array[s].driver != &netio_driver) {
    errno = ENOTSOCK;
    return (-1);
  }
  return (netio_read (s, buf, len));
}

static ssize_t netio_read (int fd, __ptr_t buf, size_t nbytes)

{
  uLong sts;

  switch (oz_crtl_fd_array[fd].flags1) {
    case SOCK_STREAM: {
      OZ_IO_ip_tcpreceive ip_tcpreceive;
      uLong rcvlen;

      /* Maybe a 'shutdown' call has been made to block reads */

      if ((oz_crtl_fd_array[fd].flags2 == SHUT_RD) || (oz_crtl_fd_array[fd].flags2 == SHUT_RDWR)) {
        errno = EPIPE;
        return (-1);
      }

      /* Ok, receive a buffer */

      memset (&ip_tcpreceive, 0, sizeof ip_tcpreceive);
      ip_tcpreceive.rawsize = nbytes;
      ip_tcpreceive.rawbuff = buf;
      ip_tcpreceive.rawrlen = &rcvlen;
      ip_tcpreceive.immed   = ((oz_crtl_fd_array[fd].flags3 & O_NONBLOCK) != 0);

      sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                       OZ_IO_IP_TCPRECEIVE, sizeof ip_tcpreceive, &ip_tcpreceive);
      switch (sts) {
        case         OZ_SUCCESS: if (rcvlen > 0) return (rcvlen); errno = EWOULDBLOCK; break;
        case       OZ_ENDOFFILE: return (0);
        case     OZ_CHANOTBOUND: errno = ENOTCONN;     break;
        case          OZ_ACCVIO: errno = EFAULT;       break;
        case   OZ_NOROUTETODEST: errno = ENETUNREACH;  break;
        case     OZ_LINKDROPPED:
        case     OZ_LINKABORTED: 
        case         OZ_ABORTED: errno = ECONNRESET;   break;
        default: errno = EOZSYSERR; errno_ozsts = sts; break;
      }
      return (-1);
    }

    //case SOCK_DGRAM: {
    //}

    default: {
      errno = EPROTONOSUPPORT;
      return (-1);
    }
  }
}

ssize_t sendmsg (int s, const struct msghdr *msg, int flags)

{
  if (flags != 0) {
    errno = EINVAL;
    return (-1);
  }
  if (msg -> msg_iovlen != 1) {
    errno = EINVAL;
    return (-1);
  }

  return (send (s, msg -> msg_iov[0].iov_base, msg -> msg_iov[0].iov_len, flags));
}

int send (int s, const void *buf, size_t len, int flags)

{
  if (!oz_crtl_fd_check (s)) return (-1);
  if (oz_crtl_fd_array[s].driver != &netio_driver) {
    errno = ENOTSOCK;
    return (-1);
  }
  return (netio_write (s, buf, len));
}

static ssize_t netio_write (int fd, const __ptr_t buf, size_t nbytes)

{
  uLong sts;

  switch (oz_crtl_fd_array[fd].flags1) {
    case SOCK_STREAM: {
      OZ_IO_ip_tcptransmit ip_tcptransmit;

      /* Maybe a 'shutdown' call has been made to block sends */

      if ((oz_crtl_fd_array[fd].flags2 == SHUT_WR) || (oz_crtl_fd_array[fd].flags2 == SHUT_RDWR)) {
        errno = EPIPE;
        return (-1);
      }

      /* Ok, transmit the buffer */

      memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);
      ip_tcptransmit.rawsize = nbytes;
      ip_tcptransmit.rawbuff = buf;

      sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                       OZ_IO_IP_TCPTRANSMIT, sizeof ip_tcptransmit, &ip_tcptransmit);
      switch (sts) {
        case   OZ_SUCCESS: return (nbytes);
        case OZ_ENDOFFILE: return (0);
        case     OZ_CHANOTBOUND: errno = ENOTCONN;     break;
        case          OZ_ACCVIO: errno = EFAULT;       break;
        case   OZ_NOROUTETODEST: errno = ENETUNREACH;  break;
        case     OZ_LINKDROPPED:
        case     OZ_LINKABORTED: 
        case         OZ_ABORTED: errno = ECONNRESET;   break;
        default: errno = EOZSYSERR; errno_ozsts = sts; break;
      }
      return (-1);
    }

    //case SOCK_DGRAM: {
    //}

    default: {
      errno = EPROTONOSUPPORT;
      return (-1);
    }
  }
}

static int netio_vfcntl (int fd, int cmd, va_list ap)

{
  switch (cmd) {
    case F_SETFL: {
      oz_crtl_fd_array[fd].flags3 = va_arg (ap, int);
      return (0);
    }
    case F_GETFL: {
      *(va_arg (ap, int *)) = oz_crtl_fd_array[fd].flags3;
      return (0);
    }
  }
  oz_sys_io_fs_printerror ("oz_crtl_netio vfcntl*: (%d, %d, )\n", fd, cmd);	// ??
  return (0);
}

static int netio_close (int fd)

{
  Ctx *ctx;
  int i;
  uLong sts;

  ctx = oz_crtl_fd_array[fd].point1;
  if (ctx != NULL) {
    oz_crtl_fd_array[fd].point1 = NULL;
    if (ctx -> lisctxs != NULL) {
      for (i = 0; i < ctx -> backlog; i ++) {
        oz_sys_handle_release (OZ_PROCMODE_KNL, ctx -> lisctxs[i].iochan);
        ctx -> lisctxs[i].iochan = 0;
      }
      free (ctx -> lisctxs);
    }
    free (ctx);
  }

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, OZ_IO_FS_CLOSE, 0, NULL);
  if (!oz_crtl_fd_free (fd)) return (-1);
  if (sts == OZ_SUCCESS) return (0);
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

int shutdown (int s, int how)

{
  /* Make sure we have a network socket */

  if (!oz_crtl_fd_check (s)) return (-1);
  if (oz_crtl_fd_array[s].driver != &netio_driver) {
    errno = ENOTSOCK;
    return (-1);
  }

  /* Combine new state with the previous state saved in flags2 */

  switch (oz_crtl_fd_array[s].flags2) {
    case -1: {
      switch (how) {
        case SHUT_WR:
        case SHUT_RDWR: {
          oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[s].h_iochan, oz_crtl_fd_array[s].h_event, OZ_IO_IP_TCPCLOSE, 0, NULL);
        }
        case SHUT_RD: {
          oz_crtl_fd_array[s].flags2 = how;
          return (0);
        }
      }
    }
    case SHUT_RD: {
      switch (how) {
        case SHUT_WR:
        case SHUT_RDWR: {
          oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[s].h_iochan, oz_crtl_fd_array[s].h_event, OZ_IO_IP_TCPCLOSE, 0, NULL);
          oz_crtl_fd_array[s].flags2 = SHUT_RDWR;
        }
        case SHUT_RD: {
          return (0);
        }
      }
    }
    case SHUT_WR: {
      switch (how) {
        case SHUT_RD:
        case SHUT_RDWR: {
          oz_crtl_fd_array[s].flags2 = SHUT_RDWR;
        }
        case SHUT_WR: {
          return (0);
        }
      }
    }
    case SHUT_RDWR: {
      switch (how) {
        case SHUT_RD:
        case SHUT_WR:
        case SHUT_RDWR: {
          return (0);
        }
      }
    }
  }

  errno = EINVAL;
  return (-1);
}

struct protoent *getprotobyname (const char *name)

{
  static char *aliases[2];
  static struct protoent protoent;

  memset (&protoent, 0, sizeof protoent);
  if (strcmp (name, "ip") == 0) {
    protoent.p_name = "ip";
    protoent.p_aliases = aliases;
    aliases[0] = "IP";
    aliases[1] = NULL;
    protoent.p_proto = 0;
    return (&protoent);
  }

  return (NULL);
}

char *inet_ntoa (struct in_addr in)

{
  char *p;
  int i;

  static char buf[4+4*sizeof in];

  p = buf;
  for (i = 0; i < sizeof in; i ++) {
    sprintf (p, "%u.", ((uByte *)&in)[i]);
    p += strlen (p);
  }
  *(-- p) = 0;

  return (buf);
}

struct servent *getservbyname (const char *name, const char *proto)

{
  char tmpbuf[256];
  int proto_l;
  OZ_Handle h_logname, h_lognamtbl;
  uLong index, nvalues, sts;

  static char officialname[256], officialproto[256];
  static char *aliases[2];
  static struct servent se;

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_KNL, "OZ_IP_SERVICES", NULL, NULL, NULL, &h_lognamtbl);
  if (sts != OZ_SUCCESS) return (NULL);

  sts = oz_sys_logname_lookup (h_lognamtbl, OZ_PROCMODE_KNL, name, NULL, NULL, &nvalues, &h_logname);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_lognamtbl);
  if (sts != OZ_SUCCESS) return (NULL);

  proto_l = strlen (proto);
  for (index = 0; index < nvalues; index ++) {
    sts = oz_sys_logname_getval (h_logname, index, NULL, sizeof tmpbuf, tmpbuf, NULL, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) break;
    if (tmpbuf[proto_l] != ':') continue;
    if (strncasecmp (proto, tmpbuf, proto_l) != 0) continue;
    memset (&se, 0, sizeof se);
    strncpyz (officialname, name, sizeof officialname);
    strncpyz (officialproto, proto, sizeof officialname);
    aliases[0] = officialname;
    aliases[1] = NULL;
    se.s_name = officialname;
    se.s_aliases = aliases;
    se.s_port = atoi (tmpbuf + proto_l + 1);
    se.s_proto = officialproto;
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
    return (&se);
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  return (NULL);
}

int getaddrinfo (const char *node, 
                 const char *service,
                 const struct addrinfo *hints,
                 struct addrinfo **res)

{
  char *colon, tmpbuf[256];
  int usedup;
  OZ_Handle h_logname, h_lognamtbl;
  struct addrinfo *ai;
  struct sockaddr_in *sa;
  uByte ipaddr[OZ_IO_IP_ADDRSIZE];
  uLong index, nvalues, sts;
  uWord port;

  if ((node == NULL) && (service == NULL)) return (EAI_NONAME);

  /* Look up given host name */

  memset (ipaddr, 0, sizeof ipaddr);
  if (node != NULL) {
    sts = oz_sys_gethostipaddr (node, sizeof ipaddr, ipaddr);
    if (sts == OZ_DNSNOSUCHNAME) return (EAI_NONAME);
    if (sts != OZ_SUCCESS) {
      errno = EOZSYSERR;
      errno_ozsts = sts;
      return (EAI_SYSTEM);
    }
  }

  /* Look up given service name */

  nvalues = 0;
  port    = 0;
  if (service != NULL) {
    port = oz_hw_atoi (service, &usedup);
    if (service[usedup] != 0) {
      port = 0;
      sts = oz_sys_logname_lookup (0, OZ_PROCMODE_KNL, "OZ_IP_SERVICES", NULL, NULL, NULL, &h_lognamtbl);
      if (sts != OZ_SUCCESS) {
        errno = EOZSYSERR;
        errno_ozsts = sts;
        return (EAI_SYSTEM);
      }
      sts = oz_sys_logname_lookup (h_lognamtbl, OZ_PROCMODE_KNL, service, NULL, NULL, &nvalues, &h_logname);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_lognamtbl);
      if (sts != OZ_SUCCESS) {
        if (sts == OZ_NOLOGNAME) return (EAI_NONAME);
        errno = EOZSYSERR;
        errno_ozsts = sts;
        return (EAI_SYSTEM);
      }
    }
  }

  sa = malloc (sizeof *sa);
  memset (sa, 0, sizeof *sa);
  sa -> sin_family = AF_INET;
  sa -> sin_addr   = *(typeof (sa -> sin_addr) *)ipaddr;

  ai = malloc (sizeof *ai);
  memset (ai, 0, sizeof *ai);
  ai -> ai_family  = PF_INET;
  ai -> ai_addrlen = OZ_IO_IP_ADDRSIZE;
  ai -> ai_addr    = (struct sockaddr *)sa;

  for (index = 0; index < nvalues; index ++) {
    sts = oz_sys_logname_getval (h_logname, index, NULL, sizeof tmpbuf, tmpbuf, NULL, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) break;
    colon = NULL;
    if (memcmp (tmpbuf, "tcp:", 4) == 0) {
      ai -> ai_socktype  = SOCK_STREAM;
      ai -> ai_protocol  = 6;
      colon = tmpbuf + 4;
    }
    if (memcmp (tmpbuf, "udp:", 4) == 0) {
      ai -> ai_socktype  = SOCK_DGRAM;
      ai -> ai_protocol  = 17;
      colon = tmpbuf + 4;
    }
    if (colon != NULL) {
      port = atoi (colon);
      sa -> sin_port = htons (port);
      break;
    }
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  if ((nvalues > 0) && (index == nvalues)) {
    freeaddrinfo (ai);
    return (EAI_SERVICE);
  }
  sa -> sin_port = htons (port);

  if ((hints != NULL) && (hints -> ai_flags & AI_CANONNAME)) {
    ai -> ai_canonname = strdup (node);
  }

  *res = ai;
  return (0);
}

void freeaddrinfo (struct addrinfo *res)

{
  if (res -> ai_canonname != NULL) free (res -> ai_canonname);
  if (res -> ai_addr      != NULL) free (res -> ai_addr);
  free (res);
}

const char *gai_strerror (int errcode)

{
  switch (errcode) {
    case EAI_FAMILY: return ("The requested address family is not supported at all");
    case EAI_SOCKTYPE: return ("The requested socket type is not supported at all");
    case EAI_BADFLAGS: return ("ai_flags contains invalid flags");
    case EAI_NONAME: return ("The node or service is not known");
    case EAI_SERVICE: return ("The requested service is not available for the requested socket type");
    case EAI_ADDRFAMILY: return ("The specified network host does not have any network addresses in the requested address family");
    case EAI_NODATA: return ("The specified network host exists, but does not have any network addresses defined");
    case EAI_MEMORY: return ("Out of memory");
    case EAI_FAIL: return ("The name server returned a permanent failure indication");
    case EAI_AGAIN: return ("The name server returned a temporary failure indication");
    case EAI_SYSTEM: return (strerror (errno));
    default: return ("Unknown EAI error code");
  }
}

int getnameinfo (__const struct sockaddr *__restrict sa,
                 socklen_t salen, char *__restrict host,
                 socklen_t hostlen, char *__restrict serv,
                 socklen_t servlen, unsigned int flags)

{
  if ((host != NULL) && (hostlen > 0)) snprintf (host, hostlen, "%u.%u.%u.%u", 
						(((struct sockaddr_in *)sa) -> sin_addr.s_addr      ) & 0xFF, 
						(((struct sockaddr_in *)sa) -> sin_addr.s_addr >>  8) & 0xFF, 
						(((struct sockaddr_in *)sa) -> sin_addr.s_addr >> 16) & 0xFF, 
						(((struct sockaddr_in *)sa) -> sin_addr.s_addr >> 24) & 0xFF);
  if ((serv != NULL) && (servlen > 0)) snprintf (serv, servlen, "%u", ntohs (((struct sockaddr_in *)sa) -> sin_port));
  return (0);
}

int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)

{
  static int const one = 1;

  if (!oz_crtl_fd_check (s)) return (-1);
  if (oz_crtl_fd_array[s].driver != &netio_driver) {
    errno = ENOTSOCK;
    return (-1);
  }
  if ((level == SOL_SOCKET) && (optname == SO_KEEPALIVE)) {
    memcpy (optval, &one, *optlen);
    return (0);
  }
  if ((level == IPPROTO_TCP) && (optname == TCP_NODELAY)) {
    memcpy (optval, &one, *optlen);
    return (0);
  }
  errno = ENOPROTOOPT;
  return (-1);
}

int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)

{
  if (!oz_crtl_fd_check (s)) return (-1);
  if (oz_crtl_fd_array[s].driver != &netio_driver) {
    errno = ENOTSOCK;
    return (-1);
  }
  if ((level == SOL_SOCKET) && (optname == SO_KEEPALIVE)) return (0);
  if ((level == IPPROTO_TCP) && (optname == TCP_NODELAY)) return (0);
  errno = ENOPROTOOPT;
  return (-1);
}
