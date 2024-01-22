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
/*  User mode debugger							*/
/*									*/
/*    Input:								*/
/*									*/
/*	entry = program's entrypoint					*/
/*	param = parameter to pass to program				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_usr_debug_init = program's status value			*/
/*									*/
/*    Note:								*/
/*									*/
/*	Input is taken from OZ_DEBUG_INPUT and output goes to 		*/
/*	OZ_DEBUG_OUTPUT.  If OZ_DEBUG_INPUT is not defined, OZ_INPUT 	*/
/*	is used.  If OZ_DEBUG_OUTPUT is not defined, OZ_OUTPUT is 	*/
/*	used.								*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_misc.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"
#include "oz_usr_debug.h"

static uLong getchan (OZ_Handle h_defaulttbl, char *dname, char *name, OZ_Handle *h_r);

uLong oz_usr_debug_init (uLong (*entry) (void *param), void *param)

{
  char imagebuf[256], namebuf[64], threadidbuf[16];
  OZ_Handle h_dbgprclnmtbl, h_defaulttbl, h_error, h_initevent, h_input, h_logname, h_output, h_thread;
  OZ_Threadid threadid;
  uLong sts;

  char *paramv[] = { imagebuf, "-resume", threadidbuf };
  OZ_Handle_item proclnmtblitm = { OZ_HANDLE_CODE_PROCESS_LOGNAMTBL, sizeof h_dbgprclnmtbl, &h_dbgprclnmtbl, NULL };
  OZ_Logvalue ozdebugzero = { 0, "0" };

  /* Lookup logical name default table */

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_DEFAULT_TBL", NULL, NULL, NULL, &h_defaulttbl);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u looking up OZ_DEFAULT_TBL\n", sts);
    return (sts);
  }

  /* Set up INPUT/OUTPUT/ERROR channels */

  sts = getchan (h_defaulttbl, "OZ_DEBUG_INPUT",  "OZ_INPUT",  &h_input);
  if (sts != OZ_SUCCESS) return (sts);
  sts = getchan (h_defaulttbl, "OZ_DEBUG_OUTPUT", "OZ_OUTPUT", &h_output);
  if (sts != OZ_SUCCESS) return (sts);
  sts = getchan (h_defaulttbl, "OZ_DEBUG_ERROR",  "OZ_ERROR",  &h_error);
  if (sts != OZ_SUCCESS) return (sts);

  /* Get debugger image from OZ_DEBUG_IMAGE */

  sts = oz_sys_logname_lookup (h_defaulttbl, OZ_PROCMODE_USR, "OZ_DEBUG_IMAGE", NULL, NULL, NULL, &h_logname);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u fetching logical name OZ_DEBUG_IMAGE\n", sts);
    return (sts);
  }
  sts = oz_sys_logname_getval (h_logname, 0, NULL, sizeof imagebuf, imagebuf, NULL, NULL, 0, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u readhing logical name OZ_DEBUG_IMAGE\n", sts);
    return (sts);
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);

  /* Spawn debugger process */

  sts = oz_sys_thread_getid (0, &threadid);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  oz_sys_sprintf (sizeof threadidbuf, threadidbuf, "%u", threadid);
  oz_sys_sprintf (sizeof namebuf, namebuf, "debug %u", threadid);

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_usr_debug_init", &h_initevent);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  sts = oz_sys_spawn (0, imagebuf, h_input, h_output, h_error, h_initevent, 0, NULL, 3, paramv, namebuf, &h_thread, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u spawning %s\n", sts, imagebuf);
    return (sts);
  }

  /* Create OZ_DEBUG 0 logical in debugger process so it won't recursively start a debugger for itself */

  sts = oz_sys_handle_getinfo (h_thread, 1, &proclnmtblitm, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u getting debugger's process logical name table\n", sts);
    return (sts);
  }
  sts = oz_sys_logname_create (h_dbgprclnmtbl, "OZ_DEBUG", OZ_PROCMODE_KNL, 0, 1, &ozdebugzero, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u setting debugger's process logical name OZ_DEBUG 0\n", sts);
    return (sts);
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_dbgprclnmtbl);

  /* Release the debugger thread to start now that its OZ_DEBUG is set to zero */

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_initevent, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_initevent);

  /* Orphan the debugger thread so it won't suspend when this thread suspends itself */

  sts = oz_sys_thread_orphan (h_thread);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);

  /* Suspend self - The debugger will attach to this thread then resume it */

  sts = oz_sys_thread_suspend (0);
  if ((sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR)) oz_sys_condhand_signal (2, sts, 0);

  /* Call the program and return with its status */

  return ((*entry) (param));
}

/* Get an I/O channel for debugger input or output */

static uLong getchan (OZ_Handle h_defaulttbl, char *dname, char *name, OZ_Handle *h_r)

{
  char *lnm;
  uLong sts;
  OZ_Handle h_logname;

  /* Look up first the dname, then the name if dname was not found */

  lnm = dname;
  sts = oz_sys_logname_lookup (h_defaulttbl, OZ_PROCMODE_USR, lnm, NULL, NULL, NULL, &h_logname);
  if (sts == OZ_NOLOGNAME) {
    lnm = name;
    sts = oz_sys_logname_lookup (h_defaulttbl, OZ_PROCMODE_USR, lnm, NULL, NULL, NULL, &h_logname);
  }
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u looking up %s\n", sts, lnm);
    return (sts);
  }

  /* Now get the I/O channel from the logical name and assign an handle to it */

  sts = oz_sys_logname_getval (h_logname, 0, NULL, 0, NULL, NULL, h_r, OZ_OBJTYPE_IOCHAN, NULL);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u getting %s channel\n", sts, lnm);

  return (sts);
}
