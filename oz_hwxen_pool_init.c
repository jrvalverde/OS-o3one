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
/*  Initialize non-paged pool area					*/
/*									*/
/*    Input:								*/
/*									*/
/*	*pages_required = minimum number of pages required		*/
/*									*/
/*    Output:								*/
/*									*/
/*	*pages_required = actual pages set up				*/
/*	*first_physpage = first physical page number used		*/
/*	*first_virtpage = first virtual page number used		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"

void oz_hw_pool_init (OZ_Mempage *pages_required, OZ_Mempage *first_physpage, OZ_Mempage *first_virtpage)

{
  OZ_Mempage firstphyspage, freephyspages, pagesrequired;

  /* Normally, round to 4Meg boundary.  But since Xen doesn't support 4Meg pages, don't bother. */
  /* We'd also have to group the pages (probably using swappages) in the start routine.         */

  pagesrequired   = *pages_required;
  //pagesrequired = (pagesrequired + 1023) & -1024;
  *pages_required = pagesrequired;

  /* Use pseudo-physical pages on the very high end of what's available */

  firstphyspage   = oz_s_phymem_totalpages - pagesrequired;
  *first_physpage = firstphyspage;

  /* Return its static page number as the pool's virtual page number                                           */
  /* Also decrement oz_s_phymem_totalpages as oz_knl_phymem_init won't see them when it scans through its loop */

  *first_virtpage = oz_hwxen_patosa (firstphyspage << 12) >> 12;
  oz_s_phymem_totalpages -= pagesrequired;

  oz_knl_printk ("oz_hw_pool_init: 0x%X pages, ppage 0x%X, vpage 0x%X\n", pagesrequired, firstphyspage, *first_virtpage);
}
