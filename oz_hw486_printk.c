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
/*  Print a debugging message to the Bochs Debug Port (0x403)		*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_xprintf.h"

static uLong printbuf (void *dummy, uLong *size, char **buff);

void oz_hw486_printk (const char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  oz_hw486_printkv (fmt, ap);
  va_end (ap);
}

void oz_hw486_printkv (const char *fmt, va_list ap)

{
  char buf[256];

  oz_sys_vxprintf (printbuf, NULL, sizeof buf, buf, NULL, fmt, ap);
}

static uLong printbuf (void *dummy, uLong *size, char **buff)

{
  oz_hw486_outsb (*size, *buff, 0x403);
  return (OZ_SUCCESS);
}
