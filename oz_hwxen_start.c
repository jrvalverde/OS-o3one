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
/*  Xen main kernel program						*/
/*									*/
/************************************************************************/
/*
  Address terminology:

    machine  - output from the address translation done by the CPU
               sometimes called real-world physical address

               machine page = pagetableentry[virtualpage]

    physical - what the rest of OZONE uses for physical address, indexes the oz_s_phymem_pages array
               sometimes called pseudo-physical address

               physical page = static page - PP0SA
               physical page = machine_to_phys_mapping[machinepage]

    static   - a virtual address that is the static mapping of all physical pages to kernel space

               static page = physical page + PP0SA

    virtual  - input to the address translation done by the CPU

  This is the only module that knows about machine pages.
  All other modules use 'pseudo-physical' pages.
*/

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

extern start_info_t oz_hwxen_startinfo;		// copy of the start info struct
uLong const oz_hwxen_startinfo_size = sizeof (start_info_t);
extern shared_info_t volatile oz_hwxen_sharedinfo; // we map the shared info struct over this page
uLong *oz_hwxen_mpdsa;				// master pagedirectory static address
uLong *oz_hwxen_sptsa;				// system pagetable static address (pte that maps PP0SA)
uLong volatile *oz_hwxen_sysreqprots;		// system (common) reqprots table static address
OZ_Smplock oz_hwxen_smplock_events[32];		// event spinlocks

static void swappages (uLong newsa, uLong oldsa, int ismpd);
static void writepdentry (uLong *pdeva, uLong pdentry);

/************************************************************************/
/*									*/
/*  This is called by oz_kernel_xen.s once we're loaded in memory	*/
/*									*/
/************************************************************************/

void oz_hwxen_start (void)

