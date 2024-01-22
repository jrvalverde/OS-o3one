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
/*  Definitions for hardware-specific type "xen"			*/
/*									*/
/************************************************************************/

#ifndef _OZ_HW_XEN_H
#define _OZ_HW_XEN_H

#define globaldef
#define globalref extern

#define OZ_HW_MAXINSLEN 16		// max bytes of an instruction (for disassembly, etc)

/* Processor mode - kernel or user */

typedef enum { OZ_PROCMODE_SYS = -1, 	// special mode flag to pass to oz_sys_pdata_... routines
					// ... to indicate we want the system global data (oz_s_systempdata)
               OZ_PROCMODE_KNL =  0, 
               OZ_PROCMODE_USR =  1 } OZ_Procmode;

#define OZ_PROCMODE_MIN OZ_PROCMODE_KNL
#define OZ_PROCMODE_MAX OZ_PROCMODE_USR

/* Max CPU's we can handle */

#define OZ_HW_MAXCPUS (1)

/* LOG-2 of the page size */

#define OZ_HW_PROCLINKBASE OZ_HW_BASE_PROC_VA	// base address user images get linked at
#define OZ_HW_L2PAGESIZEMAX (12)		// max page size possible on these CPU's
#define OZ_HW_L2PAGESIZE (12)			// actual page size on this CPU
#define OZ_HW_PAGESIZE (4096)
#define OZ_HW_PAGEMASK (4095)

/* Endianness */

#define OZ_HW_LITTLEENDIAN 1
#define OZ_HW_BIGENDIAN 0

/* Number of virtual address bits */

#define OZ_HW_32BIT 1

/* Non-zero to include memoury colouring (keeps phypages */
/* aligned with processor cache, see oz_knl_phymem.c)    */

#define OZ_HW_MEMORYCOLORING 1

/* Holds a quadword date */

typedef unsigned long long OZ_Datebin;

/* Add, Subtract, Compare two datebin's */

#define OZ_HW_DATEBIN_ADD(__s,__a,__b) (__s) = (__a) + (__b)
#define OZ_HW_DATEBIN_SUB(__d,__s,__m) (__d) = (__s) - (__m)
#define OZ_HW_DATEBIN_CMP(__s,__m) ((__s) < (__m) ? -1 : ((__s) > (__m)))

/* Clear and test a datebin */

#define OZ_HW_DATEBIN_CLR(__d) (__d) = 0
#define OZ_HW_DATEBIN_TST(__d) ((__d) != 0)

/* Holds an Iota time */

typedef unsigned long long OZ_Iotatime;

/* Base virt address of sytem common and per-process space */
/* These must match the setup in oz_params_486.s           */

#define OZ_HW_BASE_SYSC_VA (0xC0000000)	/* system common addresses go from C0000000 to FBFFFFFF (960M) */
#define OZ_HW_BASE_SYSC_VP (0xC0000)	/* system common base page number */
#define OZ_HW_BASE_PROC_VA (0)		/* process addresses go from 0 to BFFFFFFF (3G) */
#define OZ_HW_BASE_PROC_VP (0)		/* process base page number */

#define OZ_HW_ISSYSADDR(__x) (((OZ_Pointer)(__x)) >= (OZ_Pointer)OZ_HW_BASE_SYSC_VA)
#define OZ_HW_ISSYSPAGE(__x) ((__x) >= OZ_HW_BASE_SYSC_VP)

/* Misc memory layout */

#define OZ_LDR_KNLBASEVA OZ_HW_BASE_SYSC_VA	// used by oz_knl_image_elf32.c for kernel image link address
#define OZ_HW_PROCLINKBASE (0x08000000)		// used by oz_util_ldelf32.c for default image link address

/* Where to put dynamically allocated stuff in virtual address space */

#define OZ_HW_DVA_PIMAGES_VA OZ_HW_PROCLINKBASE
#define OZ_HW_DVA_PIMAGES_AT OZ_MAPSECTION_ATBEG

#define OZ_HW_DVA_SIMAGES_VA OZ_HW_BASE_SYSC_VA
#define OZ_HW_DVA_SIMAGES_AT OZ_MAPSECTION_ATBEG

