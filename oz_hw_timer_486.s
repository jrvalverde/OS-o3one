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
##  This routines use the 8254 PIT chip to perform system event timing 	##
##  functions								##
##									##
##########################################################################

	.include "oz_params_486.s"

	PIT_8254_DAT = 0x40
	PIT_8254_CTL = 0x43

	TIMER_IRQ = 0

	.data

	.align	4
	.globl	oz_hw_smplock_tm
	.type	oz_hw_smplock_tm,@object
oz_hw_smplock_tm:	.long	0		# points to timer smplock
timer_event:		.long	-1,-1		# rdtsc to call oz_knl_timer_timeisup

	.globl	oz_hw486_pitfactor
	.globl	oz_hw486_pitshift
	.type	oz_hw486_pitfactor,@object
	.type	oz_hw486_pitshift,@object
oz_hw486_pitfactor:	.long	0		# PIT interrupts per TSC * 2^pitshift
oz_hw486_pitshift:	.long	0

timer_irq_many:	.space	IRQ_MANY_SIZE		# timer interrupt descriptor

	.text
timer_description:	.string	"oz_hw486_timer"

##########################################################################
##									##
##  Called at boot time to initialize timer functions			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_timer_init
	.type	oz_hw486_timer_init,@function
oz_hw486_timer_init:
	pushl	%ebp
	movl	%esp,%ebp
	movl	$timer_irq_many,%eax
	movl	$timer_interrupt,IRQ_MANY_ENTRY(%eax)
	movl	$timer_description,IRQ_MANY_DESCR(%eax)
	pushl	%eax				# descriptor
	pushl	$TIMER_IRQ			# the irq level
						# note - on mobo's with an IOAPIC, the timer is 
						#        routed thru IRQ 2.  The IOAPIC interrupt 
						#        routines compensate for that (ugly crap)
	call	oz_hw486_irq_many_add		# add it to list of stuff for that irq level
	addl	$8,%esp
	movl	%eax,oz_hw_smplock_tm		# save pointer to smp lock
	popl	%ebp
	ret

##########################################################################
##									##
##  Set the datebin of the next event					##
##  When this time is reached, call oz_knl_timer_timeisup		##
##									##
##    Input:								##
##									##
##	8:4(%esp) = datebin of next event				##
##	smplock = tm							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_timer_setevent
	.type	oz_hw_timer_setevent,@function
oz_hw_timer_setevent:
	movl	4(%esp),%eax		# get absolute system time
	movl	8(%esp),%edx
	pushl	%ebx
	pushfl
	cli
	call	oz_hw486_asys2iota	# convert to absolute RDTSC time
	popfl
	movl	%eax,timer_event+0	# save it
	movl	%edx,timer_event+4
	call	timer_start		# start the PIT counting down
	popl	%ebx
	ret

##########################################################################
##									##
##  This interrupt routine is called when the timer reaches 0		##
##									##
##    Input:								##
##									##
##	timer_event = when to call oz_knl_timer_timeisup		##
##	smplock = tm							##
##									##
##########################################################################

	.text

	.align	4
timer_interrupt:
	pushl	%ebx
	call	timer_start
	xorl	%eax,%eax
	popl	%ebx
	ret

##########################################################################
##									##
##  Start timer								##
##									##
##    Input:								##
##									##
##	timer_event = absolute iota time of event			##
##	smplock = tm							##
##									##
##    Output:								##
##									##
##	timer started or oz_knl_timer_timeisup called			##
##									##
##    Scratch:								##
##									##
##	%eax,%ebx,%ecx,%edx						##
##									##
##########################################################################

	.align	4
timer_start:
	call	oz_hw_tod_iotanow	# get current iota (RDTSC) time, SMP safe
	movl	timer_event+0,%ecx	# get next event's iota time
	movl	timer_event+4,%ebx
	subl	%eax,%ecx		# calculate delta iota time
	sbbl	%edx,%ebx
	jc	timer_start_exp		# - it has already happened
	movl	%ecx,%eax		# multiply by pithz per tsc
	mull	oz_hw486_pitfactor
	movl	%edx,%ecx
	xchgl	%ebx,%eax
	mull	oz_hw486_pitfactor
	addl	%ecx,%eax
	adcl	$0,%edx

	# edx:eax:ebx = iotatime * pitfactor

	movb	oz_hw486_pitshift,%cl
	testb	$32,%cl
	jz	timer_start_shift
	movl	%eax,%ebx
	movl	%edx,%eax
	xorl	%edx,%edx
timer_start_shift:
	shrdl	%cl,%eax,%ebx
	shrdl	%cl,%edx,%eax
	shrl	%cl,%edx

	# edx:eax:ebx = (iotatime * pitfactor) >> pitshift = number of pit cycles to wait

	orl	%edx,%eax		# we can only handle a 16-bit count
	jne	timer_start_long
	testl	%ebx,%ebx
	je	timer_start_exp		# if zero, it has already happened
	cmpl	$0xFFFF,%ebx		# if ge 2^16, it'll be a long time
	jc	timer_start_set
timer_start_long:
	xorl	%ebx,%ebx		# - so program counter to zeroes = 2^16
timer_start_set:
	movb	$0x30,%al		# mode 0, ie, one shot
	outb	$PIT_8254_CTL
	movb	%bl,%al			# write counter lsb
	outb	$PIT_8254_DAT
	movb	%bh,%al			# write counter msb & start counting down
	outb	$PIT_8254_DAT
	ret
timer_start_exp:
	jmp	oz_knl_timer_timeisup	# expired, call 'timeisup' routine in the kernel
