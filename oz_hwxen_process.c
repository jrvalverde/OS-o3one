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
/*  Xen process context							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"

#include "oz_hwxen_defs.h"
#include "oz_hwxen_hypervisor.h"

typedef struct { OZ_Phyaddr ppdma;		// page directory machine (real-world physical) address
                 uLong *ppdsa;			// page directory static address
                 OZ_Section *ptsec;		// pagetable section
                 OZ_Quota *quota;		// quota for the pagedirectory page
               } Prctx;

static Prctx *system_process_hw_ctx = NULL;

void oz_hwxen_definesyspdataarry (void)

{
  /* Tell any linker that's interested where oz_sys_pdata is */

  asm volatile ("	.globl	oz_sys_pdata_array\n"
		"	.type	oz_sys_pdata_array,@object\n"
		"	oz_sys_pdata_array = %c0\n" : : "i" (PDATA_KNL_VPAGE*4096));
}

static uLong init_pdata (void *dummy);

/************************************************************************/
/*									*/
/*  This routine sets up the pagetables for a new process		*/
/*									*/
/*  This is the routine that sets up the per-process addressing layout	*/
/*									*/
/*    Input:								*/
/*									*/
/*	process_hw_ctx = points to Prctx struct within process struct	*/
/*	process = points to software process structure			*/
/*	sysproc = 0 : this is a normal process				*/
/*	          1 : this is the system process			*/
/*	copyproc = 0 : create empty process				*/
/*	           1 : create process with pagetables equal to		*/
/*	               current process					*/
/*	smplock level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_process_initctx = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*process_hw_ctx = filled in					*/
/*									*/
/************************************************************************/

uLong oz_hw_process_initctx (void *process_hw_ctx, OZ_Process *process, int sysproc, int copyproc)

