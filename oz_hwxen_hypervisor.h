//+++2004-08-09
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
//---2004-08-09

/************************************************************************/
/*									*/
/*  XEN Hypervisor interface definitions				*/
/*									*/
/*  Since it uses the typical Unix hodgepodge naming conventions, 	*/
/*  ie none, it must be explicitly included in modules that need it	*/
/*									*/
/*  This is a consolidation of several .h files from the original Xen	*/
/*  block.h, network.h, hypervisor.h, hypervisor-if.h, dom0_ops.h	*/
/*  None of them had copyright notice on them.  The above copyright 	*/
/*  notice may not be valid.						*/
/*									*/
/************************************************************************/

#ifndef _OZ_HWXEN_HYPERVISOR_H
#define _OZ_HWXEN_HYPERVISOR_H

typedef uWord u16;
typedef uLong u32;
typedef uQuad u64;

typedef unsigned int domid_t;
typedef u32 pte_t;

#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

/******************************************************************************
 * block.h
 *
 * shared structures for block IO.
 *
 */

typedef unsigned long xen_sector_t;

/*
 *
 * These are the ring data structures for buffering messages between 
 * the hypervisor and guestos's.  
 *
 */

/* the first four definitions match fs.h */
#define XEN_BLOCK_READ         0
#define XEN_BLOCK_WRITE        1
#define XEN_BLOCK_READA        2
#define XEN_BLOCK_SPECIAL      4
#define XEN_BLOCK_DEBUG        5   /* debug */

/* NB. Ring size must be small enough for sizeof(blk_ring_t) <= PAGE_SIZE. */
#define BLK_RING_SIZE        64
#define BLK_RING_INC(_i)     (((_i)+1) & (BLK_RING_SIZE-1))

/*
 * Maximum scatter/gather segments per request.
 * This is carefully chosen so that sizeof(blk_ring_t) <= PAGE_SIZE.
 */
#define MAX_BLK_SEGS 12

typedef struct blk_ring_req_entry 
{
    unsigned long  id;                     /* private guest os value       */
    xen_sector_t   sector_number;          /* start sector idx on disk     */
    unsigned short device;                 /* XENDEV_??? + idx             */
    unsigned char  operation;              /* XEN_BLOCK_???                */
    unsigned char  nr_segments;            /* number of segments           */
    /* Least 9 bits is 'nr_sects'. High 23 bits are the address.           */
    unsigned long  buffer_and_sects[MAX_BLK_SEGS];
} blk_ring_req_entry_t;

typedef struct blk_ring_resp_entry
{
    unsigned long   id;                   /* copied from request          */
    unsigned short  operation;            /* copied from request          */
    unsigned long   status;               /* cuurently boolean good/bad   */
} blk_ring_resp_entry_t;

typedef struct blk_ring_st 
{
    unsigned int req_prod;  /* Request producer. Updated by guest OS. */
    unsigned int resp_prod; /* Response producer. Updated by Xen.     */
    union {
        blk_ring_req_entry_t  req;
        blk_ring_resp_entry_t resp;
    } ring[BLK_RING_SIZE];
} blk_ring_t;

/*
 * Information about the real and virtual disks we have; used during 
 * guest device probing. 
 */ 

/* XXX SMH: below types chosen to align with ide_xxx types in ide.h */
#define XD_TYPE_FLOPPY  0x00
#define XD_TYPE_TAPE    0x01
#define XD_TYPE_CDROM   0x05
#define XD_TYPE_OPTICAL 0x07
#define XD_TYPE_DISK    0x20 

#define XD_TYPE_MASK    0x3F
#define XD_TYPE(_x)     ((_x) & XD_TYPE_MASK) 

/* The top two bits of the type field encode various flags */
#define XD_FLAG_RO      0x40
#define XD_FLAG_VIRT    0x80
#define XD_READONLY(_x) ((_x) & XD_FLAG_RO)
#define XD_VIRTUAL(_x)  ((_x) & XD_FLAG_VIRT) 

typedef struct xen_disk
{
    unsigned short device;       /* device number (opaque 16 bit val)  */
    unsigned short info;         /* device type and flags              */
    xen_sector_t   capacity;     /* size in terms of #512 byte sectors */
    domid_t        domain;       /* if a VBD, domain this 'belongs to' */
} xen_disk_t;

typedef struct xen_disk_info
{
    /* IN variables  */
    int         max;             /* maximumum number of disks to report */
    xen_disk_t *disks;           /* pointer to array of disk info       */
    /* OUT variables */
    int         count;           /* how many disks we have info about   */
} xen_disk_info_t;

/* 
 * Block I/O trap operations and associated structures.
 */

#define BLOCK_IO_OP_SIGNAL       0    /* let xen know we have work to do     */
#define BLOCK_IO_OP_RESET        1    /* reset ring indexes on quiescent i/f */
#define BLOCK_IO_OP_RING_ADDRESS 2    /* returns machine address of I/O ring */
#define BLOCK_IO_OP_VBD_CREATE   3    /* create a new VBD for a given domain */
#define BLOCK_IO_OP_VBD_GROW     4    /* append an extent to a given VBD     */
#define BLOCK_IO_OP_VBD_SHRINK   5    /* remove last extent from a given VBD */
#define BLOCK_IO_OP_VBD_SET_EXTENTS 6 /* provide a fresh extent list for VBD */
#define BLOCK_IO_OP_VBD_DELETE   7    /* delete a VBD */
#define BLOCK_IO_OP_VBD_PROBE    8    /* query VBD information for a domain */
#define BLOCK_IO_OP_VBD_INFO     9    /* query info about a particular VBD */

