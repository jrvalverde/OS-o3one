//+++2004-08-09
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
//---2004-08-09

#ifndef _OZ_HWXEN_DEFS
#define _OZ_HWXEN_DEFS

#include "oz_hwxen_hypervisor.h"

extern start_info_t oz_hwxen_startinfo;
extern shared_info_t oz_hwxen_sharedinfo;

extern char OZ_IMAGE_BASEADDR[1];		// linker/loader puts this at lowest address of image (inclusive)
extern char OZ_IMAGE_RDWRADDR[1];		// linker/loader puts this at lowest address of read/write section
extern char OZ_IMAGE_ZEROADDR[1];		// linker/loader puts this at lowest address of zero (bss) section
extern char OZ_IMAGE_LASTADDR[1];		// linker/loader puts this at highest address of image (inclusive)

#define PP0SA OZ_HW_BASE_SYSC_VA		// static address of pseudo-physical page 0
#define PP0SP OZ_HW_BASE_SYSC_VP		// static page of pseudo-physical page 0

#define TOPVA 0xD0000000 // 0xFC000000		// we won't ever map anything at or above this virtual address
#define TOPVP (OZ_HW_VADDRTOVPAGE (TOPVA))

#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)

#define PD_P  1
#define PD_UR 5
#define PD_UW 7
#define PD_4M 0x80
#define PD_G  0 // 0x100	// hypervisor doesn't allow G bits

#define PT_P  1
#define PT_W  2
#define PT_U  4
#define PT_KW 3
#define PT_UR 5
#define PT_UW 7
#define PT_G  0 // 0x100	// hypervisor doesn't allow G bits

#define PPVPAGE (8*1024*1024 / 4096)				// general process private area starts at 8Meg

#define PTSEC_NPAGEM (((OZ_HW_BASE_SYSC_VP * 4) + 256*1024 + 8192) >> 12) // total nubmer of pages in ptsec
								// = room for process private pagetable (max 4M)
								// + room for reqprot table (256K)
								// + room for oz_sys_pdata_array (8K)

#define PPPAGETBL_VPAGE (PPVPAGE - (4*1024*1024 / 4096))	// last 4M is the pagetable (but the top part is not used)
#define PPREQPROT_VPAGE (PPPAGETBL_VPAGE - 256/4)		// 256K before that is reqprot table
#define PDATA_USR_VPAGE (PPREQPROT_VPAGE - 1)			// just before that is oz_sys_pdata_array[OZ_PROCMODE_USR] page
#define PDATA_KNL_VPAGE (PDATA_USR_VPAGE - 1)			// just before that is oz_sys_pdata_array[OZ_PROCMODE_KNL] page

#define PTSEC_SVPAGE PDATA_KNL_VPAGE				// here is where it starts

								// these are due to the self-referencing pointer:
#define CPTVA ((uLong *)OZ_HW_VPAGETOVADDR (PPPAGETBL_VPAGE))	// current page table virtual address
#define CPDVA ((uLong *)OZ_HW_VPAGETOVADDR (PPPAGETBL_VPAGE + (PPPAGETBL_VPAGE >> 10))) // current page directory virtual address

#define oz_hwxen_marksyspagero(vpage) oz_hwxen_markpageprot (vpage, PT_UR | PT_G)
#define oz_hwxen_marksyspagerw(vpage) oz_hwxen_markpageprot (vpage, PT_KW | PT_G)

void oz_hwxen_markpageprot (OZ_Mempage vpage, uLong pageprot);
void oz_hwxen_pinpdpage (OZ_Pointer pdpma);
void oz_hwxen_pinptpage (OZ_Pointer ptpma);
void oz_hwxen_unpinpage (OZ_Pointer pma);

#endif