{
  int rc;
  mmu_update_t ureqs[2];
  OZ_Mempage const *newpdatapages;
  OZ_Mempage npagem, phypage, svpage;
  OZ_Process *oldprocess;
  OZ_Seclock *seclock;
  OZ_Section *section;
  Prctx *prctx;
  uLong sts;

  prctx = process_hw_ctx;

  if (sizeof *prctx > OZ_PROCESS_HW_CTX_SIZE) {
    oz_crash ("oz_hw_process_initctx: sizeof *prctx (%d) > OZ PROCESS HW CTX SIZE (%d)", sizeof *prctx, OZ_PROCESS_HW_CTX_SIZE);
  }

  memset (prctx, 0, sizeof *prctx);

  /* The system process uses the global PD based at address oz_hwxen_mpdsa             */
  /* It just maps the addresses that are common to all processes (PP0SA <= va < TOPVA) */

  /* Also map sections to cover the image and static page mapping so the       */
  /* oz_knl_boot_firstcpu routine won't choke on the oz_hwxen_sharedinfo page. */

  if (sysproc) {
    prctx -> ppdma = oz_hwxen_vatoma ((OZ_Pointer)oz_hwxen_mpdsa);
    prctx -> ppdsa = oz_hwxen_mpdsa;
    prctx -> ptsec = NULL;
    sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, oz_s_sysmem_pagtblsz, OZ_HW_BASE_SYSC_VP);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u creating system process pagetable", sts);

    npagem = (OZ_IMAGE_RDWRADDR - OZ_IMAGE_BASEADDR) >> 12;
    sts = oz_knl_section_create (NULL, npagem, 0, OZ_SECTION_TYPE_ZEROES, NULL, &section);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u creating %u page read-only section", sts, npagem);
    svpage = OZ_HW_VADDRTOVPAGE (OZ_IMAGE_BASEADDR);
    sts = oz_knl_process_mapsection (section, &npagem, &svpage, OZ_MAPSECTION_EXACT | OZ_MAPSECTION_SYSTEM, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_UR);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u mapping %u page read-only section at %X", sts, npagem, svpage);

    npagem = oz_hwxen_startinfo.nr_pages - npagem;
    sts = oz_knl_section_create (NULL, npagem, 0, OZ_SECTION_TYPE_ZEROES, NULL, &section);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u creating %u page read/write section", sts, npagem);
    svpage = OZ_HW_VADDRTOVPAGE (OZ_IMAGE_RDWRADDR);
    sts = oz_knl_process_mapsection (section, &npagem, &svpage, OZ_MAPSECTION_EXACT | OZ_MAPSECTION_SYSTEM, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u mapping %u page read/write section at %X", sts, npagem, svpage);

    system_process_hw_ctx = prctx;
    return (sts);
  }

  /* Normal process, set up a section for mapping pagetable (max 4M), reqprot table (256K) and pdata pages (8K) */

  sts = oz_knl_section_create (NULL, PTSEC_NPAGEM, 0, OZ_SECTION_TYPE_PAGTBL | OZ_SECTION_TYPE_ZEROES, NULL, &(prctx -> ptsec));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_initctx: error %u creating pagetable section\n", sts);
    return (sts);
  }

  /* Make sure there is quota for a physical memory page for the pagedirectory page */

  prctx -> quota = oz_knl_section_getquota (prctx -> ptsec);
  if ((prctx -> quota != NULL) && !oz_knl_quota_debit (prctx -> quota, OZ_QUOTATYPE_PHM, 1)) {
    oz_knl_section_increfc (prctx -> ptsec, -1);
    return (OZ_EXQUOTAPHM);
  }

  /* Create pagetable structs - this is how we tell the kernel where it can map things for the process                           */
  /* We give it one that goes from the bottom of the oz_sys_pdata_array through the top per-process pagetable page               */
  /* We skip the range where the shadow of the system pagetable pages go, or the higher-level routines will try to free them off */
  /* Then we give it one that maps from above all the pagetable pages up to the kernel image, for normal process stuff           */

  sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, OZ_HW_BASE_SYSC_VP - PPVPAGE, PPVPAGE);
  if (sts == OZ_SUCCESS) sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, PTSEC_NPAGEM, PTSEC_SVPAGE);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_initctx: error %u creating process pagetable\n", sts);
    if (prctx -> quota != NULL) oz_knl_quota_credit (prctx -> quota, OZ_QUOTATYPE_PHM, 1, -1);
    oz_knl_section_increfc (prctx -> ptsec, -1);
    return (sts);
  }

  /* Allocate a page to use for the new process' page directory */

  phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, 0); // get its pseudo-physical page number
  prctx -> ppdma = oz_hwxen_patoma (phypage << 12);			// get ppd's machine (real-world physical) address
  prctx -> ppdsa = (uLong *)oz_hwxen_patosa (phypage << 12);		// get its static address
  memset (prctx -> ppdsa, 0, OZ_HW_BASE_SYSC_VP >> 8);			// this much is process-private 
									// (don't copy oz_s_systempdata mapping at oz_sys_pdata)
									// (and don't copy its self-reference pointer)
  memcpy (prctx -> ppdsa + (OZ_HW_BASE_SYSC_VP >> 10), 			// the rest is copy of master page directory
          oz_hwxen_mpdsa + (OZ_HW_BASE_SYSC_VP >> 10), 			// ... that never changes
          PAGESIZE - (OZ_HW_BASE_SYSC_VP >> 8));

  /* Set up a self-reference pointer so the pagetable appears at PPPAGETBL_VPAGE when the process is current */

  prctx -> ppdsa[PPPAGETBL_VPAGE>>10] = prctx -> ppdma | PD_UR;		// self-reference to give process private pagetable pages
									// ... their virtual address range
									// it also maps the ppd itself at virt address CPDVA

  /* Tell Xen we are going to use this page for a pagedirectory.  HYPERVISOR_mmu_update prints an error    */
  /* message about the self-referencing pointer but internally recovers from the error and does it anyway. */

  ureqs[0].ptr = MMU_NORMAL_PT_UPDATE | (uLong)(oz_hwxen_sptsa + (((uLong)(prctx -> ppdsa) - (uLong)PP0SA) >> 12));
  ureqs[0].val = prctx -> ppdma | PT_UR | PT_G;				// first off, we can't write to it directly any more
  ureqs[1].ptr = MMU_EXTENDED_COMMAND | prctx -> ppdma;			// now Xen knows we are going to use it for a directory
  ureqs[1].val = MMUEXT_PIN_L2_TABLE;
  rc = HYPERVISOR_mmu_update (ureqs, 2);
  if (rc != 0) oz_crash ("oz_hw_process_initctx: HYPERVISOR_mmu_update error %d pinning dir page %X", rc, prctx -> ppdma);

  /* Map the pagetable section to the process and initialize pdata pages */

  npagem = PTSEC_NPAGEM;
  svpage = PTSEC_SVPAGE;
  oldprocess = oz_knl_process_getcur ();				// save current high-level CR3
  oz_knl_process_setcur (process);					// switch CR3 to the pagedirectory page just created above
  sts = oz_knl_process_mapsection (prctx -> ptsec, &npagem, &svpage, OZ_MAPSECTION_EXACT | OZ_MAPSECTION_SYSTEM, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_hw_process_initctx: error %u mapping pagetable section %X at %X\n", sts, npagem, svpage);
  else {
    sts = oz_knl_section_setpageprot (1, PDATA_USR_VPAGE, OZ_HW_PAGEPROT_UW, NULL, NULL);
    if (sts == OZ_SUCCESS) {
      if (copyproc) {
        sts = oz_knl_section_iolock (OZ_PROCMODE_KNL, sizeof oz_sys_pdata_array, oz_sys_pdata_array, 1, &seclock, NULL, &newpdatapages, NULL);
      } else {
        sts = oz_sys_condhand_try (init_pdata, NULL, oz_sys_condhand_rtnanysig, NULL);
        if (sts == OZ_SUCCESS) sts = oz_knl_handletbl_create ();
      }
    }
    if (sts != OZ_SUCCESS) {
      oz_knl_process_unmapsec (svpage);
      oz_knl_printk ("oz_hw_process_initctx: error %u initializing oz_sys_pdata_array\n", sts);
    }
  }
  oz_knl_process_setcur (oldprocess);					// return to caller's CR3

  if (sts != OZ_SUCCESS) {
    oz_hwxen_unpinpage (prctx -> ppdma);
    oz_hwxen_marksyspagerw (OZ_HW_VADDRTOVPAGE (prctx -> ppdsa));
    if (prctx -> quota != NULL) oz_knl_quota_credit (prctx -> quota, OZ_QUOTATYPE_PHM, 1, -1);
    oz_knl_section_increfc (prctx -> ptsec, -1);
  }

  /* If we're copying the process, we have to manually copy the two pdata pages as */
  /* they are part of a pagetable section which doesn't get automatically copied   */

  else if (copyproc) {
    oz_hw_phys_movefromvirt (sizeof oz_sys_pdata_array, oz_sys_pdata_array, newpdatapages, 0);
    oz_knl_section_iounlk (seclock);
  }

  return (sts);
}