#define OZ_HW_DVA_KNLHEAP_VA OZ_HW_PROCLINKBASE
#define OZ_HW_DVA_KNLHEAP_AT OZ_MAPSECTION_ATBEG

#define OZ_HW_DVA_USRHEAP_VA OZ_HW_PROCLINKBASE
#define OZ_HW_DVA_USRHEAP_AT OZ_MAPSECTION_ATBEG

#define OZ_HW_DVA_USRSTACK_VA OZ_HW_PROCLINKBASE
#define OZ_HW_DVA_USRSTACK_AT OZ_MAPSECTION_ATEND

/* Crash dumper allows us to have a memory hole - these are phypage numbers */

#define OZ_HW_CRASH_HOLEBEG 0
#define OZ_HW_CRASH_HOLEEND 0

/* This unsigned integer type can hold a pointer */

typedef uLong OZ_Pointer;

/* This unsigned integer type can hold an physical address */

typedef uLong OZ_Phyaddr;

/* Macro to extract the offset in a page of a pointer */

#define OZ_HW_VADDRTOPOFFS(vaddr) (uLong)(((uLong)vaddr) & ((1 << 12) - 1))

/* Macro to convert a virtual address (of type void *) to a virtual page number (of type OZ_Mempage) */

#define OZ_HW_VADDRTOVPAGE(vaddr) (OZ_Mempage)(((uLong)vaddr) >> 12)

/* Macro to convert a virtual page number (of type OZ_Mempage) to a virtual address (of type void *) */

#define OZ_HW_VPAGETOVADDR(vpage) (void *)(((uLong)vpage) << 12)

/* Macro to say whether or not an hardware page table entry has an extra bit to distinguish */
/* OZ_SECTION_PAGESTATE_VALID_* states from the OZ_SECTION_PAGESTATE_WRITEINPROG state.     */
/* Set to '1' if it does have this ability, set to '0' if not.                              */
/* See oz_knl_section.h and oz_knl_section.c for more information.                          */

#define OZ_HW_WRITEINPROG_VALID 1

/* Values returned by oz_hw_pte_read... and passed to oz_hw_pte_write... for page protections */

typedef uByte OZ_Hw_pageprot;

#define OZ_HW_PAGEPROT_NA (0)
#define OZ_HW_PAGEPROT_KW (1)
#define OZ_HW_PAGEPROT_UR (2)
#define OZ_HW_PAGEPROT_UW (3)
#define OZ_HW_PAGEPROT_MAX (4)

/* Size (in bytes) of thread hardware context block */
/* Must match THCTX__SIZE in oz_params_xen.s        */

#define OZ_THREAD_HW_CTX_SIZE 576

/* Size (in bytes) of process hardware context block */
/* Must match sizeof Prctx in oz_hw_process_486.c    */

#define OZ_PROCESS_HW_CTX_SIZE 24

/* Size (in bytes) of smp lock block      */
/* Must match SL__SIZE in oz_params_xen.s */

#define OZ_SMPLOCK_SIZE 2
typedef struct { uByte opaque[OZ_SMPLOCK_SIZE]; } OZ_Smplock;

/* Value returned by oz_hw_smplock_wait and oz_hw_setsoftint when */
/* there were no smp locks set and softint delivery was enabled   */

#define OZ_SMPLOCK_NULL 0

/* Value returned by oz_hw_smplock_wait and oz_hw_setsoftint when */
/* there were no smp locks set and softint delivery was inhibited */

#define OZ_SMPLOCK_SOFTINT 1

/* All the other SMP lock levels - this implementation allows 8 bits for the smp level */

