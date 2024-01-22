##+++2004-08-01
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
##---2004-08-01

##########################################################################
##									##
##  Scheduled threads							##
##									##
##########################################################################

	.include "oz_params_xen.s"

##########################################################################
##									##
##  Initialize thread hardware context block				##
##									##
##    Input:								##
##									##
##	 4(%esp) = pointer to hardware context block			##
##	 8(%esp) = number of user stack pages				##
##	           0 if executive-mode only thread			##
##	12(%esp) = thread routine entrypoint				##
##	           0 if initializing cpu as a thread for the first time	##
##	16(%esp) = thread routine parameter				##
##	20(%esp) = process hardware context block pointer		##
##	24(%esp) = thread software context block pointer		##
##									##
##	smplock  = softint						##
##									##
##    Output:								##
##									##
##	thread hardware context block = initialized so that call to 	##
##	oz_hw_thread_switchctx will set up to call the thread routine	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_initctx
	.type	oz_hw_thread_initctx,@function
oz_hw_thread_initctx:
	pushl	%ebp					# make call frame
	movl	%esp,%ebp
	pushl	%edi					# save C registers
	pushl	%esi
	pushl	%ebx

##  Initialize hardware context area to zeroes

	xorl	%eax,%eax
	movl	$THCTX__SIZE/4,%ecx
	movl	8(%ebp),%edi
	cld
	rep
	stosl
	movl	8(%ebp),%edi				# point to hardware context area

##  If we're initializing this cpu as a thread, set it as current on the cpu and skip the rest

	cmpl	$0,16(%ebp)				# see if initializing cpu for the first time
	jne	thread_initctx_other
	call	oz_hw486_getcpu				# ok, get cpudb pointer
	movl	%edi,CPU_L_THCTX(%esi)			# save current thread hwctx pointer
	jmp	thread_initctx_rtnsuc
thread_initctx_other:

##  Set up user stack parameters - stack gets created when thread starts

	movl	$OZ_IMAGE_BASEADDR-1,%edx		# set up user stack page number
	movl	12(%ebp),%eax				# save number of user stack pages
	shrl	$12,%edx
	movl	%eax,THCTX_L_USTKSIZ(%edi)
	movl	%edx,THCTX_L_USTKVPG(%edi)

##  Allocate an executive stack

	leal	THCTX_L_ESTACKVA(%edi),%eax		# set up executive stack memory
	pushl	%eax
	pushl	%edi
	call	oz_hw486_kstack_create
	addl	$8,%esp
	cmpl	oz_SUCCESS,%eax
	jne	thread_initctx_rtn
	movl	THCTX_L_ESTACKVA(%edi),%eax

##  Set up what we want to happen when the 'RET' of the oz_hw_thread_switchctx routine executes

	leal	-THSAV__SIZE-20(%eax),%esi		# make room for 11 longs on new stack
	movl	$0x1202,THSAV_L_EFL(%esi)		# set eflags = interrupts enabled; IOPL=1
	movl	$0,THSAV_L_EBP(%esi)			# saved ebp = zeroes
	movl	$thread_init,THSAV_L_EIP(%esi)		# return address = thread_init routine
	movl	16(%ebp),%eax				# thread routine entrypoint
	movl	20(%ebp),%ebx				# thread routine parameter
	movl	24(%ebp),%ecx				# process hardware context
	movl	28(%ebp),%edx				# thread software context
	movl	%edx,THSAV__SIZE+ 0(%esi)		# thread software context
	movl	%edi,THSAV__SIZE+ 4(%esi)		# thread hardware context pointer
	movl	%eax,THSAV__SIZE+ 8(%esi)		# thread routine entrypoint
	movl	%ebx,THSAV__SIZE+12(%esi)		# thread routine parameter
	movl	%ecx,THSAV__SIZE+16(%esi)		# process hardware context pointer
	movl	%esi,THCTX_L_EXESP(%edi)		# save initial stack pointer
