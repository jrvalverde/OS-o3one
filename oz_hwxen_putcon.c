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
/*  Display a message on console					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"

#include "oz_hwxen_hypervisor.h"

static char outbuf[256];
static int outidx = 0;

void oz_hw_putcon (uLong size, char const *buff)

{
  while (size > 0) {
    -- size;
    if (((outbuf[outidx++] = *(buff ++)) == '\n') || (outidx == sizeof outbuf)) {
      HYPERVISOR_console_write (outbuf, outidx);
      outidx = 0;
    }
  }
}

int oz_hw_getcon (uLong size, char *buff, uLong pmtsize, const char *pmtbuff)

{
  oz_hw_putcon (pmtsize, pmtbuff);
  oz_hw_putcon (42, "\noz_hw_getcon: Xen console can't do reads\n");
  HYPERVISOR_exit ();
  return (0);
}
