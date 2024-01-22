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
/*  XEN Disk driver - accesses virtual disks				*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_disk.h"
#include "oz_io_disk.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#include "oz_hwxen_defs.h"

#define DISK_BLOCK_SIZE (512)
#define BUFFER_ALIGNMENT (511)
#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)

/* Struct defs */

typedef struct Devex Devex;
typedef struct Iopex Iopex;

/* Device extension structure */

struct Devex { OZ_Devunit *devunit;		/* devunit pointer */
               const char *name;		/* device unit name (for messages) */
               int drive_id;			/* index in vbd_info array */
             };

/* I/O operation extension structure */

struct Iopex { Iopex *next;			/* next in iopex_qh/qt */
               OZ_Ioop *ioop;			/* pointer to io operation node */
               OZ_Procmode procmode;		/* requestor's processor mode */
               Devex *devex;			/* device extension data pointer */
               int writedisk;			/* 0: data transfer from device to memory; 1: data transfer from memory to device */
               int ringslots;			/* number of ring slots currently occupied */
               uLong status;			/* completion status */
               const OZ_Mempage *phypages;	/* physical page array pointer for next segment */
               uLong byteoffs;			/* starting physical page byte offset for next segment */
               uLong size;			/* buffer size remaining to be queued */
               OZ_Dbn slbn;			/* starting logical block number for next segment */
             };

/* Function tables */

static uLong vbd_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);
static const OZ_Devfunc vbd_functable = { sizeof (Devex), 0, sizeof (Iopex), 0, NULL, NULL, NULL, 
                                          NULL, NULL, NULL, vbd_start, NULL };

/* Internal static data */

static int initialized = 0;
static OZ_Devclass  *devclass_disk;
static OZ_Devdriver *devdriver_disk;

static blk_ring_t *blk_ring;		// block IO request ring buffer pointer
static int resp_cons;

static OZ_Smplock *smplock;		/* pointer to irq level smp lock */
static Iopex *iopex_qh;			/* requests waiting to be processed, NULL if none */
static Iopex **iopex_qt;
static int requestcount;		/* number of iopex's queued but not yet iodone'd (master only) */

#define MAX_VBDS 64			// max number of Xen devices
static int nr_vbds;			// number of devices defined by Xen
static Devex *devexs[MAX_VBDS];		// convert Xen device index to OZONE device struct
static xen_disk_t vbd_info[MAX_VBDS];	// Xen-provided device info

static Devex *crash_devex = NULL;
static int crash_inprog = 0;
static Iopex crash_iopex;
static OZ_Devunit *crash_devunit = NULL;

/* Internal routines */

static uLong vbd_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset);
static uLong vbd_queuereq (uLong size, OZ_Dbn slbn, const OZ_Mempage *phypages, uLong byteoffs, Iopex *iopex);
static void startreq (void);
static void procresponses (void);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_xendisk_init ()