thread_initctx_rtnsuc:
	movl	oz_SUCCESS,%eax				# set up success status
thread_initctx_rtn:
	popl	%ebx					# pop call frame
	popl	%esi
	popl	%edi
	leave
	ret

##########################################################################
##									##
##  The oz_hw_thread_switchctx routine just executed its RET		##
##  We are now in the target thread's context				##
##									##
##    Input:								##
##									##
##	 0(%esp) = thread software context block pointer		##
##	 4(%esp) = thread hardware context block pointer		##
##	 8(%esp) = thread routine entrypoint				##
##	12(%esp) = thread routine parameter				##
##	16(%esp) = process hardware context block pointer		##
##	smplevel = (see oz_knl_thread_start)				##
##									##
##########################################################################

	.align	4
thread_init:

##  Start thread

	call	oz_knl_thread_start		# do executive mode thread startup
						# comes back at smplock_null level
	addl	$4,%esp				# pop thread software context block pointer
  ##	pushl	12(%esp)			# maybe the pagetable needs to be mapped
  ##	call	oz_hw_process_firsthread
  ##	addl	$4,%esp
	popl	%esi				# point to thread hardware context block pointer
	movl	THCTX_L_USTKSIZ(%esi),%ecx	# see if any user stack requested
	jecxz	thread_init_executive		# if not, it executes in executive mode
	leal	THCTX_L_USTKVPG(%esi),%edx	# point to where to return base virtual page number
	pushl	%edx				# create the stack section and map it
	pushl	%ecx
	call	oz_hw486_ustack_create
	addl	$8,%esp
	popl	%edi				# get user mode thread routine entrypoint
	popl	%edx				# get user mode thread routine parameter
	addl	$4,%esp				# get rid of process hw ctx

	pushl	$FLAT_RING3_DS			# set up operands on executive stack for IRET instruction
	pushl	%eax				# - its user mode stack pointer
	pushl	$0x200				# - its EFLAGS = interrupts enabled
	pushl	$FLAT_RING3_CS			# - its code segment
	pushl	$thread_init_either		# - its instruction pointer

	iret					# iret with 32-bit operands
						# the thread's executive stack should now be completely empty
						# - ready to accept interrupts, syscalls, pagefaults, etc
						# - the oz_hw_thread_switchctx routine set up tss' esp for this stack

##########################################################################
##									##
##  This is the thread for threads that run in executive mode		##
##  Softint delivery is enabled						##
##									##
##########################################################################

	.align	4
thread_init_executive:
	popl	%edi				# get routine's entrypoint
	popl	%edx				# get routine's parameter
	popl	%eax				# pop and ignore process hw ctx pointer

##########################################################################
##									##
##  We are now in user mode for the first time				##
##  (This routine is also used for executive mode threads)		##
##									##
##    Input:								##
##									##
##	edi = thread entrypoint						##
##	edx = thread parameter						##
##	esp = very top of the stack					##
##									##
##########################################################################

thread_init_either:
	pushl	$0				# make a null call frame
						# the above push is the thread's first user-mode pagefault
	pushl	$0
	movl	%esp,%ebp			# point to the null stack frame

	pushl	%edx				# push the parameter for the thread routine
	call	*%edi				# call the thread routine

	pushl	%eax				# force call to oz_sys_thread_exit
	call	oz_sys_thread_exit
	pushl	%eax
	pushl	$thread_init_msg1
	call	oz_crash

thread_init_msg1:	.string	"oz_hw_thread_initctx: returned from oz_sys_thread_exit with status %u"

##########################################################################
##									##
##  Determine how much room is left on current kernel stack		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_kstackleft
	.type	oz_hw_thread_kstackleft,@function
