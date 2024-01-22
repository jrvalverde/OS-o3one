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
/*  Physical memory access routines					*/
/*									*/
/*  The modules that call these are using pseudo-physical address and	*/
/*  page numbers.  Since we still have our initial mapping intact that 	*/
/*  we were given by Xen, all we have to do is take the given pseudo-	*/
/*  physical address and add PP0SA to get the corresponding virtual 	*/
/*  address and we can access it that way.  We do not need an pte to 	*/
/*  map the physical page.						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"

#include "oz_hwxen_defs.h"

void oz_hw486_phys_filllong (uLong fill, OZ_Phyaddr phyaddr)

{
  asm ("cld ; rep stosl" : : "a" (fill), "c" (PAGESIZE / 4), "D" (phyaddr + PP0SA));
}

void oz_hw486_phys_storelong (uLong value, OZ_Phyaddr phyaddr)

{
  *(uLong *)(phyaddr + PP0SA) = value;
}

uLong oz_hw486_phys_fetchlong (OZ_Phyaddr phyaddr)

{
  return (*(uLong *)(phyaddr + PP0SA));
}

void oz_hw_phys_initpage (OZ_Mempage phypage, OZ_Mempage ptvirtpage)

{
  OZ_Hw_pageprot pageprot;
  OZ_Mempage npages, pageoffs, svpage, vpage;
  OZ_Process *process;
  OZ_Procmode procmode;
  OZ_Section *section;
  uByte *reqprotbytes, pageprotbyte;
  uLong mapsecflags;

  /* Always zero fill the page to start */

  memset ((void *)((phypage << 12) + PP0SA), 0, PAGESIZE);

  /* If its a reqprot page, fill in initial values from the protection of the sections that it maps */

  if ((ptvirtpage >= PPREQPROT_VPAGE) && (ptvirtpage < PPPAGETBL_VPAGE)) {
    svpage = vpage = (ptvirtpage - PPREQPROT_VPAGE) * 16384;						// get vpage assoc with first reqprot bitpair
    reqprotbytes = (uByte *)((phypage << 12) + PP0SA);							// point to reqprot table just zeroed out
    process = oz_s_systemproc;
    if (!OZ_HW_ISSYSPAGE (svpage)) process = oz_knl_process_getcur ();
    while ((npages = oz_knl_process_getsecfromvpage2 (process, &vpage, &section, &pageoffs, 		// see if there are any sections mapped by it
                                                      &pageprot, &procmode, &mapsecflags)) != 0) {
      if (vpage >= svpage + 16384) break;
      if (npages + vpage >= svpage + 16384) npages = svpage + 16384 - vpage;
      pageprotbyte = pageprot * 0x55;									// ok, make up a byte of reqprot values
      if (((vpage & 3) + npages) <= 4) {								// see if it all fits in a single reqprot byte
        reqprotbytes[(vpage-svpage)/4] |= (pageprotbyte >> (8 - npages * 2)) << ((vpage & 3) * 2);	// if so, merge in with what's there
        vpage += npages;										// check for more sections
        continue;
      }
      if (vpage & 3) {											// see if it starts in middle of byte
        reqprotbytes[(vpage-svpage)/4] |= pageprotbyte << ((vpage & 3) * 2);				// if so, set the upper bits
        npages += (vpage & 3) + -4;									// increment past those pages
        vpage   = (vpage + 3) & -4;
      }
      if (npages >= 4) {										// see if there are any whole bytes to set
        memset (reqprotbytes + (vpage - svpage) / 4, pageprotbyte, npages / 4);				// if so, set them
        vpage  += npages & -4;										// increment past those pages
        npages &= 3;
      }
      if (npages > 0) {											// see if there are any left on the end
        reqprotbytes[(vpage-svpage)/4] |= pageprotbyte >> (8 - npages * 2);				// if so, set the lower bits
        vpage += npages;										// increment past those pages
      }
    }
  }
}

void *oz_hw_phys_mappage (OZ_Mempage phypage, OZ_Pagentry *savepte)

{
  return ((void *)((phypage << 12) + PP0SA));
}

void oz_hw_phys_unmappage (OZ_Pagentry savepte) {}

void oz_hw_phys_movefromvirt (uLong nbytes, const void *vaddr, const OZ_Mempage *phypages, uLong byteoffs)

{
  uLong ncopy;

  while (nbytes > 0) {
    phypages += byteoffs / PAGESIZE;
    byteoffs %= PAGESIZE;
    ncopy = PAGESIZE - byteoffs;
    if (ncopy > nbytes) ncopy = nbytes;
    memcpy ((void *)((*phypages << 12) + PP0SA + byteoffs), vaddr, ncopy);
    nbytes -= ncopy;
    byteoffs += ncopy;
    (OZ_Pointer)vaddr += ncopy;
  }
}

void oz_hw_phys_movetovirt (uLong nbytes, void *vaddr, const OZ_Mempage *phypages, uLong byteoffs)

{
  uLong ncopy;

  while (nbytes > 0) {
    phypages += byteoffs / PAGESIZE;
    byteoffs %= PAGESIZE;
    ncopy = PAGESIZE - byteoffs;
    if (ncopy > nbytes) ncopy = nbytes;
    memcpy (vaddr, (void *)((*phypages << 12) + PP0SA + byteoffs), ncopy);
    nbytes   -= ncopy;
    byteoffs += ncopy;
    (OZ_Pointer)vaddr += ncopy;
  }
}

void oz_hw_phys_movephys (uLong nbytes, const OZ_Mempage *src_pages, uLong src_offs, const OZ_Mempage *dst_pages, uLong dst_offs)

{
  uLong ncopy;

  while (nbytes > 0) {
    src_pages += src_offs / PAGESIZE;
    dst_pages += dst_offs / PAGESIZE;
    src_offs  %= PAGESIZE;
    dst_offs  %= PAGESIZE;
    ncopy = PAGESIZE - src_offs;
    if (ncopy > PAGESIZE - dst_offs) ncopy = PAGESIZE - dst_offs;
    if (ncopy > nbytes) ncopy = nbytes;
    memcpy ((void *)((*dst_pages << 12) + PP0SA + dst_offs), 
            (void *)((*src_pages << 12) + PP0SA + src_offs), 
            ncopy);
    nbytes   -= ncopy;
    src_offs += ncopy;
    dst_offs += ncopy;
  }
}
