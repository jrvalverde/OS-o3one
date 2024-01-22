##+++2004-08-03
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
##---2004-08-03

##########################################################################
##									##
##  Uniprocessor routines						##
##									##
##########################################################################

	.include "oz_params_xen.s"

		.data
		.align	4

	.globl	oz_hwxen_softintpend
	.type	oz_hwxen_softintpend,@object
oz_hwxen_softintpend:	.long	0	# set indicates that a software interrupt is pending
					# - to be processed when smplevel is 0 and event delivery is enabled
					#   <0> : set to indicate oz_knl_thread_handleint needs to be called
					#   <1> : set to indicate oz_knl_lowipl_handleint needs to be called
					#   <2> : set to indicate oz_knl_diag needs to be called

	.globl	oz_hwxen_cpudb
	.type	oz_hwxen_cpudb,@object
oz_hwxen_cpudb:	.space	CPU__SIZE	# we only have one cpu

quantum_timer:	.long	0		# OZ_Timer quantum timer struct

	.text

	.align	4

# Translate (smp lock level - LOWEST_IRQ_SMPLEVEL) to irq number

smplevel_to_irq: .byte	7,6,5,4,3,15,14,13,12,11,10,9,8,2,1,0

##########################################################################
##									##
##  Start up the alternate cpus, cause them to call 			##
##  oz_knl_boot_othercpu						##
##									##
##    Input:								##
##									##
##	4(%esp) = oz_s_cpucount						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_bootalts
	.type	oz_hw_cpu_bootalts,@function
oz_hw_cpu_bootalts:
	ret

##########################################################################
##									##
##  Halt the other cpu's, cause them to call oz_knl_debug_halted no 	##
##  matter what they are currently doing				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_debug_halt
	.type	oz_hw_debug_halt,@function
oz_hw_debug_halt:
	ret

##########################################################################
##									##
##  Quantum timer							##
##									##
##  These routines use the system timer queue to control cpu time 	##
##  usage by scheduled threads.						##
##									##
##  When the kernel loads a threads context up, it calls 		##
##  oz_hw_quantimer_start.  It expects to be called back at 		##
##  oz_knl_thread_quantimex when the timer runs out.			##
##									##
##    Input:								##
##									##
##	 4(%esp) = delta time from now to expire (<= 0.1 sec)		##
##	12(%esp) = current date/time					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_quantimer_start
	.type	oz_hw_quantimer_start,@function
oz_hw_quantimer_start:
				ret	### put this ret to disable it
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	movl	16(%ebp),%eax			# get current iota time in %edx:%eax
	movl	20(%ebp),%edx
	addl	 8(%ebp),%eax			# add the given delta to it (<= 0.1 sec)
	adcl	12(%ebp),%edx
	call	oz_hw486_aiota2sys		# convert to system time
	movl	quantum_timer,%ecx
	pushl	$0				# parameter to oz_knl_thread_quantimex = cpuid 0
	pushl	$oz_knl_thread_quantimex	# entrypoint to call at softint level when time is up
	pushl	%edx				# when time is up
	pushl	%eax
	jecxz	quantimer_getone_uni
quantimer_haveone_uni:
	pushl	%ecx				# address of the timer struct
	call	oz_knl_timer_insert		# insert into queue (or remove and reinsert if already there)
	addl	$20,%esp
	popl	%ebx
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

quantimer_getone_uni:
	pushl	$0				# allocate a timer struct
	call	oz_knl_timer_alloc
	addl	$4,%esp
	movl	%eax,%ecx
	movl	%eax,quantum_timer		# save it for repeated re-use
	jmp	quantimer_haveone_uni

##########################################################################
##									##
##  Enable/Inhibit softint delivery on this cpu				##
##									##
##    Input:								##
##									##
##	4(%esp) = 0 : ihnibit softint delivery				##
##	          1 : enable softint delivery				##
##									##
##    Output:								##
##									##
##	oz_hw_cpu_setsoftint = 0 : softint delivery was inhibited	##
##	                       1 : softint delivery was enabled		##
##	softint delivery mode set as given				##
##									##
##    Note:								##
##									##
##	No smplocks can be held by this cpu				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_setsoftint
	.type	oz_hw_cpu_setsoftint,@function
