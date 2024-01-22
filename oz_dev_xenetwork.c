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
/*  XEN V1.2 Pseudo-Ethernet driver					*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_timer.h"
#include "oz_io_ether.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_sys_xprintf.h"

#include "oz_hwxen_defs.h"

#define ARPHWTYPE 1
#define ADDRSIZE 6
#define PROTOSIZE 2
#define DATASIZE 1500
#define BUFFSIZE (2*ADDRSIZE+PROTOSIZE+DATASIZE+4)

#define N2HW(__nw) (((__nw)[0] << 8) + (__nw)[1])
#define CEQENADDR(__ea1,__ea2) (memcmp (__ea1, __ea2, ADDRSIZE) == 0)

#define NRCVPAGES (RX_RING_SIZE/2)	// max number of entries active in receive ring
					// max this can be is RX_RING_SIZE-1
					// it cost us a physical page for each one

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Rxbuf Rxbuf;
typedef struct Txbuf Txbuf;

/* Format of ethernet buffers */

typedef struct { uByte dstaddr[ADDRSIZE];
                 uByte srcaddr[ADDRSIZE];
                 uByte proto[PROTOSIZE];
                 uByte data[DATASIZE];
                 uByte crc[4];
               } Ether_buf;

/* Format of receive buffers */

struct Rxbuf { Rxbuf *next;		/* next in receive list */
               uLong dlen;		/* length of data received (without header or crc) */
               Long volatile refcount;	/* ref count (number of iopex -> rxbuf's pointing to it) */
               int pteidx;		/* receive ring pagetable index */
               int ringindex;		/* lt 0 : no ring entry */
					/* else : index of owned ring entry, free it when refcount goes zero */
               unsigned short id;	/* ring request id number */
             };

/* Format of transmit buffers */

struct Txbuf { Txbuf *next;		/* next in transmit list */
               uLong dlen;		/* length of data to transmit (not incl header or crc) */
               Iopex *iopex;		/* associated iopex to post */
               uLong dmad;		/* data machine address (of .buf) */
               OZ_Datebin started;	/* when it was queued for transmit */
               unsigned short id;	/* ring request id number */
					/* buf must be physically contiguous */
               Ether_buf buf;		/* ethernet transmit data */
             };

/* Device extension structure */

struct Devex { Devex *next;			// next in devexs list
               OZ_Devunit *devunit;		// devunit pointer
               const char *name;		// devunit name string pointer
               int vif;				// virtual interface number

               int receive_requests_queued;
               int receive_buffers_queued;

               int in_procrcvring;
               int in_procxmtring;

               net_idx_t *indx;			// indices pointer
               net_ring_t *ring;		// ring buffer pointer
               unsigned int rx_resp_cons;	// receive response consumer index
               unsigned int tx_resp_cons;	// transmit response consumer index

               int rcvpteidx;			// index of next available rcvptemas/rcvbufvas
               uLong rcvptemas[NRCVPAGES];	// machine address of the receive buffer pte's
               uByte *rcvbufvas[NRCVPAGES];	// corresponding virtual addresses

               Txbuf *xmtprogqh, **xmtprogqt;	// transmits in progress (they're in the ring)
               Txbuf *xmtpendqh, **xmtpendqt;	// transmits pending (they're waiting for ring)

               Rxbuf *rcvprogqh, **rcvprogqt;	// receives in progress (they're in the ring)
               Rxbuf *rcvpendqh, **rcvpendqt;	// receives pending (they're waiting for ring)

               uLong promiscuous;		// >0: promiscuous mode; =0: normal
               Chnex *chnexs;			// all open channels on the device
               unsigned short lastid;		// last id assigned to receive or transmit request
               uByte enaddr[ADDRSIZE];		// hardware address
             };

/* Channel extension structure */

struct Chnex { Chnex *next;			/* next in devex->chnexs list */
               uWord proto;			/* protocol number (or 0 for all) */
               uWord promis;			/* promiscuous mode (0 normal, 1 promiscuous) */

               /* Receive related data */

               uLong rcvmissed;			/* number of receive packets missed */
               Iopex *rxreqh;			/* list of pending receive requests */
               Iopex **rxreqt;			/* points to rxreqh if list empty */
						/* points to last one's iopex -> next if requests in queue */
						/* NULL if channel is closed */
             };

/* I/O extension structure */

struct Iopex { Iopex *next;				/* next in list of requests */
               OZ_Ioop *ioop;				/* I/O operation block pointer */
               OZ_Procmode procmode;			/* processor mode of request */
               Devex *devex;				/* pointer to device */
               Chnex *chnex;				/* pointer to channel */

               /* Receive related data */

               OZ_IO_ether_receive receive;		/* receive request I/O parameters */
               Rxbuf *rxbuf;				/* pointer to buffer received */

               /* Transmit related data */

               OZ_IO_ether_transmit transmit;		/* transmit request I/O parameters */
               uLong status;				/* completion status */
             };

/* Function table */

static int xenetwork_shutdown (OZ_Devunit *devunit, void *devexv);
static uLong xenetwork_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int xenetwork_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static uLong xenetwork_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc xenetwork_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                                xenetwork_shutdown, NULL, NULL, xenetwork_assign, xenetwork_deassign, 
                                                NULL, xenetwork_start, NULL };

/* Internal static data */

static Devex *devexs = NULL;
static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Smplock *const smplock = oz_hwxen_smplock_events + _EVENT_NET;
static Rxbuf *volatile free_rxbufs = NULL;	// atomic access only
static Txbuf *volatile free_txbufs = NULL;	// atomic access only
static uByte broadcast[ADDRSIZE];

/* Internal routines */

static uLong close_channel (Devex *devex, Chnex *chnexx, Iopex *iopexx);
static int receive_done (Devex *devex, Rxbuf *rxbuf);
static void receive_iodone (void *iopexv, uLong *status_r);
static void freercvresp (Devex *devex, Rxbuf *rxbuf);
static Rxbuf *allocrcvbuf (void);
static void queuercvbuf (Devex *devex, Rxbuf *rxbuf);
static void procrcvring (Devex *devex);
static void freercvbuf (Rxbuf *rxbuf);
static Txbuf *allocxmtbuf (void);
static void queuexmtbuf (Devex *devex, Txbuf *txbuf);
static void procxmtring (Devex *devex);
static void freexmtbuf (Txbuf *txbuf);
static void transmit_iodone (void *txbufv, uLong *status_r);

