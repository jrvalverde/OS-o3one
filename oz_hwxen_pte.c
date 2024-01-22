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
/*  Pagetable access							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_hw_bootblock.h"
#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_ldr_params.h"
#include "oz_sys_dateconv.h"

#include "oz_hwxen_defs.h"
#include "oz_hwxen_hypervisor.h"

static OZ_Mempage checkptpage (OZ_Mempage vpage, int unmap, OZ_Mempage skipthispage);

/************************************************************************/
/*									*/
/*  An pagetable page is about to be paged out or unmapped.  Check it 	*/
/*  to see that all pages it maps are also paged out or unmapped.  If 	*/
/*  it finds any that aren't, return the vpage of one that isn't.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = virt page number of pagetable page that is about to be 	*/
/*	        paged out or unmapped					*/
/*	unmap = 0 : paging it out, just check for 			*/
/*	            pagestate=PAGEDOUT and curprot=NA			*/
/*	        1 : unmapping it, also check for phypage=0		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_pte_checkptpage = 0 : all pages are ok			*/
/*	                     else : at least this page needs attention	*/
/*									*/
/************************************************************************/

OZ_Mempage oz_hw_pte_checkptpage (OZ_Mempage vpage, int unmap)

{
  uLong i, inpage;

  /* If taking out an reqprot page, make sure the corresponding pages are out */
  /* Process cleanup gets stuck in an infinite loop without this check        */

  vpage -= PPREQPROT_VPAGE;
  if (vpage < 64) {
    for (i = 0; i < 16; i ++) {							// reqprot pages are 16x as dense as pagetable pages
      inpage = checkptpage (vpage * 16 + i, unmap, vpage + PPREQPROT_VPAGE);	// check out the corresponding pagetable page
      if (inpage != 0) return (inpage);						// if it has something to swap out, swap it out
    }
    return (0);									// all pagetable pages are clear, then so are we
  }

  /* Check out normal pagetable page */

  vpage -= PPPAGETBL_VPAGE - PPREQPROT_VPAGE;		// get number of page within pagetable to be scanned
  if (vpage >= 1024) return (0);			// if not pt page, say it's empty
  return (checkptpage (vpage, unmap, 0));		// go check it out
}

/* Scan actual pagetable page                                         */
/*   vpage = page within pagetable to scan                            */
/*   unmap = as above                                                 */
/*   skipthispage = don't return this page number, just keep scanning */

static OZ_Mempage checkptpage (OZ_Mempage vpage, int unmap, OZ_Mempage skipthispage)

