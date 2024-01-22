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
/*  Timer routines							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_sys_dateconv.h"

#include "oz_hwxen_hypervisor.h"

OZ_Smplock *oz_hw_smplock_tm = oz_hwxen_smplock_events + _EVENT_TIMER;

static OZ_Datebin nextevent = 0xFFFFFFFFFFFFFFFFULL;

/************************************************************************/
/*									*/
/*  Set the datebin of the next event					*/
/*  When this time is reached, call oz_knl_timer_timeisup		*/
/*									*/
/************************************************************************/

void oz_hw_timer_setevent (OZ_Datebin datebin)

{
  nextevent = datebin;					// just save the event time
}

/* This event handler is called about every 20mS */

void oz_hwxen_event_timer (OZ_Mchargs *mchargs)

{
  OZ_Datebin now;
  uLong tm;

  now = oz_hw_tod_getnow ();				// see what time it is now
  if (now >= nextevent) {				// see if it's event time
    nextevent = 0xFFFFFFFFFFFFFFFFULL;			// ok, reset 'next event' time
    tm = oz_hw_smplock_wait (oz_hw_smplock_tm);		// set spinlock as required by oz_knl_timer_timeisup
    oz_knl_timer_timeisup ();				// call the kernel to do its thing
    oz_hw_smplock_clr (oz_hw_smplock_tm, tm);		// release spinlock
  }
}
