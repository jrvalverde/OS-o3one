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
/*  Pseudo-terminal IO routines						*/
/*									*/
/************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/termios.h>
#include <sys/types.h>

#include <ozone.h>
#include <oz_crtl_fd.h>
#include <oz_io_conpseudo.h>
#include <oz_io_console.h>
#include <oz_knl_hw.h>
#include <oz_knl_status.h>
#include <oz_sys_condhand.h>
#include <oz_sys_event.h>
#include <oz_sys_io.h>

int getpt_name (char const *name);

static int ptio_fxstat (int ver, int fildes, struct stat *buf);
static off_t ptio_lseek (int fd, off_t offset, int whence);
static ssize_t ptio_read (int fd, __ptr_t buf, size_t nbytes);
static ssize_t ptio_write (int fd, const __ptr_t buf, size_t nbytes);
static int ptio_vioctl (int fd, int request, va_list ap);

static OZ_Crtl_fd_driver const ptio_driver = { ptio_fxstat, ptio_lseek, ptio_read, ptio_write, ptio_vioctl, NULL, NULL };

/************************************************************************/
/*									*/
/*  Create pseudo-terminal and return fd's to its master and slave 	*/
/*  ports								*/
/*									*/
/************************************************************************/

int openpty (int *master, int *slave, char *name, struct termios *termp, struct winsize *winp)

{
  int mfd, sfd;
  OZ_Console_modebuff console_modebuff;
  OZ_IO_console_setmode console_setmode;
  uLong sts;

  mfd = -1;
  sfd = -1;

  /* Create an pseudo-terminal */

  mfd = getpt_name (name);
  if (mfd < 0) goto err_fd;

  /* Open the slave side */

  sfd = open (name, O_RDWR);
  if (sfd < 0) goto err_fd;

  /* Set slave characteristics */

  memset (&console_modebuff, 0, sizeof console_modebuff);
  memset (&console_setmode,  0, sizeof console_setmode);

  console_modebuff.columns = winp -> ws_col;
  console_modebuff.rows    = winp -> ws_row;

  console_setmode.size = sizeof console_modebuff;
  console_setmode.buff = &console_modebuff;

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[sfd].h_iochan, oz_crtl_fd_array[sfd].h_event, 
                   OZ_IO_CONSOLE_SETMODE, sizeof console_setmode, &console_setmode);
  if (sts != OZ_SUCCESS) goto err_sts;

  /* Return fd's and success status */

  *master = mfd;
  *slave  = sfd;
  return (0);

  /* Some error, close everything up and return error status */

err_sts:
  errno = EOZSYSERR;
  errno_ozsts = sts;
err_fd:
  if (sfd >= 0) close (sfd);
  if (mfd >= 0) close (mfd);
  return (-1);
}

/************************************************************************/
/*									*/
/*  Create a pseudo-terminal and return fd to the master port		*/
/*									*/
/*  Note that in OZONE, the master and slave are actually the same 	*/
/*  device.  Only the function codes distinguish which 'side' you're 	*/
/*  accessing.								*/
/*									*/
/************************************************************************/

int getpt ()

{
  char name[48];

  sprintf (name, "pty.%u.%QX", getpid (), oz_hw_tod_getnow ());
  return (getpt_name (name));
}

int getpt_name (char const *name)

{
  int mfd;
  OZ_Handle h_iochan;
  OZ_IO_conpseudo_setup conpseudo_setup;
  uLong sts;

  h_iochan = 0;
  mfd      = -1;

  /* Create an pseudo-terminal */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, OZ_IO_CONPSEUDO_DEV, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) goto err_sts;

  memset (&conpseudo_setup, 0, sizeof conpseudo_setup);
  conpseudo_setup.portname  = name;
  conpseudo_setup.classname = OZ_IO_CONSOLE_SETUPDEV;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_CONPSEUDO_SETUP, sizeof conpseudo_setup, &conpseudo_setup);
  if (sts != OZ_SUCCESS) goto err_sts;

  mfd = oz_crtl_fd_alloc ();
  if (mfd < 0) goto err_fd;

  oz_crtl_fd_array[mfd].h_iochan = h_iochan;
  oz_crtl_fd_array[mfd].driver   = &ptio_driver;

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "pty master", &(oz_crtl_fd_array[mfd].h_event));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Return fd's and success status */

  return (mfd);

  /* Some error, close everything up and return error status */

err_sts:
  errno = EOZSYSERR;
  errno_ozsts = sts;
err_fd:
  if (mfd >= 0) close (mfd);
  else if (h_iochan != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);
  return (-1);
}

static int ptio_fxstat (int ver, int fildes, struct stat *buf)

{
  errno = EINVAL;
  return (-1);
}

static off_t ptio_lseek (int fd, off_t offset, int whence)

{
  errno = EINVAL;
  return (-1);
}

static ssize_t ptio_read (int fd, __ptr_t buf, size_t nbytes)

{
  OZ_IO_conpseudo_getscrdata conpseudo_getscrdata;
  uLong rlen, sts;

  memset (&conpseudo_getscrdata, 0, sizeof conpseudo_getscrdata);
  conpseudo_getscrdata.size = nbytes;
  conpseudo_getscrdata.buff = buf;
  conpseudo_getscrdata.rlen = &rlen;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_CONPSEUDO_GETSCRDATA, sizeof conpseudo_getscrdata, &conpseudo_getscrdata);
  if (sts == OZ_SUCCESS) return (nbytes);
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

static ssize_t ptio_write (int fd, const __ptr_t buf, size_t nbytes)

{
  OZ_IO_conpseudo_putkbddata conpseudo_putkbddata;
  uLong sts;

  memset (&conpseudo_putkbddata, 0, sizeof conpseudo_putkbddata);
  conpseudo_putkbddata.size = nbytes;
  conpseudo_putkbddata.buff = (void *)buf;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_CONPSEUDO_PUTKBDDATA, sizeof conpseudo_putkbddata, &conpseudo_putkbddata);
  if (sts == OZ_SUCCESS) return (nbytes);
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

static int ptio_vioctl (int fd, int request, va_list ap)

{
  errno = EINVAL;
  return (-1);
}

/************************************************************************/
/*									*/
/*  Given the fd of the master pseudo-terminal device, return name of 	*/
/*  the slave								*/
/*									*/
/************************************************************************/

char *ptsname (int fd)

{
  static char slavename[OZ_DEVUNIT_NAMESIZE];

  if (ptsname_r (fd, slavename, sizeof slavename) < 0) return (NULL);
  return (slavename);
}

int ptsname_r (int fd, char *slavename, int slavenamelen)

{
  uLong sts;

  /* Make sure it's one of our devices for the heck of it */

  if (oz_crtl_fd_array[fd].driver != &ptio_driver) {
    errno = ENOTTY;
    return (-1);
  }

  /* Get the unit's name.  Both master and slave are the same device, so we can use the master's channel. */

  sts = oz_sys_iochan_getunitname (oz_crtl_fd_array[fd].h_iochan, slavenamelen, slavename);
  if (sts == OZ_SUCCESS) return (0);
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

int grantpt (int fildes)

{
  return (0);
}

int unlockpt (int fildes)

{
  return (0);
}