oz_hw_thread_kstackleft:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp

	call	oz_knl_thread_getcur		# get pointer to current thread struct
	pushl	%eax
	call	oz_knl_thread_gethwctx		# get corresponding hardware context

	movl	THCTX_L_ESTACKVA(%eax),%edx	# get the top of the stack
	movl	%esp,%eax			# get current stack pointer
	subl	$ESTACKSIZE,%edx		# calc the bottom of the stack
	subl	%edx,%eax			# calc how much room is left

	leave
	ret

##########################################################################
##									##
##  The thread has changed user stacks					##
##									##
##  This unmaps the old user stack section and sets up the new one to 	##
##  be unmapped when the thread exits.					##
##									##
##########################################################################

	.align	4
	.globl	oz_sys_thread_newusrstk
	.type	oz_sys_thread_newusrstk,@function
oz_sys_thread_newusrstk:
	pushl	%ebp
	pushl	%edi			# save scratch register
	int	$INT_NEWUSRSTK		# do the dirty work in exec mode
	popl	%edi			# restore scratch register
	popl	%ebp
	ret

##  0(%esp) = return address (just past the int instruction)
##  4(%esp) = caller's code segment
##  8(%esp) = caller's eflags
## 12(%esp) = caller's stack pointer
## 16(%esp) = caller's stack segment

	.p2align 4
	.globl	oz_hwxen_thread_newusrstk
	.type	oz_hwxen_thread_newusrstk,@function
oz_hwxen_thread_newusrstk:
	pushl	$0			# make a stack frame again, but terminate chain here
					# - because we don't want an exception handler searching user stack frames
	movl	%esp,%ebp		# now point to my kernel stack frame

	call	oz_knl_thread_getcur	# get pointer to current thread struct
	pushl	%eax
	call	oz_knl_thread_gethwctx	# get corresponding hardware context
	movl	%eax,%edi

	pushl	THCTX_L_USTKVPG(%edi)	# unmap the old section
	call	oz_hw486_ustack_delete

	movl	16(%ebp),%eax		# get caller's (user-mode) stack pointer
	shrl	$12,%eax
	movl	%eax,THCTX_L_USTKVPG(%edi)

	movl	oz_SUCCESS,%eax		# always successful
	leave
	iret

##########################################################################
##									##
##  Terminate as much as possible about the thread while still in its 	##
##  context								##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to thread hardware context block		##
##									##
##	smplock = softint delivery inhibited				##
##									##
##    Output:								##
##									##
##	everything possible cleared out while in thread context		##
##									##
##########################################################################

	.align	4
	.global	oz_hw_thread_exited
	.type	oz_hw_thread_exited,@function
oz_hw_thread_exited:
	pushl	%ebp
	movl	%esp,%ebp

## Unmap and delete the user stack section, oz_hw_thread_termctx deletes executive stack

	movl	8(%ebp),%ecx			# point to hardware context block
	cmpl	$0,THCTX_L_USTKSIZ(%ecx)	# see if a user stack was created
	je	thread_exited_rtn		# all done if there wasn't one created
	pushl	THCTX_L_USTKVPG(%ecx)		# ok, unmap the section
	movl	$0,THCTX_L_USTKSIZ(%ecx)
	movl	$0,THCTX_L_USTKVPG(%ecx)
	call	oz_hw486_ustack_delete
thread_exited_rtn:

	leave
	ret

##########################################################################
##									##
##  Terminate thread hardware context					##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to thread hardware context block		##
##	8(%esp) = pointer to process hardware context block		##
##	smplevel <= ts							##
##									##
##    Output:								##
##									##
##	@4(%esp) = voided out						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_termctx
	.type	oz_hw_thread_termctx,@function
oz_hw_thread_termctx:
	pushl	%ebp
	movl	%esp,%ebp

##  Free memory used for its executive stack
##  If ESTACKVA is zero, it means this is the cpu's initial thread (?? should we free it to non-paged pool ??)

	movl	8(%ebp),%edx			# point to thread's hw context block
	movl	THCTX_L_ESTACKVA(%edx),%ecx	# get top address of executive stack
	jecxz	thread_termctx_rtn
	movl	$0,THCTX_L_ESTACKVA(%edx)	# clear it out now that we're freeing it (paranoia)
	pushl	%ecx				# free off the memory pages and the spte's
	pushl	%edx
	call	oz_hw486_kstack_delete
