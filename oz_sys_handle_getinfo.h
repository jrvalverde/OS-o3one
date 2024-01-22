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

#ifndef _OZ_SYS_HANDLE_GETINFO_H
#define _OZ_SYS_HANDLE_GETINFO_H

#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"

typedef enum {
	OZ_HANDLE_CODE_OBJTYPE, 
	OZ_HANDLE_CODE_USER_HANDLE, 
	OZ_HANDLE_CODE_USER_REFCOUNT, 
	OZ_HANDLE_CODE_USER_LOGNAMDIR, 
	OZ_HANDLE_CODE_USER_LOGNAMTBL, 
	OZ_HANDLE_CODE_USER_NAME, 
	OZ_HANDLE_CODE_USER_FIRST, 
	OZ_HANDLE_CODE_USER_NEXT, 
	OZ_HANDLE_CODE_JOB_HANDLE, 
	OZ_HANDLE_CODE_JOB_REFCOUNT, 
	OZ_HANDLE_CODE_JOB_LOGNAMDIR, 
	OZ_HANDLE_CODE_JOB_LOGNAMTBL, 
	OZ_HANDLE_CODE_JOB_PROCESSCOUNT, 
	OZ_HANDLE_CODE_JOB_FIRST, 
	OZ_HANDLE_CODE_JOB_NEXT, 
	OZ_HANDLE_CODE_PROCESS_HANDLE, 
	OZ_HANDLE_CODE_PROCESS_REFCOUNT, 
	OZ_HANDLE_CODE_PROCESS_NAME, 
	OZ_HANDLE_CODE_PROCESS_ID, 
	OZ_HANDLE_CODE_PROCESS_LOGNAMDIR, 
	OZ_HANDLE_CODE_PROCESS_LOGNAMTBL, 
	OZ_HANDLE_CODE_PROCESS_THREADCOUNT, 
	OZ_HANDLE_CODE_PROCESS_FIRST, 
	OZ_HANDLE_CODE_PROCESS_NEXT, 
	OZ_HANDLE_CODE_THREAD_HANDLE, 
	OZ_HANDLE_CODE_THREAD_REFCOUNT, 
	OZ_HANDLE_CODE_THREAD_STATE, 
	OZ_HANDLE_CODE_THREAD_TIS_RUN, 
	OZ_HANDLE_CODE_THREAD_TIS_COM, 
	OZ_HANDLE_CODE_THREAD_TIS_WEV, 
	OZ_HANDLE_CODE_THREAD_TIS_ZOM, 
	OZ_HANDLE_CODE_THREAD_TIS_INI, 
	OZ_HANDLE_CODE_THREAD_EXITSTS, 
	OZ_HANDLE_CODE_THREAD_EXITEVENT, 
	OZ_HANDLE_CODE_THREAD_NAME, 
	OZ_HANDLE_CODE_THREAD_FIRST, 
	OZ_HANDLE_CODE_THREAD_NEXT, 
	OZ_HANDLE_CODE_DEVICE_HANDLE, 
	OZ_HANDLE_CODE_DEVICE_REFCOUNT, 
	OZ_HANDLE_CODE_DEVICE_IOCHANCOUNT, 
	OZ_HANDLE_CODE_DEVICE_FIRST, 
	OZ_HANDLE_CODE_DEVICE_NEXT, 
	OZ_HANDLE_CODE_DEVICE_UNITNAME, 
	OZ_HANDLE_CODE_DEVICE_CLASSNAME, 
	OZ_HANDLE_CODE_IOCHAN_HANDLE, 
	OZ_HANDLE_CODE_IOCHAN_REFCOUNT, 
	OZ_HANDLE_CODE_IOCHAN_LOCKMODE, 
	OZ_HANDLE_CODE_IOCHAN_FIRST, 
	OZ_HANDLE_CODE_IOCHAN_NEXT, 
	OZ_HANDLE_CODE_USER_JOBCOUNT, 
	OZ_HANDLE_CODE_DEVICE_UNITDESC, 
	OZ_HANDLE_CODE_SYSTEM_BOOTTIME, 
	OZ_HANDLE_CODE_SYSTEM_PHYPAGETOTAL, 
	OZ_HANDLE_CODE_SYSTEM_PHYPAGEFREE, 
	OZ_HANDLE_CODE_SYSTEM_PHYPAGEL2SIZE, 
	OZ_HANDLE_CODE_SYSTEM_NPPTOTAL, 
	OZ_HANDLE_CODE_SYSTEM_NPPINUSE, 
	OZ_HANDLE_CODE_SYSTEM_NPPPEAK, 
	OZ_HANDLE_CODE_SYSTEM_PGPTOTAL, 
	OZ_HANDLE_CODE_SYSTEM_PGPINUSE, 
	OZ_HANDLE_CODE_SYSTEM_PGPPEAK, 
	OZ_HANDLE_CODE_SYSTEM_CPUCOUNT, 
	OZ_HANDLE_CODE_SYSTEM_CPUSAVAIL, 
	OZ_HANDLE_CODE_SYSTEM_SYSPAGETOTAL, 
	OZ_HANDLE_CODE_SYSTEM_SYSPAGEFREE, 
	OZ_HANDLE_CODE_SYSTEM_USERCOUNT, 
	OZ_HANDLE_CODE_SYSTEM_DEVICECOUNT, 
	OZ_HANDLE_CODE_THREAD_BASEPRI, 
	OZ_HANDLE_CODE_THREAD_CURPRIO, 
	OZ_HANDLE_CODE_JOB_NAME, 
	OZ_HANDLE_CODE_OBJADDR, 
	OZ_HANDLE_CODE_THREAD_ID, 
	OZ_HANDLE_CODE_THREAD_WEVENT0, 
	OZ_HANDLE_CODE_THREAD_WEVENT1, 
	OZ_HANDLE_CODE_THREAD_WEVENT2, 
	OZ_HANDLE_CODE_THREAD_WEVENT3, 
	OZ_HANDLE_CODE_EVENT_VALUE, 
	OZ_HANDLE_CODE_EVENT_NAME, 
	OZ_HANDLE_CODE_SYSTEM_CACHEPAGES, 
	OZ_HANDLE_CODE_DEVICE_ALIASNAME, 
	OZ_HANDLE_CODE_PROCESS_SECATTR,
	OZ_HANDLE_CODE_THREAD_SECATTR,
	OZ_HANDLE_CODE_THREAD_DEFCRESECATTR,
	OZ_HANDLE_CODE_THREAD_SECKEYS,
	OZ_HANDLE_CODE_DEVICE_SECATTR,
	OZ_HANDLE_CODE_LOGNAME_SECATTR,
	OZ_HANDLE_CODE_IOCHAN_SECATTR, 
	OZ_HANDLE_CODE_JOB_SECATTR, 
	OZ_HANDLE_CODE_USER_SECATTR, 
	OZ_HANDLE_CODE_USER_QUOTA_USE, 
	OZ_HANDLE_CODE_IOCHAN_LASTIOTID, 
	OZ_HANDLE_CODE_THREAD_PARENT, 
	OZ_HANDLE_CODE_EVENT_GETIMINT, 
	OZ_HANDLE_CODE_EVENT_GETIMNXT, 
	OZ_HANDLE_CODE_IOCHAN_READCOUNT, 
	OZ_HANDLE_CODE_IOCHAN_WRITECOUNT, 
	OZ_HANDLE_CODE_THREAD_NUMIOS, 
	OZ_HANDLE_CODE_THREAD_NUMPFS, 
	OZ_HANDLE_CODE_SYSTEM_KNLVERINT, 
	OZ_HANDLE_CODE_END
             } OZ_Handle_code;

typedef struct { OZ_Handle_code code;
                 uLong size;
                 void *buff;
                 uLong *rlen;
               } OZ_Handle_item;

OZ_HW_SYSCALL_DCL_4 (handle_getinfo, OZ_Handle, h, uLong, count, const OZ_Handle_item *, items, uLong *, index_r)

#endif