#define OZ_SMPLOCK_LEVEL_SH 0x10		/* shutdown handler table */
#define OZ_SMPLOCK_LEVEL_HT 0x18		/* handle tables */
#define OZ_SMPLOCK_LEVEL_VL 0x20		/* filesystem volume locks */
#define OZ_SMPLOCK_LEVEL_DV 0x28		/* devices */
#define OZ_SMPLOCK_LEVEL_PT 0x30		/* per-process page table */
#define OZ_SMPLOCK_LEVEL_GP 0x40		/* global sections page table */
#define OZ_SMPLOCK_LEVEL_PM 0x48		/* physical memory tables */
#define OZ_SMPLOCK_LEVEL_CP 0x49		/* cache private lock */
#define OZ_SMPLOCK_LEVEL_PS 0x4A		/* per-process state */
#define OZ_SMPLOCK_LEVEL_TF 0x4C		/* thread family list */
#define OZ_SMPLOCK_LEVEL_EV 0x4E		/* individual event flag state */
#define OZ_SMPLOCK_LEVEL_TP 0x50		/* individual thread private lock */
#define OZ_SMPLOCK_LEVEL_TC 0x52		/* thread COM queue lock */
#define OZ_SMPLOCK_LEVEL_PR 0x54		/* process lists */
#define OZ_SMPLOCK_LEVEL_SE 0x60		/* security structs */
#define OZ_SMPLOCK_LEVEL_ID 0x64		/* id numbers (<NP, >PR, >TF) */
#define OZ_SMPLOCK_LEVEL_NP 0x68		/* non-paged pool */
#define OZ_SMPLOCK_LEVEL_QU 0x70		/* quota */

#define OZ_SMPLOCK_LEVEL_EVENTS 0xA0		/* async events use 0xA0..0xBF ** must match oz_params_xen.s ** */
						/* A0 corresponds to bit 0, BF corresponds to bit 31, etc */

#define OZ_SMPLOCK_LEVEL_LG 0xFA		/* log routines */
#define OZ_SMPLOCK_LEVEL_HI 0xFC		/* lowipl routines */

/* Array of smplocks used for event handlers                       */
/* Indexed by event bit number (_EVENT_* in oz_hwxen_hypervisor.h) */

extern OZ_Smplock oz_hwxen_smplock_events[32];

/* Thread priority definitions */

#define OZ_THREADPRIO_MAXIMUM 0xFFFFFF	/* this implementation allows 24 bits */
#define OZ_THREADPRIO_STARTUP 1000000	/* this is the startup thread's priority */

/* Wait loop macros - see oz_knl_thread.c's and oz_ldr_thread.c's oz_knl_thread_wait routine */

#define OZ_HW_WAITLOOP_INIT oz_hwxen_waitloop_init ()				// enable softints but inhibit hardints
#define OZ_HW_WAITLOOP_BODY oz_hwxen_waitloop_body ()				// enable all ints, wait for int, inhib all ints
#define OZ_HW_WAITLOOP_TERM oz_hwxen_waitloop_term ()				// enable hardints but inhibit softints
#define OZ_HW_WAITLOOP_WAKE(cpuidx) 						// wake the indicated cpu from waitloop body

/* Unsigned value that can hold what the pte holds a physical page number in */
/* It is also used to hold virtual page numbers                              */

typedef uLong OZ_Mempage;

/* Unsigned value that can hold the offset within a page of where a particular byte is */

typedef uLong OZ_Mempofs;	/* note - could really be just a uWord but this keeps things longword aligned */

/* A page table entry - opaque struct of correct size */

#define OZ_L2PAGENTRYSIZE 2
typedef struct { uByte opaque[1<<OZ_L2PAGENTRYSIZE]; } OZ_Pagentry;

/* A physical page number that will never be used */

#define OZ_PHYPAGE_NULL 0

/* A virtual page number that will never be mapped to anything */

#define OZ_VIRTPAGE_NULL 0

/* Various address conversion routines */

OZ_Pointer oz_hwxen_matopa (OZ_Pointer ma);
OZ_Pointer oz_hwxen_matosa (OZ_Pointer ma);
OZ_Pointer oz_hwxen_patoma (OZ_Pointer pa);
OZ_Pointer oz_hwxen_patosa (OZ_Pointer pa);
OZ_Pointer oz_hwxen_satoma (OZ_Pointer sa);
OZ_Pointer oz_hwxen_satopa (OZ_Pointer sa);
OZ_Pointer oz_hwxen_vatoma (OZ_Pointer va);
OZ_Pointer oz_hwxen_vatopa (OZ_Pointer va);
OZ_Pointer oz_hwxen_vatosa (OZ_Pointer va);

/* Memory mapping */

extern uLong *oz_hwxen_mpdsa;			// master pagedirectory static address
extern uLong *oz_hwxen_sptsa;			// system pagetable static address (pt that maps OZ_HW_BASE_SYSC_VP)
extern uLong oz_hwxen_mpdma;			// master pagedirectory machine address
extern uLong volatile *oz_hwxen_sysreqprots;	// system (common) reqprots table static address