typedef struct _xen_extent { 
    u16          device; 
    u16          unused;
    xen_sector_t start_sector; 
    xen_sector_t nr_sectors;
} xen_extent_t; 

#define VBD_MODE_R         0x1
#define VBD_MODE_W         0x2

#define VBD_CAN_READ(_v)  ((_v)->mode & VBD_MODE_R)
#define VBD_CAN_WRITE(_v) ((_v)->mode & VBD_MODE_W)

  
typedef struct _vbd_create { 
    domid_t      domain;              /* create VBD for this domain */
    u16          vdevice;             /* id by which dom will refer to VBD */ 
    u16          mode;                /* OR of { VBD_MODE_R , VBD_MODE_W } */
} vbd_create_t; 

typedef struct _vbd_grow { 
    domid_t      domain;              /* domain in question */
    u16          vdevice;             /* 16 bit id domain refers to VBD as */
    xen_extent_t extent;              /* the extent to add to this VBD */
} vbd_grow_t; 

typedef struct _vbd_shrink { 
    domid_t      domain;              /* domain in question */
    u16          vdevice;             /* 16 bit id domain refers to VBD as */
} vbd_shrink_t; 

typedef struct _vbd_setextents { 
    domid_t      domain;              /* domain in question */
    u16          vdevice;             /* 16 bit id domain refers to VBD as */
    u16          nr_extents;          /* number of extents in the list */
    xen_extent_t *extents;            /* the extents to add to this VBD */
} vbd_setextents_t; 

typedef struct _vbd_delete {          
    domid_t      domain;              /* domain in question */
    u16          vdevice;             /* 16 bit id domain refers to VBD as */
} vbd_delete_t; 

#define VBD_PROBE_ALL 0xFFFFFFFF
typedef struct _vbd_probe { 
    domid_t          domain;          /* domain in question or VBD_PROBE_ALL */
    xen_disk_info_t  xdi;             /* where's our space for VBD/disk info */
} vbd_probe_t; 

typedef struct _vbd_info { 
    /* IN variables  */
    domid_t       domain;             /* domain in question */
    u16           vdevice;            /* 16 bit id domain refers to VBD as */ 
    u16           maxextents;         /* max # of extents to return info for */
    xen_extent_t *extents;            /* pointer to space for extent list */
    /* OUT variables */
    u16           nextents;           /* # extents in the above list */
    u16           mode;               /* VBD_MODE_{READONLY,READWRITE} */
} vbd_info_t; 


typedef struct block_io_op_st
{
    unsigned long cmd;
    union
    {
        /* no entry for BLOCK_IO_OP_SIGNAL */
        /* no entry for BLOCK_IO_OP_RESET  */
	unsigned long    ring_mfn; 
	vbd_create_t     create_params; 
	vbd_grow_t       grow_params; 
	vbd_shrink_t     shrink_params; 
	vbd_setextents_t setextents_params; 
	vbd_delete_t     delete_params; 
	vbd_probe_t      probe_params; 
	vbd_info_t       info_params; 
    }
    u;
} block_io_op_t;

/******************************************************************************
 * network.h
 *
 * ring data structures for buffering messages between hypervisor and
 * guestos's.  As it stands this is only used for network buffer exchange.
 *
 * This file also contains structures and interfaces for the per-domain
 * routing/filtering tables in the hypervisor.
 *
 */

/*
 * Command values for block_io_op()
 */

#define NETOP_PUSH_BUFFERS    0  /* Notify Xen of new buffers on the rings. */
#define NETOP_FLUSH_BUFFERS   1  /* Flush all pending request buffers.      */
#define NETOP_RESET_RINGS     2  /* Reset ring indexes on a quiescent vif.  */
#define NETOP_GET_VIF_INFO    3  /* Query information for this vif.         */
typedef struct netop_st {
    unsigned int cmd; /* NETOP_xxx */
    unsigned int vif; /* VIF index */
    union {
        struct {
            unsigned long ring_mfn; /* Page frame containing net_ring_t. */
            unsigned char vmac[6];  /* Virtual Ethernet MAC address.     */
        } get_vif_info;
    } u;
} netop_t;


typedef struct tx_req_entry_st
{
    unsigned short id;
    unsigned short size;   /* packet size in bytes */
    unsigned long  addr;   /* machine address of packet */
} tx_req_entry_t;

typedef struct tx_resp_entry_st
{
    unsigned short id;
    unsigned char  status;
} tx_resp_entry_t;

typedef union tx_entry_st
{
    tx_req_entry_t  req;
    tx_resp_entry_t resp;
} tx_entry_t;


typedef struct rx_req_entry_st
{
    unsigned short id;
    unsigned long  addr;   /* machine address of PTE to swizzle */
} rx_req_entry_t;