/************************************************************************/
/*									*/
/*  HYPERVISOR data structures						*/
/*									*/
/************************************************************************/

/*

  oz_hwxen_sharedinfo.net_idx[MAX_DOMAIN_VIFS]   <- one entry per possible virtual interface
                                                    devex->vif indexes this array
                                                    devex->indx points to element of this array

    .tx_req_prod = guest OS places packets into ring at this index
    .tx_resp_prod = Xen increments this past the last repsonse placed in ring
    .tx_event = guest OS receives EVENT_NET when tx_resp_prod equals tx_event

    .rx_req_prod = guest OS places empty buffers into ring at this index
    .rx_resp_prod = Xen increments this past the last repsonse placed in ring
    .rx_event = guest OS receives EVENT_NET when rx_resp_prod equals rx_event

  each mapped device has a net_ring_t that it gets pointer to from NETOP_GET_VIF_INFO call

    .tx_ring[TX_RING_SIZE] = transmit buffer descriptors
       .req = request parameters
       .resp = response parameters

    .rx_ring[RX_RING_SIZE] = receive buffer descriptors
       .req = request parameters
          .id = buffer id
          .addr = ma of pte to swizzle
       .resp = response parameters
          .id = buffer id
          .size = received packet size in bytes
          .status = per-descriptor status
          .offset = offset in page of received packet

  Ring pointer usage (same for receive or transmit) (mod XX_RING_SIZE):

    xx_resp_cons <= xx_resp_prod <= xx_event <= xx_req_prod

    An empty condition is indicated by all four pointers being equal
    The fullest we can have is when xx_req_prod-xx_resp_cons == XX_RING_SIZE-1

    This driver increments xx_resp_cons as it processes responses
    Xen increments xx_resp_prod as it receives or transmits packets
    This driver increments xx_event to keep it ahead of xx_resp_prod
    This driver increments xx_req_prod as it queues requests

*/

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_xenetwork_init ()

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  int i, j, rc;
  Devex *devex;
  netop_t netop;
  OZ_Devunit *devunit;
  OZ_Mempage phypage, sysvpage;
  uLong sts;

  if (initialized) return;

  oz_knl_printk ("oz_dev_xenetwork_init\n");
  initialized = 1;

  memset (broadcast, -1, sizeof broadcast);

  devclass  = oz_knl_devclass_create (OZ_IO_ETHER_CLASSNAME, OZ_IO_ETHER_BASE, OZ_IO_ETHER_MASK, "xenetwork");
  devdriver = oz_knl_devdriver_create (devclass, "xenetwork");

  for (i = 0; i < MAX_DOMAIN_VIFS; i ++) {
    netop.cmd = NETOP_RESET_RINGS;							// reset ring indices for net dev [i]
    netop.vif = i;
    rc = HYPERVISOR_net_io_op (&netop);
    if (rc != 0) {
      oz_knl_printk ("oz_dev_xenetwork_init: error %d resetting vif %d rings\n", rc, i);
      continue;
    }

    netop.cmd = NETOP_GET_VIF_INFO;							// see if there is a net dev [i]
    netop.vif = i;
    rc = HYPERVISOR_net_io_op (&netop);
    if (rc != 0) {
      oz_knl_printk ("oz_dev_xenetwork_init: error %d getting vif %d info\n", rc, i);
      continue;
    }

    oz_knl_printk ("oz_dev_xenetwork_init: found vif %d, address %2.2X-%2.2X-%2.2X-%2.2X-%2.2X-%2.2X\n", i, 
	netop.u.get_vif_info.vmac[0], netop.u.get_vif_info.vmac[1], netop.u.get_vif_info.vmac[2], 
	netop.u.get_vif_info.vmac[3], netop.u.get_vif_info.vmac[4], netop.u.get_vif_info.vmac[5]);

    oz_sys_sprintf (sizeof unitname, unitname, "xenet_%d", i);				// create xenet_<i> device table entry
    oz_sys_sprintf (sizeof unitdesc, unitdesc, "virtual ethernet %2.2X-%2.2X-%2.2X-%2.2X-%2.2X-%2.2X", 
	netop.u.get_vif_info.vmac[0], netop.u.get_vif_info.vmac[1], netop.u.get_vif_info.vmac[2], 
	netop.u.get_vif_info.vmac[3], netop.u.get_vif_info.vmac[4], netop.u.get_vif_info.vmac[5]);
    devunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &xenetwork_functable, 0, oz_s_secattr_sysdev);
    devex   = oz_knl_devunit_ex (devunit);						// point to driver-specific extension area
    memset (devex, 0, sizeof *devex);							// clear it out

    devex -> devunit = devunit;								// save devunit pointer
    devex -> name    = oz_knl_devunit_devname (devunit);				// save devname string pointer
    devex -> vif     = i;								// save Xen's device index number
    devex -> indx    = oz_hwxen_sharedinfo.net_idx + i;					// set up indices struct pointer
    memcpy (devex -> enaddr, netop.u.get_vif_info.vmac, sizeof devex -> enaddr);	// save its ethernet address

    devex -> xmtprogqt = &(devex -> xmtprogqh);						// set up queue tail pointers
    devex -> xmtpendqt = &(devex -> xmtpendqh);
    devex -> rcvprogqt = &(devex -> rcvprogqh);
    devex -> rcvpendqt = &(devex -> rcvpendqh);

    /* Map ring buffer to a free spte.  This puts it in the kernel dynamic expansion area. */

    sts = oz_knl_spte_alloc (1, (void **)&(devex -> ring), &sysvpage, NULL);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_xenetwork_init: error %u allocating sptes for ring buffer\n", sts);
      return;
    }
    oz_hwxen_pte_write (sysvpage, OZ_SECTION_PAGESTATE_VALID_W, netop.u.get_vif_info.ring_mfn, OZ_HW_PAGEPROT_KW, OZ_HW_PAGEPROT_KW);

    /* Allocate some physical pages for use in the receive ring.  I don't think we can assume we get the same actual pages back. */

    for (j = 0; j < NRCVPAGES; j ++) {
      phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, j);		// allocate page, get pseudo-phys page number
      devex -> rcvptemas[j] = oz_hwxen_vatoma ((OZ_Pointer)(oz_hwxen_sptsa + phypage));	// this is static pte's machine address
      devex -> rcvbufvas[j] = (uByte *)oz_hwxen_patosa (phypage << OZ_HW_L2PAGESIZE);	// save static mapping address
    }

    devex -> next = devexs;								// link to list for interrupt routine to see
    devexs = devex;
  }

  if (devexs == NULL) oz_knl_printk ("oz_dev_xenetwork_init: no devices found\n");
}

