##+++2004-07-15
##    Copyright (C) 2004 Mike Rieker, Beverly, MA USA
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
##---2004-07-15

##########################################################################
##									##
##  This is the main program for the kernel				##
##									##
##  It is loaded into memory at 0xC0000000 by the Xen loader		##
##  The processor is in 32-bit mode with paging enabled and in ring 1	##
##									##
##  The Xen loader passes the address of the startinfo struct in %esi	##
##									##
##  At this point, all physical pages we will get are mapped to 	##
##  virtual memory starting at 0xC0000000.  The startinfo struct has 	##
##  a pointer to the virtual address of the pagetable that maps all 	##
##  this memory.  There are no pages mapped below 0xC0000000.		##
##									##
##  Xen reserves virtual addresses 0xFC000000 thru 0xFFFFFFFF to 	##
##  itself.								##
##									##
##  As far as the main OZONE code is concerned, what it thinks is a 	##
##  physical address is really just an byte offset from 0xC0000000.  	##
##  We and Xen call these pseudo-physical addresses.  To get the 	##
##  corresponding real-world physical address, we read the pte in the 	##
##  startinfo supplied pagetable and translate.  To translate a real-	##
##  world physical address to a pseudo-physical address, Xen provides 	##
##  a translation table at 0xFC000000.					##
##									##
##  %esi = points to the 'startinfo' struct				##
##  %cs, %ds, %es, %ss = appropriate semi-flat segments			##
##  We're running in Ring 1						##
##									##
##########################################################################

	.include "oz_params_xen.s"
##
##  Block or Allow Asynchronous Event delivery
##  This is what we considler inhibiting or enabling 'interrupt' delivery
##
.macro	BLOCK_EVENTS
	andb	$~SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3
	.endm	BLOCK_EVENTS

.macro	ALLOW_EVENTS
	orb	$SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3
	.endm	ALLOW_EVENTS

	.data
	.p2align 12
##
##  The initial stack must be located within our image so we know it can't overlap any data set up by Xen
##
initial_stack:		.space	ESTACKSIZE
initial_stack_end:
##
##  The startinfo struct gets copied over this
##
	.globl	oz_hwxen_startinfo
	.type	oz_hwxen_startinfo,@object
oz_hwxen_startinfo:	.space	4096
##
##  The sharedinfo struct page gets mapped over this
##
	.globl	oz_hwxen_sharedinfo
	.type	oz_hwxen_sharedinfo,@object
oz_hwxen_sharedinfo:	.space	4096

	.text
ssw_msg1:	.string	"oz_kernel_xen: error %d setting stack pointer"
stt_msg1:	.string	"oz_kernel_xen: error %d setting trap table"
scb_msg1:	.string	"oz_kernel_xen: error %d setting callbacks"

	.align	4
	.globl	_start
	.type	_start,@function
_start:
##
##  Put our stack somewhere where it won't get tromped on
##  This is somewhere within our linked image so Xen won't have put anything there
##
	movl	$initial_stack_end,%esp
	movl	$__HYPERVISOR_stack_switch,%eax
	movl	%ss,%ebx
	movl	%esp,%ecx
	int	$TRAP_INSTR
	testl	%eax,%eax
	je	1000f
	pushl	%eax
	pushl	$ssw_msg1
	call	oz_crash
1000:
##
##  Copy the startinfo struct where we know it won't get tromped on
##  This is somewhere within our linked image so Xen won't have put anything there
##
	cld
	movl	oz_hwxen_startinfo_size,%ecx
	movl	$oz_hwxen_startinfo,%edi
	rep
	movsb
1000:
	cmpl	$oz_hwxen_startinfo+4096,%edi
	je	2000f
	lodsb
	stosb
	testb	%al,%al
	jne	1000b
2000:
##
##  Now it's OK to call the C-language routine (it should never return)
##
	xorl	%ebp,%ebp
	call	oz_hwxen_start
	call	oz_hw_halt
##
##  Initialization routine called when oz_hwxen_sharedinfo has been mapped
##
	.globl	oz_hwxen_init2
	.type	oz_hwxen_init2,@function
oz_hwxen_init2:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx
	pushl	%esi
##
##  Calculate (2^32 * 10^7) / (cpufreq >> bitshift)
##
	movl	$10000000,%edx
	movl	oz_hwxen_sharedinfo+sh_rdtsc_bitshift,%ebx
	testl	%ebx,%ebx
	je	2000f
1000:
	testl	%edx,%edx
	js	2000f
	addl	%edx,%edx
	decl	%ebx
	jne	1000b
2000:
	pushl	oz_hwxen_sharedinfo+sh_cpu_freq+4
	pushl	oz_hwxen_sharedinfo+sh_cpu_freq+0
	pushl	%edx
	pushl	$0
	call	__udivdi3
	addl	$16,%esp
	shldl	%cl,%eax,%edx
	shll	%cl,%eax
	movl	%eax,tensevenovershcpufreq+0
	movl	%edx,tensevenovershcpufreq+4
##
##  Set up trap table
##
	movl	$__HYPERVISOR_set_trap_table,%eax
	movl	$trap_table,%ebx
	int	$TRAP_INSTR
	testl	%eax,%eax
	je	1000f
	pushl	%eax
	pushl	$stt_msg1
	call	oz_crash
1000:
##
##  Enable asynchronous event delivery
##
	movl	$__HYPERVISOR_set_callbacks,%eax
	movl	%cs,%ebx
	movl	$oz_hwxen_cb_asyncevent,%ecx
	movl	%cs,%edx
	movl	$oz_hwxen_cb_failsafe,%esi
	int	$TRAP_INSTR
	testl	%eax,%eax
	je	1000f
	pushl	%eax
	pushl	$scb_msg1
	call	oz_crash
1000:
	movl	$-1,oz_hwxen_sharedinfo+sh_events_mask
	call	oz_hwxen_enablevents
##
	popl	%esi
	popl	%ebx
	popl	%ebp
	ret

##########################################################################
##									##
##  Synchronous trap callbacks						##
##									##
##########################################################################

	## vector = standard vector number
	## dpl = what mode it's callable from
	##       +4 to clear SH_M_EVENTS_MASTER_ENABLE on input
	##       never use +4 because it doesn't save prior state
	## address = address of handler

.macro	TTE	vector,dpl,address
	.byte	\vector,\dpl
	.word	FLAT_RING1_CS
	.long	\address
	.endm	TTE

	.p2align 3
trap_table:
	TTE	            0,0,oz_hwxen_exception_de	# divide error
	TTE	            1,0,oz_hwxen_exception_db	# debug trap
	TTE	            3,3,oz_hwxen_exception_bp	# breakpoint instr
	TTE	            4,3,oz_hwxen_exception_of	# overflow trap
	TTE	            5,3,oz_hwxen_exception_br	# bound instr
	TTE	            6,3,oz_hwxen_exception_ud	# undefined opcode
	TTE	            7,0,oz_hwxen_exception_nm	# fpu disabled
	TTE	           10,0,oz_hwxen_exception_ts	# task segment bad
	TTE	           11,0,oz_hwxen_exception_np	# segment not present
	TTE	           12,0,oz_hwxen_exception_ss	# stack segment bad
	TTE	           13,0,oz_hwxen_exception_gp	# general protection fault
	TTE	           14,0,oz_hwxen_exception_pf	# page fault
	TTE	           16,0,oz_hwxen_exception_mf	# fpu exception
	TTE	           17,0,oz_hwxen_exception_ac	# alignment check
	TTE	   INT_GETNOW,3,oz_hwxen_except_getnow	# get current datebin
	TTE	  INT_SYSCALL,3,oz_hwxen_except_syscall	# system calls
	TTE	INT_NEWUSRSTK,3,oz_hwxen_thread_newusrstk # new user stack base
	.long	0,0