oz_hw_cpu_setsoftint:
	pushl	%ebp
	movl	%esp,%ebp
	movzbl	oz_hwxen_cpudb+CPU_B_SMPLEVEL,%eax # get cpu's current smp level
	cmpb	$2,%al				# make sure smp level not too high
	jnc	setsoftint_crash1
	movl	$1,%ecx				# get new smp level
	subl	8(%ebp),%ecx
	jc	setsoftint_crash2
	movb	%cl,oz_hwxen_cpudb+CPU_B_SMPLEVEL # set the new level
	jnz	setsoftint_rtn			# if softints now inhibited, don't check for pending softints
	cmpb	$0,oz_hwxen_softintpend		# enabled, see if there are any pending softints
	jz	setsoftint_rtn
	call	oz_hwxen_procsoftint		# ... go process it
setsoftint_rtn:
	xorb	$1,%al				# flip smplevel<->softint enabled value
	popl	%ebp
	ret

setsoftint_crash1:
	pushl	%eax
	pushl	$setsoftint_msg1
	call	oz_crash

setsoftint_crash2:
	pushl	8(%ebp)
	pushl	$setsoftint_msg2
	call	oz_crash

setsoftint_msg1:	.string	"oz_hw_cpu_setsoftint: called at smplevel %u"
setsoftint_msg2:	.string	"oz_hw_cpu_setsoftint: invalid argument %u"

##########################################################################
##									##
##  Set this cpu's thread execution priority				##
##									##
##    Input:								##
##									##
##	4(%esp) = new thread execution priority for the current cpu	##
##	smplevel = at least softint					##
##									##
##    Note:								##
##									##
##	In this implementation, we don't use it for anything, so we 	##
##	don't bother saving it						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_sethreadpri
	.type	oz_hw_cpu_sethreadpri,@function
oz_hw_cpu_sethreadpri:
	ret

##########################################################################
##									##
##  Sofint this or another cpu						##
##									##
##    Input:								##
##									##
##	4(%esp)  = cpu index number					##
##	smplevel = any							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_reschedint
	.type	oz_hw_cpu_reschedint,@function
oz_hw_cpu_reschedint:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx
	movb	$1,%bl
	jmp	softint

	.align	4
	.globl	oz_hw_cpu_lowiplint
	.type	oz_hw_cpu_lowiplint,@function
oz_hw_cpu_lowiplint:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx
	movb	$2,%bl

softint:
	cmpl	$0,8(%ebp)				# we only do cpu #0
	jne	softint_crash1
	orb	%bl,oz_hwxen_softintpend		# say it is pending
	cmpb	$0,oz_hwxen_cpudb+CPU_B_SMPLEVEL	# see if softint delivery is enabled
	jne	softint_rtn
	call	oz_hwxen_procsoftint			# if so, process it immediately
softint_rtn:
	popl	%ebx
	popl	%ebp
	ret

softint_crash1:
	pushl	4(%esp)
	pushl	$softint_msg1
	call	oz_crash

softint_msg1:	.string	"oz_hw_cpu_softint: cpu id %d out of range"

##########################################################################
##									##
##  Cause all cpus to enter diag mode					##
##									##
##  This routine is called at high ipl when control-shift-D is pressed.	##
##  It sets the softintpend<2> bit then softints the cpu.  The softint 	##
##  interrupt routine calls oz_knl_diag for every cpu as long as the 	##
##  diagmode flag is set.						##
##									##
##########################################################################

	.align	4
	.global	oz_hw_diag
	.type	oz_hw_diag,@function
oz_hw_diag:
	orb	$4,oz_hwxen_softintpend			# set the flag for the procsoftint routine
	cmpb	$0,oz_hwxen_cpudb+CPU_B_SMPLEVEL	# see if softint delivery enabled
	jne	diag_ret
	call	oz_hwxen_procsoftint			# if so, process it