/************************************************************************/
/*									*/
/*  Shutdown device							*/
/*									*/
/************************************************************************/

static int xenetwork_shutdown (OZ_Devunit *devunit, void *devexv)

{
  return (1);
}

/************************************************************************/
/*									*/
/*  A new channel was assigned to the device				*/
/*  This routine initializes the chnex area				*/
/*									*/
/************************************************************************/

static uLong xenetwork_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  A channel is about to be deassigned from a device			*/
/*  Here we do a close if it is open					*/
/*									*/
/************************************************************************/

static int xenetwork_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  uLong sts;

  chnex = chnexv;

  if (chnex -> rxreqt == NULL) return (0);

  sts = oz_knl_iostart3 (1, NULL, iochan, OZ_PROCMODE_KNL, NULL, NULL, NULL, NULL, NULL, NULL, OZ_IO_ETHER_CLOSE, 0, NULL);
  if (sts == OZ_STARTED) return (1);
  if (chnex -> rxreqt != NULL) oz_crash ("oz_dev_xenetwork deassign: channel still open");
  return (0);
}

/************************************************************************/
/*									*/
/*  Start performing an ethernet I/O function				*/
/*									*/
/************************************************************************/

static uLong xenetwork_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex, **lchnex;
  Devex *devex;
  Iopex *iopex, **liopex;
  int i;
  uLong dv, sts;
  Rxbuf *rxbuf;
  Txbuf *txbuf;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  iopex -> ioop     = ioop;
  iopex -> next     = NULL;
  iopex -> procmode = procmode;
  iopex -> devex    = devex;
  iopex -> chnex    = chnex;

  switch (funcode) {

    /* Open - associates a protocol number with a channel and starts reception */

    case OZ_IO_ETHER_OPEN: {
      OZ_IO_ether_open ether_open;

      movc4 (as, ap, sizeof ether_open, &ether_open);

      /* Make sure not already open */

      dv = oz_hw_smplock_wait (smplock);
      if (chnex -> rxreqt != NULL) {
        oz_hw_smplock_clr (smplock, dv);
        return (OZ_FILEALREADYOPEN);
      }

      /* Put channel on list of open channels - the interrupt routine will now see it */

      chnex -> proto  = N2HW (ether_open.proto);
      chnex -> promis = ether_open.promis;
      chnex -> rxreqt = &(chnex -> rxreqh);
      chnex -> next   = devex -> chnexs;
      devex -> chnexs = chnex;
      if (chnex -> promis) {
        if (devex -> promiscuous == 0) { /**?? put in promiscuous mode ??**/ }
        devex -> promiscuous ++;
      }
      if (chnex -> next == NULL) { /**?? enable device ??**/ }
      oz_hw_smplock_clr (smplock, dv);
      return (OZ_SUCCESS);
    }

    /* Disassociates a protocol with a channel and stops reception */

    case OZ_IO_ETHER_CLOSE: {
      return (close_channel (devex, chnex, iopex));
    }

    /* Receive a message - note that in this driver (and all our ethernet */
    /* drivers), if there is no receive queued, the message is lost       */

    case OZ_IO_ETHER_RECEIVE: {

      /* Get parameter block into iopex for future reference */

      movc4 (as, ap, sizeof iopex -> receive, &(iopex -> receive));

      /* If any of the rcv... parameters are filled in, it must be called from kernel mode */

      if ((iopex -> receive.rcvfre != NULL) || (iopex -> receive.rcvdrv_r != NULL) || (iopex -> receive.rcveth_r != NULL)) {
        if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      }

      /* Set up the request parameters and queue request so the interrupt routine can fill the buffer with an incoming message */

      iopex -> ioop     = ioop;
      iopex -> next     = NULL;
      iopex -> procmode = procmode;
      iopex -> devex    = devex;

      rxbuf = iopex -> receive.rcvfre;						/* maybe requestor has a buffer to free off */
      if (rxbuf != NULL) {
        if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) > 0) rxbuf = NULL;	/* maybe others are still using it, though */
        else {
          dv = oz_hw_smplock_wait (smplock);
          freercvresp (devex, rxbuf);
          oz_hw_smplock_clr (smplock, dv);
        }
      }
      if ((rxbuf == NULL) && (devex -> receive_requests_queued >= devex -> receive_buffers_queued)) { /* see if we have enough receive buffers to cover all receive requests */
        rxbuf = allocrcvbuf ();							/* if not, allocate a new one */
      }

      dv = oz_hw_smplock_wait (smplock);					/* lock database */
      liopex = chnex -> rxreqt;
      if (liopex == NULL) {							/* make sure channel is still open */
        oz_hw_smplock_clr (smplock, dv);
        if (rxbuf != NULL) freercvbuf (rxbuf);
        return (OZ_FILENOTOPEN);
      }
      *liopex = iopex;								/* put reqeuest on end of queue - interrupt routine can now see it */
      chnex -> rxreqt = &(iopex -> next);
      devex -> receive_requests_queued ++;					/* one more receive request queued to some channel of the device */
      if (rxbuf != NULL) queuercvbuf (devex, rxbuf);				/* maybe queue a new receive buffer */
      oz_hw_smplock_clr (smplock, dv);						/* unlock database */
      return (OZ_STARTED);
    }

    /* Free a receive buffer */

    case OZ_IO_ETHER_RECEIVEFREE: {
      OZ_IO_ether_receivefree ether_receivefree;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);			/* can only be called from kernel mode */
      movc4 (as, ap, sizeof ether_receivefree, &ether_receivefree);		/* get the parameters */
      rxbuf = ether_receivefree.rcvfre;						/* free off the given buffer */
      if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0) {		/* ... if no one else is using it */
        dv = oz_hw_smplock_wait (smplock);
        freercvresp (devex, rxbuf);
        freercvbuf (rxbuf);
        oz_hw_smplock_clr (smplock, dv);
      }
      return (OZ_SUCCESS);
    }

    /* Allocate a send buffer */

    case OZ_IO_ETHER_TRANSMITALLOC: {
      OZ_IO_ether_transmitalloc ether_transmitalloc;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);						/* can only be called from kernel mode */
      movc4 (as, ap, sizeof ether_transmitalloc, &ether_transmitalloc);					/* get the parameters */
      txbuf = allocxmtbuf ();										/* allocate a transmit buffer */
      if (ether_transmitalloc.xmtsiz_r != NULL) *(ether_transmitalloc.xmtsiz_r) = DATASIZE;		/* this is size of data it can handle */
      if (ether_transmitalloc.xmtdrv_r != NULL) *(ether_transmitalloc.xmtdrv_r) = txbuf;		/* this is the pointer we want returned in ether_transmit.xmtdrv */
      if (ether_transmitalloc.xmteth_r != NULL) *(ether_transmitalloc.xmteth_r) = (uByte *)&(txbuf -> buf); /* this is where they put the ethernet packet to be transmitted */
      return (OZ_SUCCESS);
    }

    /* Transmit a message */

    case OZ_IO_ETHER_TRANSMIT: {

      /* Get parameter block into iopex for future reference */

      movc4 (as, ap, sizeof iopex -> transmit, &(iopex -> transmit));

      /* Any of the xmt... params requires caller be in kernel mode */

      if ((iopex -> transmit.xmtdrv != NULL) || (iopex -> transmit.xmtsiz_r != NULL) || (iopex -> transmit.xmtdrv_r != NULL) || (iopex -> transmit.xmteth_r != NULL)) {
        if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
        if (iopex -> transmit.xmtsiz_r != NULL) *(iopex -> transmit.xmtsiz_r) = 0;
        if (iopex -> transmit.xmtdrv_r != NULL) *(iopex -> transmit.xmtdrv_r) = NULL;
        if (iopex -> transmit.xmteth_r != NULL) *(iopex -> transmit.xmteth_r) = NULL;
      }

      /* See if they gave us a system buffer */

      if (iopex -> transmit.xmtdrv != NULL) txbuf = iopex -> transmit.xmtdrv;

      /* If not, allocate one (but they have to give us a user buffer then so we can copy in data to be transmitted) */

      else if (iopex -> transmit.buff == NULL) return (OZ_MISSINGPARAM);
      else txbuf = allocxmtbuf ();

      /* No iopex associated with it yet */

      txbuf -> iopex = NULL;

      /* Anyway, copy in any data that they supplied */

      txbuf -> dlen = iopex -> transmit.dlen;
      if (iopex -> transmit.buff != NULL) {
        if (iopex -> transmit.size < txbuf -> buf.data - (uByte *)&(txbuf -> buf)) sts = OZ_BADBUFFERSIZE;						/* have to give at least the ethernet header stuff */
        else if (iopex -> transmit.size > sizeof txbuf -> buf) sts = OZ_BADBUFFERSIZE;									/* can't give us more than buffer can hold */
        else sts = oz_knl_section_uget (procmode, iopex -> transmit.size, iopex -> transmit.buff, &(txbuf -> buf));					/* copy it in */
        if ((sts == OZ_SUCCESS) && (iopex -> transmit.size < txbuf -> dlen + txbuf -> buf.data - (uByte *)&(txbuf -> buf))) sts = OZ_BADBUFFERSIZE;	/* must give enough to cover the dlen */
        if (sts != OZ_SUCCESS) {
          freexmtbuf (txbuf);
          return (sts);
        }
      }

      /* Make sure dlen (length of data not including header) not too int */

      if (txbuf -> dlen > DATASIZE) {			/* can't be longer than hardware will allow */
        freexmtbuf (txbuf);				/* free off internal buffer */
        return (OZ_BADBUFFERSIZE);			/* return error status */
      }

      /* Queue it for processing */

      txbuf -> iopex = iopex;				/* remember what IO to post when transmit completes */
      dv = oz_hw_smplock_wait (smplock);		/* lock out interrupts */
      queuexmtbuf (devex, txbuf);			/* queue to device */
      oz_hw_smplock_clr (smplock, dv);			/* unlock interrupts */
      return (OZ_STARTED);
    }

    /* Get info - part 1 */

    case OZ_IO_ETHER_GETINFO1: {
      OZ_IO_ether_getinfo1 ether_getinfo1;

      movc4 (as, ap, sizeof ether_getinfo1, &ether_getinfo1);
      if (ether_getinfo1.enaddrbuff != NULL) {
        if (ether_getinfo1.enaddrsize > ADDRSIZE) ether_getinfo1.enaddrsize = ADDRSIZE;
        sts = oz_knl_section_uput (procmode, ether_getinfo1.enaddrsize, devex -> enaddr, ether_getinfo1.enaddrbuff);
        if (sts != OZ_SUCCESS) return (sts);
      }
      ether_getinfo1.datasize   = DATASIZE;					// max length of data portion of message
      ether_getinfo1.buffsize   = ADDRSIZE * 2 + PROTOSIZE + DATASIZE + 4;	// max length of whole message (header, data, crc)
      ether_getinfo1.dstaddrof  = 0;						// offset of dest address in packet
      ether_getinfo1.srcaddrof  = 0 + ADDRSIZE;					// offset of source address in packet
      ether_getinfo1.protooffs  = 0 + 2 * ADDRSIZE;				// offset of protocol in packet
      ether_getinfo1.dataoffset = 0 + 2 * ADDRSIZE + PROTOSIZE;			// offset of data in packet
      ether_getinfo1.arphwtype  = ARPHWTYPE;					// ARP hardware type
      ether_getinfo1.addrsize   = ADDRSIZE;					// size of each address field
      ether_getinfo1.protosize  = PROTOSIZE;					// size of protocol field
      ether_getinfo1.rcvmissed  = chnex -> rcvmissed;				// get number of missed receives
      if (as > sizeof ether_getinfo1) as = sizeof ether_getinfo1;
      sts = oz_knl_section_uput (procmode, as, &ether_getinfo1, ap);
      return (sts);
    }
  }

  return (OZ_BADIOFUNC);
}