##########################################################################
##									##
##  Standard exceptions - no error - just move to caller's stack and 	##
##  signal the exception						##
##									##
##  We are entered here in exec mode with event mask unchanged		##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwxen_exception_de
	.type	oz_hwxen_exception_de,@function
oz_hwxen_exception_de:
	pushl	%ebp
	movl	oz_DIVBYZERO,%ebp
	jmp	exception_xx

	.p2align 4
	.globl	oz_hwxen_exception_bp
	.type	oz_hwxen_exception_bp,@function
oz_hwxen_exception_bp:
	pushl	%ebp
	movl	oz_BREAKPOINT,%ebp
	jmp	exception_xx

	.p2align 4
	.globl	oz_hwxen_exception_of
	.type	oz_hwxen_exception_of,@function
oz_hwxen_exception_of:
	pushl	%ebp
	movl	oz_ARITHOVER,%ebp
	jmp	exception_xx

	.p2align 4
	.globl	oz_hwxen_exception_br
	.type	oz_hwxen_exception_br,@function
oz_hwxen_exception_br:
	pushl	%ebp
	movl	oz_SUBSCRIPT,%ebp
	jmp	exception_xx

	.p2align 4
	.globl	oz_hwxen_exception_ud
	.type	oz_hwxen_exception_ud,@function
oz_hwxen_exception_ud:
	pushl	%ebp
	movl	oz_UNDEFOPCODE,%ebp

exception_xx:
	pushal				# finish build mchargs (exception code in %ebp will go into MCH_L_EC1)
	pushl	$0			# push a zero for EC2
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs
	movl	MCH_L_EC1(%esp),%ebx	# get OZ_exceptioncode in %ebx
	movl	$0,MCH_L_EC1(%esp)
	jmp	exception_move		# go signal the exception

##########################################################################
##									##
##  General protection exception					##
##									##
##    Input:								##
##									##
##	 0(%esp) = faulting address (from CR2)				##
##	 4(%esp) = error code						##
##	 8(%esp) = old EIP						##
##	12(%esp) = old CS						##
##	16(%esp) = old EFLAGS						##
##									##
##  We are entered here in exec mode with event mask unchanged		##
##									##
##########################################################################

	UDSUDS = (FLAT_RING3_DS << 16) | FLAT_RING3_DS

	.p2align 4
	.globl	oz_hwxen_exception_ts
	.globl	oz_hwxen_exception_np
	.globl	oz_hwxen_exception_ss
	.globl	oz_hwxen_exception_gp
	.type	oz_hwxen_exception_ts,@function
	.type	oz_hwxen_exception_np,@function
	.type	oz_hwxen_exception_ss,@function
	.type	oz_hwxen_exception_gp,@function
oz_hwxen_exception_ts:
oz_hwxen_exception_np:
oz_hwxen_exception_ss:
oz_hwxen_exception_gp:
	pushw	%es				# about all a fool can do is set these to null
	pushw	%ds				# ... then call the kernel to try to mess it up
	cmpl	$UDSUDS,(%esp)
	je	except_gp_real
	movl	$UDSUDS,(%esp)			# so if they're not what we want, 
	popw	%ds				# ... just put them back
	popw	%es
	xchgl	(%esp),%ebp			# now that %ds and %es are ok, push mchargs
	pushal
	movb	oz_hwxen_sharedinfo+sh_events_mask+3,%al
	pushl	$0
	movb	%al,MCH_W_PAD1+1(%esp)
	call	oz_hw_fixkmexceptebp		# point to stack frame
	call	oz_hw_fixkmmchargesp		# calc esp at time of exception in the mchargs
	BLOCK_EVENTS				# ... and we can check for events and ast's that may have slipped in
	jmp	oz_hwxen_iretwithmchargs

except_gp_real:
	addl	$4,%esp				# both %ds and %es are ok, so it's not usermode trying to trick execmode
	xchgl	(%esp),%ebp			# save mchargs on stack
	pushal
	pushl	$0				# with a dummy EC2
	call	oz_hw_fixkmexceptebp		# point to stack frame
	call	oz_hw_fixkmmchargesp		# calc esp at time of exception in the mchargs
	movl	oz_GENERALPROT,%ebx
	jmp	exception_move

##########################################################################
##									##
##  Pagefault exception							##
##									##
##    Input:								##
##									##
##	 0(%esp) = faulting address (from CR2)				##
##	 4(%esp) = error code						##
##	 8(%esp) = old EIP						##
##	12(%esp) = old CS						##
##	16(%esp) = old EFLAGS						##
##									##
##  We are entered here in exec mode with event mask unchanged		##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwxen_exception_pf
	.type	oz_hwxen_exception_pf,@function
oz_hwxen_exception_pf:
					# it has two values aleady on the stack
	xchgl	4(%esp),%ebp		# save ebp, get error code
	xchgl	 (%esp),%eax		# save eax, get cr2 contents
	pushl	%ecx			# save registers on stack
	pushl	%edx
	pushl	%ebx
	pushl	%esp
	movb	oz_hwxen_sharedinfo+sh_events_mask+3,%dl
	pushl	%ebp			# - ec1 = why it faulted
	pushl	%esi
	pushl	%edi
	pushl	%eax			# - ec2 = where it faulted
	movb	%dl,MCH_W_PAD1+1(%esp)
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs

	testb	$SH_M_EVENTS_MASTER_ENABLE>>24,MCH_W_PAD1+1(%esp) # see if event delivery inhibited
	je	except_pf_signal	# if so, signal the pagefault as an executive mode access violation
	call	oz_hw486_getcpu		# see which cpu we're on
	cmpb	$2,CPU_B_SMPLEVEL(%esi)	# see if above softint (ie, spinlocks are taken)
	jnc	except_pf_signal	# if above softint, signal the pagefault as an executive mode access violation
	movl	MCH_L_EC2(%esp),%eax	# softint or below, get virtual address that caused fault
	movl	MCH_L_EC1(%esp),%ebx	# get reason for pagefault
	xorl	%ecx,%ecx		# assume they were trying to read
	bt	$1,%ebx			# maybe they were trying to write
	adcb	$0,%cl
	shrl	$12,%eax		# calculate vpage that caused fault
	movl	oz_PROCMODE_KNL,%edx	# assume access was from executive mode
	bt	$2,%ebx
	jnc	except_pf_exe
	movl	oz_PROCMODE_USR,%edx	# - it was from user mode
