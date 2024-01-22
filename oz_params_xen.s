##+++2004-07-31
##    Copyright (C) 2004  Mike Rieker, Beverly, MA USA
##
##    This program is free software; you can redistribute it and/or modify
##    it under the terms of the GNU General Public License as published by
##    the Free Software Foundation; version 2 of the License.
##
##    This program is distributed in the hope that it will be useful,
##    but WITHOUT ANY WARRANTY; without even the implied warranty of
##    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##    GNU General Public License for more details.
##
##    You should have received a copy of the GNU General Public License
##    along with this program; if not, write to the Free Software
##    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
##---2004-07-31

##########################################################################
##
##  These must match values in oz_hwxen_hypervisor.h

FLAT_RING1_CS = 0x0819
FLAT_RING1_DS = 0x0821
FLAT_RING3_CS = 0x082B
FLAT_RING3_DS = 0x0833

## EAX = vector; EBX, ECX, EDX, ESI, EDI = args 1, 2, 3, 4, 5

__HYPERVISOR_set_trap_table     =   0
__HYPERVISOR_mmu_update         =   1
__HYPERVISOR_console_write      =   2
__HYPERVISOR_set_gdt            =   3
__HYPERVISOR_stack_switch       =   4
__HYPERVISOR_set_callbacks      =   5
__HYPERVISOR_net_io_op          =   6
__HYPERVISOR_fpu_taskswitch     =   7
__HYPERVISOR_sched_op           =   8
__HYPERVISOR_dom0_op            =   9
__HYPERVISOR_network_op         =  10
__HYPERVISOR_block_io_op        =  11
__HYPERVISOR_set_debugreg       =  12
__HYPERVISOR_get_debugreg       =  13
__HYPERVISOR_update_descriptor  =  14
__HYPERVISOR_set_fast_trap      =  15
__HYPERVISOR_dom_mem_op         =  16
__HYPERVISOR_multicall          =  17
__HYPERVISOR_kbd_op             =  18
__HYPERVISOR_update_va_mapping  =  19
__HYPERVISOR_event_channel_op   =  20

TRAP_INSTR = 0x82

SCHEDOP_yield = 0

## Offsets in oz_hwxen_sharedinfo

	sh_events                 =   0	# outstanding event notification
	sh_events_mask            =   4	# mask to enable event delivery
		SH_M_EVENTS_MASTER_ENABLE = 0x80000000
		SH_V_EVENTS_MASTER_ENABLE = 31
	sh_event_channel_pend     =   8
	sh_event_channel_pend_sel = 136
	sh_event_channel_disc     = 140
	sh_event_channel_disc_sel = 268
	sh_rdtsc_bitshift         = 272
	sh_cpu_freq               = 276
	sh_time_version1          = 284
	sh_time_version2          = 288
	sh_tsc_timestamp          = 292
	sh_system_time            = 296
	sh_wc_sec                 = 304
	sh_wc_usec                = 308

##  End of stuff from oz_hwxen_hypervisor.h
##
##########################################################################

	ESTACKSIZE = 12288

	# Block Asynchronous Event delivery

.macro	BLOCK_EVENTS
	andb	$~SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3
	.endm	BLOCK_EVENTS

	# Allow Asynchronous Event delivery
	# probably should call oz_hwxen_enablevents instead

.macro	ALLOW_EVENTS
	orb	$SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3
	.endm	ALLOW_EVENTS

	# Push events mask and make sure event delivery is blocked
	# This macro is the analog to 'PUSHFL ; CLI'

.macro	PBL_EVENTS
	pushl	oz_hwxen_sharedinfo+sh_events_mask
	andb	$~SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3
	.endm	PBL_EVENTS

	# Pop events mask and check for pending events
	# This macro is the analog to 'POPFL' (except it wastes eax,ecx,edx)

.macro	POP_EVENTS
	popl	oz_hwxen_sharedinfo+sh_events_mask
	testb	$~SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3
	je	pop_events_\@
	call	oz_hwxen_checkevents