{
  block_io_op_t op;
  char const *typestr;
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *devex;
  int i, rc;
  OZ_Devunit *devunit;
  OZ_Mempage sysvpage;
  uLong sts;

  if (initialized) return;

  oz_knl_printk ("oz_dev_xendisk_init\n");
  initialized    = 1;
  devclass_disk  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "xendisk");
  devdriver_disk = oz_knl_devdriver_create (devclass_disk, "xendisk");

  /* Reset ring buffer and get its machine (real-world) physical address */

  op.cmd = BLOCK_IO_OP_RESET;
  rc = HYPERVISOR_block_io_op (&op);
  if (rc != 0) {
    oz_knl_printk ("oz_dev_xendisk_init: error %d resetting ring\n", rc);
    return;
  }

  op.cmd = BLOCK_IO_OP_RING_ADDRESS;
  HYPERVISOR_block_io_op (&op);

  /* Map it to system virtual address space */

  sts = oz_knl_spte_alloc (1, (void **)&blk_ring, &sysvpage, NULL);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_xendisk_init: error %u allocating spte for ring buffer\n", sts);
    return;
  }

  oz_hwxen_pte_write (sysvpage, OZ_SECTION_PAGESTATE_VALID_W, op.u.ring_mfn, OZ_HW_PAGEPROT_KW, OZ_HW_PAGEPROT_KW);

  /* Clear the ring indexes */

  blk_ring -> req_prod = blk_ring -> resp_prod = resp_cons = 0;

  /* Find out about virtual disks (vbd's) defined for this domain */

  memset (&op, 0, sizeof op);
  op.cmd = BLOCK_IO_OP_VBD_PROBE;
  op.u.probe_params.domain    = 0;
  op.u.probe_params.xdi.max   = MAX_VBDS;
  op.u.probe_params.xdi.disks = vbd_info;
  op.u.probe_params.xdi.count = 0;

  rc = HYPERVISOR_block_io_op (&op);
  if (rc != 0) {
    oz_knl_printk ("oz_dev_xendisk_init: error %d probing number of vbds\n", rc);
    return;
  }
  nr_vbds = op.u.probe_params.xdi.count;
  if (nr_vbds <= 0) {
    oz_knl_printk ("oz_dev_xendisk_init: no vbd's defined\n");
    return;
  }

  /* Create an OZONE device table entry for each VBD */

  for (i = 0; i < nr_vbds; i ++) {

    /* Decode type and make sure it's a device we can actually process */

    typestr = NULL;
    if ((vbd_info[i].info & XD_TYPE_MASK) == XD_TYPE_FLOPPY) typestr = "floppy";
    if ((vbd_info[i].info & XD_TYPE_MASK) == XD_TYPE_CDROM)  typestr = "cdrom";
    if ((vbd_info[i].info & XD_TYPE_MASK) == XD_TYPE_DISK)   typestr = "hardisk";
    if (typestr == NULL) continue;

    /* Make up unit name and description strings */

    oz_sys_sprintf (sizeof unitname, unitname, "xen%cd_%d", typestr[0], i);
    oz_sys_sprintf (sizeof unitdesc, unitdesc, "virtual %s 0x%X", typestr, vbd_info[i].device);

    /* Create OZONE device table struct */

    devunit = oz_knl_devunit_create (devdriver_disk, unitname, unitdesc, &vbd_functable, 0, oz_s_secattr_sysdev);
    if (devunit == NULL) continue;

    /* Fill in driver-specific extension area */

    devexs[i] = devex = oz_knl_devunit_ex (devunit);
    memset (devex, 0, sizeof *devex);

    devex -> devunit  = devunit;
    devex -> name     = oz_knl_devunit_devname (devunit);
    devex -> drive_id = i;

    oz_knl_printk ("oz_dev_xendisk: %s totalblocks %u\n", devex -> name, vbd_info[i].capacity);

    /* Set up to autogen partitions and filesystems */

    oz_knl_devunit_autogen (devunit, oz_dev_disk_auto, NULL);
  }

  /* Ready to accept requests */

  smplock  = oz_hwxen_smplock_events + _EVENT_BLKDEV;
  iopex_qh = NULL;
  iopex_qt = &iopex_qh;
}

/************************************************************************/
/*									*/
/*  Start performing a disk i/o function				*/
/*									*/
/************************************************************************/