except_pf_exe:
	movl	CPU_L_THCTX(%esi),%edi	# don't allow access to lowest user stack page
	cmpl	%eax,THCTX_L_USTKVPG(%edi)
	je	except_pf_lowustkpg
	pushl	%esp			# push mchargs pointer
	pushl	%ecx			# push read/write flag that caused fault
	pushl	%eax			# push vpage that caused fault
	pushl	%edx			# push access mode that caused fault
	call	oz_knl_section_faultpage # try to read in page
	addl	$16,%esp		# wipe call args from stack
	BLOCK_EVENTS			# have to block events for oz_hwxen_iretwithmchargs
	cmpl	oz_SUCCESS,%eax		# see if successful
	je	oz_hwxen_iretwithmchargs # if so, iret out to retry faulting instruction
	call	oz_hwxen_enablevents

## Page read or other fatal error, signal it

	movl	%eax,%ebx		# save error status
	xorl	%eax,%eax		# no extra longs on stack to copy
	call	oz_hwxen_movemchargstocallerstack
	cmpl	oz_SUCCESS,%eax
	jne	except_pf_baduserstack
	movl	%esp,%esi		# point to mchargs
	cmpl	oz_ACCVIO,%ebx		# see if simple access violation
	je	except_pf_signal
	pushl	$0			# if not, also signal fault failure code
	pushl	%ebx
	pushl	MCH_L_EC1(%esi)		# signal the fault
	pushl	MCH_L_EC2(%esi)
	pushl	$2
	pushl	oz_ACCVIO
	pushl	$6
	jmp	exception_test
except_pf_signal:
	movl	%esp,%esi		# point to mchargs
	pushl	MCH_L_EC1(%esi)		# signal the fault
	pushl	MCH_L_EC2(%esi)
	pushl	$2
	pushl	oz_ACCVIO
	pushl	$4
	jmp	exception_test

## User stack cannot hold mchargs, so we abort the thread (we are still on executive stack)

except_pf_baduserstack:
	pushl	MCH_L_ESP(%esi)		# output message to console indicating why we are aborting it
	pushl	%ebx
	pushl	$except_pf_msg1
	call	oz_knl_printk
	pushl	%ebx			# exit thread with original oz_knl_section_faultpage error code as the exit status
	jmp	except_pf_usertrace

## Lowest user stack page is a guard, print error message and exit

except_pf_lowustkpg:
	movl	%esp,%esi		# point to mchargs on stack
	pushl	%eax			# output message to console indicating why we are aborting it
	pushl	$except_pf_msg2
	call	oz_knl_printk
	pushl	oz_USERSTACKOVF		# push thread exit status on stack

## Try to trace a few call frames before exiting

except_pf_usertrace:
	pushl	$0			# push current thread's name
	call	oz_knl_thread_getname
	movl	%eax,(%esp)
	pushl	$0			# push current thread's id
	call	oz_knl_thread_getid
	movl	%eax,(%esp)
	pushl	$except_pf_msg3		# print them out
	call	oz_knl_printk
	pushl	%esi			# print out the mchargs
	pushl	$MCH__SIZE
	call	oz_knl_dumpmem
	addl	$20,%esp		# wipe call params so exit status is on top of stack
	movl	MCH_L_EBP(%esi),%edi	# try to do a traceback of a few frames
	movl	$8,%ebx
except_pf_usertraceloop:
	pushl	$0			# - make sure we can read the 8 bytes at %edi
	pushl	$0
	pushl	%edi
	pushl	$8
	call	oz_hw_probe
	addl	$16,%esp
	testl	%eax,%eax
	je	except_pf_usertracedone	# - if not, stop tracing
	pushl	%edi			# - ok, print out a call frame
	pushl	(%edi)
	pushl	4(%edi)
	pushl	$except_pf_msg4
	call	oz_knl_printk
	addl	$16,%esp
	movl	(%edi),%edi		# - link to next frame in chain
	decl	%ebx			# - but only do this many so we dont go banannas
	jne	except_pf_usertraceloop
except_pf_usertracedone:
	call	oz_knl_thread_exit	# finally, exit with status on stack
	jmp	except_pf_usertracedone

except_pf_msg1:	.string	"oz_hwxen_exception_pf: error %u locking user stack (below %p) in memory\n"
except_pf_msg2:	.string	"oz_hwxen_exception_pf: user stack overflow, vpage 0x%X\n"
except_pf_msg3:	.string	"oz_hwxen_exception_pf: - thread id %u, name %s, mchargs:\n"
except_pf_msg4:	.string	"oz_hwxen_exception_pf:   rtn addr %8.8X  next ebp %8.8X : %8.8X\n"

##########################################################################
##									##
##  Debug exception							##
##									##
##  This is called in exec mode with event mask unchanged		##
##									##
##  It reads and clears dr6, setting mchargs ec1=dr6's value		##
##  It pushes sigargs (perhaps multiples) for what's in dr6		##
##  Finally, it switches to user stack and locates condition handler	##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwxen_exception_db
	.type	oz_hwxen_exception_db,@function
oz_hwxen_exception_db:
	pushl	%ebp			# build mchargs on stack
	pushal
	movb	oz_hwxen_sharedinfo+sh_events_mask,%dl
	pushl	$0
	movb	%dl,MCH_W_PAD1+1(%esp)
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs
	movl	$__HYPERVISOR_get_debugreg,%eax
	movl	$6,%ebx			# set MCH_L_EC1 = %dr6
	int	$TRAP_INSTR
	movl	%eax,MCH_L_EC1(%esp)
	movl	$__HYPERVISOR_set_debugreg,%eax
	movl	$6,%ebx			# set %dr6 = 0
	xorl	%ecx,%ecx
	int	$TRAP_INSTR
	movl	%esp,%esi		# save pointer to mchargs
	xorl	%edi,%edi		# no sigargs pushed on stack yet
	movl	MCH_L_EC1(%esi),%ecx	# get dr6 contents
	bt	$14,%ecx		# see if singlestep caused exception
	jnc	2000f
	pushl	$0			# ok, push singlestep exception
	pushl	oz_SINGLESTEP
	addl	$2,%edi
2000:
	testb	$8,%cl			# see if dr3 match detected
	je	3000f
	movl	$__HYPERVISOR_get_debugreg,%eax
	movl	$3,%ebx
	int	$TRAP_INSTR
	pushl	%eax
	pushl	$1
	pushl	oz_WATCHPOINT
	addl	$3,%edi
3000:
	testb	$4,%cl			# see if dr2 match detected
	je	4000f
	movl	$__HYPERVISOR_get_debugreg,%eax
	movl	$2,%ebx
	int	$TRAP_INSTR
	pushl	%eax
	pushl	$1
	pushl	oz_WATCHPOINT
	addl	$3,%edi
4000:
	testb	$2,%cl			# see if dr1 match detected
	je	5000f
	movl	$__HYPERVISOR_get_debugreg,%eax
	movl	$1,%ebx
	int	$TRAP_INSTR
	pushl	%eax
	pushl	$1
	pushl	oz_WATCHPOINT
	addl	$3,%edi