/************************************************************************/
/*									*/
/*  Close an open channel						*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnexx = channel to be closed					*/
/*	iopexx = OZ_IO_ETHER_CLOSE to be posted when close complete	*/
/*	smplock = softint						*/
/*									*/
/************************************************************************/

static uLong close_channel (Devex *devex, Chnex *chnexx, Iopex *iopexx)

{
  Chnex *chnex, **lchnex;
  Iopex *iopex;
  netop_t netop;
  Txbuf **ltxbuf, *txbuf;
  uLong dv;

  dv = oz_hw_smplock_wait (smplock);
  if (dv != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_dev_xenetwork close: called at smplevel %X", dv);

  if (chnex -> rxreqt == NULL) {
    oz_hw_smplock_clr (smplock, OZ_SMPLOCK_SOFTINT);
    return (OZ_FILENOTOPEN);
  }

  /* Unlink from list of open channels */

  for (lchnex = &(devex -> chnexs); (chnex = *lchnex) != chnexx; lchnex = &(chnex -> next)) { }
  *lchnex = chnex -> next;

  /* If it was promiscuous, disable it */

  if (chnex -> promis && (-- (devex -> promiscuous) == 0)) {
    /**?? disable promiscuous mode ??**/
  }

  /* Abort any transmits in the pending queue (there may be some in the ring buffer, though) */

  for (ltxbuf = &(devex -> xmtpendqh); (txbuf = *ltxbuf) != NULL;) {	// loop through queue of stuff not in ring yet
    iopex = txbuf -> iopex;						// get associated io request
    if ((iopex != NULL) && (iopex -> chnex == chnex)) {			// see if it's for the channel being closed
      *ltxbuf = txbuf -> next;						// ok, remove from queue
      iopex -> status = OZ_ABORTED;					// mark it with aborted status
      freexmtbuf (txbuf);						// post IO and free transmit buffer
    } else {
      ltxbuf = &(txbuf -> next);					// leave it alone
    }
  }
  devex -> xmtpendqt = ltxbuf;						// maybe there's a new queue tail

  /* Abort all pending receive requests and don't let any more queue */

  chnex -> rxreqt = NULL;				// mark it closed - abort any receive requests that try to queue
  while ((iopex = chnex -> rxreqh) != NULL) {		// abort any receive requests we may have
    chnex -> rxreqh = iopex -> next;
    devex -> receive_requests_queued --;
    oz_knl_iodonehi (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }

  /* If it's the last channel on device, tell hypervisor we don't want anything more done on it */
  /* NETOP_FLUSH_BUFFERS tells hypervisor to terminate all requests with RING_STATUS_DROPPED    */
  /* We may have to wait for them to actually post, and some transmits may already be started   */

  if (devex -> chnexs == NULL) {
    netop.cmd = NETOP_FLUSH_BUFFERS;
    netop.vif = devex -> vif;
    HYPERVISOR_net_io_op (&netop);
  }

  oz_hw_smplock_clr (smplock, OZ_SMPLOCK_SOFTINT);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Interrupt routine - called when there is a response in a ring	*/
/*									*/
/************************************************************************/

void oz_hwxen_event_net (OZ_Mchargs *mchargs)

{
  Devex *devex;
  uLong dv;

  dv = oz_hw_smplock_wait (smplock);				// lock other CPUs out
  for (devex = devexs; devex != NULL; devex = devex -> next) {	// loop through defined devices
    procrcvring (devex);					// check the receive ring
    procxmtring (devex);					// check the transmit ring
  }
  oz_hw_smplock_clr (smplock, dv);				// release other CPUs
}

/************************************************************************/
/*									*/
/*  Allocate a receive buffer						*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock <= np (ie, below device level so we can allocate pool)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	allocrcvbuf = points to an rxbuf				*/
/*									*/
/************************************************************************/

static Rxbuf *allocrcvbuf (void)

{
  Rxbuf *rxbuf;

  /* Maybe there is a free one we can take */

  do rxbuf = free_rxbufs;
  while ((rxbuf != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)&free_rxbufs, rxbuf -> next, rxbuf));

  /* If not, allocate a new buffer */

  if (rxbuf == NULL) {
    rxbuf = OZ_KNL_NPPMALLOC (sizeof *rxbuf);
    rxbuf -> ringindex = -1;
  }

  return (rxbuf);
}

/************************************************************************/
/*									*/
/*  This routine puts a buffer on the end of the busy chain for a 	*/
/*  device so it can receive a packet					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = ethernet device to queue it to				*/
/*	rxbuf = buffer to be queued					*/
/*									*/
/*	smplock = device level						*/
/*									*/
/*    Output:								*/
/*									*/
/*	buffer placed on end of device's receive queue			*/
/*									*/
/************************************************************************/

static void queuercvbuf (Devex *devex, Rxbuf *rxbuf)

{
  /* If we already have enough buffers queued, just free this one off */

  if (devex -> receive_buffers_queued >= devex -> receive_requests_queued) {
    freercvbuf (rxbuf);
    return;
  }

  /* Need more, queue it up on the pending list */

  *(devex -> rcvpendqt) = rxbuf;
  devex -> rcvpendqt = &(rxbuf -> next);
  rxbuf -> next = NULL;
  devex -> receive_buffers_queued ++;

  /* Maybe we can queue it in the ring */

  procrcvring (devex);
}

/************************************************************************/
/*									*/
/*  Process whatever we can in the receive ring				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pointer to device struct				*/
/*	smplevel = smplock locked					*/
/*									*/
/*    Output:								*/
/*									*/
/*	responses processed						*/
/*	new transmits queued						*/
/*									*/
/************************************************************************/

static void procrcvring (Devex *devex)

{
  int didsomething, diff, indx, rx_resp_prod, wackit;
  Iopex *iopex;
  net_ring_t *ring;
  netop_t netop;
  rx_req_entry_t *rxreqent;
  rx_resp_entry_t *rxrespent;
  Rxbuf *rxbuf;

  if (devex -> in_procrcvring) return;				// block recursive calls from queuercvbuf
  devex -> in_procrcvring = 1;

  wackit = 0;							// haven't put anything in receive ring yet
  ring = devex -> ring;
  do {
    do {
      didsomething = 0;

      /* Process responses if any - rcvprogqh points to entry we haven't seen a response for yet,  */
      /* rx_resp_prod points past last response from hypervisor.  We don't increment rx_resp_cons */
      /* until the receive has been posted and the data have been copied out of the ring.         */

      rxbuf = devex -> rcvprogqh;				// get top in-progress buffer
      if (rxbuf != NULL) {
        indx = rxbuf -> ringindex;				// see what it's ring index is
        rx_resp_prod = devex -> indx -> rx_resp_prod;		// see where producer is at
        if (indx != rx_resp_prod) {				// see if hypervisor has processed it
          OZ_HW_MB;						// ok, make sure response content is valid first
          rxrespent = &(ring -> rx_ring[indx].resp);		// point to response entry in receive ring
          if (rxrespent -> id != rxbuf -> id) oz_crash ("oz_dev_xenetwork procrcvring: id %u completed, but %u queued", rxrespent -> id, rxbuf -> id);
          if ((devex -> rcvprogqh = rxbuf -> next) == NULL) devex -> rcvprogqt = &(devex -> rcvprogqh); // remove from queue
          receive_done (devex, rxbuf);				// post IO, free receive buffer and increment rx_resp_cons
          didsomething = 1;
        }
      }

      /* Queue pending receives if there's room */

      rxbuf = devex -> rcvpendqh;
      if (rxbuf != NULL) {
        indx = devex -> indx -> rx_req_prod;			// get index for next receive packet
        diff = indx - devex -> rx_resp_cons;			// see how much is currently in use
        if (diff < 0) diff += RX_RING_SIZE;
        if (diff < NRCVPAGES) {					// stop if ring is filled
          rxbuf -> ringindex = indx;				// assign receive buffer a ring index
          rxbuf -> pteidx    = devex -> rcvpteidx;		// give xen a page for the data
          if (++ (devex -> rcvpteidx) == NRCVPAGES) devex -> rcvpteidx = 0;
          rxreqent = &(ring -> rx_ring[indx].req);		// point to receive ring request entry
          rxreqent -> id   = rxbuf -> id = ++ (devex -> lastid); // fill in request id number
          rxreqent -> addr = devex -> rcvptemas[rxbuf->pteidx];	// fill in pte's machine address
          OZ_HW_MB;						// make sure other CPUs see that before index incremented
          if (++ indx == RX_RING_SIZE) indx = 0;		// increment index to tell hypervisor to receive buffer
          devex -> indx -> rx_req_prod = indx;
          if ((devex -> rcvpendqh = rxbuf -> next) == NULL) devex -> rcvpendqt = &(devex -> rcvpendqh); // remove from pending queue
          *(devex -> rcvprogqt) = rxbuf;			// put on end of in-progress queue
          devex -> rcvprogqt = &(rxbuf -> next);
          rxbuf -> next = NULL;
          wackit = 1;						// we queued something
          didsomething = 1;
        }
      }
    } while (didsomething);

    /* Make sure 'event' is at 'resp_prod+1' so we get an interrupt when next buffer is received */

    if (devex -> rcvprogqh == NULL) break;			// don't bother if there are no pending receives
    indx = rx_resp_prod;					// ok, calculate incremented index, with wrap
    if (++ indx == RX_RING_SIZE) indx = 0;			// ... using same index sampled way above
    OZ_HW_MB;
    devex -> indx -> rx_event = indx;
    OZ_HW_MB;
  } while (devex -> indx -> rx_resp_prod != rx_resp_prod);	// repeat if receiver produced more in case we just missed it

  /* If we queued something, tell hypervisor about it */

  if (wackit) {
    netop.cmd = NETOP_PUSH_BUFFERS;
    netop.vif = devex -> vif;
    HYPERVISOR_net_io_op (&netop);
  }

  devex -> in_procrcvring = 0;
}

/************************************************************************/
/*									*/
/*  This routine is called at interrupt level when a receive response 	*/
/*  is seen in the ring							*/
/*									*/
/*    Input:								*/
/*									*/
/*	rxbuf -> ringindex = index in device's receive ring that just 	*/
/*	                     completed					*/
/*	smplevel = device						*/
/*									*/
/************************************************************************/

static int receive_done (Devex *devex, Rxbuf *rxbuf)

{
  Chnex *chnex;
  Ether_buf *ebuf;
  Iopex *iopex;
  rx_resp_entry_t *rxrespent;
  uWord proto;

  devex -> receive_buffers_queued --;						/* one less buffer queued to device */
  rxbuf -> refcount = 1;							/* initialize refcount */
										/* when it goes zero, free the response entry */
  rxrespent = &(devex -> ring -> rx_ring[rxbuf->ringindex].resp);		/* point to reponse entry that just completed */

  if (rxrespent -> status == RING_STATUS_DROPPED) goto tossit;			/* maybe channel is being closed */
  if (rxrespent -> status != RING_STATUS_OK) {					/* it doesn't like our pte */
    oz_crash ("oz_dev_xenetwork receive_done: status %u", rxrespent -> status);
  }
  if (rxrespent -> size < 14) goto tossit;					/* toss if no data */

  rxbuf -> dlen = rxrespent -> size - 14;					/* fill in data length (excludes dstaddr, srcaddr, proto, and crc) */
  ebuf = (void *)(devex -> rcvbufvas[rxbuf->pteidx] + rxrespent -> offset);	/* point to ethernet packet */

  proto = N2HW (ebuf -> proto);							/* get received message's protocol */
  for (chnex = devex -> chnexs; chnex != NULL; chnex = chnex -> next) {		/* find a suitable I/O channel */
    if ((chnex -> proto != 0) && (chnex -> proto != proto)) continue;		/* ... with matching protocol */
    if (!(chnex -> promis) && !CEQENADDR (ebuf -> dstaddr, devex -> enaddr) && !CEQENADDR (ebuf -> dstaddr, broadcast)) continue; /* matching enaddr */
    iopex = chnex -> rxreqh;							/* see if any receive I/O requests pending on it */
    if (iopex == NULL) {
      chnex -> rcvmissed ++;
      continue;
    }
    chnex -> rxreqh = iopex -> next;
    if (chnex -> rxreqh == NULL) chnex -> rxreqt = &(chnex -> rxreqh);
    OZ_HW_ATOMIC_INCBY1_LONG (rxbuf -> refcount);				/* increment received buffer's reference count */
    iopex -> rxbuf = rxbuf;							/* assign the received buffer to the request */
    oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, receive_iodone, iopex);	/* post it for completion */
  }

tossit:
  if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0) {			/* toss if no one wants it */
    freercvresp (devex, rxbuf);
    queuercvbuf (devex, rxbuf);
  }
}

