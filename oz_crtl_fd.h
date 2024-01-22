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

#ifndef _OZ_CRTL_FD_H
#define _OZ_CRTL_FD_H

#define EOZSYSERR 65535

#include "oz_knl_handle.h"

#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int (*fxstat) (int ver, int fildes, struct stat *buf);
                 off_t (*lseek) (int fd, off_t offset, int whence);
                 ssize_t (*read) (int fd, __ptr_t buf, size_t nbytes);
                 ssize_t (*write) (int fd, const __ptr_t buf, size_t nbytes);
                 int (*vioctl) (int fd, int request, va_list ap);
                 int (*vfcntl) (int fd, int cmd, va_list ap);
                 int (*close) (int fd);
               } OZ_Crtl_fd_driver;

typedef struct { OZ_Handle h_iochan;			// IO channel pointing to device
                 OZ_Handle h_event;			// event flag to use for IO on device
                 OZ_Crtl_fd_driver const *driver;	// points to driver struct
                 int flags1, flags2, flags3, flags4;	// for use by driver routines
                 void *point1, *point2, *point3;	// for use by driver routines
                 uLong pollpendmask;			// mask of pending OZ_IO_GEN_POLL (see oz_crtl_select.c)
                 uLong volatile pollstatus;		// status of pending OZ_IO_GEN_POLL (see oz_crtl_select.c)
                 char allocated;			// set if in use, clear otherwise
                 char append;				// set by open's O_APPEND, clear otherwise
                 char isatty;				// 0=unknown; 1=known to be tty; 2=known to not be tty
               } OZ_Crtl_fd_array;

extern uLong errno_ozsts;
extern OZ_Crtl_fd_array *oz_crtl_fd_array;

int oz_crtl_fd_alloc (void);
int oz_crtl_fd_check (int fd);
int oz_crtl_fd_free (int fd);
OZ_Handle oz_crtl_fd_gethiochan (int fd);

#endif