5000:
	testb	$1,%cl			# see if dr0 match detected
	je	6000f
	movl	$__HYPERVISOR_get_debugreg,%eax
	xorl	%ebx,%ebx
	int	$TRAP_INSTR
	pushl	%eax
	pushl	$1
	pushl	oz_WATCHPOINT
	addl	$3,%edi
6000:
	pushl	%edi			# push number of longs in sigargs (not incl this long)
	leal	1(%edi),%eax		# number of longs on stack until mchargs
	call	oz_hwxen_movemchargstocallerstack # if caller was user mode, move schmeel to user stack
	cmpl	oz_SUCCESS,%eax		# ... pagefaulting as necessary, then switch to user mode
	je	exception_test
	movl	oz_SINGLESTEP,%ebx
	jmp	exception_move_bad

##########################################################################
##									##
##  Other specially handled exceptions					##
##									##
##  We are entered here in exec mode with event mask unchanged		##
##									##
##########################################################################

## Attempt to access FPU when it is disabled
## The hypervisor has already cleared the TS bit for us

	.p2align 4
	.globl	oz_hwxen_exception_nm
	.type	oz_hwxen_exception_nm,@function
oz_hwxen_exception_nm:
	pushl	%ebp				# build mchargs on stack
	pushal
	movb	oz_hwxen_sharedinfo+sh_events_mask+3,%dl
	pushl	$0
	movb	%dl,MCH_W_PAD1+1(%esp)
	call	oz_hw486_getcpu			# get pointer to cpudb for this cpu
	movl	CPU_L_THCTX(%esi),%eax		# get the current thread hardware context pointer
	fninit					# initialize fpu
	testb	$2,THCTX_B_FPUSED(%eax)		# see if it has been initialized for this thread once upon a time
	movb	$3,THCTX_B_FPUSED(%eax)		# set bit 0 saying that this thread has used fpu since last oz_hw_thread_switchctx
						# set bit 1 saying that this thread has used fpu in its lifetime
	je	exception_nm_rtn
	andb	$0xF0,%al			# fxrstor requires 16-byte alignment
frstor_pat:			# frstor/nop gets patched to fxrstor if cpu is mmx
	frstor	THCTX_X_FPSAVE+16(%eax)		# if so, restore prior fpu state
	nop
exception_nm_rtn:
	BLOCK_EVENTS				# go check for deliverable events and asts before returning
	jmp	oz_hwxen_iretwithmchargs

## FPU arithmetic error

	.p2align 4
	.globl	oz_hwxen_exception_mf
	.type	oz_hwxen_exception_mf,@function
oz_hwxen_exception_mf:
	pushl	%ebp			# build mchargs on stack
	pushal
	movb	oz_hwxen_sharedinfo+sh_events_mask+3,%dl
	pushl	$0			# push a zero for EC2
	movb	%dl,MCH_W_PAD1+1(%esp)
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs
	xorl	%eax,%eax		# copy out to user' stack
	call	oz_hwxen_movemchargstocallerstack
	cmpl	oz_SUCCESS,%eax
	movl	oz_FLOATPOINT,%ebx
	jne	exception_move_bad
	subl	$28,%esp		# back in user mode ...
	fnstenv	(%esp)			# push fp state as signal arguments
	pushl	$7
	pushl	%ebx
	pushl	$9
	jmp	exception_test

## Alignment check
## Error code (always zero) pushed on stack

	.p2align 4
	.globl	oz_hwxen_exception_ac
	.type	oz_hwxen_exception_ac,@function
oz_hwxen_exception_ac:
	xchgl	(%esp),%ebp		# save ebp
	pushal				# finish mchargs
	pushl	$0			# including an EC2 = 0
	movl	oz_ALIGNMENT,%ebx
	jmp	exception_move

##########################################################################
##									##
##  Common exception handling routines					##
##									##
##########################################################################

##########################################################################
##									##
##  Exception handling - calls executive debugger if above softint 	##
##  level, otherwise the exception is signalled				##
##									##
##    Input:								##
##									##
##	processor stack = same as that of instruction causing exception	##
##	eax = error code (OZ_...)					##
##	esp = points to mchargs						##
##									##
##########################################################################

exception_move:				# - enter here with esp=mchargs on exec stack, ebx=exception code
	movb	oz_hwxen_sharedinfo+sh_events_mask+3,%dl
	xorl	%eax,%eax		# move to caller's stack
	movb	%dl,MCH_W_PAD1+1(%esp)
	call	oz_hwxen_movemchargstocallerstack
	cmpl	oz_SUCCESS,%eax
	jne	exception_move_bad
	movl	%ebx,%eax

exception_push:				# - enter here with esp=mchargs on faulter's stack, eax=exception code
	pushl	$0			# push sigargs on stack
	pushl	%eax
	pushl	$2

# sigargs on faulter's stack, followed directly by mchargs
# if we are in user mode, call the condition handler
# else if hardware interrupt delivery inhibited, call the executive debugger
# else call the condition handler

exception_test:
	movl	(%esp),%edx		# get number of longwords in sigargs
	leal	4(%esp,%edx,4),%edi	# point to machine args
	testb	$2,MCH_W_CS(%edi)	# see if in executive mode
	jne	exception_signal	# if not, go signal the exception
	testb	$SH_M_EVENTS_MASTER_ENABLE>>24,MCH_W_PAD1+1(%edi) # see if event delivery enabled
	je	exception_crash		# if not, call the debugger
	movl	4(%esp),%eax		# restore signal code
	cmpl	oz_SINGLESTEP,%eax	# call executive debugger if one of these exceptions
	je	exception_crash		# ... without checking for exception handler first
	cmpl	oz_BREAKPOINT,%eax
	je	exception_crash

			jmp	exception_crash	####???? always crash for now!!!

## call condition handler

exception_signal:
	movl	%esp,%esi		# point to sigargs
	pushl	%edi			# push mchargs pointer
	pushl	%esi			# push sigargs pointer
	call	oz_sys_condhand_call	# call condition handler, may unwind or return with:
					#  %eax = 0 : no handler present, use default
					#      else : resume execution
	testl	%eax,%eax		# see if it was able to process it
	je	exception_default
	movl	%edi,%esp		# if so, resume execution by first pointing stack pointer at mch args

## stack is assumed to contain the machine args with MCH_W_PAD1+1<7> containing event delivery enable bit

exception_rtn:
	movl	%cs,%ecx		# if currently in user mode, 
	testb	$2,%cl
	jne	exception_rtndirect	# ... just return using an iret instruction
	BLOCK_EVENTS			# block event delivery for oz_hwxen_iretwithmchargs
	testb	$SH_M_EVENTS_MASTER_ENABLE>>24,MCH_W_PAD1+1(%esp) # if event delivery to be enabled on return, 
	jne	oz_hwxen_iretwithmchargs # ... go check for pending events and softints
exception_rtndirect:
	addl	$4,%esp			# they stay blocked, pop ec2 from stack
	popal				# restore all the registers (except ebp gets ec1's value)
	popl	%ebp			# pop ebp from stack
	iret				# retry the faulting instruction

## fault was in user mode but we're unable to push exception frame on user's stack
## exec stack=mchargs; eax=push fault code; ebx=original fault code

exception_move_bad:
	pushl	%ebx
	pushl	%eax
	pushl	$except_move_bad_msg1
	call	oz_knl_printk
	pushl	%ebx