pop_events_\@:
	.endm	POP_EVENTS

	# miscellaneous interrupt vectors

	INT_LOWIPL       = 0x20		# low priority for apic tskpri
					# (must be same block of 16 as INT_QUANTIM so inhibiting softint delivery inhibits INT_QUANTIM interrupts)
	  L2_INT_SOFTINT = 5		# log2 (INT_SOFTINT)

	INT_SYSCALL      = 0x21		# system calls
	INT_PRINTKP      = 0x22		# temp call to access oz_knl_printk(p)
	INT_GETNOW       = 0x23		# get current date/time in edx:eax (must match oz_common_486.s)
	INT_QUANTIM      = 0x24		# quantum timer (must be same block of 16 as INT_SOFTINT)
	INT_CHECKASTS    = 0x25		# check for outer mode ast's
	INT_DIAGMODE     = 0x26		# put all processors in diag mode (via APIC - see oz_hw_diag)
	INT_RESCHED      = 0x27		# cause a reschedule interrupt on the cpu
	INT_NEWUSRSTK    = 0x28		# there is a new user stack for this thread
	INT_FAKENMI      = 0x29		# fake an nmi (oz_hw_smproc_486.s only)
	INT_WAITLOOPWAKE = 0x2A		# wake from the waitloop's HLT (oz_hw_smproc_486.s only)

	# SMP locks

	SL_B_LEVEL  =  0			# smp lock level - this value is never modified
	SL_B_CPUID  =  1			# cpu index that has it locked
						# -1 if not locked
	SL__SIZE    =  2			# size of struct
						# must match oz_common_xen.s
						# must be no larger than OZ_SMPLOCK_SIZE

	OZ_SMPLOCK_LEVEL_EVENTS = 0xA0		# SMP levels used for events - ** must match oz_hw_xen.h **

	# Per-CPU data (in oz_hw486_cpudb)

	CPU_L_PRIORITY     =  0			# current smp lock level and thread priority
	  CPU_B3_THREADPRI = CPU_L_PRIORITY+0	# - thread priority is low 24 bits
	  CPU_B_SMPLEVEL   = CPU_L_PRIORITY+3	# - smp level is high 8 bits
	CPU_L_SPTENTRY     =  4			# cpu's own spt entry for accessing physical pages
						# - there are two pages here
	CPU_L_SPTVADDR     =  8			# virtual address mapped by CPU_L_SPTENTRY spt
						# - covers two pages
	CPU_L_THCTX        = 12			# current thread hardware context pointer
	CPU_L_PRCTX        = 16			# current process hardware context pointer
	CPU_L_INVLPG_VADR  = 20			# virtual address to be invalidated
	CPU_L_INVLPG_CPUS  = 24			# bitmask of cpu's that haven't invalidated yet
	CPU_Q_QUANTIM      = 28			# quantum timer expiration (absolute biased RDTSC value)
	CPU_L_MPDBASE      = 36			# per cpu MPD base (PARANOID_MPD mode only)
	CPU_Q_RDTSCBIAS    = 40			# bias to keep RDTSC the same on all CPU's
	CPU__SIZE          = 48			# size of struct

	CPU__L2SIZE = 6				# power of 2 that is >= CPU__SIZE
.if ((1<<CPU__L2SIZE)<CPU__SIZE)
	error : (1<<CPU__L2SIZE) < CPU__SIZE
.endif
	CPU__SIZE = 1<<CPU__L2SIZE		# now make table entries exactly that size

	# Hardware context saved in OZ_Thread block

	THCTX_L_USTKSIZ  =   0		# number of pages in user stack
	THCTX_L_USTKVPG  =   4		# virtual page of base of user stack
	THCTX_L_ESTACKVA =   8		# executive stack top virtual address (initial value of esp)
	THCTX_L_EXESP    =  12		# saved executive stack pointer at context switch
	THCTX_B_FPUSED   =  16		# <00> = 0 : fp not used last time around
					#        1 : fp was used last time around
					# <01> = 0 : THCTX_X_FPSAVE has not been initialized
					#        1 : THCTX_X_FPSAVE has been initialized
	THCTX_B_DBUSED   =  17		# <00> = 0 : debug registers not in use by thread
					#        1 : debug registers are in use by thread
        THCTX_L_DR0      =  20		# saved debug registers
        THCTX_L_DR1      =  24
        THCTX_L_DR2      =  28
        THCTX_L_DR3      =  32
        THCTX_L_DR7      =  36
	THCTX_X_FPSAVE   =  48		# floating point save area (512+16 bytes)
					# (extra 16 bytes are for 16-byte alignment)
	THCTX__SIZE      =  48+16+512	# size of hardware thread context block
					# - must be no larger than OZ_THREAD_HW_CTX_SIZE

	# What oz_hw_thread_switchctx expects on the stack when it switches
	# (this is pointed to by THCTX_L_EXESP when the thread is not active in a cpu)

	THSAV_L_EBX =  0		# saved C registers
	THSAV_L_ESI =  4
	THSAV_L_EDI =  8
        THSAV_L_EFL = 12		# eflags (in case IOPL is different)
	THSAV_L_EBP = 16		# saved frame pointer
	THSAV_L_EIP = 20		# saved instruction pointer
	THSAV__SIZE = 24

	# Machine argument list, OZ_Mchargs in oz_hw_xen.h must match this

	MCH_L_EC2     =  0
	MCH_L8_PUSHAL =  4
	MCH_L_EDI     =  4
	MCH_L_ESI     =  8
	MCH_L_EC1     = 12
	MCH_L_ESP     = 16
	MCH_L_EBX     = 20
	MCH_L_EDX     = 24
	MCH_L_ECX     = 28
	MCH_L_EAX     = 32
	MCH_L_EBP     = 36
	MCH_L_EIP     = 40
	MCH_W_CS      = 44
	MCH_W_PAD1    = 46	# MCH_W_PAD1<7> is used to store the SH_V_EVENTS_MASTER_ENABLE bit
	MCH_L_EFLAGS  = 48	# MCH_L_EFLAGS<IF> should always be set
	MCH__SIZE     = 52

	MCH_L_XSP     = 52	# not part of mchargs returned to user, but this is where the outer mode stack pointer is in an exception handler
	MCH_W_XSS     = 56	# ditto for the outer mode stack segment register

	# Extended machine arguments (must match oz_hw_486.h OZ_Mchargx struct)

	MCHX_W_DS    =  0
	MCHX_W_ES    =  2
	MCHX_W_FS    =  4
	MCHX_W_GS    =  6
	MCHX_W_SS    =  8
	MCHX_W_PAD1  = 10
	MCHX_L_DR0   = 12
	MCHX_L_DR1   = 16
	MCHX_L_DR2   = 20
	MCHX_L_DR3   = 24
	MCHX_L_DR7   = 28
	MCHX_L_CR0   = 32
	MCHX_L_CR2   = 36
	MCHX_L_CR3   = 40
	MCHX_L_CR4   = 44