diag_ret:
	ret

##########################################################################
##									##
##  Get current cpu index and point to cpudb struct			##
##									##
##    Input:								##
##									##
##	apic = cpu specific registers					##
##									##
##    Output:								##
##									##
##	eax = cpu index (starts at zero)				##
##	esi = points to cpu data					##
##									##
##    Preserved:							##
##									##
##	edi, edx (used by the thread_init and pte_writeact stuff)	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_getcur
	.type	oz_hw_cpu_getcur,@function
oz_hw_cpu_getcur:				# C callable routine just sets up %eax ...
	xorl	%eax,%eax
	ret

	.align	4
	.globl	oz_hw486_getcpu
	.type	oz_hw486_getcpu,@function
oz_hw486_getcpu:				# assembler routine sets up %esi as well as %eax ...
	xorl	%eax,%eax
	leal	oz_hwxen_cpudb,%esi
	ret

##########################################################################
##									##
##  Get current cpu's lock level					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_smplevel
	.type	oz_hw_cpu_smplevel,@function
oz_hw_cpu_smplevel:
	movzbl	oz_hwxen_cpudb+CPU_B_SMPLEVEL,%eax
	ret

##########################################################################
##									##
##  Set hardware interrupt enable bit on calling cpu			##
##									##
##    Input:								##
##									##
##	4(%esp) = 0 : inhibit hardware interrupt delivery		##
##	          1 : enable hardware interrupt delivery		##
##	         -1 : inhibit nmi's also				##
##									##
##    Output:								##
##									##
##	%eax = 0 : interrupt delivery was previously inhibited		##
##	       1 : interrupt delivery was previously enabled		##
##									##
##    Note:								##
##									##
##	For Xen, this means allowing or blocking event delivery		##
##									##
##########################################################################

	.align	4
	.global	oz_hw_cpu_sethwints
	.type	oz_hw_cpu_sethwints,@function
oz_hw_cpu_sethwints:
	movl	4(%esp),%ecx				# get new setting
	movl	oz_hwxen_sharedinfo+sh_events_mask,%eax
	testl	%ecx,%ecx
	js	sethwints_inhnmi
sethwints_getcur:
	shrl	$SH_V_EVENTS_MASTER_ENABLE,%eax		# get current 'enable' bit
	andl	$1,%ecx
	cmpl	%eax,%ecx				# see if staying the same
	je	sethwints_rtn
	xorb	$SH_M_EVENTS_MASTER_ENABLE>>24,oz_hwxen_sharedinfo+sh_events_mask+3 # different, flip bit
	jecxz	sethwints_rtn
	call	oz_hwxen_checkevents			# just enabled, see if there is anything to process now
	xorl	%eax,%eax				# tell caller hwints were previously blocked
sethwints_rtn:
	ret
sethwints_inhnmi:
	xorl	%ecx,%ecx				# we don't use NMI for anything
	jmp	sethwints_getcur

##########################################################################
##									##
##  Initialize an smp lock						##
##									##
##    Input:								##
##									##
##	 4(%esp) = size of smplock struct				##
##	 8(%esp) = address of smplock struct				##
##	12(%esp) = priority to initialize it to				##
##									##
##    Output:								##
##									##
##	smp lock struct initialized, unowned				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_init
	.type	oz_hw_smplock_init,@function
oz_hw_smplock_init:
	pushl	%ebp
	movl	%esp,%ebp
	cmpl	$SL__SIZE,8(%ebp)			# make sure they have enough room for the struct we use
	jc	smplock_init_crash1
	movl	12(%ebp),%eax				# ok, point to it
	movb	$-1,SL_B_CPUID(%eax)			# mark it unowned
	movl	16(%ebp),%edx				# get the priority they want it to be
	cmpl	$256,%edx				# make sure it will fit in smplevel field
	jnc	smplock_init_crash2
	movb	%dl,SL_B_LEVEL(%eax)			# ok, set up its priority
	popl	%ebp					# all done
	ret

smplock_init_crash1:
	pushl	$SL__SIZE
	pushl	8(%ebp)
	pushl	$smplock_init_msg1
	call	oz_crash