/* Maximize processor mode */

extern inline OZ_Procmode oz_hw_procmode_max (OZ_Procmode procmode)

{
  uWord cs;

  asm ("movw %%cs,%0" : "=r" (cs));
  if (cs & 2) procmode = OZ_PROCMODE_USR;
  return (procmode);
}

/* Determine if a particular range of addesses is accessible */
/* in the current memory mapping by the given processor mode */

int oz_hw_probe (uLong size, const void *adrs, OZ_Procmode mode, int write);
int oz_hw_prober_strz (const void *adrz, OZ_Procmode mode);
#define OZ_HW_READABLE(__size,__adrs,__mode) oz_hw_probe ((__size), (__adrs), (__mode), 0)
#define OZ_HW_WRITABLE(__size,__adrs,__mode) oz_hw_probe ((__size), (__adrs), (__mode), 1)
#define OZ_HW_READABLE_STRZ(__adrs,__mode) oz_hw_prober_strz ((__adrs), (__mode))

/* Make sure all memory ops before this complete before the ones after it */

#define OZ_HW_MB asm volatile ("xorl %%eax,%%eax \n cpuid" : : : "eax", "ebx", "ecx", "edx")

/* Write breakpoint */

typedef uByte OZ_Breakpoint;
#define OZ_OPCODE_BPT 0xCC

/* Create and Delete a thread's kernel stack */

#define OZ_HW_KSTACKINTHCTX 1			// 0 = oz_hw_thread_initctx will create its own kernel stack
						// 1 = oz_knl_thread_create will add space for kernel stack to hw ctx block
						// -- only applies to kernel, loader must always allocate kernel stack
uLong oz_hw486_kstack_create (void *thctx, void **sysvaddr_r);
void oz_hw486_kstack_delete (void *thctx, void *sysvaddr);

/* The sigargs and mchargs for condition handlers */

typedef struct OZ_Mchargs OZ_Mchargs;
typedef struct OZ_Mchargx OZ_Mchargx;

	/* Signal argument array elements */

typedef uLong OZ_Sigargs;

	/* Standard Machine arguments - describe state of processor at time of an exception or interrupt */
	/* Must match MCH_... offsets in oz_params_xen.s */

struct OZ_Mchargs { uLong ec2;				/* error code #2 */
                    uLong edi, esi;			/* pointer registers */
                    uLong ec1;				/* error code #1 */
                    uLong esp;				/* stack pointer */
                    uLong ebx, edx, ecx, eax;		/* general registers */
                    uLong ebp;				/* frame pointer */
                    uLong eip;				/* instruction pointer */
                    uWord cs;				/* code segment */
                    uWord pad1;				/* pad1<15> = event master enable flag */
                    uLong eflags;			/* processor eflags */
                  };

	/* Extended Machine arguments */
	/* They are not saved/restored automatically at the time of an exception or interrupt */
	/* Fetched via oz_hw_mchargx_fetch, Stored via oz_hw_mchargx_store */
	/* Must match MCHX_... offsets in oz_params_xen.s */

struct OZ_Mchargx { uWord ds, es, fs, gs, ss, pad1;	/* data and stack segment registers */
                    uLong dr0, dr1, dr2, dr3, dr7;	/* debug registers */
                    uLong cr0, cr2, cr3, cr4;		/* control registers */
                  };

/* The jmpbuf used by setjmp/longjmp routines */
/* Must match glibc jmp_buf definition        */
/* Must match oz_knl_ctrl_486.s definition    */

typedef struct { uLong ebx, esi, edi, ebp, esp, eip; } OZ_Hw_jmpbuf;

/* Define OZ_HW_SYSCALL_DCL_(0..16) and OZ_HW_SYSCALL_DEF_(0..16) macros */
/* The oz_hw486_syscall.h file is generated by oz_hw_486_syscall_h_gen.c */

#include "oz_hw486_syscall.h"

/* Physical page access */

void oz_hw486_phys_filllong (uLong fill, OZ_Phyaddr phyaddr);
void oz_hw486_phys_storelong (uLong value, OZ_Phyaddr phyaddr);
uLong oz_hw486_phys_fetchlong (OZ_Phyaddr phyaddr);