exception_move_die:
	call	oz_knl_thread_exit
	jmp	exception_move_die

## no suitable condition handler active, so either call the default handler (for user mode) or trap to the debugger

exception_default:
	movl	%esi,%esp		# wipe junk from stack, point it at sigargs
	testb	$2,MCH_W_CS(%edi)	# see if in executive mode
	je	exception_crash		# if so, go to debugger
	pushl	%edi			# user mode, push mchargs pointer
	pushl	%esi			# push sigargs pointer
	call	oz_sys_condhand_default	# print error message and exit

##########################################################################
##									##
##  Exception occurred in executive mode with no suitable handler, so 	##
##  call the debugger							##
##									##
##    Input:								##
##									##
##	stack contains sigargs followed by mchargs			##
##									##
##########################################################################

	.align	4
exception_crash:
	movl	(%esp),%ebx		# get # of longwords to sigargs, not counting this longword
	movl	%esp,%eax		# point to sigargs
	leal	4(%esp,%ebx,4),%ebx	# point to mchargs
	BLOCK_EVENTS			# make sure event delivery inhibited for debugger
	pushl	%ebx			# call executive debugger
	pushl	%eax
	call	oz_knl_debug_exception
	movl	%ebx,%esp		# wipe stack up to the machine arguments
	testl	%eax,%eax
	je	exception_rtn		# continue, restore registers from the mchargs on stack
exception_hang:
	call	oz_hw_halt
	jmp	exception_hang

except_move_bad_msg1:	.ascii	"oz_kernel_xen: exception %u pushing exception %u frame to user stack\n"
			.ascii	"  EC2 %8.8X  EDI %8.8X  ESI %8.8X  EC1 %8.8X  ESP %8.8X\n"
			.ascii	"  EBX %8.8X  EDX %8.8X  ECX %8.8X  EAX %8.8X  EBP %8.8X\n"
			.ascii	"  EIP %8.8X   CS %8.8X   EF %8.8X  XSP %8.8X  XSS %8.8X\n"
			.byte	0

##########################################################################
##									##
##  Move the machine args to the caller's stack				##
##									##
##    Input:								##
##									##
##	%eax = number of additional lw's to copy to outer stack		##
##	4(%esp) = additional lw's followed by mchargs			##
##	event delivery = enabled					##
##									##
##    Output:								##
##									##
##	%eax = OZ_SUCCESS : successfully moved				##
##	                    now on caller's stack			##
##	             else : error code					##
##	                    still on executive stack			##
##									##
##    Scratch:								##
##									##
##	%ecx, %edx, %esi, %edi						##
##									##
##    Pagefault stuff depends on it preserving %ebx			##
##									##
##########################################################################

	.align	4
	.globl	oz_hwxen_movemchargstocallerstack
	.type	oz_hwxen_movemchargstocallerstack,@function
oz_hwxen_movemchargstocallerstack:
	leal	4(%esp,%eax,4),%esi	# point to mchargs
	testb	$2,MCH_W_CS(%esi)	# see if it was from executive code, ie, it was on executive stack
	je	oncallerstack		# if so, the mchargs are already on the correct stack

## It was on user stack, copy mchargs to user stack then iret to user mode
## Use oz_knl_section_uput to copy so it can't page out or move on us

	movl	MCH_L_XSP(%esi),%edi	# this is the user's stack pointer
	leal	MCH__SIZE(,%eax,4),%eax	# calculate number of bytes to move
	subl	%eax,%edi		# this is the start of where to put sigargs and mchargs on user stack
	movl	%edi,MCH_L_XSP(%esi)	# put new stack pointer back where iret will see it
	leal	4(%esp),%edx		# point to start of data to move

	pushl	%eax			# save number of bytes being copied
	pushl	%edi			# - copy to this address
	pushl	%edx			# - copy from this address
	pushl	%eax			# - copy this how many bytes to copy
	pushl	oz_PROCMODE_USR		# - destination is in user mode
	call	oz_knl_section_uput
	addl	$16,%esp
	popl	%ecx			# restore number of bytes copied

	cmpl	oz_SUCCESS,%eax		# hopefully the oz_knl_section_uput succeeded
	jne	callerstackbad		# abort thread if it doesn't have any more user stack

	popl	%edx			# get my return address
	movl	%esi,%esp		# wipe extra longwords from executive stack
	movl	%eax,MCH_L_EAX(%esi)	# change EAX to success status
	movl	%ebx,MCH_L_EBX(%esi)	# return EBX same as on entry
	movl	%edx,MCH_L_EIP(%esi)	# change EIP to my return address
	movl	MCH_L_XSP(%esp),%ebp	# point to user stack area (but don't touch - it could be faulted back out)
	andb	$0xFE,MCH_L_EFLAGS+1(%esp) # make sure the trace (T) bit is clear in eflags
					# (so we dont trace the rest of the exception code or softint handler)
					# (the T bit is preserved in the mchargs that was moved to the user stack)
	leal	MCH_L_EBP-MCH__SIZE(%ebp,%ecx),%ebp # set up ebp to point to MCH_L_EBP in the machine args on the user stack
					# (just as it would be if we were all on executive stack returning via oncallerstack)
	BLOCK_EVENTS			# ... as required by oz_hwxen_iretwithmchargs
	jmp	oz_hwxen_iretwithmchargs # return to caller in user mode with everything now on user stack

	.align	4
oncallerstack:
	movl	oz_SUCCESS,%eax		# already on correct stack, return success status
callerstackbad:
	ret

##########################################################################
##									##
##  System calls							##
##									##
##    Input:								##
##									##
##	ecx = size of parameters on stack				##
##	edx = syscall index number					##
##	ebp = user call frame pointer					##
##									##
##    Output:								##
##									##
##	eax = status from system call routine				##
##									##
##########################################################################

	MAXARGBYTES = 64	# allows up to 16 long args to oz_sys_... routines

	.text
	.p2align 4
	.globl	oz_hw486_syscall
	.type	oz_hw486_syscall,@function
oz_hw486_syscall:
	movl	%ebp,%esp	# wipe in case gcc put extra stuff on stack
	popl	%ebp		# pop ebp to what it was just before call oz_sys_...
	popl	%eax		# pop return address just past call oz_sys_...
				# esp should now point to call args
	int	$INT_SYSCALL

## Input is as follows:
##
##       %eax = return address back to caller of oz_sys_... routine
##       %ecx = call arg bytecount
##       %edx = call index number
##       %ebp = value just before call to oz_sys_... routine
##    0(%esp) = points just past the int instruction (ignored)
##    4(%esp) = caller's code segment (FLAT_RING3_CS)
##    8(%esp) = eflags (ignored)
##   12(%esp) = caller's stack segment (FLAT_RING3_DS)
##   16(%esp) = caller's stack pointer (points to arg list)

	.align	4
	.p2align 4
	.globl	oz_hwxen_except_syscall