smplock_init_crash2:
	pushl	%edx
	pushl	$smplock_init_msg2
	call	oz_crash

smplock_init_msg1:	.string	"oz_hw_smplock_init: smp lock size %u must be at least %u"
smplock_init_msg2:	.string	"oz_hw_smplock_init: smp lock level %u must be less than 256"

##########################################################################
##									##
##  Get an smp lock's level						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_level
	.type	oz_hw_smplock_level,@function
oz_hw_smplock_level:
	movl	4(%esp),%eax			# point to smplock structure
	movzbl	SL_B_LEVEL(%eax),%eax		# get its level
	ret

##########################################################################
##									##
##  Get the cpu that owns an smplock					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_cpu
	.type	oz_hw_smplock_cpu,@function
oz_hw_smplock_cpu:
	movl	4(%esp),%eax
	movzbl	SL_B_CPUID(%eax),%eax
	testb	%al,%al
	jns	smplock_cpu_rtn
	subl	$0x100,%eax
smplock_cpu_rtn:
	ret

##########################################################################
##									##
##  Wait for an smp lock to become available and lock it		##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to smplock struct to lock			##
##									##
##    Output:								##
##									##
##	%eax = prior smp lock level for this cpu			##
##	sw ints always inhibited					##
##	hw ints inhibited iff at or above highest irq level		##
##	cpu smp level = that of the new lock				##
##									##
##    Note:								##
##									##
##	you can only lock:						##
##	 1) the same lock as the last one that was locked by this cpu	##
##	 2) a lock defined at a higher level than the last one locked by this cpu
##	these rules prevent deadlock situations				##
##									##
##	Locking an irq's smplock will inhibit delivery of that event's 	##
##	delivery							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_wait
	.type	oz_hw_smplock_wait,@function
oz_hw_smplock_wait:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp
	movzbl	oz_hwxen_cpudb+CPU_B_SMPLEVEL,%eax # get existing smplevel here
	movl	8(%ebp),%edx			# point to lock block
	movzbl	SL_B_LEVEL(%edx),%ecx		# get new smplevel here
# %edx = smplock pointer
# %ecx = new smp level
# %eax = old smp level
	cmpb	%al,%cl				# compare new level : current level
	jc	smplock_wait_crash1		# crash if new level < current level
	je	smplock_wait_same		# no-op if new level == current level

# Lock is at higher level than this cpu is at, acquire it

	cmpb	$OZ_SMPLOCK_LEVEL_EVENTS,%cl	# see if we will be blocking any event delivery
	jnc	smplock_wait_blockevents
smplock_wait_noblock:
	movb	%cl,oz_hwxen_cpudb+CPU_B_SMPLEVEL # increase cpu's smplock level
	incb	SL_B_CPUID(%edx)		# increment ownership from -1 to 0
	jne	smplock_wait_crash2		# crash if it was not -1 indicating it was unowned
	popl	%ebp
	ret

# Lock is at current level, just return with current level

smplock_wait_same:
	cmpb	$0,SL_B_CPUID(%edx)		# level staying the same, i better already own this lock
	jne	smplock_wait_crash3		# (if not, might have two locks at the same level)
	popl	%ebp
	ret

# Going at or above an event level, block associated event deliveries before locking
# %cl = new smplock level

smplock_wait_blockevents:
	pushl	%eax
	movl	$SH_M_EVENTS_MASTER_ENABLE,%eax		# if at or above maximum, we only preserve master enable
	cmpb	$OZ_SMPLOCK_LEVEL_EVENTS+31,%cl
	jnc	smplock_wait_blockall
	leal	-1(%eax,%eax),%eax			# otherwise, clear the bottom %cl<4:0>+1 bits
	shll	%cl,%eax
smplock_wait_blockall:
	andl	%eax,oz_hwxen_sharedinfo+sh_events_mask
	popl	%eax
	jmp	smplock_wait_noblock

# Crashes