{
  uLong i, mask, *ptpva;

  if (vpage == (PPPAGETBL_VPAGE >> 10)) return (0);	// maybe they're scanning the pagedirectory page itself
							// if so, say it is empty, its pages get unloaded as the rest of the pt gets scanned

  if (!(CPDVA[vpage] & PD_P)) return (0);		// make sure the pagetable page is paged in
							// if it isn't, it can't be referencing any pages

  vpage <<= 10;						// data page mapped by the first pte of the pagetable page
  ptpva   = CPTVA + vpage;				// point to pagetable entry for that data page
							// = start of pagetable page we're supposed to scan
  mask    = (7 << 9) | PT_P;				// always check software pagestate and the present bit
  if (unmap) mask |= 0xFFFFF000;			// if unmapping, phypage must also be zero
  for (i = 0; i < 1024; i ++) {				// scan the whole pagetable page
    if ((vpage != skipthispage) && ((*ptpva & mask) != (OZ_SECTION_PAGESTATE_PAGEDOUT << 9))) return (vpage);
    ptpva ++;
    vpage ++;
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Read pagetable entry						*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpn = virtual pagenumber to read the pte of			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_pte_readany = NULL : successfully read			*/
/*	                    else : va that needs to be faulted in	*/
/*	*pagestate_r = software pagestate (VALID_W, READINPROG, etc)	*/
/*	*phypage_r   = physical pagenumber				*/
/*	*curprot_r   = current page protection (UR, KW, etc)		*/
/*	*reqprot_r   = requested page protection			*/
/*									*/
/*    Note:								*/
/*									*/
/*	In the Xen implementation, phypage refers to the PSEUDO-	*/
/*	physical page number						*/
/*									*/
/************************************************************************/

void *oz_hw_pte_readany (OZ_Mempage vpn,
                         OZ_Section_pagestate *pagestate_r,
                         OZ_Mempage *phypage_r,
                         OZ_Hw_pageprot *curprot_r,
                         OZ_Hw_pageprot *reqprot_r)

{
  uLong pdentry, ptentry, *ptentva, reqprotbyteva;

  /* If system global page, use the system pte read routine */

  if (vpn >= PP0SP) {
    oz_hw_pte_readsys (vpn, pagestate_r, phypage_r, curprot_r, reqprot_r);
    return (NULL);
  }

  /* It's a per-process page */

  if ((pagestate_r != NULL) || (phypage_r != NULL) || (curprot_r != NULL)) {

    /* If caller is trying to read the self-reference PTE, return zero.  Do this because we don't want the higher-level       */
    /* routines messing with it at all.  Specifically, the process termination would try to free the page off before the rest */
    /* of the pagetable is freed off which is no good.  We explicitly free the pagedirectory page in oz_hw_process_termctx.   */
    /* Hey, checking oz_s_phymem_pages[phypage].u.s.ptrefcount==1 in oz_knl_section_unmappage actually paid off somewhere.    */

    if (vpn == PPPAGETBL_VPAGE + (PPPAGETBL_VPAGE >> 10)) ptentry = 0;	// phantom self-ref pointer

    /* Read pagetable entry.  If PDE indicates pagetable page is 'not present'       */
    /* return vaddr of the pagetable page so caller will know to fault in that page. */

    else {
      ptentva = CPTVA + vpn;						// this is the VA of PTE in self-ref pointer mapping
      pdentry = CPDVA[vpn>>10];						// make sure it's paged in by checking corresponding PDE
      if (!(pdentry & PD_P)) return (ptentva);				// tell caller to fault in the pagetable page if not present
      ptentry = *ptentva;						// read pagetable entry from self-ref pointer mapping
    }

    /* Extract requested values from pagetable entry */

    if (pagestate_r != NULL) *pagestate_r = (ptentry >> 9) & 7;		// extract software pagestate
    if (curprot_r   != NULL) *curprot_r   = (ptentry >> 1) & 3;		// extract current protection

    /* If caller wants phypage, translate machine to pseudo-physical iff PT_P is set */

    if (phypage_r != NULL) {
      if (!(ptentry & PT_P)) *phypage_r = ptentry >> 12;
      else *phypage_r = oz_hwxen_matopa (ptentry) >> 12;
    }
  }

  /* See if caller wants the requested protection bits */

  if (reqprot_r != NULL) {

    /* If asking for bits for pages not in the pagetable section, fetch them from the array */

    if (vpn >= PPVPAGE) {
      reqprotbyteva = (PPREQPROT_VPAGE << 12) + (vpn / 4);		// ok, this is the va of the byte where they are
      ptentva = ((uLong *)(PPPAGETBL_VPAGE << 12)) + OZ_HW_VADDRTOVPAGE (reqprotbyteva);
      pdentry = CPDVA[reqprotbyteva>>22];				// see if pagedirectory entry says it's pt page is present
      if (!(pdentry & PD_P)) return (ptentva);				// if not, tell caller to fault in it's pt page
      ptentva = ((uLong *)oz_hwxen_matosa (pdentry & 0xFFFFF000)) + (OZ_HW_VADDRTOVPAGE (reqprotbyteva) & 1023);
      ptentry = *ptentva;						// see if pagetable entry says it is present
      if (!(ptentry & PT_P)) return ((void *)reqprotbyteva);		// if not, tell caller to fault in reqprotbyte page
      *reqprot_r = ((*(uByte *)reqprotbyteva) >> ((vpn & 3) * 2)) & 3;	// get the requested protection bit pair
    }

    /* If asking for bits for pages within pagetable section, just return KW (or UW for the usermode oz_sys_pdata_array element) */
    /* This will avoid infinite loop nesting to fault them in                                                                    */

    else {
      *reqprot_r = OZ_HW_PAGEPROT_KW;					// whole pagetable section itself is KW
      if (vpn == PDATA_USR_VPAGE) *reqprot_r = OZ_HW_PAGEPROT_UW;	// except usermode oz_sys_pdata page is UW
    }
  }

  /* All successful, nothing needs to be faulted in */

  return (NULL);
}

/*******************************************************************************************/
/* This one can only read system pagetable entries and crashes if something is faulted out */
/*******************************************************************************************/

void oz_hw_pte_readsys (OZ_Mempage vpn,
                        OZ_Section_pagestate *pagestate_r,
                        OZ_Mempage *phypage_r,
                        OZ_Hw_pageprot *curprot_r,
                        OZ_Hw_pageprot *reqprot_r)

{
  uLong pa, ptentry;

  if (vpn < PP0SP) oz_crash ("oz_hw_pte_readsys: vpn %X not a system page", vpn);
  if (vpn >= TOPVP) oz_crash ("oz_hw_pte_readsys: vpn %X too big", vpn);

  vpn -= PP0SP;
  ptentry = oz_hwxen_sptsa[vpn];

  if (pagestate_r != NULL) *pagestate_r = (ptentry >> 9) & 7;				// return software page state
  if (phypage_r   != NULL) {								// return pseudo-physical page number
    if (!(ptentry & PT_P)) *phypage_r = ptentry >> 12;
    else *phypage_r = oz_hwxen_matopa (ptentry) >> 12;
  }
  if (curprot_r != NULL) *curprot_r = (ptentry >> 1) & 3;				// return current page protection
  if (reqprot_r != NULL) *reqprot_r = (oz_hwxen_sysreqprots[vpn/16] >> ((vpn & 15) * 2)) & 3; // return requested page protection
}

/************************************************************************/
/*									*/
/*  Write pagetable entry						*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpn       = virtual page number that PTE maps			*/
/*	pagestate = software page state (VALID_W, READINPROG, etc)	*/
/*	phypage   = physical page to map to				*/
/*	curprot   = current protection to apply to page (UR, KW, etc)	*/
/*	reqprot   = requested protection for page			*/
/*	smplevel  = usually PT but this routine doesn't care		*/
/*									*/
/*    Output:								*/
/*									*/
/*	pte written or crash if can't					*/
/*									*/
/*    Note:								*/
/*									*/
/*	In the Xen implementation, phypage refers to the PSEUDO-	*/
/*	physical page number						*/
/*									*/
/************************************************************************/

void oz_hwxen_pte_write (OZ_Mempage vpn,
                         OZ_Section_pagestate pagestate,
                         OZ_Mempage phypage,
                         OZ_Hw_pageprot curprot,
                         OZ_Hw_pageprot reqprot);

	/* Invalidate on all CPUs */

void oz_hw_pte_writeall (OZ_Mempage vpn,
                         OZ_Section_pagestate pagestate,
                         OZ_Mempage phypage,
                         OZ_Hw_pageprot curprot,
                         OZ_Hw_pageprot reqprot)

{
  oz_hw_pte_writecur (vpn, pagestate, phypage, curprot, reqprot);
}

	/* Invalidate on current CPU only */

void oz_hw_pte_writecur (OZ_Mempage vpn,
                         OZ_Section_pagestate pagestate,
                         OZ_Mempage phypage,
                         OZ_Hw_pageprot curprot,
                         OZ_Hw_pageprot reqprot)

{
  /* Translate given pseudo-physical page number to machine page number iff page will be accessable            */
  /* This allows OS to write anything (like disk page number) in the ppn field when the page is not accessable */

  if (curprot != OZ_HW_PAGEPROT_NA) phypage = oz_hwxen_patoma (phypage << 12) >> 12;

  oz_hwxen_pte_write (vpn, pagestate, phypage, curprot, reqprot);
}

/* This one always interprets 'phypage' as a machine (real-world) page number */

void oz_hwxen_pte_write (OZ_Mempage vpn,
                         OZ_Section_pagestate pagestate,
                         OZ_Mempage phypage,
                         OZ_Hw_pageprot curprot,
                         OZ_Hw_pageprot reqprot)

{
  int i, rc;
  mmu_update_t ureqs[6];
  uLong mask, newptentry, newreqprotlong, oldptentry, oldreqprotlong, pa, *pteva, shiftedreqprot;
  uLong volatile *reqprotlong;

  if (vpn >= TOPVP) oz_crash ("oz_hwxen_pte_write: vpn %X too big", vpn);

  newptentry = phypage << 12;								// set up machine address
  if (curprot != OZ_HW_PAGEPROT_NA) newptentry |= curprot + curprot + 1;		// ... then put in page protection
  newptentry |= pagestate << 9;								// pack software pagestate in there

  if (vpn >= PP0SP) newptentry |= PT_G;							// if system global page, mark it global

  /* If writing pagetable entry for a pagetable page, we must change static mapping to read-only */
  /* Xen will not allow us to have write access to any pagetable page                            */
  /* Also, if this is a new pagetable page, we should pin it                                     */

  i = 0;

  if ((vpn - PPPAGETBL_VPAGE) < 1024) {							// see if this is pte for a process private pagetable page
    if (vpn == PPPAGETBL_VPAGE + (PPPAGETBL_VPAGE >> 10)) {				// ok, see if writing self-reference pointer
      if (newptentry == 0) return;							// - zero is OK because that's what oz_hw_pte_read returns
      oz_crash ("oz_hwxen_pte_write: trying to write ppd pointer = %8.8X", newptentry);	//   anything else is puque
    }
    pteva = CPDVA + (vpn - PPPAGETBL_VPAGE);						// get current ppd entry
    oldptentry  = *pteva;
    if (newptentry & PT_P) newptentry |= PT_W | PT_U;					// pagedirectory entries must have UW
    if ((newptentry & 0xFFFFFE07) == (oldptentry & 0xFFFFFE07)) goto updreqprot;	// skip if not changing anything significant
    if (newptentry & PT_P) {
      pa = oz_hwxen_matopa (newptentry);						// change static pte to read-only
      ureqs[i].ptr   = (uLong)(oz_hwxen_sptsa + (pa >> 12)) + MMU_NORMAL_PT_UPDATE;
      ureqs[i++].val = newptentry & ~PT_W;
      ureqs[i].ptr   = (newptentry & 0xFFFFF000) + MMU_EXTENDED_COMMAND;		// tell Xen we're going to use it for ptpage
      ureqs[i++].val = MMUEXT_PIN_L1_TABLE;
    }
  }

  /* Writing a standard pagetable entry */

  else {
    pa = CPDVA[vpn>>10];								// get pagetable page's machine address
    if (!(pa & PD_P)) oz_crash ("oz_hwxen_pte_write: pagetable page for %X not present (%X)", vpn, pa);
    pteva = CPTVA + vpn;								// point to entry we are going to write
    oldptentry = *pteva;
    if ((newptentry & 0xFFFFFE07) == (oldptentry & 0xFFFFFE07)) goto updreqprot;
  }

  /* Write pagetable entry */

  ureqs[i].ptr   = ((uLong)pteva) + MMU_NORMAL_PT_UPDATE;				// vaddr of pte to update
  ureqs[i++].val = newptentry;								// new pagetable entry
  ureqs[i].ptr   = MMU_EXTENDED_COMMAND;						// invalidate old TLB entry
  ureqs[i++].val = (vpn << 12) + MMUEXT_INVLPG;

  /* If we just dumped an old pagetable page, unpin it and allow read/write access in permanent mapping */

  if ((vpn - PPPAGETBL_VPAGE) < 1024) {							// see if this is pte for a process private pagetable page
    if ((oldptentry & PT_P) && ((newptentry ^ oldptentry) & 0xFFFFF001)) {		// see if we dumped an old page
      ureqs[i].ptr   = (oldptentry & 0xFFFFF000) + MMU_EXTENDED_COMMAND;		// if so, unpin it
      ureqs[i++].val = MMUEXT_UNPIN_TABLE;
      pa = oz_hwxen_matopa (oldptentry);						// ... and allow read/write in static mapping
      ureqs[i].ptr   = (uLong)(oz_hwxen_sptsa + (pa >> 12)) + MMU_NORMAL_PT_UPDATE;
      ureqs[i++].val = (oldptentry & 0xFFFFF000) | PT_KW;
    }
  }

  /* Send updates to Xen */

  rc = HYPERVISOR_mmu_update (ureqs, i);
  if (rc != 0) oz_crash ("oz_hwxen_pte_write: HYPERVISOR_mmu_update error %d writing %X pte with %X", rc, vpn, newptentry);

  /* Write requested protection bits */

updreqprot:
  if (vpn >= PPVPAGE) {									// the process pagetable section has no reqprot bits
    if (vpn >= PP0SP) {									// check for system global pages
      reqprotlong = oz_hwxen_sysreqprots + (vpn - PP0SP) / 16;				// ok, point to long for them
    } else {
      reqprotlong = ((uLong *)OZ_HW_VPAGETOVADDR (PPREQPROT_VPAGE)) + vpn / 16;		// point to process private reqprot array element
    }

    vpn  = (vpn & 15) * 2;
    mask = ~(3 << vpn);									// get long mask for the two bits
    shiftedreqprot = ((uLong)reqprot) << vpn;						// get value we want to insert
    do {
      oldreqprotlong = *reqprotlong;							// sample the reqprot long
      newreqprotlong = (oldreqprotlong & mask) | shiftedreqprot;			// make new value
    } while (!oz_hw_atomic_setif_ulong (reqprotlong, newreqprotlong, oldreqprotlong));	// store if hasn't changed, repeat if changed
  }
}

/************************************************************************/
/*									*/
/*  Determine if a particular null terminated string is readable 	*/
/*  in the current memory mapping by the given processor mode 		*/
/*									*/
/*    Input:								*/
/*									*/
/*	 4(%esp) = address of area					*/
/*	 8(%esp) = processor mode					*/
/*									*/
/*    Output:								*/
/*									*/
/*	%eax = 0 : string is not accessible				*/
/*	       1 : string is accessible					*/
/*									*/
/************************************************************************/

int oz_hw_prober_strz (const void *adrz, OZ_Procmode mode)

{
  char const *p;
  uLong mask, pte, *ptpva, vp;

  p = adrz;							// point to string to scan
  ptpva = NULL;							// need to read pde

  mask = PT_P;							// always need 'page present' bit
  if (mode == OZ_PROCMODE_USR) mask |= PT_U;			// maybe need 'user accessable' bit

  while (1) {							// repeat while there's more to check
    vp = OZ_HW_VADDRTOVPAGE (p);				// get page number
    if (ptpva == NULL) {					// see if we need to read pagedirectory entry
      pte = CPDVA[vp>>10];					// ok, get pagedirectory entry
      if ((pte & mask) != mask) return (0);			// it must have all required bits
      if (pte & PD_4M) {					// see if it's a 4Meg page
        do if (*(p ++) == 0) return (1);			// ok, scan for end of string
        while (((OZ_Pointer)p & 0x3FFFFF) != 0);		// up to end of page
        continue;
      }
      ptpva = CPTVA + (vp & -1024);				// 4K pages, point to pagetable page
    }

    pte = ptpva[vp&1023];					// read pagetable entry
    if ((pte & mask) != mask) return (0);			// make sure it has all required bits
    do if (*(p ++) == 0) return (1);				// scan for end of string
    while (((OZ_Pointer)p & 0xFFF) != 0);			// up to end of page
  }
  return (1);
}

/************************************************************************/
/*									*/
/*  Determine if a particular range of addesses is accessible 		*/
/*  in the current memory mapping by the given processor mode 		*/
/*									*/
/*    Input:								*/
/*									*/
/*	 4(%esp) = size of area						*/
/*	 8(%esp) = address of area					*/
/*	12(%esp) = processor mode					*/
/*	16(%esp) = 0 : validate for read				*/
/*	           1 : validate for write				*/
/*									*/
/*    Output:								*/
/*									*/
/*	%eax = 0 : location is not accessible				*/
/*	       1 : location is accessible				*/
/*									*/
/************************************************************************/

int oz_hw_probe (uLong size, const void *adrs, OZ_Procmode mode, int write)

{
  OZ_Pointer endaddr;
  uLong mask, pte, *ptpva, vp;

  if (size == 0) return (1);					// null buffers are always good
  endaddr = ((OZ_Pointer)adrs) + size - 1;			// point to last byte to check, inclusive
  if (endaddr < (OZ_Pointer)adrs) return (0);			// fail if it wraps
  if (endaddr >= TOPVA) return (0);				// fail if it's out of range
  vp = OZ_HW_VADDRTOVPAGE (adrs);				// get starting page number
  size = OZ_HW_VADDRTOVPAGE (endaddr) - vp + 1;			// get number of pages to check

  ptpva = NULL;							// need to read pde

  mask = PT_P;							// always need 'page present' bit
  if (write) mask |= PT_W;					// maybe need 'page writeable' bit
  if (mode == OZ_PROCMODE_USR) mask |= PT_U;			// maybe need 'user accessable' bit

  while (size > 0) {						// repeat while there's more to check
    if (ptpva == NULL) {					// see if we need to read pagedirectory entry
      pte = CPDVA[vp>>10];					// ok, get pagedirectory entry
      if ((pte & mask) != mask) return (0);			// it must have all required bits
      if (pte & PD_4M) {					// see if it's a 4Meg page
        pte = 1024 - (vp & 1023);				// ok, get number of 4K pages to boundary
        if (size <= pte) break;					// stop if that's all that's left
        size -= pte;						// decrement number of remaining pages
        vp   += pte;						// increment virtual page number
        continue;
      }
      ptpva = CPTVA + (vp & -1024);				// 4K pages, point to pagetable page
    }

    pte = ptpva[vp&1023];					// read pagetable entry
    if ((pte & mask) != mask) return (0);			// make sure it has all required bits
    -- size;							// ok, one less page to be checked
    if ((++ vp & 1023) == 0) ptpva = NULL;			// increment virt page number, if crosses 4M boundary, need new pde
  }
  return (1);
}

/************************************************************************/
/*									*/
/*  Set a page's protection						*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = virtual page to mark					*/
/*	pageprot = new protection					*/
/*									*/
/************************************************************************/

void oz_hwxen_markpageprot (OZ_Mempage vpage, uLong pageprot)

{
  int rc;
  uLong ptentry;

  if (vpage >= 0xFC000) oz_crash ("oz_hwxen_markpageprot: vpage %X out of range", vpage);
  ptentry = oz_hwxen_vatoma (vpage << 12) | pageprot;
  rc = HYPERVISOR_update_va_mapping (vpage, ptentry, UVMF_INVLPG);
  if (rc != 0) oz_crash ("oz_hwxen_markpageprot: HYPERVISOR_update_va_mapping error %d writing 0x%X with 0x%X", rc, vpage, ptentry);
}

/************************************************************************/
/*									*/
/*  Pin and Unpin page table and directory pages			*/
/*									*/
/************************************************************************/

void oz_hwxen_pinpdpage (OZ_Pointer pdpma)

{
  int rc;
  mmu_update_t ureqs[0];

  ureqs[0].ptr = MMU_EXTENDED_COMMAND | pdpma;
  ureqs[0].val = MMUEXT_PIN_L2_TABLE;
  rc = HYPERVISOR_mmu_update (ureqs, 1);
  if (rc != 0) oz_crash ("oz_hwxen_pinpdpage: HYPERVISOR_mmu_update error %d pinning dir page %X", rc, pdpma);
}

void oz_hwxen_pinptpage (OZ_Pointer ptpma)

{
  int rc;
  mmu_update_t ureqs[0];

  ureqs[0].ptr = MMU_EXTENDED_COMMAND | ptpma;
  ureqs[0].val = MMUEXT_PIN_L1_TABLE;
  rc = HYPERVISOR_mmu_update (ureqs, 1);
  if (rc != 0) oz_crash ("oz_hwxen_pinptpage: HYPERVISOR_mmu_update error %d pinning table page %X", rc, ptpma);
}

void oz_hwxen_unpinpage (OZ_Pointer pma)

{
  int rc;
  mmu_update_t ureqs[0];

  ureqs[0].ptr = MMU_EXTENDED_COMMAND | pma;
  ureqs[0].val = MMUEXT_UNPIN_TABLE;
  rc = HYPERVISOR_mmu_update (ureqs, 1);
  if (rc != 0) oz_crash ("oz_hwxen_unpinpage: HYPERVISOR_mmu_update error %d unpinning page %X", rc, pma);
}