typedef struct rx_resp_entry_st
{
    unsigned short id;
    unsigned short size;   /* received packet size in bytes */
    unsigned char  status; /* per descriptor status */
    unsigned char  offset; /* offset in page of received pkt */
} rx_resp_entry_t;

typedef union rx_entry_st
{
    rx_req_entry_t  req;
    rx_resp_entry_t resp;
} rx_entry_t;


#define TX_RING_SIZE 256
#define RX_RING_SIZE 256

#define MAX_DOMAIN_VIFS 8

/* This structure must fit in a memory page. */
typedef struct net_ring_st
{
    tx_entry_t tx_ring[TX_RING_SIZE];
    rx_entry_t rx_ring[RX_RING_SIZE];
} net_ring_t;

typedef struct net_idx_st
{
    /*
     * Guest OS places packets into ring at tx_req_prod.
     * Guest OS receives EVENT_NET when tx_resp_prod passes tx_event.
     * Guest OS places empty buffers into ring at rx_req_prod.
     * Guest OS receives EVENT_NET when rx_rssp_prod passes rx_event.
     */
    unsigned int tx_req_prod, tx_resp_prod, tx_event;
    unsigned int rx_req_prod, rx_resp_prod, rx_event;
} net_idx_t;

/*
 * Packet routing/filtering code follows:
 */

#define NETWORK_ACTION_ACCEPT   0
#define NETWORK_ACTION_COUNT    1

#define NETWORK_PROTO_ANY       0
#define NETWORK_PROTO_IP        1
#define NETWORK_PROTO_TCP       2
#define NETWORK_PROTO_UDP       3
#define NETWORK_PROTO_ARP       4

typedef struct net_rule_st 
{
    u32  src_addr;
    u32  dst_addr;
    u16  src_port;
    u16  dst_port;
    u32  src_addr_mask;
    u32  dst_addr_mask;
    u16  src_port_mask;
    u16  dst_port_mask;
    u16  proto;
    unsigned long src_vif;
    unsigned long dst_vif;
    u16  action;
} net_rule_t;

#define VIF_DOMAIN_MASK  0xfffff000UL
#define VIF_DOMAIN_SHIFT 12
#define VIF_INDEX_MASK   0x00000fffUL
#define VIF_INDEX_SHIFT  0

/* These are specified in the index if the dom is SPECIAL. */
#define VIF_SPECIAL      0xfffff000UL
#define VIF_UNKNOWN_INTERFACE   (VIF_SPECIAL | 0)
#define VIF_PHYSICAL_INTERFACE  (VIF_SPECIAL | 1)
#define VIF_ANY_INTERFACE       (VIF_SPECIAL | 2)

typedef struct vif_query_st
{
    domid_t  domain;
    int     *buf;   /* reply buffer -- guest virtual address */
} vif_query_t;

typedef struct vif_getinfo_st
{
    domid_t             domain;
    unsigned int        vif;

    /* domain & vif are supplied by dom0, the rest are response fields */
    long long           total_bytes_sent;
    long long           total_bytes_received;
    long long           total_packets_sent;
    long long           total_packets_received;

    /* Current scheduling parameters */
    unsigned long credit_bytes;
    unsigned long credit_usec;
} vif_getinfo_t;

/*
 * Set parameters associated with a VIF. Currently this is only scheduling
 * parameters --- permit 'credit_bytes' to be transmitted every 'credit_usec'.
 */
typedef struct vif_setparams_st
{
    domid_t             domain;
    unsigned int        vif;
    unsigned long       credit_bytes;
    unsigned long       credit_usec;
} vif_setparams_t;

/* Network trap operations and associated structure. 
 * This presently just handles rule insertion and deletion, but will
 * evenually have code to add and remove interfaces.
 */

#define NETWORK_OP_ADDRULE      0
#define NETWORK_OP_DELETERULE   1
#define NETWORK_OP_GETRULELIST  2
#define NETWORK_OP_VIFQUERY     3
#define NETWORK_OP_VIFGETINFO   4
#define NETWORK_OP_VIFSETPARAMS 5

typedef struct network_op_st 
{
    unsigned long cmd;
    union
    {
        net_rule_t net_rule;
        vif_query_t vif_query;
        vif_getinfo_t vif_getinfo;
        vif_setparams_t vif_setparams;
    }
    u;
} network_op_t;

typedef struct net_rule_ent_st
{
    net_rule_t r;
    struct net_rule_ent_st *next;
} net_rule_ent_t;

/* Drop a new rule down to the network tables. */
int add_net_rule(net_rule_t *rule);

/* Descriptor status values */
#define RING_STATUS_OK               0  /* Everything is gravy. */
#define RING_STATUS_BAD_PAGE         1  /* What they gave us was pure evil */
#define RING_STATUS_DROPPED          2  /* Unrouteable packet */

/******************************************************************************
 * hypervisor-if.h
 * 
 * Interface to Xeno hypervisor.
 */

/*
 * SEGMENT DESCRIPTOR TABLES
 */
/*
 * A number of GDT entries are reserved by Xen. These are not situated at the
 * start of the GDT because some stupid OSes export hard-coded selector values
 * in their ABI. These hard-coded values are always near the start of the GDT,
 * so Xen places itself out of the way.
 * 
 * NB. The reserved range is inclusive (that is, both FIRST_RESERVED_GDT_ENTRY
 * and LAST_RESERVED_GDT_ENTRY are reserved).
 */