/* This is a separate routine in a 'try'/'catch' handler in case there is no memory to fault the pages in */

static uLong init_pdata (void *dummy)

{
  oz_sys_pdata_array[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN].data.rwpageprot = OZ_HW_PAGEPROT_KW;	// read/write by kernel only
  oz_sys_pdata_array[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN].data.procmode   = OZ_PROCMODE_KNL;	// owned by kernel mode
  oz_sys_pdata_array[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN].data.pprocmode  = OZ_PROCMODE_KNL;	// this is the per-process knl data

  oz_sys_pdata_array[OZ_PROCMODE_USR-OZ_PROCMODE_MIN].data.rwpageprot = OZ_HW_PAGEPROT_UW;	// read/write by any mode
  oz_sys_pdata_array[OZ_PROCMODE_USR-OZ_PROCMODE_MIN].data.procmode   = OZ_PROCMODE_USR;	// owned by user mode
  oz_sys_pdata_array[OZ_PROCMODE_USR-OZ_PROCMODE_MIN].data.pprocmode  = OZ_PROCMODE_USR;	// this is the per-process user data

  return (OZ_SUCCESS);										// sucessfully faulted and initted
}

/************************************************************************/
/*									*/
/*  Switch pagetables							*/
/*									*/
/************************************************************************/

void oz_hw_process_switchctx (void *old_hw_ctx, void *new_hw_ctx)

{
  int rc;
  mmu_update_t ureqs[0];

  ureqs[0].ptr = MMU_EXTENDED_COMMAND | ((Prctx *)new_hw_ctx) -> ppdma;
  ureqs[0].val = MMUEXT_NEW_BASEPTR;
  rc = HYPERVISOR_mmu_update (ureqs, 1);
  if (rc != 0) oz_crash ("oz_hw_process_switchctx: HYPERVISOR_mmu_update error %d setting current directory %X", rc, ureqs[0].ptr);
}

/************************************************************************/
/*									*/
/*  Terminate current normal process hardware context block		*/
/*  System process hardware context block is never terminated		*/
/*									*/
/*  Note that the final thread (routine cleanupproc in 			*/
/*  oz_knl_process_increfc0 is stil active in the process		*/
/*									*/
/*    Input:								*/
/*									*/
/*	process_hw_ctx = pointer to process hardware context block	*/
/*	process = pointer to process block				*/
/*									*/
/*	ipl = softint							*/
/*									*/
/************************************************************************/

void oz_hw_process_termctx (void *process_hw_ctx, OZ_Process *process)

{
  OZ_Quota *quota;
  Prctx *prctx;
  uLong pm;

  prctx = process_hw_ctx;

  /* Make sure we're not trying to wipe the system process */

  if (prctx == system_process_hw_ctx) oz_crash ("oz_hw_process_termctx: trying to terminate system process");

  /* Switch to system global page directory because we are about to wipe out this process' page directory */

  oz_hw_process_switchctx (prctx, system_process_hw_ctx);

  /* Unpin and write-enable the old process private directory page */

  oz_hwxen_unpinpage (prctx -> ppdma);					// tell Xen we aren't using it for a directory anymore
  oz_hwxen_marksyspagerw (OZ_HW_VADDRTOVPAGE (prctx -> ppdsa));		// allow writing in the static area now

  /* Free off old process private directory page */

  memset (prctx -> ppdsa, 0x96, PAGESIZE);				// fill with even garbazhe so PT_P's will all be clear
  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);				// oz_knl_phymem_freepage must be called at smplock level pm
  oz_knl_phymem_freepage (oz_hwxen_matopa (prctx -> ppdma) >> 12);	// free the pagedirectory page
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);				// restore smplock level

  /* Release quota for that physical memory page */

  quota = oz_knl_section_getquota (prctx -> ptsec);
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, 1, -1);

  /* Decrement pagetable section reference count - this undoes the refcount from when it was created in oz_hw_process_initctx */

  oz_knl_section_increfc (prctx -> ptsec, -1);

  /* Garbage fill the closed out prctx */

  memset (prctx, 0x96, sizeof *prctx);
}