/* Event handlers */

void oz_hwxen_event_blkdev  (OZ_Mchargs *mchargs);
void oz_hwxen_event_timer   (OZ_Mchargs *mchargs);
void oz_hwxen_event_die     (OZ_Mchargs *mchargs);
void oz_hwxen_event_debug   (OZ_Mchargs *mchargs);
void oz_hwxen_event_net     (OZ_Mchargs *mchargs);
void oz_hwxen_event_ps2     (OZ_Mchargs *mchargs);
void oz_hwxen_event_stop    (OZ_Mchargs *mchargs);
void oz_hwxen_event_evtchn  (OZ_Mchargs *mchargs);
void oz_hwxen_event_vbd_upd (OZ_Mchargs *mchargs);

/* Inline replacements for atomic routines */

#define OZ_HW_ATOMIC_INCBY1_LONG(loc)  asm volatile ("lock\n\tincl %0" : : "m" (loc) : "cc", "memory")
#define OZ_HW_ATOMIC_DECBY1_LONG(loc)  asm volatile ("lock\n\tdecl %0" : : "m" (loc) : "cc", "memory")
#define OZ_HW_ATOMIC_INCBY1_ULONG(loc) asm volatile ("lock\n\tincl %0" : : "m" (loc) : "cc", "memory")

static inline Long oz_hw_atomic_inc_long (volatile Long *loc, Long inc)

{
  Long newval;

  asm volatile ("movl  %2,%0\n\t"
                "lock\n\t"
                "xaddl %0,%1\n\t"
                "addl  %2,%0"
                : "=&r" (newval)
                : "m" (*loc), "g" (inc)
                : "cc", "memory");

  return (newval);
}

static inline uLong oz_hw_atomic_inc_ulong (volatile uLong *loc, uLong inc)

{
  uLong newval;

  asm volatile ("movl  %2,%0\n\t"
                "lock\n\t"
                "xaddl %0,%1\n\t"
                "addl  %2,%0"
                : "=&r" (newval)
                : "m" (*loc), "g" (inc)
                : "cc", "memory");

  return (newval);
}

static inline Long oz_hw_atomic_or_long (volatile Long *loc, Long inc)

{
  Long newval, oldval;

  asm volatile ("movl     %3,%%eax\n"
                "0:\n\t"
                "movl     %%eax,%%edx\n\t"
                "orl      %2,%%edx\n\t"
                "lock\n\t"
                "cmpxchgl %%edx,%3\n\t"
                "jne      0b"
                : "=&a" (oldval), "=&d" (newval)
                : "g" (inc), "m" (*loc)
                : "cc", "memory");

  return (oldval);
}

static inline Long oz_hw_atomic_and_long (volatile Long *loc, Long inc)

{
  Long newval, oldval;

  asm volatile ("movl     %3,%%eax\n"
                "0:\n\t"
                "movl     %%eax,%%edx\n\t"
                "andl     %2,%%edx\n\t"
                "lock\n\t"
                "cmpxchgl %%edx,%3\n\t"
                "jne      0b"
                : "=&a" (oldval), "=&d" (newval)
                : "g" (inc), "m" (*loc)
                : "cc", "memory");

  return (oldval);
}

static inline Long oz_hw_atomic_set_long (volatile Long *loc, Long newval)

{
  Long oldval;

  asm volatile ("xchgl %0,%2"
                : "=r" (oldval)
                : "0" (newval), "m" (*loc)
                : "cc", "memory");

  return (oldval);
}

static inline int oz_hw_atomic_setif_long (volatile Long *loc, Long newval, Long oldval)

{
  char rc;

  asm volatile ("lock\n\t"
                "cmpxchgl %2,%3\n\t"
                "sete     %%al"
                : "=a" (rc)
                :  "a" (oldval), "r" (newval), "m" (*loc)
                : "cc", "memory");

  return (rc);
}

static inline int oz_hw_atomic_setif_ulong (volatile uLong *loc, uLong newval, uLong oldval)

{
  char rc;

  asm volatile ("lock\n\t"
                "cmpxchgl %2,%3\n\t"
                "sete     %%al"
                : "=a" (rc)
                :  "a" (oldval), "r" (newval), "m" (*loc)
                : "cc", "memory");

  return (rc);
}