oz_hwxen_except_syscall:
	pushl	%ebp				# save frame pointer to what it was just before call oz_sys_...
	pushal					# finish mchargs
	pushl	$0				# ... including dummy EC2
	movl	%eax,MCH_L_EIP(%esp)		# save return address just past the call oz_sys_...
	movb	$0x80,MCH_W_PAD1+1(%esp)	# SH_M_EVENTS_MASTER_ENABLE should always be set in user mode
	movl	%esp,%ebx			# save mchargs pointer in exec stack
	cmpl	$MAXARGBYTES+1,%ecx		# check for bad arg bytecount
	jnc	except_syscall_bad		# - must be .le. MAXARGBYTES
	cmpl	oz_s_syscallmax,%edx		# see if call index exceeds maximum
	jnc	except_syscall_bad
	subl	$MAXARGBYTES,%esp		# make room on exec stack
	movl	MCH_L_XSP(%ebx),%eax		# point to user mode arg list
	pushl	%esp				# - always use max size in case of bad count
	pushl	%eax				# copy arg list from user stack to exec stack
	pushl	%ecx
	pushl	oz_PROCMODE_USR
	call	oz_knl_section_uget
	addl	$16,%esp
	cmpl	oz_SUCCESS,%eax
	jne	except_syscall_rtnsts
	movl	MCH_L_EDX(%ebx),%edx		# get validated call index back
	pushl	$0				# dummy saved eip
	pushl	$0				# dummy saved ebp
	movl	oz_s_syscalltbl(,%edx,4),%eax	# get oz_syscall_... routine entrypoint
	movl	%esp,%ebp			# point to dummy call frame so exception handlers won't try usermode handlers
	pushl	oz_PROCMODE_USR			# push caller's processor mode
	call	*%eax				# process the system call
except_syscall_rtnsts:
	movl	%ebx,%esp			# reset exec stack to mchargs
	movl	%eax,MCH_L_EAX(%ebx)		# save return status
	BLOCK_EVENTS				# block events as required by oz_hwxen_iretwithmchargs
	jmp	oz_hwxen_iretwithmchargs	# check for events, softints and ast's now deliverable
except_syscall_bad:
	movl	oz_BADSYSCALL,%eax
	jmp	except_syscall_rtnsts

##########################################################################
##									##
##  Get time-of-day							##
##									##
##########################################################################

	.data
	.p2align 3

last_bin:	.long	0,0	# last datebin value returned by oz_hw_tod_getnow
xtime_bin:	.long	0,0	# datebin last sampled from oz_hwxen_sharedinfo
tensevenovershcpufreq: .long 0,0 # 2^32 * 10^7 / shifted cpu frequency
xtime_version:	.long	0	# sh_time_version2 last time it was sampled
xtime_rdtsc:	.long	0	# low order rdtsc at sample time shifted by sh_rdtsc_bitshift

	.text

##  Usermode callable routine that returns current datebin in %edx:%eax

	.p2align 4
	.globl	oz_hw_tod_getnow
	.type	oz_hw_tod_getnow,@function
oz_hw_tod_getnow:
	movl	%cs,%eax	# get caller's mode
	testb	$2,%al
	je	getnow_knl
	int	$INT_GETNOW	# usermode, do it via int
	ret

	.p2align 4
getnow_knl:
	pushl	%edi		# save scratch registers
	pushl	%esi
	pushl	%ebx
	call	getnow		# kernelmode, do it via call
	popl	%ebx		# restore scratch
	popl	%esi
	popl	%edi
	ret

##  Internal routine to handle 'int $INT_GETNOW' that calculates datebin in %edx:5eax

	.p2align 4
	.globl	oz_hwxen_except_getnow
	.type	oz_hwxen_except_getnow,@function
oz_hwxen_except_getnow:
	pushl	%ebp						# finish mchargs on stack
	pushal							# because this is the only iret mechanism we have
	pushl	$0
	xorl	%ebp,%ebp
	movb	$SH_M_EVENTS_MASTER_ENABLE>>24,MCH_W_PAD1+1(%esp) # event delivery *should* be enabled in user code at all times
	call	getnow						# get current datebin into %edx:%eax
	movl	%eax,MCH_L_EAX(%esp)				# write them to return registers
	movl	%edx,MCH_L_EDX(%esp)
	BLOCK_EVENTS						# block events as required by oz_hwxen_iretwithmchargs
	jmp	oz_hwxen_iretwithmchargs			# check for events, softints and ast's now deliverable

##  Calculate current datebin into %edx:%eax

	.p2align 4
getnow:
	movl	xtime_version,%ebx				# get version of private data
	cmpl	oz_hwxen_sharedinfo+sh_time_version2,%ebx	# see if same as current data
	jne	2000f
1000:
	rdtsc							# see how many ticks since xtime_rdtsc
	movb	oz_hwxen_sharedinfo+sh_rdtsc_bitshift,%cl
	shrdl	%cl,%edx,%eax					# we just worry about low 32 bits since we shifted down
	subl	xtime_rdtsc,%eax
	movl	%eax,%ecx					# convert to 100nS units
	mull	tensevenovershcpufreq+0
	movl	%ecx,%eax
	movl	%edx,%ecx
	mull	tensevenovershcpufreq+4
	addl	%ecx,%eax
	adcl	$0,%edx
	addl	xtime_bin+0,%eax				# add base time
	adcl	xtime_bin+4,%edx
	movl	last_bin+0,%esi					# make sure time never marches backward
	movl	last_bin+4,%edi
	subl	%eax,%esi
	sbbl	%edx,%edi
	jnc	1500f
	movl	%eax,last_bin+0
	movl	%edx,last_bin+4
	ret
1500:
	addl	%esi,%eax
	addl	%edi,%edx
	ret

## Base time needs updating first

2000:
	pushl	oz_hwxen_sharedinfo+sh_events_mask
	BLOCK_EVENTS
2100:
	movl	oz_hwxen_sharedinfo+sh_time_version2,%ebx	# no, get new version number
	movl	oz_hwxen_sharedinfo+sh_wc_sec,%esi		# get new data
	movl	oz_hwxen_sharedinfo+sh_wc_usec,%edi
	movl	oz_hwxen_sharedinfo+sh_tsc_timestamp,%ecx
	cmpl	oz_hwxen_sharedinfo+sh_time_version1,%ebx	# see if version stable
	jne	2000b						# repeat if version changed
	movl	%ebx,xtime_version				# save new version number
	movl	%ecx,xtime_rdtsc				# save when it was written
	xorl	%edx,%edx					# add seconds since Jan 1, 1601
	addl	$0xB611E280,%esi
	adcl	$0x00000002,%edx
	movl	$1000000,%eax					# get microseconds since Jan 1, 1601
	mull	%edx
	movl	%eax,%ecx
	movl	$1000000,%eax
	mull	%esi
	addl	%edi,%eax
	adcl	%ecx,%edx
	addl	%eax,%eax					# get 100nS since Jan 1, 1601
	adcl	%edx,%edx
	movl	%eax,%esi
	movl	%edx,%edi
	shldl	$2,%eax,%edx
	shll	$2,%eax
	addl	%esi,%eax
	adcl	%edi,%edx
	movl	%eax,xtime_bin+0				# = time at xtime_rdtsc
	movl	%edx,xtime_bin+4
	popl	oz_hwxen_sharedinfo+sh_events_mask
	jmp	1000b