thread_termctx_rtn:

	leave
	ret

##########################################################################
##									##
##  Switch thread hardware context					##
##									##
##	 4(%esp) = old thread hardware context block address		##
##	 8(%esp) = new thread hardware context block address		##
##	smplevel = ts							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_switchctx
	.type	oz_hw_thread_switchctx,@function
oz_hw_thread_switchctx:

	# save general registers
	# if you change the order pushed below, you must change THSAV_... symbols

	pushl	%ebp				# make a stack frame
	movl	%esp,%ebp
	pushfl					# save eflags (in case IOPL is different)
	pushl	%edi				# save C registers
	pushl	%esi
	pushl	%ebx

	call	oz_hw486_getcpu			# this is safe to do because we are at or above softint

	# %esp now points to old THSAV_...

	movl	 8(%ebp),%edi			# get old thread's hardware context block
	movl	12(%ebp),%ebx			# get new thread's hardware context block

	# save fpu/mmx context

	btr	$0,THCTX_B_FPUSED(%edi)		# see if old thread accessed its fp regs this last time around
	movl	%edi,%eax
	jc	thread_switchctx_fpsave		# if so, go save them and reset FPU
thread_switchctx_nofpsave:

	# save debug registers

	testb	$1,THCTX_B_DBUSED(%edi)		# see if this thread is using them
	jne	thread_switchctx_dbsave		# since use is extremely rare, do it out-of-line
thread_switchctx_nodbsave:

	# switch stacks

	movl	%esp,THCTX_L_EXESP(%edi)	# save current executive stack pointer
	movl	THCTX_L_EXESP(%ebx),%esp	# we're now on the new thread's executive stack

	# %esp now points to new THSAV_...

	cmpl	%edi,CPU_L_THCTX(%esi)		# make sure old pointer is ok
	jne	thread_switchctx_crash1
	movl	%ebx,CPU_L_THCTX(%esi)		# save new thread context pointer

	movl	$0x96696996,THCTX_L_EXESP(%ebx)	# wipe it until we save it again

	# restore debug registers

	testb	$1,THCTX_B_DBUSED(%ebx)		# see if new thread uses debug registers
	jne	thread_switchctx_dbload		# since it is extremely rare, do it out-of-line
thread_switchctx_nodbload:

	# restore general registers
	# if you change the order popped below, you must change THSAV_... symbols

	movl	THCTX_L_ESTACKVA(%ebx),%ecx	# set up the esp that will be used when switching from user to executive mode
	movl	%ss,%ebx
	movl	$__HYPERVISOR_stack_switch,%eax
	int	$0x82

	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	popfl					# restore eflags (in case IOPL is different)
						# (only works if running in true kernel mode)
	popl	%ebp				# restore new thread's frame pointer
	ret					# return to new thread

	# save fpu/mmx registers and reset fpu
	# we don't explicitly restore them, we just clear the TS bit, causing an exception when the next access occurs

	.align	4
thread_switchctx_fpsave:
	andb	$0xF0,%al			# fxsave requires 16-byte alignment
fnsave_pat:			# fnsave/nop gets patched to fxsave if cpu is mmx
	fnsave	THCTX_X_FPSAVE+16(%eax)		# if so, save fp registers and re-init fpu (TS bit should be clear)
	nop
	movl	$__HYPERVISOR_fpu_taskswitch,%eax # set the TS bit to inhibit access to fpu
	int	$TRAP_INSTR
	jmp	thread_switchctx_nofpsave

	# save debug registers and disable them

	.align	4
