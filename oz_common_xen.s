##+++2003-11-18
##    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
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
##---2003-11-18

##########################################################################
##									##
##  Routines common to the loader and the kernel			##
##									##
##########################################################################

	.include "oz_params_xen.s"

## This defines the address of the oz_sys_pdata_array for processes
## It puts the array just below the reqprot/pagetable pages
## oz_hw_process_486.c sets them up this way

##   oz_sys_pdata_array[0] = data for kernel mode
##   oz_sys_pdata_array[1] = data for user mode

## The system process and loader double-map the oz_s_systempdata page here

	.data
	.p2align 12
	.globl	oz_s_systempdata	# system process' OZ_Pdata struct
	.type	oz_s_systempdata,@object
oz_s_systempdata:	.space	4096	# - mapped here always 
					# - also mapped at same address as per-process oz_sys_pdata 
					#   kernel struct when the system process is current on the cpu

	.text


##########################################################################
##									##
##  Fix the current ebp given machine args on the stack			##
##									##
##    Input:								##
##									##
##	4(%esp) = machine arguments					##
##									##
##    Output:								##
##									##
##	ebp = fixed							##
##									##
##    Scratch:								##
##									##
##	nothing								##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_fixkmexceptebp
	.type	oz_hw_fixkmexceptebp,@function
oz_hw_fixkmexceptebp:
	xorl	%ebp,%ebp			# assume previous outer mode, break call frame chain
	testb	$2,4+MCH_W_CS(%esp)		# see if previous exec mode
	jne	fixkmexceptebp_wasntknl
	leal	4+MCH_L_EBP(%esp),%ebp		# if so, keep the call frame chain intact
fixkmexceptebp_wasntknl:
	ret

##########################################################################
##									##
##  Fix the esp saved in the kernel machine arguments			##
##									##
##    Input:								##
##									##
##	4(%esp) = machine arguments					##
##									##
##    Output:								##
##									##
##	4+MCH_L_ESP(%esp) = fixed					##
##									##
##    Scratch:								##
##									##
##	%edx								##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_fixkmmchargesp
	.type	oz_hw_fixkmmchargesp,@function
oz_hw_fixkmmchargesp:
	leal	4+MCH_L_XSP(%esp),%edx		# this is where exec stack pointer was before exception
	testb	$2,4+MCH_W_CS(%esp)		# see if previous exec mode
	jne	fixkmmchargesp_outer
	movl	%edx,4+MCH_L_ESP(%esp)		# save stack pointer at time of exception
	ret
fixkmmchargesp_outer:
	movl	(%edx),%edx			# if not, get outer mode stack pointer
	movl	%edx,4+MCH_L_ESP(%esp)
	ret

##########################################################################
##									##
##  Routines we need to provide to the C stuff in loader and kernel	##
##									##
##########################################################################

##########################################################################
##									##
##  Get return address of a particular frame				##
##									##
##    Input:								##
##									##
##	4(%esp) = frame index						##
##	          0 returns the rtn address of call to oz_hw_getrtnadr	##
##									##
##    Output:								##
##									##
##	%eax = return address of that frame, 0 if no more frames	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_getrtnadr
	.type	oz_hw_getrtnadr,@function
oz_hw_getrtnadr:
	pushl	%ebp			# make a stack frame
	movl	%esp,%ebp
	movl	8(%ebp),%ecx		# get frame count
	movl	%ebp,%eax		# start with my return address as frame zero
	jecxz	getrtnadr_done		# all done if frame count is zero
getrtnadr_loop:
	movl	(%eax),%eax		# non-zero param, link up a frame
	testl	%eax,%eax		# stop if ran out of frames
	loopnz	getrtnadr_loop		# more frames, see if count still says go up more
	je	getrtnadr_exit
getrtnadr_done:
	movl	4(%eax),%eax		# get return address in that frame
getrtnadr_exit:
	leave
	ret

##########################################################################
##									##
##  Increment a long, even in an SMP					##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = new value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_inc_long
	.type	oz_hw_atomic_inc_long,@function
oz_hw_atomic_inc_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	8(%esp),%eax			# get value to be added to memory location
	lock					# lock others out
	xaddl	%eax,(%edx)			# add %eax to (%edi), get old (%edi) value in %eax
	addl	8(%esp),%eax			# get new value in %eax
	ret