##  We don't allow setting the current time

	.p2align 4
	.globl	oz_hw_tod_setnow
	.type	oz_hw_tod_setnow,@function
oz_hw_tod_setnow:
	ret

##########################################################################
##									##
##  Wait loop routines - used by oz_knl_thread_wait when there is 	##
##  nothing to do							##
##									##
##########################################################################

	# This is called just before checking to see if we should wait
	# So we block async event delivery and enable softint delivery

	.p2align 4
	.globl	oz_hwxen_waitloop_init
	.type	oz_hwxen_waitloop_init,@function
oz_hwxen_waitloop_init:
	BLOCK_EVENTS
	pushl	$1
	call	oz_hw_cpu_setsoftint
	addl	$4,%esp
	ret

	# There's nothing to do so enable async event delivery and wait for something
	# Softint delivery is already enabled

	.p2align 4
	.globl	oz_hwxen_waitloop_body
	.type	oz_hwxen_waitloop_body,@function
oz_hwxen_waitloop_body:
	pushl	%ebx				# save scratch register
	ALLOW_EVENTS				# we assume yield is smart enough to skip through if an event is pending
	movl	$__HYPERVISOR_sched_op,%eax	# wait for an event
	movl	$SCHEDOP_yield,%ebx
	int	$TRAP_INSTR
	call	oz_hwxen_checkevents		# process any events that queued whilst waiting
	BLOCK_EVENTS				# block async callback for now
	popl	%ebx				# restore scratch register
	ret

	# We're done waiting so inhibit softint delivery and enable async event delivery

	.p2align 4
	.globl	oz_hwxen_waitloop_term
	.type	oz_hwxen_waitloop_term,@function
oz_hwxen_waitloop_term:
	pushl	$0
	call	oz_hw_cpu_setsoftint
	addl	$4,%esp

	##### fall through to oz_hwxen_enablevents #####

##########################################################################
##									##
##  These routines enable event delivery and process any events that 	##
##  became pending whilst event delivery was blocked			##
##									##
##  Calling oz_hwxen_enablevents is analogous to doing 'sti'		##
##									##
##########################################################################

	##### fall through from oz_hwxen_waitloop_term #####

	.p2align 4
	.globl	oz_hwxen_enablevents
	.globl	oz_hwxen_checkevents
	.type	oz_hwxen_enablevents,@function
	.type	oz_hwxen_checkevents,@function
oz_hwxen_enablevents:			# - call this routine to enable delivery and check for pending events
	ALLOW_EVENTS					# set the master enable bit
oz_hwxen_checkevents:			# - call this routine to check for pending events
	movl	oz_hwxen_sharedinfo+sh_events_mask,%eax	# see which events have delivery enabled
	andl	oz_hwxen_sharedinfo+sh_events,%eax	# see which of those are actually pending
	je	1000f					# if none, we're done
	btr	$SH_V_EVENTS_MASTER_ENABLE,oz_hwxen_sharedinfo+sh_events_mask # see if master enable is set and clear it
	jc	2000f
1000:
	ret
2000:
	popl	%eax					# it was set, so process the events
	pushfl
	pushl	%cs
	pushl	%eax

	##### fall through to oz_hwxen_cb_asyncevent #####

##########################################################################
##									##
##  This is the asynchronous event callback				##
##									##
##  It calls oz_hwxen_do_asyncevent with pointer to mchargs		##
##    MCH_L_EC2 = bits that were set in sh_events			##
##									##
##  This routine assumes SH_M_EVENTS_MASTER_ENABLE was set prior to 	##
##  calling but is now clear.  It will be enabled upon return.		##
##									##
##  Also, the Xen hypervisor clears the sh_events_mask bits that it is 	##
##  delivering the events for, so we just assume that the whole thing 	##
##  is totally wasted.  So we restore it from the CPU's smplevel.	##
##									##
##########################################################################

	##### fall through from oz_hwxen_checkevents #####

	.p2align 4
	.globl	oz_hwxen_cb_asyncevent
	.type	oz_hwxen_cb_asyncevent,@function
oz_hwxen_cb_asyncevent:
	cmpl	$cb_asy_crithit_1,(%esp)		# see if we hit critical iret
	jnc	cb_asy_crithitck
cb_asy_crithit__NOT:
	orb	$SH_M_EVENTS_MASTER_ENABLE>>24,7(%esp)	# set MCH_W_PAD1+1<7> to indicate event delivery was enabled
cb_asy_crithit_ent_3:
	pushl	%ebp					# save what goes just below MCH_L_EIP
	movl	%esp,%ebp				# make standard call frame
cb_asy_crithit_ent_2:
	pushal						# push rest of mchargs
cb_asy_crithit_ent_1:
	call	oz_hwxen_geteventsmask			# repair events mask from smplevel and get into %eax
	andl	oz_hwxen_sharedinfo+sh_events,%eax	# see if any enabled are pending
	je	cb_asy_crithit_out			# just return if none
cb_asy_crithit_loop:
	pushl	%eax					# ok, save one's we're doing as MCH_L_EC2
	notl	%eax					# clear the ones we're about to do
	lock
	andl	%eax,oz_hwxen_sharedinfo+sh_events
	pushl	%esp					# push mchargs pointer
	call	oz_hwxen_do_asyncevent			# process asynchronous event
	addl	$4,%esp

##########################################################################
##
##  About to do an iret with mchargs on stack.  Check for pending softints or ast's before returning.
##  We also enable async event delivery before returning and check for pending events.
##  At this point, though, event interrupt delivery is inhibited.
##
	.p2align 4
	.globl	oz_hwxen_iretwithmchargs
	.type	oz_hwxen_iretwithmchargs,@function
oz_hwxen_iretwithmchargs:
	cmpb	$0,oz_hwxen_cpudb+CPU_B_SMPLEVEL	# see if softints enabled
	jne	iretmchargs_rtn				# if not, can't deliver anything
	cmpb	$0,oz_hwxen_softintpend			# see if softint pending
	jne	oz_hwxen_procsoftint_doit		# if so, go back to process it
	pushl	%esp					# push pointer to mchargs
	pushl	$0					# we're always cpu #0
	call	oz_knl_thread_checkknlastq		# process kernel mode ast's
	addl	$8,%esp					# pop cpu index and mchargs pointer
	testb	$2,MCH_W_CS(%esp)			# see if returning to user mode
	je	iretmchargs_rtn				# if not, just return
	cmpl	$iretmchargs_userasts_loop,MCH_L_EIP(%esp) # see if caller is about to do user ast's anyway
	je	iretmchargs_rtn				# if so, don't keep pounding on them (and running out of stack!)
	pushl	oz_PROCMODE_USR				# we are about to return to user mode
	call	oz_knl_thread_chkpendast		# ... see if there will be any deliverable ast's for that mode
	addl	$4,%esp
	testl	%eax,%eax
	jne	iretmchargs_userasts			# if so, go process them
iretmchargs_rtn:
	addl	$4,%esp					# pop MCH_L_EC2
	testb	$SH_M_EVENTS_MASTER_ENABLE>>24,MCH_W_PAD1+1-4(%esp) # see if event delivery to be enabled on return
	je	cb_asy_crithit_out			# if not, just return as is
	ALLOW_EVENTS					# tell Xen it's OK to call oz_hwxen_cb_asyncevent for new events
