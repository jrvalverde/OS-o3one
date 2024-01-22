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
/*   Various address conversions					*/
/*									*/
/*	ma = machine (real world physical) address			*/
/*	pa = pseudo-physical address					*/
/*	sa = static address (va that Xen originally mapped page at)	*/
/*	va = virtual address (any currently valid virtual address)	*/
/*									*/
/*   To pronounce correctly, for example, matopa, its ma-TOE-pa		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"

#include "oz_hwxen_defs.h"

OZ_Pointer oz_hwxen_matopa (OZ_Pointer ma)

{
  uLong pa;

  pa = (machine_to_phys_mapping[ma>>12] << 12) | (ma & 0xFFF);			// convert machine to pseudo-physical address
  if (oz_hwxen_patoma (pa) != ma) oz_crash ("oz_hwxen_matopa: ma %X not ours", ma); // make sure it's one of our machine addresses
  return (pa);
}

OZ_Pointer oz_hwxen_matosa (OZ_Pointer ma)

{
  uLong pa;

  pa = (machine_to_phys_mapping[ma>>12] << 12) | (ma & 0xFFF);			// convert machine to pseudo-physical address
  if (oz_hwxen_patoma (pa) != ma) oz_crash ("oz_hwxen_matosa: ma %X not ours", ma); // make sure it's one of our machine addresses
  return (pa + PP0SA);								// convert pseudo-physical to virtual address
}

OZ_Pointer oz_hwxen_patoma (OZ_Pointer pa)

{
  if ((pa >> 12) >= oz_hwxen_startinfo.nr_pages) oz_crash ("oz_hwxen_patoma: %X out of range", pa); // make sure pseudo-physical address is in range
  return (oz_hwxen_vatoma (pa + PP0SA));					// convert to virtual then to machine address
}

OZ_Pointer oz_hwxen_patosa (OZ_Pointer pa)

{
  if ((pa >> 12) >= oz_hwxen_startinfo.nr_pages) oz_crash ("oz_hwxen_patosa: %X out of range", pa); // make sure pseudo-physical address is in range
  return (pa + PP0SA);								// simple addition converts it
}

OZ_Pointer oz_hwxen_satoma (OZ_Pointer sa)

{
  return (oz_hwxen_vatoma (sa));
}

OZ_Pointer oz_hwxen_satopa (OZ_Pointer sa)

{
  if (((sa >> 12) - PP0SP) >= oz_hwxen_startinfo.nr_pages) oz_crash ("oz_hwxen_satopa: %X out of range", sa);
  return (sa - PP0SA);
}

OZ_Pointer oz_hwxen_vatoma (OZ_Pointer va)

{
  uLong pdentry, ptentry, *ptpagsa;

  pdentry = CPDVA[va>>22];							// read pagedirectory entry
  if (!(pdentry & PD_P)) oz_crash ("oz_hwxen_vatoma: va %X pde %X not present", va, pdentry); // pagedirectory entry not valid
  if (pdentry & PD_4M) return ((pdentry & 0xFFC00000) | (va & 0x003FFFFF));	// 4Meg page entry
  ptentry = CPTVA[va>>12];							// get pagetable entry
  if (!(ptentry & PT_P)) oz_crash ("oz_hwxen_vatoma: va %X pte %X not present", va, ptentry); // pagetable entry not valid
  return ((ptentry & 0xFFFFF000) | (va & 0x00000FFF));				// return corresponding machine address
}

OZ_Pointer oz_hwxen_vatopa (OZ_Pointer va)

{
  OZ_Pointer ma;

  ma = oz_hwxen_vatoma (va);							// get machine address
  return (oz_hwxen_matopa (ma));						// convert machine to pseudo-physical
}

OZ_Pointer oz_hwxen_vatosa (OZ_Pointer va)

{
  OZ_Pointer ma;

  ma = oz_hwxen_vatoma (va);							// get machine address
  return (oz_hwxen_matosa (ma));						// convert machine to static
}