smplock_wait_crash1:
	pushl	%eax
	pushl	%ecx
	pushl	$smplock_wait_msg1
	call	oz_crash

smplock_wait_crash2:
	pushl	%ecx
	pushl	%edx
	pushl	$smplock_wait_msg2
	call	oz_crash

smplock_wait_crash3:
	pushl	%edx
	pushl	%ecx
	pushl	$smplock_wait_msg3
	call	oz_crash

smplock_wait_msg1:	.string	"oz_hw_smplock_wait: new level %x .lt. current level %x"
smplock_wait_msg2:	.string	"oz_hw_smplock_wait: already own lock %p at level %x"
smplock_wait_msg3:	.string	"oz_hw_smplock_wait: re-locking level %x but dont own lock %p"

##########################################################################
##									##
##  Clear an owned lock, return cpu to a previous level			##
##									##
##    Input:								##
##									##
##	4(%esp)  = pointer to lock being released			##
##	8(%esp)  = previous smp level to return to			##
##	smplevel = that exact same level as lock being released		##
##									##
##    Output:								##
##									##
##	cpu level returned to 8(%esp)					##
##	smp lock released if now below its level			##
##	hw ints enabled if now below highest irq level			##
##	sw ints enabled if now at level 0				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_clr
	.type	oz_hw_smplock_clr,@function
oz_hw_smplock_clr:
	pushl	%ebp					# make a call frame
	movl	%esp,%ebp
	pushl	%edi					# save scratch registers
	movl	 8(%ebp),%edi				# point to lock block
	movzbl	12(%ebp),%ecx				# get level we're restoring to
	movzbl	oz_hwxen_cpudb+CPU_B_SMPLEVEL,%edx	# get existing smplevel here
	cmpb	SL_B_LEVEL(%edi),%dl			# we must be exactly at lock's level
	jne	smplockclr_crash1
	cmpb	$0,SL_B_CPUID(%edi)			# we must own the lock being released
	jne	smplockclr_crash2
	cmpb	%cl,%dl					# compare current level : level we're restoring to
	jc	smplockclr_crash3			# crash if restoring to higher level than current level
	je	smplockclr_ret				# nop if staying at same level (keep lock)

# Lock level is being lowered from level %dl to level %cl, so release the lock

	movb	$-1,SL_B_CPUID(%edi)			# release the lock so others can use it now
	movb	%cl,oz_hwxen_cpudb+CPU_B_SMPLEVEL	# set my cpu's new level

# Maybe enable some event delivery that was blocked

	cmpb	$OZ_SMPLOCK_LEVEL_EVENTS,%dl		# see if we started with all enabled
	jc	smplockclr_ret				# if so, don't bother changing anything
	cmpb	$OZ_SMPLOCK_LEVEL_EVENTS+30,%cl		# see if all are still blocked
	jnc	smplockclr_ret				# if so, don't bother changing anything

	movl	$SH_M_EVENTS_MASTER_ENABLE-1,%eax	# this will enable everything, leaving the master alone
	cmpb	$OZ_SMPLOCK_LEVEL_EVENTS,%cl		# see if any are to remain blocked
	jc	smplockclr_setmask			# if not, enable everything
	leal	(%eax,%eax),%edx			# if so, get zeroes for those to leave blocked
	shll	%cl,%edx
	andl	%edx,%eax
smplockclr_setmask:
	orl	%eax,oz_hwxen_sharedinfo+sh_events_mask	# set bits for stuff we're enabling

# If some blocked events happened whilst smplevel was elevated, process them now

	testl	%eax,%eax				# see if master enable is set
	jns	smplockclr_ret				# if not, we're done
	notl	%eax					# ok, comlpement mask so we get enable bits
	testl	oz_hwxen_sharedinfo+sh_events,%eax	# see if any enabled events are pending
	je	smplockclr_ret
	call	oz_hwxen_checkevents			# if so, process them
smplockclr_ret:
	popl	%edi
	popl	%ebp
	ret

# Crashes

smplockclr_crash1:
	movzbl	%cl,%ecx
	movzbl	SL_B_LEVEL(%edi),%eax
	pushl	%ecx
	pushl	%eax
	pushl	$smplockclr_msg1
	call	oz_crash