cb_asy_crithit_1:
	movl	oz_hwxen_sharedinfo+sh_events_mask,%eax	# see which events have delivery enabled
	andl	oz_hwxen_sharedinfo+sh_events,%eax	# see which of those are actually pending
	je	cb_asy_crithit_out			# if none, we're all done
	BLOCK_EVENTS
	jmp	cb_asy_crithit_loop			# if found, repeat
cb_asy_crithit_out:
	popal
cb_asy_crithit_2:
	popl	%ebp
cb_asy_crithit_3:
	iret
##
##  Check for redundant entry at the critical points
##
cb_asy_crithitck:
	cmpw	$FLAT_RING1_CS,4(%esp)			# make sure no monkey business
	jne	cb_asy_crithit__NOT
	cmpl	$cb_asy_crithit_3,(%esp)		# see if interrupted after crit region
	ja	cb_asy_crithit__NOT			# if so, it's not critical
	je	cb_asy_crithit_at_3			# it hit just before the iret
	cmpl	$cb_asy_crithit_2,(%esp)
	je	cb_asy_crithit_at_2			# it hit just before the popl %ebp
	addl	$12,%esp				# somewhere before popal, wipe redundant exception frame
	jmp	cb_asy_crithit_ent_1			# now we're on original exception frame with everything pushed
cb_asy_crithit_at_2:
	addl	$12,%esp				# wipe redundant exception frame
	jmp	cb_asy_crithit_ent_2			# now we're on original exception frame with ebp pushed
cb_asy_crithit_at_3:
	addl	$12,%esp				# wipe redundant exception frame
	jmp	cb_asy_crithit_ent_3			# now we're on original exception frame with nothing pushed
##
##  There are deliverable usermode ast's
##
iretmchargs_userasts:
	ALLOW_EVENTS					# deliverable usermode ast's, enable events so we can access user stack
							# - if other ast's get queued to us, well we're about to do them anyway
							# - if the ast's get cancelled or delivered on us, 
							#   oz_sys_thread_checkast will just have nothing to do
	xorl	%eax,%eax				# no additional longs to copy to user stack, just copy the mchargs
	call	oz_hwxen_movemchargstocallerstack
iretmchargs_userasts_loop:
	cmpl	oz_SUCCESS,%eax				# hopefully it copied ok
	jne	iretmchargs_baduserstack
	pushl	%esp					# we're in user mode now, push pointer to mchargs
	call	oz_sys_thread_checkast			# process any user mode ast's that may be queued
	addl	$8,%esp					# (the iret below will be from user mode to user mode)
	popal
	popl	%ebp
	iret
iretmchargs_baduserstack:
	pushl	%eax					# no user stack to push mchargs for ast
	pushl	MCH_L_XSP+4(%esp)
	pushl	%eax
	pushl	$iretmchargs_msg1
	call	oz_knl_printk
	addl	$12,%esp
iretmchargs_baduserexit:
	call	oz_knl_thread_exit			# exit the thread
	jmp	iretmchargs_baduserexit

iretmchargs_msg1:	.string	"oz_hwxen_uniproc: error %u pushing to user stack pointer %p\n"

##########################################################################
##									##
##  Calculate new sh_events_mask from the CPU's smplevel		##
##  Also return the new sh_events_mask in %eax				##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwxen_geteventsmask
	.type	oz_hwxen_geteventsmask,@function
oz_hwxen_geteventsmask:
	movb	oz_hwxen_cpudb+CPU_B_SMPLEVEL,%cl	# get current smplevel
	xorl	%eax,%eax				# assume all events are blocked
	cmpb	$OZ_SMPLOCK_LEVEL_EVENTS+30,%cl		# see if all events are blocked
	jnc	geteventsmask_merge
	movl	$SH_M_EVENTS_MASTER_ENABLE-1,%eax	# assume all events are enabled
	subb	$OZ_SMPLOCK_LEVEL_EVENTS-1,%cl		# see if all events are enabled
	jbe	geteventsmask_merge
	shll	%cl,%eax				# just some are blocked
	btr	$SH_V_EVENTS_MASTER_ENABLE,%eax
geteventsmask_merge:
	movl	oz_hwxen_sharedinfo+sh_events_mask,%ecx	# get current mask
	andl	$SH_M_EVENTS_MASTER_ENABLE,%ecx		# preserve master enable, clear the rest
	orl	%ecx,%eax				# set bits according to current smplevel
	movl	%eax,oz_hwxen_sharedinfo+sh_events_mask	# store new value back
	ret						# return value in %eax

##########################################################################
##									##
##  This is the failsafe callback					##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwxen_cb_failsafe
	.type	oz_hwxen_cb_failsafe,@function
oz_hwxen_cb_failsafe:
	pushl	$1000f
	call	oz_crash

1000:	.string	"oz_hwxen_cb_failsafe: %8.8X %8.8X %8.8X"

##########################################################################
##									##
##  For us, iota and system time are the same				##
##									##
##########################################################################

	.globl	oz_hw_tod_iotanow
	.type	oz_hw_tod_iotanow,@function
oz_hw_tod_iotanow:
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	call	getnow
	popl	%ebx
	popl	%esi
	popl	%edi
	ret

	.globl	oz_hw_tod_dsys2iota
	.type	oz_hw_tod_dsys2iota,@function
oz_hw_tod_dsys2iota:
	movl	4(%esp),%eax
	movl	8(%esp),%edx
	ret

	.globl	oz_hw_tod_diota2sys
	.type	oz_hw_tod_diota2sys,@function
oz_hw_tod_diota2sys:
	movl	4(%esp),%eax
	movl	8(%esp),%edx
	ret

	.globl	oz_hw486_aiota2sys
	.type	oz_hw486_aiota2sys,@function
oz_hw486_aiota2sys:
	ret

	.globl	oz_hw_pte_print
	.type	oz_hw_pte_print,@function
oz_hw_pte_print:
	ret

	.globl	oz_dev_vgavideo_blank
	.type	oz_dev_vgavideo_blank,@function
oz_dev_vgavideo_blank:
	ret

	.globl	oz_hw_debug_writebpt
	.type	oz_hw_debug_writebpt,@function
oz_hw_debug_writebpt:
	ret

	.globl	oz_hw_mchargx_fetch
	.type	oz_hw_mchargx_fetch,@function
oz_hw_mchargx_fetch:
	ret

	.globl	oz_hw_mchargx_store
	.type	oz_hw_mchargx_store,@function
oz_hw_mchargx_store:
	ret

	.globl	oz_hw_stl_microwait
	.type	oz_hw_stl_microwait,@function
oz_hw_stl_microwait:
	xorl	%eax,%eax
	ret

	.globl	oz_knl_console_debugchk
	.type	oz_knl_console_debugchk,@function
oz_knl_console_debugchk:
	xorl	%eax,%eax
	ret

	.globl	oz_sys_printkp
	.type	oz_sys_printkp,@function
oz_sys_printkp:
	ret

