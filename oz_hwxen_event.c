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
/*  Process asynchronous events						*/
/*									*/
/*  The hypervisor calls this routine when it has something to notify 	*/
/*  us about								*/
/*									*/
/*    Input:								*/
/*									*/
/*	mchargs = machine args at time of interrupt			*/
/*	  mchargs -> ec2 = events bitmask from oz_hwxen_sharedinfo	*/
/*	oz_hwxen_sharedinfo.events = 0					*/
/*	oz_hwxen_sharedinfo.events_mask<MASTER_ENABLE> = 0		*/
/*	smplevel presumably < OZ_SMPLOCK_LEVEL_ASYNC			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"

#include "oz_hwxen_hypervisor.h"

void oz_hwxen_do_asyncevent (OZ_Mchargs *mchargs)

{
  if (mchargs -> ec2 & EVENT_BLKDEV)  oz_hwxen_event_blkdev  (mchargs);
  if (mchargs -> ec2 & EVENT_TIMER)   oz_hwxen_event_timer   (mchargs);
  if (mchargs -> ec2 & EVENT_DIE)     oz_hwxen_event_die     (mchargs);
  if (mchargs -> ec2 & EVENT_DEBUG)   oz_hwxen_event_debug   (mchargs);
  if (mchargs -> ec2 & EVENT_NET)     oz_hwxen_event_net     (mchargs);
  if (mchargs -> ec2 & EVENT_PS2)     oz_hwxen_event_ps2     (mchargs);
  if (mchargs -> ec2 & EVENT_STOP)    oz_hwxen_event_stop    (mchargs);
  if (mchargs -> ec2 & EVENT_EVTCHN)  oz_hwxen_event_evtchn  (mchargs);
  if (mchargs -> ec2 & EVENT_VBD_UPD) oz_hwxen_event_vbd_upd (mchargs);
}

void oz_hwxen_event_die     (OZ_Mchargs *mchargs) { oz_knl_printk ("oz_hwxen_event_die*:\n"); }
void oz_hwxen_event_debug   (OZ_Mchargs *mchargs) { oz_knl_printk ("oz_hwxen_event_debug*:\n"); }
void oz_hwxen_event_ps2     (OZ_Mchargs *mchargs) { oz_knl_printk ("oz_hwxen_event_ps2*:\n"); }
void oz_hwxen_event_stop    (OZ_Mchargs *mchargs) { oz_knl_printk ("oz_hwxen_event_stop*:\n"); }
void oz_hwxen_event_evtchn  (OZ_Mchargs *mchargs) { oz_knl_printk ("oz_hwxen_event_evtchn*:\n"); }
void oz_hwxen_event_vbd_upd (OZ_Mchargs *mchargs) { oz_knl_printk ("oz_hwxen_event_vbd_upd*:\n"); }