##########################################################################
##									##
##  OR a long, even in an SMP						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = old value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_or_long
	.type	oz_hw_atomic_or_long,@function
oz_hw_atomic_or_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	(%edx),%eax			# get old value
atomic_or_loop:
	movl	8(%esp),%ecx			# get mask to or in
	orl	%eax,%ecx			# or the old and new values
	lock					# lock others out
	cmpxchgl %ecx,(%edx)			# if (%edx) == %eax, then store %ecx in (%edx)
	jne	atomic_or_loop			# repeat if failure
	ret

##########################################################################
##									##
##  AND a long, even in an SMP						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = old value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_and_long
	.type	oz_hw_atomic_and_long,@function
oz_hw_atomic_and_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	(%edx),%eax			# get old value
atomic_and_loop:
	movl	8(%esp),%ecx			# get mask to and in
	andl	%eax,%ecx			# and the old and new values
	lock					# lock others out
	cmpxchgl %ecx,(%edx)			# if (%edx) == %eax, then store %ecx in (%edx)
	jne	atomic_and_loop			# repeat if failure
	ret

##########################################################################
##									##
##  Set a long, even in an SMP						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = old value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_set_long
	.type	oz_hw_atomic_set_long,@function
oz_hw_atomic_set_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	8(%esp),%eax			# get value to be stored in memory location
	lock					# lock others out
	xchgl	%eax,(%edx)			# store %eax in (%edi), get old (%edi) value in %eax
	ret

##########################################################################
##									##
##  Set a long conditionally, even in an SMP				##
##									##
##    Input:								##
##									##
##	 4(%esp) = pointer to longword					##
##	 8(%esp) = new value						##
##	12(%esp) = old value						##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified iff it was 12(%esp)				##
##	%eax = 0 : it was different than 12(%esp) so it was not set	##
##	       1 : it was the same as 12(%esp) so it was set to 8(%esp)	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_setif_long
	.globl	oz_hw_atomic_setif_ptr
	.type	oz_hw_atomic_setif_long,@function
	.type	oz_hw_atomic_setif_ptr,@function
oz_hw_atomic_setif_long:
oz_hw_atomic_setif_ptr:
	movl	 4(%esp),%edx			# point to destination memory location
	movl	 8(%esp),%ecx			# get value to be stored in memory location
	movl	12(%esp),%eax			# get expected old contents of memory location
	lock					# lock others out
	cmpxchgl %ecx,(%edx)			# if (%edx) == %eax, then store %ecx in (%edx)
	setz	%al				# set %al 0 if it failed, 1 if it succeeded
	movzbl	%al,%eax			# clear uppper bits of %eax
	ret

##########################################################################
##									##
##  Get a page that can be mapped to a device's registers		##
##									##
##    Output:								##
##									##
##	%eax = physical page number that isn't mapped to anything	##
##									##
##########################################################################

	.data
	.align	4
last_used_page:	.long	0

	.text
	.align	4
	.globl	oz_hw_get_iopage
	.type	oz_hw_get_iopage,@function
oz_hw_get_iopage:
	pushl	%ebp
	movl	%esp,%ebp
get_unused_iopage_loop:
	movl	last_used_page,%eax		# get old value
	leal	1(%eax),%ecx			# get new value in ecx
	testl	%eax,%eax
	jne	get_unused_iopage_ok
	movl	oz_s_phymem_totalpages,%ecx
get_unused_iopage_ok:
	lock					# lock others out
	cmpxchgl %ecx,last_used_page		# if last_used_page == %eax, then store %ecx in last_used_page
	jne	get_unused_iopage_loop		# repeat if failure
	movl	%ecx,%eax			# return allocate page number
	leave
	ret

##  Check to see if hardware interrupt delivery is inhibited, crash if not

	.align	4
	.globl	oz_hw_cpu_chkhwinhib
	.type	oz_hw_cpu_chkhwinhib,@function
oz_hw_cpu_chkhwinhib:
	pushl	%ebp
	movl	%esp,%ebp
	pushfl
	cli
	bt	$9,(%esp)
	jc	chkhwinhib_crash1
	leave
	ret
chkhwinhib_crash1:
	movl	8(%ebp),%edx