/************************************************************************/
/*									*/
/*  Back in requestor's process - copy out data and/or pointers		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopexv = receive request's iopex				*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void receive_iodone (void *iopexv, uLong *status_r)

{
  Devex *devex;
  Iopex *iopex;
  uLong dv, size, sts;
  rx_resp_entry_t *rxrespent;
  Rxbuf *rxbuf;

  iopex = iopexv;
  devex = iopex -> devex;
  rxbuf = iopex -> rxbuf;
  rxrespent = &(devex -> ring -> rx_ring[rxbuf->ringindex].resp);		/* point to corresponding response entry */

  /* If requested, copy data to user's buffer */

  if (iopex -> receive.buff != NULL) {
    size = rxbuf -> dlen + 20;							/* size of everything we got, including length and crc */
    if (size > iopex -> receive.size) size = iopex -> receive.size;		/* chop to user's buffer size */
    sts = oz_knl_section_uput (iopex -> procmode, size, devex -> rcvbufvas[rxbuf->pteidx] + rxrespent -> offset, iopex -> receive.buff);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_xenetwork: copy to receive buf sts %u\n", sts);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
  }

  /* If requested, return length of data received */

  if (iopex -> receive.dlen != NULL) {
    sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> receive.dlen), &(rxbuf -> dlen), iopex -> receive.dlen);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_xenetwork: return rlen sts %u\n", sts);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
  }

  /* If requested, return pointer to system buffer                                       */
  /* Note that this can only be done from kernel mode so we don't bother validating args */

  if (iopex -> receive.rcvdrv_r != NULL) {
    *(iopex -> receive.rcvdrv_r) = rxbuf;
    *(iopex -> receive.rcveth_r) = devex -> rcvbufvas[rxbuf->pteidx] + rxrespent -> offset;
  }

  /* If we didn't return the pointer, free off receive buffer */

  else if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0) {
    dv = oz_hw_smplock_wait (smplock);
    freercvresp (devex, rxbuf);
    queuercvbuf (devex, rxbuf);
    oz_hw_smplock_clr (smplock, dv);
  }
}