static uLong vbd_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  Iopex *iopex;
  uLong sts;

  devex = devexv;
  iopex = iopexv;

  iopex -> ioop     = ioop;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;

  /* Process individual functions */

  switch (funcode) {

    /* Set volume valid bit one way or the other (noop for us) */

    case OZ_IO_DISK_SETVOLVALID: {
	/* ?? have it process disk changed status bit */
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk from virtual memory */

    case OZ_IO_DISK_WRITEBLOCKS: {
      const OZ_Mempage *phypages;
      OZ_IO_disk_writeblocks disk_writeblocks;
      uLong byteoffs;

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_writeblocks.size, disk_writeblocks.buff, &phypages, NULL, &byteoffs);

      /* If that was successful, queue the request to the drive for processing */

      iopex -> writedisk = 1;
      if (sts == OZ_SUCCESS) sts = vbd_queuereq (disk_writeblocks.size, disk_writeblocks.slbn, phypages, byteoffs, iopex);
      return (sts);
    }

    /* Read blocks from the disk into virtual memory */

    case OZ_IO_DISK_READBLOCKS: {
      const OZ_Mempage *phypages;
      OZ_IO_disk_readblocks disk_readblocks;
      uLong byteoffs;

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = oz_knl_ioop_lockw (ioop, disk_readblocks.size, disk_readblocks.buff, &phypages, NULL, &byteoffs);

      /* If that was successful, queue the request to the drive for processing */

      iopex -> writedisk = 0;
      if (sts == OZ_SUCCESS) sts = vbd_queuereq (disk_readblocks.size, disk_readblocks.slbn, phypages, byteoffs, iopex);
      return (sts);
    }

    /* Get info part 1 */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
      disk_getinfo1.blocksize   = DISK_BLOCK_SIZE;
      disk_getinfo1.totalblocks = vbd_info[devex->drive_id].capacity;
      disk_getinfo1.bufalign    = BUFFER_ALIGNMENT;
      movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk from physical pages (kernel only) */

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);

      /* Queue the request to the drive for processing */

      iopex -> writedisk = 1;
      sts = vbd_queuereq (disk_writepages.size, disk_writepages.slbn, disk_writepages.pages, disk_writepages.offset, iopex);
      return (sts);
    }

    /* Read blocks from the disk into physical pages (kernel only) */

    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);

      /* Queue the request to the drive for processing */

      iopex -> writedisk = 0;
      sts = vbd_queuereq (disk_readpages.size, disk_readpages.slbn, disk_readpages.pages, disk_readpages.offset, iopex);
      return (sts);
    }

    /* Set crash dump device */

    case OZ_IO_DISK_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);		/* caller must be in kernel mode */
      if (crash_devunit != NULL) {					/* get rid of old crash stuff, if any */
        oz_knl_devunit_increfc (crash_devunit, -1);
        crash_devex   = NULL;
        crash_devunit = NULL;
      }
      if (ap != NULL) {
        if (as != sizeof (OZ_IO_disk_crash)) return (OZ_BADBUFFERSIZE);	/* param block must be exact size */
        ((OZ_IO_disk_crash *)ap) -> crashentry = vbd_crash;		/* return pointer to crash routine */
        ((OZ_IO_disk_crash *)ap) -> crashparam = NULL;			/* we don't require a parameter */
        ((OZ_IO_disk_crash *)ap) -> blocksize  = DISK_BLOCK_SIZE;	/* tell them our blocksize */
        crash_devex   = devex;						/* save the device we will write to */
        crash_devunit = devunit;
        oz_knl_devunit_increfc (crash_devunit, 1);			/* make sure it doesn't get deleted */
      }
      return (OZ_SUCCESS);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Crash dump routine - write logical blocks with interrupts disabled	*/
/*									*/
/*    Input:								*/
/*									*/
/*	lbn     = block to start writing at				*/
/*	size    = number of bytes to write (multiple of blocksize)	*/
/*	phypage = physical page to start writing from			*/
/*	offset  = offset in first physical page				*/
/*									*/
/*    Output:								*/
/*									*/
/*	vbd_crash = OZ_SUCCESS : successful				*/
/*	                  else : error status				*/
/*									*/
/************************************************************************/

static uLong vbd_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset)