thread_switchctx_dbsave:
	movl	%dr0,%eax			# save contents as they are
	movl	%dr1,%ecx
	movl	%dr2,%edx
	movl	%eax,THCTX_L_DR0(%edi)
	movl	%ecx,THCTX_L_DR1(%edi)
	movl	%edx,THCTX_L_DR2(%edi)
	movl	%dr7,%edx
	movl	%dr3,%ecx
	movzwl	%dx,%eax
	movl	%ecx,THCTX_L_DR3(%edi)
	andw	$0xFC00,%ax
	movl	%edx,THCTX_L_DR7(%edi)
	xorl	%edx,%edx
	movl	%eax,%dr7			# clear all but reserved bits of dr7
	movl	%edx,%dr0			# clear dr0..dr3 in case new thread tries to read them
	movl	%edx,%dr1
	movl	%edx,%dr2
	movl	%edx,%dr3
	jmp	thread_switchctx_nodbsave

	# restore debug registers

	.align	4
thread_switchctx_dbload:
	movl	THCTX_L_DR0(%edi),%eax
	movl	THCTX_L_DR1(%edi),%ecx
	movl	THCTX_L_DR2(%edi),%edx
	movl	%eax,%dr0
	movl	%ecx,%dr1
	movl	%edx,%dr2
	movl	THCTX_L_DR3(%edi),%eax
	movl	THCTX_L_DR7(%edi),%edx
	movl	%eax,%dr3
	movl	%edx,%dr7
	jmp	thread_switchctx_nodbload

	# crashes

thread_switchctx_crash1:
	pushl	%ebx
	pushl	%edi
	pushl	CPU_L_THCTX(%esi)
	pushl	$thread_switchctx_msg1
	call	oz_crash

thread_switchctx_msg1:	.string	"oz_hw_thread_switchctx: old CPU_L_THCTX %p, old hwctx %p, new hwctx %p"

##########################################################################
##									##
##  Set thread ast state						##
##									##
##  It's a nop for us because we poll on return to outer modes		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_aststate
	.type	oz_hw_thread_aststate,@function
oz_hw_thread_aststate:
	ret

##########################################################################
##									##
##  Thread Trace Dump routine						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to hwctx block				##
##	smplock = ts							##
##	threadstate not currently loaded in any cpu			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_tracedump
	.type	oz_hw_thread_tracedump,@function
oz_hw_thread_tracedump:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx

.if THSAV_L_EIP<>THSAV_L_EBP+4
	error : code assumes THSAV_L_EIP comes right after THSAV_L_EBP
.endif
	movl	8(%ebp),%ebx			# point to THCTX block
	movl	THCTX_L_EXESP(%ebx),%eax	# get thread's stack pointer
	leal	THSAV_L_EBP(%eax),%ebx		# get thread's frame pointer
	pushl	%ebx				# print stack and frame pointer
	pushl	%eax
	pushl	$thread_tracedump_msg1
	call	oz_knl_printk
	addl	$12,%esp
thread_tracedump_loop:
	pushl	$1				# 8 bytes at its ebp should be writable
	pushl	$0
	pushl	%ebx
	pushl	$8
	call	oz_hw_probe
	addl	$16,%esp
	testl	%eax,%eax
	je	thread_tracedump_done		# if not, we're all done
	pushl	%ebx				# ok, print out the frame
	pushl	 (%ebx)
	pushl	4(%ebx)
	pushl	$thread_tracedump_msg2
	call	oz_knl_printk
	pushl	4(%ebx)
	call	oz_knl_printkaddr
	addl	$20,%esp
	movl	(%ebx),%ebx			# check out next frame
	jmp	thread_tracedump_loop
thread_tracedump_done:
	pushl	$thread_tracedump_msg3		# all done
	call	oz_knl_printk
	addl	$4,%esp

	popl	%ebx
	leave
	ret

thread_tracedump_msg1:	.string	"oz_hw_thread_tracedump: esp %X, ebp %X"
thread_tracedump_msg2:	.string	"\n   %8.8X  %8.8X : %8.8X : "
thread_tracedump_msg3:	.string	"\n"
