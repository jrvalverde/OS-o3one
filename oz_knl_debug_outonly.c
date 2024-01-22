//+++2004-08-11
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
//---2003-11-18

/************************************************************************/
/*									*/
/*  Output-only kernel debugger module					*/
/*									*/
/************************************************************************/

#define _OZ_SYS_DEBUG_C

#include "ozone.h"
#include "oz_knl_debug.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_debug.h"
#include "oz_sys_xprintf.h"

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_knl_debug_init (void)

{
  oz_knl_printk ("oz_knl_debug_init: output-only 'debugger'\n");
}

/************************************************************************/
/*									*/
/*  This routine is called as the result of an exception		*/
/*									*/
/*    Input:								*/
/*									*/
/*	sigargs = signal args for the exception				*/
/*	          or NULL if called via control-shift-C			*/
/*	mchargs = machine args at time of exception/interrupt		*/
/*									*/
/*	hardware interrupt delivery inhibited				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_debug_exception = 0 : do normal exception processing	*/
/*	                         1 : return to mchargs to try again	*/
/*	                             (mchargs might be modified)	*/
/*									*/
/************************************************************************/

static void printraceback (void *param, OZ_Mchargs *mchargs);
static uLong printmchargs (void *param, const char *format, ...);

int oz_knl_debug_exception (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs)

{
  int i;
  typeof (GETSTACKPTR ((*mchargs))) savestackptr;

  static int alreadydumping = 0;

  i = alreadydumping;
  alreadydumping ++;
  if (i != 0) {
    alreadydumping = 2;
    if (i == 1) oz_knl_printk ("oz_knl_debug_exception: nested crash - halting\n");
    oz_hw_halt ();
  }

  oz_knl_printk ("oz_knl_debug_exception: fatal exception, sigargs %p, mchargs %p\n", sigargs, mchargs);

  if (sigargs != NULL) {
    oz_knl_printk ("oz_knl_debug_exception:   sigargs:");
    for (i = 0; i <= sigargs[0]; i ++) {
      oz_knl_printk (" %u=0x%X", sigargs[i], sigargs[i]);
    }
    oz_knl_printk ("\n");
  }

  if (mchargs != NULL) {
    savestackptr = GETSTACKPTR ((*mchargs));
    oz_knl_printk ("oz_knl_debug_exception:   mchargs:\n");
    oz_hw_mchargs_print (printmchargs, NULL, 1, mchargs);
    oz_knl_printk ("oz_knl_debug_exception:   stack at %p:\n", (void *)savestackptr);
    oz_knl_dumpmem (256, (void *)savestackptr);
    oz_knl_printk ("oz_knl_debug_exception:   traceback:\n");
    oz_hw_traceback (printraceback, NULL, 16, mchargs, NULL);
  }

  oz_knl_printk ("oz_knl_debug_exception:   halting...\n");
  while (1) oz_hw_halt ();
}

static void printraceback (void *param, OZ_Mchargs *mchargs)

{
  oz_hw_mchargs_print (printmchargs, NULL, 0, mchargs);
}

static uLong printmchargs (void *param, const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  oz_knl_printkv (format, ap);
  va_end (ap);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This routine is called by the other cpus in response to an 		*/
/*  oz_hw_debug_halt call						*/
/*									*/
/*    Input:								*/
/*									*/
/*	mchargs = machine arguments at time of interrupt		*/
/*									*/
/*	hardware interrupt delivery inhibited				*/
/*									*/
/************************************************************************/

void oz_knl_debug_halted (OZ_Mchargs *mchargs)

{
  oz_knl_printk ("oz_knl_debug_halted: cpu %d halting\n", oz_hw_cpu_getcur ());
  while (1) oz_hw_halt ();
}
