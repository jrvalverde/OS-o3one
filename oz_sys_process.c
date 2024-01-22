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

#include "oz_sys_syscall.h"

#include "oz_sys_process.h"

#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_process.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

/************************************************************************/
/*									*/
/*  Create a process							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_job = 0 : create as part of the current job			*/
/*	     else : create as part of given job				*/
/*	name = process name string pointer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_create = OZ_SUCCESS : process created		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (process_create, OZ_Procmode, procmode, OZ_Handle, h_job, char *, name, OZ_Handle *, h_process_r)

{
  int name_l, si;
  uLong sts;
  OZ_Job *job;
  OZ_Process *process;
  OZ_Secattr *secattr;

  job     = NULL;
  process = NULL;
  secattr = NULL;

  if (procmode < cprocmode) procmode = cprocmode;

  si = oz_hw_cpu_setsoftint (0);

  /* Get job to make process part of */

  if (h_job == 0) {
    job = oz_knl_process_getjob (NULL);						/* not given, use current process' job */
  } else {
    sts = oz_knl_handle_takeout (h_job, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_JOB, &job, NULL); /* given an handle, get corresponding job */
    if (sts != OZ_SUCCESS) goto rtn_si;
  }

  /* Get security attributes */

  secattr = oz_knl_thread_getdefcresecattr (NULL);
  sts = oz_knl_secattr_fromname (OZ_PROCESS_NAMESIZE, name, &name_l, NULL, &secattr);
  if (sts != OZ_SUCCESS) goto rtn_job;

  /* Create a non-system process structure */

  sts = oz_knl_process_create (job, 0, 0, name_l, name, secattr, &process);
  if (sts != OZ_SUCCESS) goto rtn_job;

  /* Assign an handle to the process -                                                                                 */
  /* If this succeeds, the increfc process -1 will not delete the process because this handle keeps the refcount alive */
  /* If this fails, the increfc process -1 will delete the process because there is no handle keeping it alive         */

  sts = oz_knl_handle_assign (process, procmode, h_process_r);

  /* Restore software interrupts and return status */

  oz_knl_process_increfc (process, -1);
rtn_job:
  if (h_job != 0) oz_knl_handle_putback (h_job);
rtn_si:
  oz_knl_secattr_increfc (secattr, -1);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Create a new process and copy the current one to it			*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = name to give to new process				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_makecopy = OZ_SUCCESS : successful		*/
/*	                                else : failure status		*/
/*	*h_process_r = handle to process (this handle is not copied)	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (process_makecopy, const char *, name, OZ_Handle *, h_process_r)

{
  int name_l, si;
  uLong sts;
  OZ_Process *process;
  OZ_Secattr *secattr;

  process = NULL;
  secattr = NULL;

  si = oz_hw_cpu_setsoftint (0);

  /* Get security attributes */

  secattr = oz_knl_thread_getdefcresecattr (NULL);
  sts = oz_knl_secattr_fromname (OZ_PROCESS_NAMESIZE, name, &name_l, NULL, &secattr);
  if (sts != OZ_SUCCESS) goto rtn_si;

  /* Create a non-system process structure then copy current process (pagetables, handles, logical names) to it */

  sts = oz_knl_process_create (oz_knl_process_getjob (NULL), 0, 1, name_l, name, secattr, &process);
  if (sts != OZ_SUCCESS) goto rtn_si;

  /* Assign an handle to the process */

  sts = oz_knl_handle_assign (process, cprocmode, h_process_r);

  /* Restore software interrupts and return status */

  oz_knl_process_increfc (process, -1);
rtn_si:
  oz_knl_secattr_increfc (secattr, -1);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Map a section to the current process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = process mode of section				*/