static inline int oz_hw_atomic_setif_ptr (void *volatile *loc, void *newval, void *oldval)

{
  char rc;

  asm volatile ("lock\n\t"
                "cmpxchgl %2,%3\n\t"
                "sete     %%al"
                : "=a" (rc)
                :  "a" (oldval), "r" (newval), "m" (*loc)
                : "cc", "memory");

  return (rc);
}

/************************************************************************/
/*									*/
/*  Macros for use by the debugger					*/
/*									*/
/************************************************************************/

#if defined (_OZ_SYS_DEBUG_C)

#define SETSINGSTEP(__mchargs) __mchargs.eflags |= 0x100		/* set single-step mode in the mchargs */
#define CLRSINGSTEP(__mchargs) __mchargs.eflags &= ~0x100		/* clear single-step mode from the mchargs */
#define TSTSINGSTEP(__mchargs) (__mchargs.eflags & 0x100)		/* test to see if single step mode enabled in mchargs */
#define GETNXTINSAD(__mchargs) ((OZ_Breakpoint *)(__mchargs.eip))	/* get next instruction's address */
#define GETSTACKPTR(__mchargs) (__mchargs.esp)				/* get the stack pointer */
#define CMPSTACKPTR(__mchargs,__oldptr) (__mchargs.esp >= __oldptr)	/* return TRUE iff mchargs stack pointer is at same or outer level than oldptr */
#define GETFRAMEPTR(__mchargs) (__mchargs.ebp)				/* get the frame pointer */

#define TRACEBACK									\
  char bump;										\
  OZ_Pointer *ebp, ebpvals[2];								\
											\
  ebp = &(mchargs -> ebp);								\
  (dc -> cb -> print) (dc -> cp, "  rtn addr  savedebp       ebp        rtn addr\n");	\
  while (-- framecount >= 0) {								\
    if (!(dc -> cb -> readmem) (dc -> cp, ebpvals, sizeof ebpvals, ebp)) break;		\
    bump = ' ';										\
    if ((ebpvals[0] != 0) && (ebpvals[0] < 0x200)) {					\
      if (!(dc -> cb -> readmem) (dc -> cp, ebpvals, sizeof ebpvals, ebp + 1)) break;	\
      bump = '+';									\
    }											\
    (dc -> cb -> print) (dc -> cp, "  %8.8x  %8.8x%c: %8.8x : ", ebpvals[1], ebpvals[0], bump, ebp); \
    printaddr (dc, ebpvals[1], mchargs, mchargx);					\
    (dc -> cb -> print) (dc -> cp, "\n");						\
    ebp = (OZ_Pointer *)(ebpvals[0]);							\
  }

#endif

/************************************************************************/
/*									*/
/*  C Runtime library-like routines provided by hardware layer		*/
/*									*/
/************************************************************************/

void bcopy (const void *src, void *dest, unsigned int n);
void bzero (void *s, unsigned int n);

void *memchr    (const void *src, int c, unsigned int len);
void *memchrnot (const void *src, int c, unsigned int len);
int   memcmp    (const void *left, const void *right, unsigned int len);
void *memcpy    (void *dst, const void *src, unsigned int len);
void *memmove   (void *dst, const void *src, unsigned int len);
void *memset    (void *dst, int val, unsigned int len);
void  movc4     (unsigned int slen, const void *src, unsigned int dlen, void *dst);

int   strcasecmp  (const char *left, const char *right);
char *strcat      (char *dst, const char *src);
char *strchr      (const char *src, int c);
int   strcmp      (const char *left, const char *right);
char *strcpy      (char *dst, const char *src);
unsigned int strlen  (const char *src);
int   strncasecmp (const char *left, const char *right, unsigned int len);
char *strncat     (char *dst, const char *src, unsigned int len);
int   strncmp     (const char *left, const char *right, unsigned int len);
char *strncpy     (char *dst, const char *src, unsigned int len);	/* doesn't guarantee a null terminator */
char *strncpyz    (char *dst, const char *src, unsigned int len);	/* guarantees at least one null terminator */
unsigned int strnlen (const char *src, unsigned int len);		/* maximum length of 'len' */
char *strrchr     (const char *src, int c);
char *strstr      (const char *haystack, const char *needle);

#endif