#define NR_RESERVED_GDT_ENTRIES    40
#define FIRST_RESERVED_GDT_ENTRY   256
#define LAST_RESERVED_GDT_ENTRY    \
  (FIRST_RESERVED_GDT_ENTRY + NR_RESERVED_GDT_ENTRIES - 1)

/*
 * These flat segments are in the Xen-private section of every GDT. Since these
 * are also present in the initial GDT, many OSes will be able to avoid
 * installing their own GDT.
 */
#define FLAT_RING1_CS 0x0819
#define FLAT_RING1_DS 0x0821
#define FLAT_RING3_CS 0x082b
#define FLAT_RING3_DS 0x0833


/*
 * HYPERVISOR "SYSTEM CALLS"
 */

/* EAX = vector; EBX, ECX, EDX, ESI, EDI = args 1, 2, 3, 4, 5. */
#define __HYPERVISOR_set_trap_table        0
#define __HYPERVISOR_mmu_update            1
#define __HYPERVISOR_console_write         2
#define __HYPERVISOR_set_gdt               3
#define __HYPERVISOR_stack_switch          4
#define __HYPERVISOR_set_callbacks         5
#define __HYPERVISOR_net_io_op             6
#define __HYPERVISOR_fpu_taskswitch        7
#define __HYPERVISOR_sched_op              8
#define __HYPERVISOR_dom0_op               9
#define __HYPERVISOR_network_op           10
#define __HYPERVISOR_block_io_op          11
#define __HYPERVISOR_set_debugreg         12
#define __HYPERVISOR_get_debugreg         13
#define __HYPERVISOR_update_descriptor    14
#define __HYPERVISOR_set_fast_trap        15
#define __HYPERVISOR_dom_mem_op           16
#define __HYPERVISOR_multicall            17
#define __HYPERVISOR_kbd_op               18
#define __HYPERVISOR_update_va_mapping    19
#define __HYPERVISOR_event_channel_op     20

/* And the trap vector is... */
#define TRAP_INSTR "int $0x82"


/*
 * MULTICALLS
 * 
 * Multicalls are listed in an array, with each element being a fixed size 
 * (BYTES_PER_MULTICALL_ENTRY). Each is of the form (op, arg1, ..., argN)
 * where each element of the tuple is a machine word. 
 */
#define BYTES_PER_MULTICALL_ENTRY 32


/* EVENT MESSAGES
 *
 * Here, as in the interrupts to the guestos, additional network interfaces
 * are defined.	 These definitions server as placeholders for the event bits,
 * however, in the code these events will allways be referred to as shifted
 * offsets from the base NET events.
 */

/* Events that a guest OS may receive from the hypervisor. */
#define EVENT_BLKDEV   0x01 /* A block device response has been queued. */
#define EVENT_TIMER    0x02 /* A timeout has been updated. */
#define EVENT_DIE      0x04 /* OS is about to be killed. Clean up please! */
#define EVENT_DEBUG    0x08 /* Request guest to dump debug info (gross!) */
#define EVENT_NET      0x10 /* There are packets for transmission. */
#define EVENT_PS2      0x20 /* PS/2 keyboard or mouse event(s) */
#define EVENT_STOP     0x40 /* Prepare for stopping and possible pickling */
#define EVENT_EVTCHN   0x80 /* Event pending on an event channel */
#define EVENT_VBD_UPD  0x100 /* Event to signal VBDs should be reprobed */

/* Bit offsets, as opposed to the above masks. */
#define _EVENT_BLKDEV   0
#define _EVENT_TIMER    1
#define _EVENT_DIE      2
#define _EVENT_DEBUG    3
#define _EVENT_NET      4
#define _EVENT_PS2      5
#define _EVENT_STOP     6
#define _EVENT_EVTCHN   7
#define _EVENT_VBD_UPD  8

/*
 * Virtual addresses beyond this are not modifiable by guest OSes. The 
 * machine->physical mapping table starts at this address, read-only.
 */
#define HYPERVISOR_VIRT_START (0xFC000000UL)
#ifndef machine_to_phys_mapping
#define machine_to_phys_mapping ((unsigned long *)HYPERVISOR_VIRT_START)
#endif


/*
 * MMU_XXX: specified in least 2 bits of 'ptr' field. These bits are masked
 *  off to get the real 'ptr' value.
 * All requests specify relevent address in 'ptr'. This is either a
 * machine/physical address (MA), or linear/virtual address (VA).
 * Normal requests specify update value in 'value'.
 * Extended requests specify command in least 8 bits of 'value'. These bits
 *  are masked off to get the real 'val' value. Except for MMUEXT_SET_LDT 
 *  which shifts the least bits out.
 */
