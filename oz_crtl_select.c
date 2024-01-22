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
/*  Implementation of 'select' call					*/
/*									*/
/************************************************************************/

#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "ozone.h"
#include "oz_crtl_fd.h"
#include "oz_io_gen.h"
#include "oz_io_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"

static OZ_Handle h_event_poll = 0;
static OZ_Handle h_iochan_timer = 0;

int select (int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)

{
  int fd, rc;
  OZ_IO_gen_poll io_poll;
  OZ_IO_timer_waituntil timer_waituntil;
  uLong sts;
  uLong volatile timerstatus;

  /* Make sure we have an event flag to work with.  Also set up timer channel while we're at it. */

  if (h_event_poll == 0) {
    sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_crtl_select", &h_event_poll);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan_timer, OZ_IO_TIMER_DEV, OZ_LOCKMODE_NL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }

  /* Set up timeout timer */

  timerstatus = OZ_PENDING;
  if (timeout != NULL) {
    timer_waituntil.datebin  = oz_hw_tod_getnow ();
    timer_waituntil.datebin += timeout -> tv_sec * OZ_TIMER_RESOLUTION;
#if OZ_TIMER_RESOLUTION % 1000000
    error : OZ TIMER RESOLUTION assumed to be multiple of 1,000,000
#endif
    timer_waituntil.datebin += timeout -> tv_usec * (OZ_TIMER_RESOLUTION / 1000000);
    timerstatus = OZ_STARTED;
  }

  /* Scan through each given fd and queue a poll request to selected devices */

  memset (&io_poll, 0, sizeof io_poll);

  for (fd = 0; fd < n; fd ++) {

    /* See what select types there are for this fd */

    io_poll.mask = 0;
    if ((readfds   != NULL) && FD_ISSET (fd, readfds  )) io_poll.mask |= OZ_IO_GEN_POLL_R;
    if ((writefds  != NULL) && FD_ISSET (fd, writefds )) io_poll.mask |= OZ_IO_GEN_POLL_W;
    if ((exceptfds != NULL) && FD_ISSET (fd, exceptfds)) io_poll.mask |= OZ_IO_GEN_POLL_X;
    if (io_poll.mask == 0) continue;

    /* Make sure the fd is valid */

    if (!oz_crtl_fd_check (fd)) return (-1);

    /* If there is already a poll pending with all the needed mask bits, just leave it as is */

    if ((oz_crtl_fd_array[fd].pollpendmask & io_poll.mask) == io_poll.mask) continue;

    /* If there is a poll queued, abort it */

    if (oz_crtl_fd_array[fd].pollpendmask != 0) {
      oz_sys_io_abort (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan);
      while (oz_crtl_fd_array[fd].pollstatus == OZ_PENDING) {
        oz_sys_event_wait (OZ_PROCMODE_KNL, h_event_poll, 0);
        oz_sys_event_set (OZ_PROCMODE_KNL, h_event_poll, 0, NULL);
      }
    }

    /* Queue a new poll to the device */

    oz_crtl_fd_array[fd].pollpendmask = io_poll.mask;
    oz_crtl_fd_array[fd].pollstatus   = OZ_PENDING;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, &(oz_crtl_fd_array[fd].pollstatus), 
                           h_event_poll, NULL, NULL, OZ_IO_GEN_POLL, sizeof io_poll, &io_poll);
    if (sts != OZ_STARTED) oz_crtl_fd_array[fd].pollstatus = sts;
  }

  /* Scan for completed poll */

scan:
  oz_sys_event_set (OZ_PROCMODE_KNL, h_event_poll, 0, NULL);
  for (fd = 0; fd < n; fd ++) {

    /* See what select types there are for this fd */

    io_poll.mask = 0;
    if ((readfds   != NULL) && FD_ISSET (fd, readfds  )) io_poll.mask |= OZ_IO_GEN_POLL_R;
    if ((writefds  != NULL) && FD_ISSET (fd, writefds )) io_poll.mask |= OZ_IO_GEN_POLL_W;
    if ((exceptfds != NULL) && FD_ISSET (fd, exceptfds)) io_poll.mask |= OZ_IO_GEN_POLL_X;
    if (io_poll.mask == 0) continue;

    /* See what, if any have completed */

    sts = oz_crtl_fd_array[fd].pollstatus;
    if (sts == OZ_PENDING) continue;
    if (sts < OZ_POLLDONE) goto perr;
    if (sts > OZ_POLLDONEMAX) goto perr;
    sts -= OZ_POLLDONE;

    /* If something caller wants, we're done */

    if (sts & io_poll.mask) goto done;

    /* Something else, queue new poll that just has stuff caller wants */

    oz_crtl_fd_array[fd].pollpendmask = io_poll.mask;
    oz_crtl_fd_array[fd].pollstatus   = OZ_PENDING;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, &(oz_crtl_fd_array[fd].pollstatus), 
                           h_event_poll, NULL, NULL, OZ_IO_GEN_POLL, sizeof io_poll, &io_poll);
    if (sts != OZ_STARTED) oz_crtl_fd_array[fd].pollstatus = sts;
  }

  /* No poll satisfied, maybe start timeout timer */

  if (timerstatus == OZ_STARTED) {
    timerstatus = OZ_PENDING;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_iochan_timer, &timerstatus, h_event_poll, NULL, NULL, 
                           OZ_IO_TIMER_WAITUNTIL, sizeof timer_waituntil, &timer_waituntil);
    if (sts != OZ_STARTED) timerstatus = sts;
  }

  /* Wait for one of the poll IOs to complete or the timer to expire */

  oz_sys_event_wait (OZ_PROCMODE_KNL, h_event_poll, 0);
  if (timerstatus == OZ_PENDING) goto scan;

  /* A request was satisfied by a completed poll or timeout timer, set up output bits and return */