{
  char *p, *q, *r;
  int i, rc;
  mmu_update_t ureqs[1];
  OZ_Pointer nextva;
  uLong pdentry, ptentry, ptpagva, vp;

  memset (OZ_IMAGE_ZEROADDR, 0, OZ_IMAGE_LASTADDR + 1 - OZ_IMAGE_ZEROADDR);
  oz_knl_printk ("oz_hwxen_start: initializing as domain %u\n", oz_hwxen_startinfo.dom_id);

  if ((OZ_Pointer)OZ_IMAGE_BASEADDR != OZ_HW_BASE_SYSC_VA) {
    oz_crash ("oz_hwxen_start: linked at 0x%X but OZ_HW_BASE_SYSC_VA is 0x%X\n", (OZ_Pointer)OZ_IMAGE_BASEADDR, OZ_HW_BASE_SYSC_VA);
  }

  oz_hw_cpu_setsoftint (0);

  for (i = 0; i < 32; i ++) {
    oz_hw_smplock_init (sizeof oz_hwxen_smplock_events[i], oz_hwxen_smplock_events + i, OZ_SMPLOCK_LEVEL_EVENTS + i);
  }

  //  At this point, memory is set up like this:
  //
  //	+--------------------------------------+
  //	|                                      |
  //	|   Xen Hypervisor                     |
  //	|                                      | FC000000
  //	+--------------------------------------+
  //	|                                      |
  //	|   No Access                          |
  //	|                                      | PP0SA + (oz_hwxen_startinfo.nr_pages << 12)
  //	+--------------------------------------+
  //	|                                      |
  //	|   Initial Pagetables                 |
  //	|                                      |
  //	+--------------------------------------+
  //	|                                      |
  //	|   Free memory                        |
  //	|                                      |
  //	+--------------------------------------+
  //	|                                      | OZ_IMAGE_LASTADDR
  //	|   Our Image                          |
  //	|                                      | OZ_IMAGE_BASEADDR = PP0SA
  //	+--------------------------------------+
  //	|                                      |
  //	|   No Access                          |
  //	|                                      | 00000000
  //	+--------------------------------------+

  if (PP0SA & 0x003FFFFF) oz_crash ("oz_hwxen_start: image not loaded on 4M boundary (%X)", PP0SA);
  if (TOPVA & 0x003FFFFF) oz_crash ("oz_hwxen_start: top virtual address not on 4M boundary (%X)", TOPVA);

  /* Save total number of physical pages allocated to this virtual machine */

  oz_s_phymem_totalpages = oz_hwxen_startinfo.nr_pages;

  /* Set up self-reference pointer in initial pagedirectory.  This will make the  */
  /* pd appear at virtual address CPDVA (Current Page Directory Virtual Address). */

  pdentry = ((uLong *)oz_hwxen_startinfo.pt_base)[oz_hwxen_startinfo.pt_base>>22];			// get pde mapping the pd
  if ((pdentry & (PD_P | PD_4M)) != PD_P) oz_crash ("oz_hwxen_start: initial pd's pde is %X", pdentry);
  ptpagva = (uLong)OZ_HW_VPAGETOVADDR (machine_to_phys_mapping[pdentry>>12] + PP0SP);			// get ptpage mapping the pd
  ptentry = ((uLong *)ptpagva)[(oz_hwxen_startinfo.pt_base>>12)&1023];					// get pte mapping the pd
  if (!(ptentry & PT_P)) oz_crash ("oz_hwxen_start: initial pd's pte is %X", ptentry);
  ureqs[0].ptr = MMU_NORMAL_PT_UPDATE | (oz_hwxen_startinfo.pt_base + (PPPAGETBL_VPAGE >> 8));		// point to entry in current pd to write
  ureqs[0].val = (ptentry & 0xFFFFF000) | PD_UR;							// this is pd page's machine address
  rc = HYPERVISOR_mmu_update (ureqs, 1);								// perform *ureqs[0].ptr = ureqs[0].val
  if (rc != 0) oz_crash ("oz_hwxen_start: error %d setting initial self-ref pointer %X to %X", rc, ureqs[0].ptr, ureqs[0].val);
  if ((oz_hwxen_vatoma (oz_hwxen_startinfo.pt_base) != oz_hwxen_vatoma ((PPPAGETBL_VPAGE << 12) + (PPPAGETBL_VPAGE << 2))) 
   || (memcmp ((void *)oz_hwxen_startinfo.pt_base, (void *)((PPPAGETBL_VPAGE << 12) + (PPPAGETBL_VPAGE << 2)), 4096) != 0)) {
    oz_crash ("oz_hwxen_start: initial pagedirectory self-ref mapping didn't work");
  }

  /* Make sure all of image's read-only pages are actually read-only and also readable by usermode */

  oz_hwxen_mpdsa = (uLong *)oz_hwxen_startinfo.pt_base;
  for (nextva = (OZ_Pointer)OZ_IMAGE_BASEADDR; nextva < (OZ_Pointer)OZ_IMAGE_RDWRADDR; nextva += PAGESIZE) {
    oz_hwxen_marksyspagero (OZ_HW_VADDRTOVPAGE (nextva));
  }

  /* The rest of image's pages must be read/write by system mode only */

  for (; nextva < (OZ_Pointer)OZ_IMAGE_LASTADDR; nextva += PAGESIZE) {
    oz_hwxen_marksyspagerw (OZ_HW_VADDRTOVPAGE (nextva));
  }

  /* Move the given page directory page to just after the kernel image (who knows where it is!) and make it current */

  swappages (nextva, oz_hwxen_startinfo.pt_base, 1);
  oz_hwxen_startinfo.pt_base = nextva;						// in case some fool tries to use this
  nextva += PAGESIZE;

  /* Move all the pagetable pages to just after the directory page, by ascending virtual address that they map */
  /* Also pad out the pagetable to end of common virtual address space so global mpd entries never change      */

  oz_hwxen_sptsa = (uLong *)nextva;
  for (i = PP0SA >> 22; i < TOPVA >> 22; i ++) {
    pdentry = oz_hwxen_mpdsa[i];
    if (pdentry & PD_4M) oz_crash ("oz_hwxen_start: pde %X is 4Meg page", pdentry);
    if (pdentry & PD_P) {
      swappages (nextva, oz_hwxen_matosa (pdentry & 0xFFFFF000), 0);
    } else {
      memset ((void *)nextva, 0, PAGESIZE);
      oz_hwxen_marksyspagero (OZ_HW_VADDRTOVPAGE (nextva));
      oz_hwxen_pinptpage (oz_hwxen_vatoma (nextva));
      writepdentry (oz_hwxen_mpdsa + i, oz_hwxen_vatoma (nextva) | PD_UW);
    }
    nextva += PAGESIZE;
  }

  /* Next comes system requested protection table pages */

  oz_hwxen_sysreqprots = (uLong *)nextva;
  i  = (TOPVA - PP0SA) / 16384;
  i +=  PAGESIZE - 1;
  i &= -PAGESIZE;
  memset ((void *)oz_hwxen_sysreqprots, 0, i);
  nextva += i;

  //  At this point, memory is set up like this:
  //
  //	+--------------------------------------+
  //	|                                      |
  //	|   Xen Hypervisor                     |
  //	|                                      | FC000000
  //	+--------------------------------------+
  //	|                                      |
  //	|   No Access / Not Used               |
  //	|                                      | TOPVA
  //	+--------------------------------------+
  //	|                                      |
  //	|   No Access / Kernel Expansion       |
  //	|                                      | PP0SA + (oz_hwxen_startinfo.nr_pages << 12)
  //	+--------------------------------------+
  //	|                                      |
  //	|   Free memory                        |
  //	|                                      |
  //	+--------------------------------------+
  //	|                                      |
  //	|   System Page Requested Protection   |  
  //	|                                      | oz_hwxen_sysreqprots
  //	+--------------------------------------+
  //	|                                      |
  //	|   System Page Table                  |
  //	|                                      | oz_hwxen_sptsa
  //	+--------------------------------------+
  //	|   Master Page Directory              | oz_hwxen_mpdsa
  //	+--------------------------------------+
  //	|                                      | OZ_IMAGE_LASTADDR
  //	|   Kernel Image                       |
  //	|                                      | OZ_IMAGE_BASEADDR = PP0SA = OZ_HW_BASE_SYSC_VA
  //	+--------------------------------------+
  //	|                                      |
  //	|   No Access                          |
  //	|                                      | 00000000
  //	+--------------------------------------+

  /* Map the shared info struct where we can access it.  Apparently it is in a machine page that is not part of our  */
  /* 64Meg or whatever so we have to simply map it (not swap something for it).  We want it at a linked address (not */
  /* a pointer) to make it possible for the event handling routine to clear the event enable bit without a register. */

  if (oz_hwxen_startinfo.shared_info & 4095) oz_crash ("oz_hwxen_start: shared_info %X not page aligned", oz_hwxen_startinfo.shared_info);
  if ((OZ_Pointer)&oz_hwxen_sharedinfo & 4095) oz_crash ("oz_hwxen_start: oz_hwxen_sharedinfo %p not page aligned", &oz_hwxen_sharedinfo);

  rc = HYPERVISOR_update_va_mapping (OZ_HW_VADDRTOVPAGE (&oz_hwxen_sharedinfo), oz_hwxen_startinfo.shared_info | PT_KW, UVMF_INVLPG);
  if (rc != 0) oz_crash ("oz_hwxen_start: error %d mapping shared info %X to vaddr %p\n", rc, oz_hwxen_startinfo.shared_info, &oz_hwxen_sharedinfo);

  oz_knl_printk ("oz_hwxen_start*: initial event 0x%X\n", oz_hwxen_sharedinfo.events);
  oz_knl_printk ("oz_hwxen_start*: initial events_mask 0x%X\n", oz_hwxen_sharedinfo.events_mask);

  oz_knl_printk ("oz_hwxen_start: CPU frequency %Qu Hz\n", oz_hwxen_sharedinfo.cpu_freq);
  oz_hwxen_init2 ();
  oz_s_boottime = oz_hw_tod_getnow ();
  oz_knl_printk ("oz_hwxen_start: boot time %t\n", oz_s_boottime);

  /* Double-map the oz_s_systempdata page onto the system process' oz_sys_pdata_array[kernelmode] element page */
  /* We have to eat up a page for its pagetable page                                                           */

  if ((OZ_Pointer)&oz_s_systempdata & 4095) oz_crash ("oz_hwxen_start: oz_s_systempdata %p not page aligned", &oz_s_systempdata);
  if ((OZ_Pointer)oz_sys_pdata_array & 4095) oz_crash ("oz_hwxen_start: oz_sys_pdata_array %p not page aligned", oz_sys_pdata_array);

  vp = OZ_HW_VADDRTOVPAGE (oz_sys_pdata_array + OZ_PROCMODE_KNL - OZ_PROCMODE_MIN); // this is vpage where ghost image goes
  memset ((void *)nextva, 0, PAGESIZE);						// clear out the pagetable page we will use
  ((uLong *)nextva)[vp&1023] = oz_hwxen_vatoma ((OZ_Pointer)&oz_s_systempdata) | PT_KW; // set up pagetable entry to point to real systempdata
  oz_hwxen_marksyspagero (OZ_HW_VADDRTOVPAGE (nextva));				// mark pagetable page read-only so Xen will accept it
  oz_hwxen_pinptpage (oz_hwxen_satoma (nextva));				// tell Xen we are using this for a pagetable page
  writepdentry (CPDVA + (vp >> 10), oz_hwxen_satoma (nextva) | PD_UW);		// write pagedirectory entry
  nextva += PAGESIZE;								// don't use that pt page for anything else

  if (oz_hwxen_vatoma ((OZ_Pointer)&oz_s_systempdata) != oz_hwxen_vatoma (vp << 12)) {
    oz_crash ("oz_hwxen_start: double-mapping oz_s_systempdata as oz_sys_pdata_array failed");
  }

  /* Set up parameters to get ready for oz_knl_boot_firstcpu */

  memset (&oz_ldr_paramblock, 0, sizeof oz_ldr_paramblock);
  for (i = 0; oz_ldr_paramtable[i].pname != NULL; i ++) {
    oz_ldr_set (oz_knl_printk, oz_ldr_paramtable[i].pname, oz_ldr_paramtable[i].pdflt);
  }
  oz_ldr_paramblock.nonpaged_pool_size = 0x400000;
  strcpy (oz_ldr_paramblock.kernel_image, "oz_kernel.elf");
  strcpy (oz_ldr_paramblock.startup_image, "oz_cli.elf");
  strcpy (oz_ldr_paramblock.startup_input, "../startup/xen_startup.cli");
  strcpy (oz_ldr_paramblock.startup_output, "console:");
  strcpy (oz_ldr_paramblock.startup_error, "console:");

  p = strchr (oz_hwxen_startinfo.cmd_line, ' ');		// point past the load image name string
  if (p != NULL) {
    rc = 1;							// assume parsing will succeed
    while (*p != 0) {						// loop through command line
      if (*p == ' ') {						// skip spaces
        ++ p;
        continue;
      }
      i = 0;							// see if 'extra:' present
      if (strncasecmp (p, "extra:", 6) == 0) {
        p += 6;							// if so, skip over it
        i = 1;							// ... and remember we found it
      }
      r = strchr (p, ' ');					// point to space after value
      if (r != NULL) *(r ++) = 0;				// if there, replace with null terminator
      else r = p + strlen (p);					// otherwise, point to end of string
      q = strchr (p, '=');					// look for = separating name from value
      if (q != NULL) *(q ++) = 0;				// if there, replace with null terminator
      if (i) rc &= oz_ldr_extra (oz_knl_printk, p, q);		// define extra parameter
        else rc &= oz_ldr_set (oz_knl_printk, p, q);		// define standard parameter
      p = r;							// process next parameter in line
    }
    if (!rc) oz_crash ("oz_hwxen_start: error in command line parameter");
  }

  oz_s_loadparams = oz_ldr_paramblock;

  oz_s_cpucount = 1;						// number of CPUs we handle
  oz_s_sysmem_baseva   = (void *)PP0SA;				// VA mapped by first system pagetable entry
  oz_s_sysmem_pagtblsz = TOPVP - PP0SP;				// number of system pagetable entries available
  oz_s_phymem_l1pages = 1;
  oz_s_phymem_l2pages = 1;

  /* Call the main boot routine then exit */

  oz_knl_boot_firstcpu (PP0SP + oz_hwxen_startinfo.nr_pages, 	// tell kernel to leave initial mapping of 
								// ... all our physical pages alone as is 
								// ... so we can perform address conversions
                        oz_hwxen_vatopa (nextva) >> 12);	// tell kernel to ignore any pseudo-physical 
								// ... pages we have filled in already
  while (1) oz_knl_thread_exit (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Swap the static addresses of two pages				*/
/*									*/
/*    Input:								*/
/*									*/
/*	newsa = static address of one page				*/
/*	oldsa = static address of other page				*/
/*									*/
/************************************************************************/

static void swappages (uLong newsa, uLong oldsa, int ismpd)

{
  int rc;
  mmu_update_t ureqs[5];
  uLong newpdentry, newptentry, oldpdentry, oldptentry;
  uLong *newptentsa, *oldptentsa;

  /* Read existing values */

  newpdentry = CPDVA[newsa>>22];
  newptentsa = ((uLong *)oz_hwxen_matosa (newpdentry & 0xFFFFF000)) + ((newsa >> 12) & 1023);
  newptentry = *newptentsa;

  oldpdentry = CPDVA[oldsa>>22];
  oldptentsa = ((uLong *)oz_hwxen_matosa (oldpdentry & 0xFFFFF000)) + ((oldsa >> 12) & 1023);
  oldptentry = *oldptentsa;

  /* Write them back out swapped around */

  ureqs[0].ptr = MMU_NORMAL_PT_UPDATE | ((OZ_Pointer)newptentsa);	// write new page's pte
  ureqs[0].val = oldptentry;						// ... with old page's pointer
  ureqs[1].ptr = MMU_NORMAL_PT_UPDATE | ((OZ_Pointer)oldptentsa);	// write old page's pte
  ureqs[1].val = newptentry;						// ... with new page's pointer
  ureqs[2].ptr = MMU_MACHPHYS_UPDATE | (oldptentry & 0xFFFFF000);	// write old page's machine_to_phys_mapping
  ureqs[2].val = oz_hwxen_satopa ((OZ_Pointer)newsa) >> 12;		// ... with new page's pseudo-physical page
  ureqs[3].ptr = MMU_MACHPHYS_UPDATE | (newptentry & 0xFFFFF000);	// write new page's machine_to_phys_mapping
  ureqs[3].val = oz_hwxen_satopa ((OZ_Pointer)oldsa) >> 12;		// ... with old page's pseudo-physical page
  if (ismpd) {
    ureqs[4].ptr = MMU_EXTENDED_COMMAND | (oldptentry & 0xFFFFF000);	// if moving mpd page around, reload CR3
    ureqs[4].val = MMUEXT_NEW_BASEPTR;					// ... (though we're not changing its machine address)
  }
  rc = HYPERVISOR_mmu_update (ureqs, 4 + ismpd);
  if (rc != 0) oz_crash ("oz_hwxen swappages: HYPERVISOR_mmu_update error %d swapping %X / %X", rc, newsa, oldsa);

  if (ismpd) oz_hwxen_mpdsa = (uLong *)newsa;				// save new static address of master pagedirectory page

  /* Verify that things got swapped properly */

  newptentry &= 0xFFFFF000;
  oldptentry &= 0xFFFFF000;
  if (oz_hwxen_satoma (newsa) != oldptentry) oz_crash ("oz_hwxen swappages: newsa %X doesn't convert to oldma", newsa);
  if (oz_hwxen_satoma (oldsa) != newptentry) oz_crash ("oz_hwxen swappages: oldsa %X doesn't convert to newma", oldsa);
  if (oz_hwxen_matosa (newptentry) != oldsa) oz_crash ("oz_hwxen swappages: newma %X doesn't convert to oldsa", newptentry);
  if (oz_hwxen_matosa (oldptentry) != newsa) oz_crash ("oz_hwxen swappages: oldma %X doesn't convert to newsa", oldptentry);
}

/************************************************************************/
/*									*/
/*  Write pagedirectory entry						*/
/*									*/
/*    Input:								*/
/*									*/
/*	pdeva = virtual address of pd entry to write			*/
/*	pdentry = contents to write to that entry			*/
/*									*/
/*    Output:								*/
/*									*/
/*	*pdeva = pdentry						*/
/*									*/
/************************************************************************/

static void writepdentry (uLong *pdeva, uLong pdentry)

{
  int rc;
  mmu_update_t ureqs[0];

  ureqs[0].ptr = MMU_NORMAL_PT_UPDATE | ((OZ_Pointer)pdeva);
  ureqs[0].val = pdentry;
  rc = HYPERVISOR_mmu_update (ureqs, 1);
  if (rc != 0) oz_crash ("writepdentry: HYPERVISOR_mmu_update error %d writing pde *%X=%X", rc, pdeva, pdentry);
}