/* A normal page-table update request. */
#define MMU_NORMAL_PT_UPDATE     0 /* checked '*ptr = val'. ptr is VA.      */
/* DOM0 can make entirely unchecked updates which do not affect refcnts. */
#define MMU_UNCHECKED_PT_UPDATE  1 /* unchecked '*ptr = val'. ptr is VA.    */
/* Update an entry in the machine->physical mapping table. */
#define MMU_MACHPHYS_UPDATE      2 /* ptr = MA of frame to modify entry for */
/* An extended command. */
#define MMU_EXTENDED_COMMAND     3 /* least 8 bits of val demux further     */
/* Extended commands: */
#define MMUEXT_PIN_L1_TABLE      0 /* ptr = MA of frame to pin              */
#define MMUEXT_PIN_L2_TABLE      1 /* ptr = MA of frame to pin              */
#define MMUEXT_PIN_L3_TABLE      2 /* ptr = MA of frame to pin              */
#define MMUEXT_PIN_L4_TABLE      3 /* ptr = MA of frame to pin              */
#define MMUEXT_UNPIN_TABLE       4 /* ptr = MA of frame to unpin            */
#define MMUEXT_NEW_BASEPTR       5 /* ptr = MA of new pagetable base        */
#define MMUEXT_TLB_FLUSH         6 /* ptr = NULL                            */
#define MMUEXT_INVLPG            7 /* ptr = NULL ; val = VA to invalidate   */
#define MMUEXT_SET_LDT           8 /* ptr = VA of table; val = # entries    */
#define MMUEXT_CMD_MASK        255
#define MMUEXT_CMD_SHIFT         8

/* These are passed as 'flags' to update_va_mapping. They can be ORed. */
#define UVMF_FLUSH_TLB          1 /* Flush entire TLB. */
#define UVMF_INVLPG             2 /* Flush the VA mapping being updated. */

/*
 * Master "switch" for enabling/disabling event delivery.
 */
#define EVENTS_MASTER_ENABLE_MASK 0x80000000UL
#define EVENTS_MASTER_ENABLE_BIT  31


/*
 * SCHEDOP_* - Scheduler hypercall operations.
 */
#define SCHEDOP_yield           0
#define SCHEDOP_exit            1
#define SCHEDOP_stop            2

/*
 * EVTCHNOP_* - Event channel operations.
 */
#define EVTCHNOP_open           0  /* Open channel to <target domain>.    */
#define EVTCHNOP_close          1  /* Close <channel id>.                 */
#define EVTCHNOP_send           2  /* Send event on <channel id>.         */
#define EVTCHNOP_status         3  /* Get status of <channel id>.         */

/*
 * EVTCHNSTAT_* - Non-error return values from EVTCHNOP_status.
 */
#define EVTCHNSTAT_closed       0  /* Chennel is not in use.              */
#define EVTCHNSTAT_disconnected 1  /* Channel is not connected to remote. */
#define EVTCHNSTAT_connected    2  /* Channel is connected to remote.     */


#ifndef __ASSEMBLY__

//#include "network.h"
//#include "block.h"

/*
 * Send an array of these to HYPERVISOR_set_trap_table()
 */
#define TI_GET_DPL(_ti)      ((_ti)->flags & 3)
#define TI_GET_IF(_ti)       ((_ti)->flags & 4)
#define TI_SET_DPL(_ti,_dpl) ((_ti)->flags |= (_dpl))
#define TI_SET_IF(_ti,_if)   ((_ti)->flags |= ((!!(_if))<<2))
typedef struct trap_info_st
{
    unsigned char  vector;  /* exception vector                              */
    unsigned char  flags;   /* 0-3: privilege level; 4: clear event enable?  */
    unsigned short cs;	    /* code selector                                 */
    unsigned long  address; /* code address                                  */
} trap_info_t;

/*
 * Send an array of these to HYPERVISOR_mmu_update()
 */
typedef struct
{
    unsigned long ptr, val; /* *ptr = val */
} mmu_update_t;

/*
 * Send an array of these to HYPERVISOR_multicall()
 */
typedef struct
{
    unsigned long op;
    unsigned long args[7];
} multicall_entry_t;

typedef struct
{
    unsigned long ebx;
    unsigned long ecx;
    unsigned long edx;
    unsigned long esi;
    unsigned long edi;
    unsigned long ebp;
    unsigned long eax;
    unsigned long ds;
    unsigned long es;
    unsigned long fs;
    unsigned long gs;
    unsigned long _unused;
    unsigned long eip;
    unsigned long cs;
    unsigned long eflags;
    unsigned long esp;
    unsigned long ss;
} execution_context_t;

/*
 * Xen/guestos shared data -- pointer provided in start_info.
 * NB. We expect that this struct is smaller than a page.
 */