done:
  if ((timerstatus == OZ_PENDING) && (timeout != NULL)) {	// see if timer still running
    oz_sys_io_abort (OZ_PROCMODE_KNL, h_iochan_timer);		// if so, cancel it out
    while (timerstatus == OZ_PENDING) {				// wait for it to cancel
      oz_sys_event_wait (OZ_PROCMODE_KNL, h_event_poll, 0);
      oz_sys_event_set (OZ_PROCMODE_KNL, h_event_poll, 0, NULL);
    }
  }

  rc = 0;							// haven't found any set bits yet
  for (fd = 0; fd < n; fd ++) {					// check all requested fd's

    /* See if poll IO has completed for this fd.  If not, clear corresponding readfds,writefds,exceptfds bit.  If so, */
    /* reset pollpendmask so next call to select will do another poll IO, then leave corresponding *fds bits set.     */

    sts = oz_crtl_fd_array[fd].pollstatus - OZ_POLLDONE;	// get poll completion bits for this fd
    if (sts > OZ_POLLDONEMAX - OZ_POLLDONE) sts = 0;		// if still waiting, say nothing is done
    else oz_crtl_fd_array[fd].pollpendmask = 0;			// else, reset them for next call to select

    /* If caller sensing read, then count if completed, else clear read bit */

    if ((readfds != NULL) && FD_ISSET (fd, readfds)) {
      if (sts & OZ_IO_GEN_POLL_R) rc ++;
              else FD_CLR (fd, readfds);
    }

    /* If caller sensing write, then count if completed, else clear write bit */

    if ((writefds != NULL) && FD_ISSET (fd, writefds)) {
      if (sts & OZ_IO_GEN_POLL_W) rc ++;
             else FD_CLR (fd, writefds);
    }

    /* If caller sensing except, then count if completed, else clear except bit */

    if ((exceptfds != NULL) && FD_ISSET (fd, exceptfds)) {
      if (sts & OZ_IO_GEN_POLL_X) rc ++;
            else FD_CLR (fd, exceptfds);
    }
  }

  /* Return number of bits found */

  return (rc);

  /* An IO error occurred polling something */

perr:
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}