/************************************************************************/
/*									*/
/*  Free receive ring response entry for a given buffer			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = device the buffer is for				*/
/*	rxbuf = receive buffer						*/
/*	smplevel = device						*/
/*									*/
/*    Output:								*/
/*									*/
/*	response entry freed						*/
/*	maybe more receives queued in its place				*/
/*									*/
/*    Note:								*/
/*									*/
/*	Call this routine whenever rxbuf->refcount is decremented to 0	*/
/*									*/
/************************************************************************/

static void freercvresp (Devex *devex, Rxbuf *rxbuf)

{
  int indx, wackit;
  net_ring_t *ring;

  ring = devex -> ring;
  indx = rxbuf -> ringindex;					// see what ring entry is owned
  if (indx < 0) oz_crash ("oz_dev_xenetwork freercvresp: rxbuf %p -> ringindex %d", rxbuf, rxbuf -> ringindex);
  ring -> rx_ring[indx].resp.size = 0xF0AD;			// mark this entry free
  rxbuf -> ringindex = -1;					// the rxbuf no longer owns a ring entry

  indx = devex -> rx_resp_cons;					// point to list of entries
  wackit = 0;							// we haven't freed any off yet
  while (ring -> rx_ring[indx].resp.size == 0xF0AD) {		// see if we can free earliest
    ring -> rx_ring[indx].resp.size = 0;			// if so, don't infinite loop
    if (++ indx == RX_RING_SIZE) indx = 0;			// and increment index
    wackit = 1;							// and we have freed something up
  }

  if (wackit) {
    devex -> rx_resp_cons = indx;				// ok, finialize index
    procrcvring (devex);					// maybe queue some more receives
  }
}

