//+++2004-09-11
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
//---2004-09-11

/************************************************************************/
/*									*/
/*  This module contains wrappers for the console routines.  They 	*/
/*  select either the standard AT keyboard / VGA screen or the 		*/
/*  'COM1' serial port.							*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_misc.h"

int oz_dev_video_serialmode = 0;

/* Called very early in boot to initialize video driver */

void oz_dev_video_init (void)

{
  char const *extra;

  oz_dev_video_serialmode = 0;
  oz_dev_vgavideo_init ();
  extra = oz_knl_misc_getextra ("console", "vga");
  oz_knl_printk ("\noz_dev_video_init: console parameter <%s>\n", extra);
  if (strncasecmp (extra, "comport.", 8) == 0) {
    if (oz_dev_comvideo_init (extra + 8)) oz_dev_video_serialmode = 1;
  } else if (strcasecmp (extra, "vga") != 0) {
    oz_knl_printk ("oz_dev_video_init: unknown console extra param <%s>, defaulting to VGA\n", extra);
    oz_knl_printk ("oz_dev_video_init: should be VGA or comport.<3F8/2F8/3E8/2E8>\n");
  }
}

/* Standard initialization routine called late in boot at softint level */
/* This creates the "console" device                                    */

void oz_dev_console_init (void)

{
  if (oz_dev_video_serialmode) oz_dev_comport_init ();	// initialize comport driver
             else oz_dev_atkcons_init ();	// initialize AT keyboard driver
}

int oz_knl_console_debugchk (void)

{
  if (!oz_dev_video_serialmode) return (oz_knl_atkcons_debugchk ());
  return (0);
}

void oz_dev_video_exclusive (int flag)

{
  if (!oz_dev_video_serialmode) oz_dev_vgavideo_exclusive (flag);
}

/* Read from keyboard (waiting in waitloop as necessary) */
/* This can be called at high IPL                        */

int oz_hw_getcon (uLong size, char *buff, uLong pmtsize, const char *pmtbuff)

{
  if (oz_dev_video_serialmode) return (oz_dev_comcons_getcon (size, buff, pmtsize, pmtbuff));
             else return (oz_dev_atkcons_getcon (size, buff, pmtsize, pmtbuff));
}

/* Output buffer to video screen (waiting in waitloop as necessary) */
/* This can be called at high IPL                                   */

void oz_hw_putcon (uLong size, const char *buff)

{
  if (oz_dev_video_serialmode) oz_dev_comvideo_putcon (size, buff);
             else oz_dev_vgavideo_putcon (size, buff);
}

/* Output single character to video screen (waiting in waitloop as necessary) */
/* This can be called at high IPL                                             */

void oz_dev_video_putchar (void *vctxv, char c)

{
  if (oz_dev_video_serialmode) oz_dev_comvideo_putchar (vctxv, c);
             else oz_dev_vgavideo_putchar (vctxv, c);
}

void oz_dev_video_statusupdate (Long cpuidx, uByte cpulevel, uByte tskpri, OZ_Pointer eip)

{
  if (!oz_dev_video_serialmode) oz_dev_vgavideo_statusupdate (cpuidx, cpulevel, tskpri, eip);
}