typedef struct shared_info_st {

    /* Bitmask of outstanding event notifications hypervisor -> guest OS. */
    unsigned long events;
    /*
     * Hypervisor will only signal event delivery via the "callback exception"
     * when a pending event is not masked. The mask also contains a "master
     * enable" which prevents any event delivery. This mask can be used to
     * prevent unbounded reentrancy and stack overflow (in this way, acts as a
     * kind of interrupt-enable flag).
     */
    unsigned long events_mask;

    /*
     * A domain can have up to 1024 bidirectional event channels to/from other
     * domains. Domains must agree out-of-band to set up a connection, and then
     * each must explicitly request a connection to the other. When both have
     * made the request the channel is fully allocated and set up.
     * 
     * An event channel is a single sticky 'bit' of information. Setting the
     * sticky bit also causes an upcall into the target domain. In this way
     * events can be seen as an IPI [Inter-Process(or) Interrupt].
     * 
     * A guest can see which of its event channels are pending by reading the
     * 'event_channel_pend' bitfield. To avoid a linear scan of the entire
     * bitfield there is a 'selector' which indicates which words in the
     * bitfield contain at least one set bit.
     * 
     * There is a similar bitfield to indicate which event channels have been
     * disconnected by the remote end. There is also a 'selector' for this
     * field.
     */
    u32 event_channel_pend[32];
    u32 event_channel_pend_sel;
    u32 event_channel_disc[32];
    u32 event_channel_disc_sel;

    /*
     * Time: The following abstractions are exposed: System Time, Clock Time,
     * Domain Virtual Time. Domains can access Cycle counter time directly.
     */

    unsigned int       rdtsc_bitshift;  /* tsc_timestamp uses N:N+31 of TSC. */
    u64                cpu_freq;        /* CPU frequency (Hz).               */

    /*
     * The following values are updated periodically (and not necessarily
     * atomically!). The guest OS detects this because 'time_version1' is
     * incremented just before updating these values, and 'time_version2' is
     * incremented immediately after. See Xenolinux code for an example of how 
     * to read these values safely (arch/xeno/kernel/time.c).
     */
    unsigned long      time_version1;   /* A version number for info below.  */
    unsigned long      time_version2;   /* A version number for info below.  */
    unsigned long      tsc_timestamp;   /* TSC at last update of time vals.  */
    u64                system_time;     /* Time, in nanosecs, since boot.    */
    unsigned long      wc_sec;          /* Secs  00:00:00 UTC, Jan 1, 1970.  */
    unsigned long      wc_usec;         /* Usecs 00:00:00 UTC, Jan 1, 1970.  */
    
    /* Domain Virtual Time */
    u64                domain_time;
	
    /*
     * Timeout values:
     * Allow a domain to specify a timeout value in system time and 
     * domain virtual time.
     */
    u64                wall_timeout;
    u64                domain_timeout;

    /*
     * The index structures are all stored here for convenience. The rings 
     * themselves are allocated by Xen but the guestos must create its own 
     * mapping -- the machine address is given in the startinfo structure to 
     * allow this to happen.
     */
    net_idx_t net_idx[MAX_DOMAIN_VIFS];

    execution_context_t execution_context;

} shared_info_t;

/*
 * NB. We expect that this struct is smaller than a page.
 */
typedef struct start_info_st {
    /* THE FOLLOWING ARE FILLED IN BOTH ON INITIAL BOOT AND ON RESUME.     */
    unsigned long nr_pages;	  /* total pages allocated to this domain. */
    unsigned long shared_info;	  /* MACHINE address of shared info struct.*/
    domid_t       dom_id;         /* Domain identifier.                    */
    unsigned long flags;          /* SIF_xxx flags.                        */
    /* THE FOLLOWING ARE ONLY FILLED IN ON INITIAL BOOT (NOT RESUME).      */
    unsigned long pt_base;	  /* VIRTUAL address of page directory.    */
    unsigned long mod_start;	  /* VIRTUAL address of pre-loaded module. */
    unsigned long mod_len;	  /* Size (bytes) of pre-loaded module.    */
    unsigned char cmd_line[1];	  /* Variable-length options.              */
} start_info_t;

/* These flags are passed in the 'flags' field of start_info_t. */
#define SIF_PRIVILEGED 1          /* Is the domain privileged? */
#define SIF_CONSOLE    2          /* Does the domain own the physical console? */

#endif /* !__ASSEMBLY__ */

/******************************************************************************
 * dom0_ops.h
 * 
 * Process command requests from domain-0 guest OS.
 * 
 * Copyright (c) 2002-2003, K A Fraser, B Dragovic
 */


/*
 * Make sure you increment the interface version whenever you modify this file!
 * This makes sure that old versions of dom0 tools will stop working in a
 * well-defined way (rather than crashing the machine, for instance).
 */
#define DOM0_INTERFACE_VERSION   0xAAAA0003


/*
 * The following is all CPU context. Note that the i387_ctxt block is filled 
 * in by FXSAVE if the CPU has feature FXSR; otherwise FSAVE is used.
 */
typedef struct full_execution_context_st
{
#define ECF_I387_VALID (1<<0)
    unsigned long flags;
    execution_context_t i386_ctxt;          /* User-level CPU registers     */
    char          i387_ctxt[256];           /* User-level FPU registers     */
    trap_info_t   trap_ctxt[256];           /* Virtual IDT                  */
    unsigned int  fast_trap_idx;            /* "Fast trap" vector offset    */
    unsigned long ldt_base, ldt_ents;       /* LDT (linear address, # ents) */
    unsigned long gdt_frames[16], gdt_ents; /* GDT (machine frames, # ents) */
    unsigned long ring1_ss, ring1_esp;      /* Virtual TSS (only SS1/ESP1)  */
    unsigned long pt_base;                  /* CR3 (pagetable base)         */
    unsigned long debugreg[8];              /* DB0-DB7 (debug registers)    */
    unsigned long event_callback_cs;        /* CS:EIP of event callback     */
    unsigned long event_callback_eip;
    unsigned long failsafe_callback_cs;     /* CS:EIP of failsafe callback  */
    unsigned long failsafe_callback_eip;
} full_execution_context_t;