/*	h_section = handle to section to be mapped			*/
/*	*npagem = number of pages to map				*/
/*	*svpage = starting virtual page number to map at		*/
/*	exact = 0 : *svpage is somewhere in the pagetable to map at	*/
/*	            just find first free spot and map there		*/
/*	        1 : *svpage is exactly where section is to be mapped	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_mapsection = OZ_SUCCESS : successful		*/
/*		*npagem = number of pages mapped			*/
/*		*svpage = actual page number it is mapped at		*/
/*	                                  else : error			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_6 (process_mapsection, OZ_Procmode, procmode, OZ_Handle, h_section, OZ_Mempage *, npagem, OZ_Mempage *, svpage, uLong, mapsecflags, OZ_Hw_pageprot, pageprot)

{
  int si;
  uLong sts;
  OZ_Mempage numpages, startpag;
  OZ_Section *section;

  if (procmode < cprocmode) procmode = cprocmode;

  si  = oz_hw_cpu_setsoftint (0);
  sts = OZ_ACCVIO;
  if (OZ_HW_WRITABLE (sizeof *npagem, npagem, cprocmode) && OZ_HW_WRITABLE (sizeof *svpage, svpage, cprocmode)) {
    numpages = *npagem;
    startpag = *svpage;
    sts = OZ_KERNELONLY;
    if (!OZ_HW_ISSYSPAGE (startpag) || (procmode == OZ_PROCMODE_KNL)) {
      sts = oz_knl_handle_takeout (h_section, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_SECTION, &section, NULL);
      if (sts == OZ_SUCCESS) {
        sts = oz_knl_process_mapsection (section, &numpages, &startpag, mapsecflags, procmode, pageprot);
        oz_knl_handle_putback (h_section);
        if (sts == OZ_SUCCESS) {
          *npagem = numpages;
          *svpage = startpag;
        }
      }
    }
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Map a list of sections to current process' address space		*/
/*									*/
/*  See oz_knl_process_mapsections for details				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (process_mapsections, uLong, mapsecflags, int, nsections, OZ_Mapsecparamh *, mapsecparamhs)

{
  int i, si;
  OZ_Handle *handles;
  OZ_Mapsecparam *mapsecparams;
  uLong sts;
  OZ_Seclock *seclock;

  if (nsections <= 0) return (OZ_SUCCESS);

  si = oz_hw_cpu_setsoftint (0);								// block thread aborts
  mapsecparams = OZ_KNL_NPPMALLOQ (nsections * sizeof *mapsecparams);				// get array to copy to
  sts = OZ_EXQUOTANPP;
  if (mapsecparams != NULL) {
    handles = OZ_KNL_PGPMALLOQ (nsections * sizeof *handles);					// copy handles so they can't change on us
    sts = OZ_EXQUOTAPGP;
    if (handles != NULL) {
      sts = oz_knl_section_iolock (cprocmode, nsections * sizeof *mapsecparamhs, 		// lock so it can't unmap on us
                                   mapsecparamhs, 1, &seclock, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        for (i = 0; i < nsections; i ++) {							// loop through all sections
          handles[i] = mapsecparamhs[i].h_section;						// - copy the handle
          sts = oz_knl_handle_takeout (handles[i], cprocmode, OZ_SECACCMSK_WRITE, 		// - convert handle to section object pointer
                                       OZ_OBJTYPE_SECTION, &(mapsecparams[i].section), NULL);
          if (sts != OZ_SUCCESS) break;
          mapsecparams[i].npagem    = mapsecparamhs[i].npagem;					// - copy the other mapsecparam elements
          mapsecparams[i].svpage    = mapsecparamhs[i].svpage;
          mapsecparams[i].ownermode = mapsecparamhs[i].ownermode;
          if (mapsecparams[i].ownermode < cprocmode) mapsecparams[i].ownermode = cprocmode;	// - don't let caller map something to inner mode
          mapsecparams[i].pageprot  = mapsecparamhs[i].pageprot;
        }
        if (sts == OZ_SUCCESS) {
          sts = oz_knl_process_mapsections (mapsecflags, nsections, mapsecparams);		// map them all to memory
          if (sts == OZ_SUCCESS) {
            for (i = 0; i < nsections; i ++) {							// if successful, return where they were mapped
              mapsecparamhs[i].npagem = mapsecparams[i].npagem;
              mapsecparamhs[i].svpage = mapsecparams[i].svpage;
            }
          }
        }
        while (i > 0) oz_knl_handle_putback (handles[--i]);					// put the handles back
        oz_knl_section_iounlk (seclock);							// unlock mapsecparamhs array
      }
      OZ_KNL_PGPFREE (handles);									// free off temp handle copy
    }
    OZ_KNL_NPPFREE (mapsecparams);								// free off temp mapsecparams array
  }
  oz_hw_cpu_setsoftint (si);									// allow thread aborts
  return (sts);											// return completion status
}

/************************************************************************/
/*									*/
/*  Get section that is mapped at a particular address of a process	*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_process = handle to process (or 0 for current process)	*/
/*	vpage = virtual page number in that process			*/
/*									*/
/*    Output:								*/
/*									*/
/*	*h_section_r = handle to section				*/
/*	*spage_r     = page within that section				*/
/*	*pageprot_r  = protection section was mapped with		*/
/*	*procmode_r  = processor mode that owns the mapping		*/
/*	*npages_r    = number of pages mapped there			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_9 (process_getsecfromvpage, OZ_Procmode, procmode, OZ_Handle, h_process, OZ_Mempage, vpage, OZ_Handle *, h_section_r, OZ_Mempage *, spage_r, OZ_Hw_pageprot *, pageprot_r, OZ_Procmode *, procmode_r, OZ_Mempage *, npages_r, uLong *, mapsecflags_r)

{
  int rc, si;
  OZ_Hw_pageprot mapprot;
  OZ_Mempage npages;
  OZ_Process *process;
  OZ_Section *section;
  uLong sts;

  /* Make sure args writable by caller */

  if (!OZ_HW_WRITABLE (sizeof *h_section_r,   h_section_r,   cprocmode)) return (OZ_ACCVIO);
  if (!OZ_HW_WRITABLE (sizeof *spage_r,       spage_r,       cprocmode)) return (OZ_ACCVIO);
  if (!OZ_HW_WRITABLE (sizeof *pageprot_r,    pageprot_r,    cprocmode)) return (OZ_ACCVIO);
  if (!OZ_HW_WRITABLE (sizeof *procmode_r,    procmode_r,    cprocmode)) return (OZ_ACCVIO);
  if (!OZ_HW_WRITABLE (sizeof *npages_r,      npages_r,      cprocmode)) return (OZ_ACCVIO);
  if (!OZ_HW_WRITABLE (sizeof *mapsecflags_r, mapsecflags_r, cprocmode)) return (OZ_ACCVIO);

  /* Maximise procmode with caller's */

  if (procmode < cprocmode) procmode = cprocmode;

  /* Keep thread from being aborted */

  si = oz_hw_cpu_setsoftint (0);

  /* Get pointer to process */

  if (h_process == 0) process = oz_knl_thread_getprocesscur ();
  else {
    sts = oz_knl_handle_takeout (h_process, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_PROCESS, &process, NULL);
    if (sts != OZ_SUCCESS) goto rtn_si;
  }

  /* Get what section and what page within the section */

  npages = oz_knl_process_getsecfromvpage (process, vpage, &section, spage_r, pageprot_r, procmode_r, mapsecflags_r);

  /* We're done with process pointer */

  if (h_process != 0) oz_knl_handle_putback (h_process);

  /* Assign an handle to the section if it was found */

  sts = OZ_ADDRNOTUSED;
  if (npages != 0) {
    *npages_r = npages;
    sts = oz_knl_handle_assign (section, procmode, h_section_r);
  }