/************************************************************************/
/*									*/
/*  Free receive buffer							*/
/*									*/
/*    Input:								*/
/*									*/
/*	rxbuf = receive buffer to be freed				*/
/*									*/
/************************************************************************/

static void freercvbuf (Rxbuf *rxbuf)

{
  if (rxbuf -> ringindex >= 0) oz_crash ("oz_dev_xenetwork freercvbuf: rxbuf %p -> ringindex %d", rxbuf, rxbuf -> ringindex);

  do rxbuf -> next = free_rxbufs;
  while (!oz_hw_atomic_setif_ptr ((void *volatile *)&free_rxbufs, rxbuf, rxbuf -> next));
}

/************************************************************************/
/*									*/
/*  Allocate a transmit buffer						*/
/*									*/
/************************************************************************/

static Txbuf *allocxmtbuf (void)

{
  Txbuf *txbuf;

  /* Maybe there is a free one we can take */

  do txbuf = free_txbufs;
  while ((txbuf != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)&free_txbufs, txbuf -> next, txbuf));

  /* If not, allocate a physically contiguous buffer and get data's machine address */

  if (txbuf == NULL) {
    txbuf = OZ_KNL_PCMALLOC ((uByte *)&(txbuf -> buf) - (uByte *)txbuf + BUFFSIZE);
    txbuf -> dmad = oz_hwxen_vatoma ((OZ_Pointer)&(txbuf -> buf));
    if ((txbuf -> dmad + BUFFSIZE - 1) != oz_hwxen_vatoma (((OZ_Pointer)&(txbuf -> buf)) + BUFFSIZE - 1)) {
      oz_crash ("oz_dev_xenetwork: txbuf %p alloc non-contig", txbuf);
    }
  }

  /* It has no associated iopex yet */

  txbuf -> iopex = NULL;

  return (txbuf);
}

/************************************************************************/
/*									*/
/*  This routine queues a buffer to the transmit ring so it will be 	*/
/*  transmitted								*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = ethernet device to queue it to				*/
/*	txbuf = buffer to be queued					*/
/*	smplock = device level						*/
/*									*/
/*    Output:								*/
/*									*/
/*	buffer placed on end of device's transmit queue			*/
/*									*/
/************************************************************************/

static void queuexmtbuf (Devex *devex, Txbuf *txbuf)

{
  *(devex -> xmtpendqt) = txbuf;				// put on end of pending transmit queue
  devex -> xmtpendqt = &(txbuf -> next);
  txbuf -> next = NULL;
  txbuf -> started = oz_hw_tod_getnow ();
  procxmtring (devex);						// process the queue
}

/************************************************************************/
/*									*/
/*  Process whatever we can in the transmit ring			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pointer to device struct				*/
/*	smplevel = smplock locked					*/
/*									*/
/*    Output:								*/
/*									*/
/*	responses processed						*/
/*	new transmits queued						*/
/*									*/
/************************************************************************/

static void procxmtring (Devex *devex)