#define MAX_CMD_LEN       256
#define MAX_DOMAIN_NAME    16

#define DOM0_CREATEDOMAIN      8
typedef struct dom0_createdomain_st 
{
    /* IN parameters. */
    unsigned int memory_kb; 
    char name[MAX_DOMAIN_NAME];
    /* OUT parameters. */
    domid_t domain; 
} dom0_createdomain_t;

#define DOM0_STARTDOMAIN      10
typedef struct dom0_startdomain_st
{
    /* IN parameters. */
    domid_t domain;
} dom0_startdomain_t;

#define DOM0_STOPDOMAIN       11
typedef struct dom0_stopdomain_st
{
    /* IN parameters. */
    domid_t domain;
} dom0_stopdomain_t;

#define DOM0_DESTROYDOMAIN     9
typedef struct dom0_destroydomain_st
{
    /* IN variables. */
    domid_t domain;
    int          force;
} dom0_destroydomain_t;

#define DOM0_GETMEMLIST        2
typedef struct dom0_getmemlist_st
{
    /* IN variables. */
    domid_t  domain;
    unsigned long max_pfns;
    void         *buffer;
    /* OUT variables. */
    unsigned long num_pfns;
} dom0_getmemlist_t;

#define DOM0_BUILDDOMAIN      13
typedef struct dom0_builddomain_st
{
    /* IN variables. */
    domid_t  domain;
    unsigned int  num_vifs;
    full_execution_context_t ctxt;
} dom0_builddomain_t;

#define DOM0_BVTCTL            6
typedef struct dom0_bvtctl_st
{
    /* IN variables. */
    unsigned long ctx_allow;  /* context switch allowance */
} dom0_bvtctl_t;

#define DOM0_ADJUSTDOM         7
typedef struct dom0_adjustdom_st
{
    /* IN variables. */
    domid_t  domain;     /* domain id */
    unsigned long mcu_adv;    /* mcu advance: inverse of weight */
    unsigned long warp;       /* time warp */
    unsigned long warpl;      /* warp limit */
    unsigned long warpu;      /* unwarp time requirement */
} dom0_adjustdom_t;

#define DOM0_GETDOMAININFO    12
typedef struct dom0_getdomaininfo_st
{
    /* IN variables. */
    domid_t domain;
    /* OUT variables. */
    char name[MAX_DOMAIN_NAME];
    int processor;
    int has_cpu;
#define DOMSTATE_ACTIVE              0
#define DOMSTATE_STOPPED             1
    int state;
    int hyp_events;
    unsigned long mcu_advance;
    unsigned int tot_pages;
    long long cpu_time;
    unsigned long shared_info_frame;  /* MFN of shared_info struct */
    full_execution_context_t ctxt;
} dom0_getdomaininfo_t;

#define DOM0_GETPAGEFRAMEINFO 18
typedef struct dom0_getpageframeinfo_st
{
    /* IN variables. */
    unsigned long pfn;          /* Machine page frame number to query.       */
    /* OUT variables. */
    domid_t domain;        /* To which domain does the frame belong?    */
    enum { NONE, L1TAB, L2TAB } type; /* Is the page PINNED to a type?       */
} dom0_getpageframeinfo_t;

#define DOM0_IOPL             14
typedef struct dom0_iopl_st
{
    domid_t domain;
    unsigned int iopl;
} dom0_iopl_t;

#define DOM0_MSR              15
typedef struct dom0_msr_st
{
    /* IN variables. */
    int write, cpu_mask, msr;
    unsigned int in1, in2;
    /* OUT variables. */
    unsigned int out1, out2;
} dom0_msr_t;

#define DOM0_DEBUG            16
typedef struct dom0_debug_st
{
    /* IN variables. */
    char opcode;
    int domain, in1, in2;
    /* OUT variables. */
    int status, out1, out2;
} dom0_debug_t;

/*
 * Set clock such that it would read <secs,usecs> after 00:00:00 UTC,
 * 1 January, 1970 if the current system time was <system_time>.
 */
#define DOM0_SETTIME          17
typedef struct dom0_settime_st
{
    /* IN variables. */
    unsigned long secs, usecs;
    u64 system_time;
} dom0_settime_t;

/*
 * Read console content from Xen buffer ring.
 */

#define DOM0_READCONSOLE      19
typedef struct dom0_readconsole_st
{
    unsigned long str;
    unsigned int count;
    unsigned int cmd;
} dom0_readconsole_t;

/* 
 * Pin Domain to a particular CPU  (use -1 to unpin)
 */
#define DOM0_PINCPUDOMAIN     20
typedef struct dom0_pincpudomain_st
{
    /* IN variables. */
    domid_t domain;
    int          cpu;  /* -1 implies unpin */
} dom0_pincpudomain_t;