rtn_si:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Unmap a section at a given virtual page number			*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = page number to unmap					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_unmapsec = OZ_SUCCESS : successful		*/
/*	                     OZ_ADDRNOTINUSE : nothing was there	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (process_unmapsec, OZ_Mempage, vpage)

{
  uLong sts;

  /* ?? check to see if pages are owned by caller's mode ?? */

  sts = oz_knl_process_unmapsec (vpage);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get a process' thread queue sequence number				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (process_getthreadqseq, OZ_Procmode, procmode, OZ_Handle, h_process, uLong *, threadqseq_r)

{
  int si;
  uLong sts;
  OZ_Process *process;
  uLong threadqseq;

  if (procmode < cprocmode) procmode = cprocmode;						/* maximise handle access mode */
  process = NULL;										/* assume 'current' process */
  sts = OZ_SUCCESS;
  si = oz_hw_cpu_setsoftint (0);								/* keep thread from being deleted */
  if (h_process != 0) sts = oz_knl_handle_takeout (h_process, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_PROCESS, &process, NULL);
  if (sts == OZ_SUCCESS) {
    threadqseq = oz_knl_process_getthreadqseq (process);					/* get the threadqseq */
    if (h_process != 0) oz_knl_handle_putback (h_process);					/* release the process */
    sts = oz_knl_section_uput (cprocmode, sizeof *threadqseq_r, &threadqseq, threadqseq_r);	/* return threadqseq to caller */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get a process' id number						*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (process_getid, OZ_Procmode, procmode, OZ_Handle, h_process, OZ_Processid *, processid_r)

{
  int si;
  uLong sts;
  OZ_Process *process;
  OZ_Processid processid;

  if (procmode < cprocmode) procmode = cprocmode;						/* maximise handle access mode */
  process = NULL;										/* assume 'current' process */
  sts = OZ_SUCCESS;
  si = oz_hw_cpu_setsoftint (0);								/* keep thread from being deleted */
  if (h_process != 0) sts = oz_knl_handle_takeout (h_process, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_PROCESS, &process, NULL); /* look up process if handle given */
  if (sts == OZ_SUCCESS) {
    processid = oz_knl_process_getid (process);							/* get the processid */
    if (h_process != 0) oz_knl_handle_putback (h_process);					/* release the process */
    sts = oz_knl_section_uput (cprocmode, sizeof *processid_r, &processid, processid_r);	/* return processid to caller */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get an handle to a process given its id number			*/
/*									*/
/*    Input:								*/
/*									*/
/*	processid = process's id number					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_getbyid = OZ_SUCCESS : successfully retrieved	*/
/*	                   OZ_NOSUCHPROCESS : there is no such thread	*/
/*	                               else : error assigning handle	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (process_getbyid, OZ_Processid, processid, OZ_Handle *, h_process_r)

{
  int si;
  uLong sts;
  OZ_Process *process;

  si = oz_hw_cpu_setsoftint (0);					/* keep from being aborted in here */
  process = oz_knl_process_frompid (processid);				/* find the process given its id number */
  sts = OZ_NOSUCHPROCESS;						/* assume it wasn't found */
  if (process != NULL) {						/* see if it was found */
    sts = oz_knl_handle_assign (process, cprocmode, h_process_r);	/* got it, assign an handle to it */
    oz_knl_process_increfc (process, -1);				/* ... release process pointer */
  }
  oz_hw_cpu_setsoftint (si);						/* restore software interrupts */
  return (sts);								/* return final status */
}

/************************************************************************/
/*									*/
/*  Peek and Poke a process' memory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_process = process' handle					*/
/*	procmode  = processor mode to access memory with		*/
/*	size      = number of bytes to copy				*/
/*	lcldst    = local process buffer dest address			*/
/*	lclsrc    = local process buffer source address			*/
/*	remdst    = remote process buffer dest address			*/
/*	remsrc    = remote process buffer source address		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_peek/poke = OZ_SUCCESS : successful		*/
/*	                                 else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_5 (process_peek, OZ_Handle, h_process, OZ_Procmode, procmode, uLong, size, void const *, remsrc, void *, lcldst)

{
  int si;
  OZ_Process *myproc, *process;
  uByte tmpbuf[64];
  uLong sts, xfer;

  if (procmode < cprocmode) procmode = cprocmode;

  si  = oz_hw_cpu_setsoftint (0);						/* keep from being aborted */
  sts = oz_knl_handle_takeout (h_process, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_PROCESS, &process, NULL);
  if (sts == OZ_SUCCESS) {
    myproc = oz_knl_process_getcur ();						/* save which process I'm in now */
    while (size > 0) {
      xfer = size;
      if (xfer > sizeof tmpbuf) xfer = sizeof tmpbuf;
      oz_knl_process_setcur (process);						/* set to target process address space */
      sts = oz_knl_section_uget (procmode, xfer, remsrc, tmpbuf);		/* read its memory */
      oz_knl_process_setcur (myproc);						/* restore my process */
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uput (cprocmode, xfer, tmpbuf, lcldst); /* copy to caller's buffer */
      if (sts != OZ_SUCCESS) break;
      size -= xfer;
      (OZ_Pointer)remsrc += xfer;
      (OZ_Pointer)lcldst += xfer;
    }
    oz_knl_handle_putback (h_process);						/* release process handle */
  }
  oz_hw_cpu_setsoftint (si);							/* restore software interrupts */
  return (sts);									/* return composite status */
}

OZ_HW_SYSCALL_DEF_5 (process_poke, OZ_Handle, h_process, OZ_Procmode, procmode, uLong, size, void const *, lclsrc, void *, remdst)

{
  int si;
  OZ_Process *myproc, *process;
  uByte tmpbuf[64];
  uLong sts, xfer;

  if (procmode < cprocmode) procmode = cprocmode;

  si  = oz_hw_cpu_setsoftint (0);						/* keep from being aborted */
  sts = oz_knl_handle_takeout (h_process, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_PROCESS, &process, NULL);
  if (sts == OZ_SUCCESS) {
    myproc = oz_knl_process_getcur ();						/* save which process I'm in now */
    while (size > 0) {
      xfer = size;
      if (xfer > sizeof tmpbuf) xfer = sizeof tmpbuf;
      sts = oz_knl_section_uget (cprocmode, size, lclsrc, tmpbuf);		/* copy data to temp buffer */
      if (sts == OZ_SUCCESS) {
        oz_knl_process_setcur (process);					/* set to target process address space */
        sts = oz_knl_section_uput (procmode, size, tmpbuf, remdst);		/* write its memory */
        oz_knl_process_setcur (myproc);						/* restore my process */
      }
      if (sts != OZ_SUCCESS) break;
      size -= xfer;
      (OZ_Pointer)lclsrc += xfer;
      (OZ_Pointer)remdst += xfer;
    }
    oz_knl_handle_putback (h_process);						/* release process handle */
  }
  oz_hw_cpu_setsoftint (si);							/* restore software interrupts */
  return (sts);									/* return composite status */
}

/************************************************************************/
/*									*/
/*  Set breakpoints in a process					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_process = handle of process					*/
/*	listsize  = number of elements in given list			*/
/*	listbuff  = points to given list array				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_setbreaks = OZ_SUCCESS : successful		*/
/*	                                 else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (process_setbreaks, OZ_Procmode, procmode, OZ_Handle, h_process, uLong, listsize, OZ_Process_Breaklist *, listbuff)

{
  int i, j, si;
  OZ_Breakpoint newopcd, oldopcd, *opcaddr;
  OZ_Hw_pageprot oldpageprots[2], pageprot;
  OZ_Mempage npages, pageoffs, svpage;
  OZ_Process *myproc, *process;
  OZ_Procmode pprocmode;
  OZ_Seclock *listseclock, *seclock;
  OZ_Section *section;
  uLong mapsecflags, sts;

  if (procmode < cprocmode) procmode = cprocmode;

  myproc = process = oz_knl_process_getcur ();					/* save which process I'm in now */
  si  = oz_hw_cpu_setsoftint (0);						/* keep from being aborted */

  sts = oz_knl_section_iolock (cprocmode, listsize * sizeof listbuff[0], listbuff, 1, &listseclock, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_handle_takeout (h_process, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_PROCESS, &process, NULL);
    if (sts == OZ_SUCCESS) {
      for (i = 0; i < listsize; i ++) {						/* loop through given array */
        opcaddr = listbuff[i].addr;						/* get address to put breakpoint at */
        newopcd = oldopcd = listbuff[i].opcd;					/* get opcode to store there */
        oz_knl_process_setcur (process);					/* switch to target process */
        sts = oz_knl_section_iolock (procmode, sizeof *opcaddr, opcaddr, 0, &seclock, &npages, NULL, NULL); /* make sure target */
        if (sts == OZ_SUCCESS) {						/* ... address is at least readable by procmode */
          if (npages > 2) oz_crash ("oz_sys_process_setbreaks: breakpoint at %p spans %u pages", opcaddr, npages);
          svpage = OZ_HW_VADDRTOVPAGE (opcaddr);				/* ... and make sure it's owned by procmode */
          if ((oz_knl_process_getsecfromvpage (process, svpage, &section, &pageoffs, &pageprot, &pprocmode, &mapsecflags) < npages) 
           || (pprocmode < procmode)) sts = OZ_PROCMODE;
          else {
            sts = oz_knl_section_setpageprot (npages, svpage, OZ_HW_PAGEPROT_KW, NULL, oldpageprots); /* ok, make it writable by kernel mode */
            if (sts == OZ_SUCCESS) {
              oldopcd = *opcaddr;						/* save old opcode that was there */
              *opcaddr = newopcd;						/* write new opcode in its place (maybe doing a copy-on-write) */
              for (j = 0; j < npages; j ++) oz_knl_section_setpageprot (1, svpage + j, oldpageprots[j], NULL, NULL);
            }
          }
          oz_knl_section_iounlk (seclock);					/* unlock the opcode page(s) */
        }
        oz_knl_process_setcur (myproc);						/* switch back to caller's process */
        listbuff[i].stat = sts;							/* return status */
        listbuff[i].opcd = oldopcd;						/* return old opcode */
      }
      sts = OZ_SUCCESS;
      oz_knl_handle_putback (h_process);					/* release process handle */
    }
    oz_knl_section_iounlk (listseclock);					/* unlock array */
  }

  /* Clean up */

  oz_hw_cpu_setsoftint (si);							/* restore software interrupts */
  return (sts);									/* return composite status */
}

/************************************************************************/
/*									*/
/*  Get list of images for a process					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_process = handle of process to fetch list for			*/
/*	listsize  = number of elements in given list			*/
/*	listbuff  = points to given list array				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_process_imagelist = OZ_SUCCESS : successful		*/
/*	                                 else : error status		*/
/*	*listbuff[i].namebuff = filled in with image name string	*/
/*	 listbuff[i].h_iochan = image file IO channel			*/
/*	 listbuff[i].baseaddr = image base load address			*/
/*	*nimages_r = number of listbuff elements filled in		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (process_imagelist, OZ_Handle, h_process, uLong, listsize, OZ_Process_Imagelist *, listbuff, int *, nimages_r)

{
  char const *name;
  int i, namel, si;
  OZ_Image *image, *lastimage;
  OZ_Iochan *iochan;
  OZ_Process *myproc, *process;
  OZ_Seclock *listseclock;
  uByte *tempbuff;
  uLong sts, sts2, tempsize;
  void *base;

  i = 0;
  tempsize = 0;
  tempbuff = NULL;
  listseclock = NULL;

  myproc = process = oz_knl_process_getcur ();					/* save which process I'm in now */
  si  = oz_hw_cpu_setsoftint (0);						/* keep from being aborted */

  sts = oz_knl_section_iolock (cprocmode, listsize * sizeof listbuff[0], listbuff, 1, &listseclock, NULL, NULL, NULL);

  if (sts == OZ_SUCCESS) {
    for (; i < listsize; i ++) if (tempsize < listbuff[i].namesize) tempsize = listbuff[i].namesize;
    if (tempsize > 0) {
      tempbuff = OZ_KNL_PGPMALLOQ (tempsize);
      if (tempbuff == NULL) sts = OZ_EXQUOTAPGP;
    }
  }

  if ((sts == OZ_SUCCESS) && (h_process != 0)) sts = oz_knl_handle_takeout (h_process, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_PROCESS, &process, NULL);

  if (sts == OZ_SUCCESS) {
    lastimage = NULL;								/* start at beg of image list */
    for (i = 0; i < listsize; i ++) {						/* loop through given array */
      oz_knl_process_setcur (process);						/* switch to target process */
      image = oz_knl_image_next (lastimage, 0, 1);				/* get the next image */
      if (image == NULL) break;							/* stop if no more to do */
      if (lastimage != NULL) oz_knl_image_increfc (lastimage, -1);		/* release last image */
      base   = oz_knl_image_base (image);					/* get image base address */
      iochan = oz_knl_image_iochan (image);					/* get image file IO channel */
      if (tempsize > 0) {
        name  = oz_knl_image_name (image);					/* get image name string pointer */
        namel = strlen (name) + 1;						/* copy name string to temp buffer */
        if (namel > tempsize) namel = tempsize;					/* chop if too long */
        memcpy (tempbuff, name, namel);
      }
      oz_knl_process_setcur (myproc);						/* switch to caller process */
      listbuff[i].baseaddr = base;						/* copy base address out */
      if (listbuff[i].namesize != 0) {						/* copy name string out */
        if (namel > listbuff[i].namesize) namel = listbuff[i].namesize;
        sts = oz_knl_section_uput (cprocmode, namel, tempbuff, listbuff[i].namebuff);
        if (sts != OZ_SUCCESS) break;
      }
      sts = oz_knl_handle_assign (iochan, cprocmode, &(listbuff[i].h_iochan));	/* assign handle to image file */
      if (sts != OZ_SUCCESS) break;
      lastimage = image;
    }
    if (lastimage != NULL) {
      oz_knl_process_setcur (process);
      oz_knl_image_increfc (lastimage, -1);
    }
    oz_knl_process_setcur (myproc);
    if (h_process != 0) oz_knl_handle_putback (h_process);			/* release process handle */
  }

  /* Clean up */

  if (tempbuff == NULL) OZ_KNL_PGPFREE (tempbuff);				/* free off temp buffer */
  if (listseclock != NULL) oz_knl_section_iounlk (listseclock);			/* unlock array */
  sts2 = oz_knl_section_uput (cprocmode, sizeof *nimages_r, &i, nimages_r);	/* return number of images */
  if (sts == OZ_SUCCESS) sts = sts2;
  oz_hw_cpu_setsoftint (si);							/* restore software interrupts */
  return (sts);									/* return composite status */
}