chkhwinhib_hang:
	jmp	chkhwinhib_hang

chkhwinhib_msg1:	.string	"oz_hw_cpu_chkhwinhib: eflags %8.8X"

##########################################################################
##									##
##  Call a subroutine given a pointer to its arg list			##
##									##
##    Input:								##
##									##
##	 4(%esp) = entrypoint						##
##	 8(%esp) = number of arguments (longwords in this case)		##
##	12(%esp) = pointer to arg list					##
##									##
##    Output:								##
##									##
##	as defined by subroutine being called				##
##									##
##########################################################################

	.align	4
	.globl	oz_sys_call
	.type	oz_sys_call,@function
oz_sys_call:
	pushl	%ebp		# make a call frame
	movl	%esp,%ebp
	pushl	%edi		# save scratch registers
	pushl	%esi
	movl	12(%ebp),%ecx	# get number of longwords in arg list
	cld			# make the copy go forward
	movl	%ecx,%eax	# get number of longwords again
	movl	16(%ebp),%esi	# point to given arg list
	shll	$2,%eax		# calculate number of bytes in arg list
	movl	8(%ebp),%edx	# get subroutine's entrypoint
	subl	%eax,%esp	# make room for arg list on stack
	movl	%esp,%edi	# point to stack's copy of arg list
	rep			# copy arg list to stack
	movsl
	call	*%edx		# call the routine
	leal	-8(%ebp),%esp	# pop arg list from stack
	popl	%esi		# restore scratch registers
	popl	%edi
	leave			# pop call frame and return
	ret

##########################################################################
##									##
##  Take a breakpoint here						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_debug_bpt
	.type	oz_hw_debug_bpt,@function
oz_hw_debug_bpt:
	pushl	%ebp
	movl	%esp,%ebp
	int3
	leave
	ret

##########################################################################
##									##
##  Determine whether or not we are in kernel mode			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_inknlmode
	.type	oz_hw_inknlmode,@function
oz_hw_inknlmode:
	movw	%cs,%dx		# get the code segment we're in
	xorl	%eax,%eax	# assume not in kernel mode
	testb	$2,%dl		# check bit <1> of code segment rpl
	sete	%al		# if zero, we are in kernel mode
	ret

##########################################################################
##									##
##  Make sure other processors see any writes done before this call 	##
##  before they see writes done after this call				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_mb
	.type	oz_hw_mb,@function
oz_hw_mb:
	xorl	%eax,%eax
	pushl	%ebx
	cpuid
	popl	%ebx
	ret

##########################################################################
##									##
##  Check to make sure hardware interrupt delivery is enabled		##
##									##
##    Input:								##
##									##
##	4(%esp) = string name of routine being called from		##
##	8(%esp) = instance number within that routine			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_chkhwien
	.type	oz_hw486_chkhwien,@function
oz_hw486_chkhwien:
	pushl	%ebp			# make call frame for error message
	movl	%esp,%ebp
	pushfl				# get current interrupt enable flag
	bt	$9,(%esp)		# test it
	jnc	chkhwien_bad
	leave				# it is set, so that's good
	ret

chkhwien_bad:
	pushl	12(%ebp)		# it's clear, print out error message
	pushl	8(%ebp)
	pushl	$chkhwien_msg1
	call	oz_knl_printk
	movl	%ebp,%ebx		# print out a trace of return addresses (system seems to hang on crash)
chkhwien_bad_loop:
	pushl	$chkhwien_msg2
	call	oz_knl_printk
	pushl	4(%ebx)
	call	oz_knl_printkaddr
	addl	$8,%esp
	movl	(%ebx),%ebx
	testl	%ebx,%ebx
	jne	chkhwien_bad_loop
	pushl	$chkhwien_msg3
	call	oz_crash

chkhwien_msg1:	.string	"oz_hw486_chkhwien: hw ints inhibited (%s %d), eflags %x"
chkhwien_msg2:	.string	"\n    "
chkhwien_msg3:	.string	"\n  crashing..."


	.globl	oz_hw_debug_watch
	.type	oz_hw_debug_watch,@function
oz_hw_debug_watch:
	pushl	$watch_msg1
	call	oz_knl_printk
	addl	$4,%esp
	ret

watch_msg1:	.string	"oz_hw_debug_watch: watch not implemented\n"
