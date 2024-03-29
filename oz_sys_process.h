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

#ifndef _OZ_SYSCALL_PROCESS_H
#define _OZ_SYSCALL_PROCESS_H

#include "ozone.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"

typedef struct { OZ_Handle h_section;		// section to map
                 OZ_Mempage npagem;		// number of pages in section to map, 0 for the whole thing
                 OZ_Mempage svpage;		// starting virtual page to map it at
                 OZ_Procmode ownermode;		// owner mode of the pages (ie, who can unmap them or modify their protection)
                 OZ_Hw_pageprot pageprot;	// protection to apply to the pages when mapped
               } OZ_Mapsecparamh;

typedef struct { OZ_Breakpoint *addr;		// address of breakpoint
                 OZ_Breakpoint opcd;		// opcode to store there, returns opcode that was there
                 uLong stat;			// status of writing this breakpoint
               } OZ_Process_Breaklist;

typedef struct { uLong namesize;		// size of namebuff
                 char *namebuff;		// filled in with null-terminated image name string
                 void *baseaddr;		// filled in with image base address
                 OZ_Handle h_iochan;		// filled in with image file handle
               } OZ_Process_Imagelist;

#include "oz_knl_process.h"

OZ_HW_SYSCALL_DCL_4 (process_create, OZ_Procmode, procmode, OZ_Handle, h_job, char *, name, OZ_Handle *, h_process_r)
OZ_HW_SYSCALL_DCL_2 (process_makecopy, const char *, name, OZ_Handle *, h_process_r)
OZ_HW_SYSCALL_DCL_6 (process_mapsection, OZ_Procmode, procmode, OZ_Handle, h_section, OZ_Mempage *, npagem, OZ_Mempage *, svpage, uLong, mapsecflags, OZ_Hw_pageprot, pageprot)
OZ_HW_SYSCALL_DCL_3 (process_mapsections, uLong, mapsecflags, int, nsections, OZ_Mapsecparamh *, mapsecparamhs)
OZ_HW_SYSCALL_DCL_9 (process_getsecfromvpage, OZ_Procmode, procmode, OZ_Handle, h_process, OZ_Mempage, vpage, OZ_Handle *, h_section_r, OZ_Mempage *, spage_r, OZ_Hw_pageprot *, pageprot_r, OZ_Procmode *, procmode_r, OZ_Mempage *, npages_r, uLong *, mapsecflags_r)
OZ_HW_SYSCALL_DCL_1 (process_unmapsec, OZ_Mempage, vpage)
OZ_HW_SYSCALL_DCL_3 (process_getid, OZ_Procmode, procmode, OZ_Handle, h_process, OZ_Processid *, processid_r)
OZ_HW_SYSCALL_DCL_3 (process_getthreadqseq, OZ_Procmode, procmode, OZ_Handle, h_process, uLong *, threadqseq_r)
OZ_HW_SYSCALL_DCL_2 (process_getbyid, OZ_Processid, processid, OZ_Handle *, h_process_r)
OZ_HW_SYSCALL_DCL_5 (process_peek, OZ_Handle, h_process, OZ_Procmode, procmode, uLong, size, void const *, remsrc, void *, lcldst)
OZ_HW_SYSCALL_DCL_5 (process_poke, OZ_Handle, h_process, OZ_Procmode, procmode, uLong, size, void const *, lclsrc, void *, remdst)
OZ_HW_SYSCALL_DCL_4 (process_setbreaks, OZ_Procmode, procmode, OZ_Handle, h_process, uLong, listsize, OZ_Process_Breaklist *, listbuff)
OZ_HW_SYSCALL_DCL_4 (process_imagelist, OZ_Handle, h_process, uLong, listsize, OZ_Process_Imagelist *, listbuff, int *, nimages_r)

#endif