{
  int didsomething, diff, indx, tx_resp_prod, wackit;
  Iopex *iopex;
  net_ring_t *ring;
  netop_t netop;
  tx_req_entry_t *txreqent;
  tx_resp_entry_t *txrespent;
  Txbuf *txbuf;

  if (devex -> in_procxmtring) return;
  devex -> in_procxmtring = 1;

  wackit = 0;							// haven't put anything in transmit ring yet
  ring = devex -> ring;
  do {
    do {
      didsomething = 0;

      /* Process responses if any */

      indx = devex -> tx_resp_cons;				// see if there are any responses to process
      tx_resp_prod = devex -> indx -> tx_resp_prod;
      if (indx != tx_resp_prod) {
        OZ_HW_MB;						// ok, make sure response content is valid first
        txrespent = &(ring -> tx_ring[indx].resp);		// point to response entry in transmit ring
        txbuf = devex -> xmtprogqh;				// get top in-progress transmit buffer
        if (txrespent -> id != txbuf -> id) oz_crash ("oz_dev_xenetwork procxmtring: id %u completed, but %u queued", txrespent -> id, txbuf -> id);
        if ((devex -> xmtprogqh = txbuf -> next) == NULL) devex -> xmtprogqt = &(devex -> xmtprogqh); // remove from queue
        txbuf -> started = oz_hw_tod_getnow () - txbuf -> started;
        iopex = txbuf -> iopex;					// get corresponding IO request
        iopex -> status = OZ_SUCCESS;				// set up completion status
        if (txrespent -> status != RING_STATUS_OK) {
          iopex -> status = OZ_ABORTED;
          if (txrespent -> status != RING_STATUS_DROPPED) {
            oz_knl_printk ("oz_dev_xenetwork: %s: error %u transmitting\n", devex -> name, txrespent -> status);
            iopex -> status = OZ_IOFAILED;
          }
        }
        freexmtbuf (txbuf);					// post IO and free transmit buffer
        if (++ indx == TX_RING_SIZE) indx = 0;			// increment index of next response to process
        devex -> tx_resp_cons = indx;
        didsomething = 1;
      }

      /* Queue pending transmits if there's room */

      txbuf = devex -> xmtpendqh;
      if (txbuf != NULL) {
        indx = devex -> indx -> tx_req_prod;			// get index for next transmit packet
        diff = indx - devex -> tx_resp_cons;			// see how much is currently in use
        if (diff < 0) diff += TX_RING_SIZE;
        if (diff < TX_RING_SIZE - 1) {				// stop if ring is filled
          txreqent = &(ring -> tx_ring[indx].req);		// point to transmit ring request entry
          txreqent -> id   = txbuf -> id = ++ (devex -> lastid); // fill in request id number
          txreqent -> size = txbuf -> dlen + 14;		// fill in data length
          txreqent -> addr = txbuf -> dmad;			// fill in data machine address
          OZ_HW_MB;						// make sure other CPUs see that before index incremented
          if (++ indx == TX_RING_SIZE) indx = 0;		// increment index to tell hypervisor to transmit buffer
          devex -> indx -> tx_req_prod = indx;
          if ((devex -> xmtpendqh = txbuf -> next) == NULL) devex -> xmtpendqt = &(devex -> xmtpendqh); // remove from pending queue
          *(devex -> xmtprogqt) = txbuf;			// put on end of in-progress queue
          devex -> xmtprogqt = &(txbuf -> next);
          txbuf -> next = NULL;
          wackit = 1;						// we queued something
          didsomething = 1;
        }
      }
    } while (didsomething);

    /* Make sure 'tx_event' is ahead of 'tx_resp_prod' so we get an interrupt when progress is made */

    diff = devex -> indx -> tx_req_prod - tx_resp_prod;		// this is how many transmits we have in the ring
    if (diff < 0) diff += TX_RING_SIZE;
    diff = (diff + 3) / 4 + tx_resp_prod;			// get notified when a quarter of them have completed
    if (diff >= TX_RING_SIZE) diff -= TX_RING_SIZE;
    OZ_HW_MB;
    devex -> indx -> tx_event = diff;
    OZ_HW_MB;
  } while (devex -> indx -> tx_resp_prod != tx_resp_prod);	// repeat in case tx_resp_prod just incd to or beyond tx_event

  /* If we queued something, tell hypervisor about it */

  if (wackit) {
    netop.cmd = NETOP_PUSH_BUFFERS;
    netop.vif = devex -> vif;
    HYPERVISOR_net_io_op (&netop);
  }

  devex -> in_procxmtring = 0;
}

/************************************************************************/
/*									*/
/*  Post IO and free transmit buffer					*/
/*									*/
/*    Input:								*/
/*									*/
/*	txbuf = transmit buffer to be freed				*/
/*	txbuf -> iopex = associated i/o request with status filled in	*/
/*	smplock <= hi							*/
/*									*/
/************************************************************************/

static void freexmtbuf (Txbuf *txbuf)

{
  Iopex *iopex;

  iopex = txbuf -> iopex;

  /* If there is an I/O to post and it is successful and it is requesting a replacement */
  /* transmit buffer, just return the transmit buffer that is going to be freed off.    */

  if ((iopex != NULL) && (iopex -> status == OZ_SUCCESS) && (iopex -> transmit.xmtdrv_r != NULL)) {
    oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, transmit_iodone, txbuf);
  }

  /* Otherwise, free the buffer then post the request (if any) */

  else {
    do txbuf -> next = free_txbufs;
    while (!oz_hw_atomic_setif_ptr ((void *volatile *)&free_txbufs, txbuf, txbuf -> next));
    if (iopex != NULL) oz_knl_iodonehi (iopex -> ioop, iopex -> status, NULL, NULL, NULL);
  }
}

/* This routine is called in requestor's process space at softint level */

static void transmit_iodone (void *txbufv, uLong *status_r)

{
  Iopex *iopex;
  Txbuf *txbuf;

  txbuf = txbufv;
  iopex = txbuf -> iopex;

  /* Completing transmit successfully, return parameters for the buffer rather  */
  /* than freeing it. Since requestor was in kernel mode, don't bother probing. */

  if (iopex -> transmit.xmtsiz_r != NULL) *(iopex -> transmit.xmtsiz_r) = DATASIZE;
  if (iopex -> transmit.xmtdrv_r != NULL) *(iopex -> transmit.xmtdrv_r) = txbuf;
  if (iopex -> transmit.xmteth_r != NULL) *(iopex -> transmit.xmteth_r) = (uByte *)&(txbuf -> buf);
}