typedef struct dom0_op_st
{
    unsigned long cmd;
    unsigned long interface_version; /* DOM0_INTERFACE_VERSION */
    union
    {
        dom0_createdomain_t     createdomain;
        dom0_startdomain_t      startdomain;
        dom0_stopdomain_t       stopdomain;
        dom0_destroydomain_t    destroydomain;
        dom0_getmemlist_t       getmemlist;
        dom0_bvtctl_t           bvtctl;
        dom0_adjustdom_t        adjustdom;
        dom0_builddomain_t      builddomain;
        dom0_getdomaininfo_t    getdomaininfo;
        dom0_getpageframeinfo_t getpageframeinfo;
        dom0_iopl_t             iopl;
	dom0_msr_t              msr;
	dom0_debug_t            debug;
	dom0_settime_t          settime;
	dom0_readconsole_t	readconsole;
	dom0_pincpudomain_t     pincpudomain;
    } u;
} dom0_op_t;

/******************************************************************************
 * hypervisor.h
 * 
 * Linux-specific hypervisor handling.
 * 
 * Copyright (c) 2002, K A Fraser
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/ptrace.h>
#include <asm/page.h>

/*
 * Assembler stubs for hyper-calls.
 */

extern inline int HYPERVISOR_set_trap_table(trap_info_t *table)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_set_trap_table),
        "b" (table) : "memory" );

    return ret;
}



extern inline int HYPERVISOR_set_gdt(unsigned long *frame_list, int entries)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_set_gdt), 
        "b" (frame_list), "c" (entries) : "memory" );


    return ret;
}

extern inline int HYPERVISOR_stack_switch(unsigned long ss, unsigned long esp)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_stack_switch),
        "b" (ss), "c" (esp) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_set_callbacks(
    unsigned long event_selector, unsigned long event_address,
    unsigned long failsafe_selector, unsigned long failsafe_address)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_set_callbacks),
        "b" (event_selector), "c" (event_address), 
        "d" (failsafe_selector), "S" (failsafe_address) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_net_io_op(netop_t *op)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_net_io_op),
        "b" (op) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_fpu_taskswitch(void)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_fpu_taskswitch) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_yield(void)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_sched_op),
        "b" (SCHEDOP_yield) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_stop(unsigned long srec)
{
    int ret;
    /* NB. On suspend, control software expects a suspend record in %esi. */
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_sched_op),
        "b" (SCHEDOP_stop), "S" (srec) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_dom0_op(dom0_op_t *dom0_op)
{
    int ret;
    dom0_op->interface_version = DOM0_INTERFACE_VERSION;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_dom0_op),
        "b" (dom0_op) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_network_op(void *network_op)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_network_op),
        "b" (network_op) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_set_debugreg(int reg, unsigned long value)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_set_debugreg),
        "b" (reg), "c" (value) : "memory" );

    return ret;
}

extern inline unsigned long HYPERVISOR_get_debugreg(int reg)
{
    unsigned long ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_get_debugreg),
        "b" (reg) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_update_descriptor(
    unsigned long pa, unsigned long word1, unsigned long word2)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_update_descriptor), 
        "b" (pa), "c" (word1), "d" (word2) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_set_fast_trap(int idx)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_set_fast_trap), 
        "b" (idx) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_dom_mem_op(void *dom_mem_op)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_dom_mem_op),
        "b" (dom_mem_op) : "memory" );

    return ret;
}

extern inline int HYPERVISOR_multicall(void *call_list, int nr_calls)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_multicall),
        "b" (call_list), "c" (nr_calls) : "memory" );

    return ret;
}

extern inline long HYPERVISOR_kbd_op(unsigned char op, unsigned char val)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_kbd_op),
        "b" (op), "c" (val) : "memory" );

    return ret;
}

/* Use macros so they'll work if we compile without any optimization */

#define HYPERVISOR_update_va_mapping(page_nr,new_val,flags) ({ \
  int ret; \
  __asm__ __volatile__ (TRAP_INSTR : "=a" (ret) \
                                   : "0" (__HYPERVISOR_update_va_mapping), "b" (page_nr), "c" (new_val), "d" (flags) \
                                   : "memory"); \
  ret; \
})

#define HYPERVISOR_console_write(str,count) ({ \
  int ret; \
  __asm__ __volatile__ (TRAP_INSTR : "=a" (ret) \
                                   : "0" (__HYPERVISOR_console_write), "b" (str), "c" (count) \
                                   : "memory"); \
  ret; \
})

#define HYPERVISOR_mmu_update(req,count) ({ \
  int ret; \
  __asm__ __volatile__ (TRAP_INSTR : "=a" (ret) \
                                   : "0" (__HYPERVISOR_mmu_update), "b" (req), "c" (count) \
                                   : "memory"); \
  ret; \
})

#define HYPERVISOR_block_io_op(block_io_op) ({ \
  int ret; \
  __asm__ __volatile__ (TRAP_INSTR : "=a" (ret) \
                                   : "0" (__HYPERVISOR_block_io_op), "b" (block_io_op) \
                                   : "memory" ); \
  ret; \
})

#define HYPERVISOR_exit() ({ \
  int ret; \
  __asm__ __volatile__ (TRAP_INSTR : "=a" (ret) \
                                   : "0" (__HYPERVISOR_sched_op), "b" (SCHEDOP_exit) \
                                   : "memory" ); \
  ret; \
})

#endif