smplockclr_crash2:
	movzbl	SL_B_CPUID(%edi),%eax
	pushl	%eax
	pushl	%ecx
	pushl	$smplockclr_msg2
	call	oz_crash

smplockclr_crash3:
	pushl	%edx
	pushl	%ecx
	pushl	$smplockclr_msg3
	call	oz_crash

smplockclr_msg1: .string "oz_hw_smplock_clr: releasing level %X while at %X"
smplockclr_msg2: .string "oz_hw_smplock_clr: releasing level %X owned by %d"
smplockclr_msg3: .string "oz_hw_smplock_clr: returning from level %X to %X"

##########################################################################
##									##
##  Process pending software interrupts					##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwxen_procsoftint
	.globl	oz_hwxen_procsoftint_doit
	.type	oz_hwxen_procsoftint,@function
	.type	oz_hwxen_procsoftint_doit,@function
oz_hwxen_procsoftint:
	bt	$SH_V_EVENTS_MASTER_ENABLE,oz_hwxen_sharedinfo+sh_events_mask # see if hwints (events) were enabled
	jnc	procsoftint_ret			# - if not, we can't do softints
	popl	%edx				# get return address
	pushfl					# make like we were called from int instruction
	pushl	%cs
	pushl	%edx
	pushl	%ebp				# make a call frame
	movl	%esp,%esp
	pushal					# finish building mchargs
	pushl	$0				# dummy ec2
	movl	$0,MCH_L_EC1(%esp)		# dummy ec1
	movb	$SH_M_EVENTS_MASTER_ENABLE>>24,MCH_W_PAD1+1(%esp) # say hwints (ie, events) were enabled as tested above

	cmpb	$0,oz_hwxen_cpudb+CPU_B_SMPLEVEL # make sure softints were enabled
	jne	procsoftint_crash1
oz_hwxen_procsoftint_doit:			# <- call here with mchargs already on stack
	movb	$1,oz_hwxen_cpudb+CPU_B_SMPLEVEL # say softints aren't enabled anymore
	movb	oz_hwxen_softintpend,%bl	# see what kind of softints are pending
	movb	$0,oz_hwxen_softintpend		# sofints no longer pending as we are about to process them

	pushl	$1				# allow hardware interrupts, softints are inhibited
	call	oz_hw_cpu_sethwints
	addl	$4,%esp

	testb	$4,%bl				# see if in diag mode
	jz	procsoftint_nodiagmode
	pushl	%esp				# point to machine args on stack
	pushl	$1				# push first cpu flag on stack
	pushl	$0				# push cpu number
	call	oz_knl_diag			# call the diagnostic routine (cpu_index, first_cpu_flag, mchargs)
	addl	$12,%esp			# wipe call args from stack
procsoftint_nodiagmode:
	testb	$2,%bl				# see if lowipl delivery pending
	jz	procsoftint_nolowipl
	call	oz_knl_lowipl_handleint		# process stuff with softints inhibited
procsoftint_nolowipl:
	testb	$1,%bl				# see if reschedule pending
	jz	procsoftint_noresched
	call	oz_knl_thread_handleint		# process stuff with softints enabled
procsoftint_noresched:
	BLOCK_EVENTS				# inhibit events so an event routine can't change stuff on us
	movb	$0,oz_hwxen_cpudb+CPU_B_SMPLEVEL # enable software interrupt delivery
	jmp	oz_hwxen_iretwithmchargs	# if no nested reschedule, do an iret with mchargs on stack, but check for ast's, etc

procsoftint_ret:
	ret					# hw ints were inhibited, so we can't deliver softint now

# Softints were inhibited

procsoftint_crash1:
	movzbl	oz_hwxen_cpudb+CPU_B_SMPLEVEL,%eax
	pushl	%eax
	pushl	$procsoftint_msg1
	call	oz_crash

procsoftint_msg1:	.string	"oz_hwxen_uniproc procsoftint: called at smplevel %x"