{
  OZ_Datebin now;
  uByte status;
  uLong sts, wsize;

  if ((size | offset) & BUFFER_ALIGNMENT) return (OZ_UNALIGNEDBUFF);

  if (!crash_inprog) {
    crash_inprog = 1;				/* ... the first time only */
  }

  iopex_qh = NULL;				/* make sure the 'pending' queue is empty */
  iopex_qt = &iopex_qh;
  smplock  = NULL;				/* we can't use the smp lock anymore */

  /* Repeat as long as there is stuff to write */

  while (size > 0) {

    /* See how much we can write (up to end of current physical page) */

    wsize = PAGESIZE - offset;
    if (wsize > size) wsize = size;

    /* Queue write request and start processing it - since the queue is empty, it should start right away */

    requestcount = 0;
    memset (&crash_iopex, 0, sizeof crash_iopex);
    crash_iopex.procmode  = OZ_PROCMODE_KNL;
    crash_iopex.devex     = crash_devex;
    crash_iopex.status    = OZ_PENDING;
    crash_iopex.writedisk = 1;
    sts = vbd_queuereq (wsize, lbn, &phypage, offset, &crash_iopex);
    if (sts == OZ_STARTED) {

      /* Now keep calling the interrupt service routine until the request completes */

      while (requestcount != 0) { procresponses (); startreq (); }	/* wait for it to finish */
      sts = crash_iopex.status;						/* get completion status */
    }

    /* Check the completion status */

    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_xendisk crash: error %u writing to lbn %u\n", sts, lbn);
      return (sts);
    }

    /* Ok, on to next physical page */

    size    -= wsize;
    offset  += wsize;
    phypage += offset >> OZ_HW_L2PAGESIZE;
    offset  &= PAGESIZE - 1;
    lbn     += wsize / DISK_BLOCK_SIZE;
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Queue I/O request							*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of transfer in bytes				*/
/*	slbn = starting logical block number				*/
/*	phypages = pointer to array of physical page numbers		*/
/*	byteoffs = byte offset in first physical page			*/
/*	iopex = iopex block to use for operation			*/
/*	iopex -> writedisk = set if writing to disk			*/
/*									*/
/*    Output:								*/
/*									*/
/*	vbd_queuereq = OZ_STARTED : requeust queued to disk drive and 	*/
/*	                            drive started if it was idle	*/
/*	                     else : error status			*/
/*									*/
/************************************************************************/

static uLong vbd_queuereq (uLong size, OZ_Dbn slbn, const OZ_Mempage *phypages, uLong byteoffs, Iopex *iopex)

{
  Devex *devex;
  uLong hd;
  OZ_Dbn elbn;

  devex = iopex -> devex;

  /* If no buffer, instant success */

  if (size == 0) return (OZ_SUCCESS);

  /* Xen requires block-aligned (size and address) transfers */

  if ((size | byteoffs) & BUFFER_ALIGNMENT) return (OZ_UNALIGNEDBUFF);

  /* Make sure request doesn't run off end of disk */

  elbn = (size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE + slbn;
  if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
  if (elbn > vbd_info[devex->drive_id].capacity) return (OZ_BADBLOCKNUMBER);

  /* Make up a read/write request struct */

  iopex -> size      = size;				/* save buffer size */
  iopex -> slbn      = slbn;				/* save starting logical block number */
  iopex -> phypages  = phypages;			/* save physical page array pointer */
  iopex -> byteoffs  = byteoffs;			/* save starting physical page byte offset */
  iopex -> ringslots = 0;				/* no ring slots so far */
  iopex -> status    = OZ_SUCCESS;			/* assume it will succeed */

  if (smplock != NULL) hd = oz_hw_smplock_wait (smplock); /* inhibit disk event notification */
  requestcount ++;					/* ok, there is now one more request */
  *iopex_qt = iopex;					/* link to end of queue */
  iopex -> next = NULL;
  iopex_qt = &(iopex -> next);
  startreq ();						/* if room in ring, start it going */
  if (smplock != NULL) oz_hw_smplock_clr (smplock, hd);	/* restore disk event notification */

  return (OZ_STARTED);					/* the request has been started, will complete asyncly */
}

/************************************************************************/
/*									*/
/*  Start processing the request that's on top of queue			*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex_qh/qt = read/write request queue				*/
/*	smplock = device's irq smp lock					*/
/*									*/
/*    Output:								*/
/*									*/
/*	iopex_qh/qt = top request removed				*/
/*	disk operation started						*/
/*									*/
/************************************************************************/

static void startreq (void)

{
  blk_ring_req_entry_t *reqslot;
  block_io_op_t op;
  int diff, i, indx, j, wackit;
  Iopex *iopex;

  wackit = 0;									// haven't put anything in ring yet

  while ((iopex = iopex_qh) != NULL) {						// see if anything in the queue
    while (iopex -> size != 0) {
      if (blk_ring -> resp_prod != resp_cons) procresponses ();			// maybe there are responses to process
      indx = blk_ring -> req_prod;						// get index to put something in the ring with
      diff = indx - blk_ring -> resp_prod;					// see how many slots are in use now
      if (diff < 0) diff += BLK_RING_SIZE;
      if (diff >= BLK_RING_SIZE - 1) goto getout;				// if full, can't queue anything more

      reqslot = &(blk_ring -> ring[indx].req);					// point to ring slot to fill in

      if (sizeof iopex > sizeof reqslot -> id) oz_crash ("oz_dev_xendisk startreq*: iopex doesn't fit in reqslot -> id");
      reqslot -> id = (uLong)iopex;						// reqid is pointer to iopex
      reqslot -> sector_number = iopex -> slbn;					// starting block number
      reqslot -> device = vbd_info[iopex->devex->drive_id].device;		// Xen's device number
      reqslot -> operation = iopex -> writedisk ? XEN_BLOCK_WRITE : XEN_BLOCK_READ;

      for (i = 0; i < MAX_BLK_SEGS; i ++) {					// we can do this many segments in one request
        if (iopex -> size == 0) break;						// see if there is anything left to do
        iopex -> phypages += iopex -> byteoffs / PAGESIZE;			// ok, normalize byteoffs .lt. PAGESIZE
        iopex -> byteoffs %= PAGESIZE;
        j = PAGESIZE - iopex -> byteoffs;					// see how many bytes to end of page
        if (j > iopex -> size) j = iopex -> size;				// but not more than there is left to do
        j /= DISK_BLOCK_SIZE;							// see how many blocks there are to do
        if (j >= DISK_BLOCK_SIZE) j = DISK_BLOCK_SIZE;				// but not more than this many at once
        reqslot -> buffer_and_sects[i] = oz_hwxen_patoma (*(iopex -> phypages) * PAGESIZE) | iopex -> byteoffs | j;
										// <31:12> = machine page number
										// <11:09> = block offset in page
										// <08:00> = number of blocks
        j *= DISK_BLOCK_SIZE;							// get number of bytes being transferred
        iopex -> size -= j;							// subtract from size
        iopex -> byteoffs += j;							// increment byte offset for next segment
      }
      reqslot -> nr_segments = i;						// save number of segments used
      iopex -> ringslots ++;							// request occupies one more ring slot
      if (++ indx == BLK_RING_SIZE) indx = 0;					// increment slot index with wrap
      OZ_HW_MB;									// make sure other CPU's see contents updated
      blk_ring -> req_prod = indx;						// ... before they see index incremented
      wackit = 1;
    }
    if ((iopex_qh = iopex -> next) == NULL) iopex_qt = &iopex_qh;		// queued whole thing, remove from queue
    iopex -> next = iopex;
  }
getout:

  /* If we put anything new in queue, wack the Hypervisor */

  if (wackit) {
    op.cmd = BLOCK_IO_OP_SIGNAL;
    wackit = HYPERVISOR_block_io_op (&op);
    if (wackit != 0) oz_knl_printk ("oz_dev_xendisk startreq: error %d signalling\n", wackit);
  }
}

/************************************************************************/
/*									*/
/*  Interrupt service routine entrypoint				*/
/*									*/
/*    Input:								*/
/*									*/
/*	mchargs = cpu state at point of interrupt (not used)		*/
/*									*/
/************************************************************************/

void oz_hwxen_event_blkdev  (OZ_Mchargs *mchargs)

{
  uLong hd;

  if (smplock != NULL) hd = oz_hw_smplock_wait (smplock);
  procresponses ();
  startreq ();
  if (smplock != NULL) oz_hw_smplock_clr (smplock, hd);
}

/************************************************************************/
/*									*/
/*  If there are any responses in the ring to process, process them	*/
/*									*/
/*    Input:								*/
/*									*/
/*	resp_cons = index of next response to process			*/
/*	blk_ring -> resp_prod = index past last response in there	*/
/*									*/
/*    Output:								*/
/*									*/
/*	resp_cons = incremented to match resp_prod			*/
/*	corresponding IO requests completed				*/
/*									*/
/************************************************************************/

static void procresponses (void)

{
  blk_ring_resp_entry_t *resp;
  Iopex *iopex;

  while (blk_ring -> resp_prod != resp_cons) {				// see if there is a response to be processed
    OZ_HW_MB;								// make sure response content is valid first
    resp = &(blk_ring -> ring[resp_cons].resp);				// point to the response packet
    iopex = (Iopex *)(resp -> id);					// get IO request pointer from id
    if (resp -> status != 0) {						// mark request failed if it failed
      oz_knl_printk ("oz_dev_xendisk: %s: io error %u\n", iopex -> devex -> name, resp -> status);
      iopex -> status = OZ_IOFAILED;
      if (iopex -> size != 0) {						// ... and don't bother with any more of it
        if (iopex != iopex_qh) oz_crash ("oz_dev_xendisk procresponses: iopex %p not on top of queue", iopex);
        if ((iopex_qh = iopex -> next) == NULL) iopex_qt = &iopex_qh;
        iopex -> next = iopex;
        iopex -> size = 0;
      }
    }
    if ((-- (iopex -> ringslots) == 0) && (iopex -> size == 0)) {	// post request if all done
      if (iopex -> next != iopex) oz_crash ("oz_dev_xendisk procresponses: iopex %p still on queue", iopex);
      if (iopex -> ioop != NULL) oz_knl_iodonehi (iopex -> ioop, iopex -> status, NULL, NULL, NULL);
      requestcount --;
    }
    if (++ resp_cons == BLK_RING_SIZE) resp_cons = 0;			// increment index to next response, with wrap
  }
}
