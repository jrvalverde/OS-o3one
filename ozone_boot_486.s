##+++2002-08-17
##    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
##---2002-08-17

#
#  The boot block is loaded into 7C00 by the BIOS
#
#  It does the following:
#
#  Sets all the segment registers to 07C0 since everything here thinks it is based at location 0
#  Sets up the stack pointer to offset 0400 of segment 07C0, ie address 8000, just below where the oz_loader_486.bb image goes
#  Prints out a message
#  Reads in the oz_loader_486.bb image starting at 8000 (skipping the first block which is a copy of the boot block)
#  Jumps to the oz_loader_486.bb image
#

# These must match oz_loader_486.s, oz_preloader_486.s, oz_ldr_loader.c, oz_kernel_486.s

BOOTSEG   = 0x07C0			# segment of boot sector
LOADSEG   = 0x0800			# segment to load loader in
STACKSIZE = 0x0400			# stack size in bytes
					# STACKSIZE must be = (LOADSEG-BOOTSEG)*16

BLOCKSIZE = 512				# bytes per sector
L2BLOCKSIZE = 9				# LOG2 (BLOCKSIZE)

	.code16
	.text
	.org	0			# everything is relative to zero
					# relocation is done by setting all segment registers to BOOTSEG

	.globl	_start
_start:
	ljmp	$BOOTSEG,$go-_start	# set code segment register to BOOTSEG
go:
	movw	$STACKSIZE,%di		# get stack size
	movw	$BOOTSEG,%ax		# set up BOOTSEG segment number
	movw	%ax,%ds			# put in ds
	movw	%ax,%ss			# put in ss
	movw	%di,%sp			# put stack just below where loader goes
	movb	%dl,driveno		# save boot drive number

##	movw	$0x0E2E,%ax		# output a dot
##	xorw	%bx,%bx
##	int	$0x10

	movl	startlbn,%ecx		# get starting lbn (of preloader)
	addl	$8,%ecx			# param block starts one page after that
	movl	%ecx,paramlbn

#
# Test the print_all routine
# Output line should be 0123 4567 89AB CDEF 5555 AAAA
#
##	mov	ax,#0x5555
##	mov	si,ax
##	add	ax,ax
##	mov	di,ax
##	mov	ax,#0x0123
##	mov	bx,#0x4567
##	mov	cx,#0x89AB
##	mov	dx,#0xCDEF
##	call	print_all
##		.ascii	"test"
##		.byte	0
#
# Print out the driveno
#
	call	print_msgi
		.byte	13,10
		.ascii	"drv="
		.byte	0
	movb	driveno,%dl
	call	print_hexb
#
# Get geometry parameters from BIOS
#
	movb	driveno,%dl
	movb	$8,%ah
	int	$0x13
	jnc	driveprmok
	call	print_msgi
		.byte	13,10
		.ascii	"error getting geom from BIOS"
		.byte	0
hang1:	jmp	hang1
driveprmok:
					# AH = status
					# BL = drive type
					# CH = number of cylinders <0:7>
					# CL<0:5> = sectors per track
					# CL<6:7> = number of cyls <8:9>
					# DH = tracks per cylinder - 1
					# DL = number of drives
	andb	$0x3F,%cl		# mask out high cyl number bits
	movb	%cl,secpertrk		# save number of sec per trk
	movb	%dh,trkpercyl_m1	# save number of trk per cyl - 1
#
# Print it out
#
	call	print_msgi
		.ascii	" sec="
		.byte	0
	movb	secpertrk,%dl
	call	print_hexb
	call	print_msgi
		.ascii	" trk="
		.byte	0
	movb	trkpercyl_m1,%dl
	xorb	%dh,%dh
	incw	%dx
	call	print_hex
#
# Read loader image into memory starting at LOADSEG
#
	call	print_msgi
		.ascii	" lbn="
		.byte	0
	movw	startlbn+2,%dx
	call	print_hex
	movw	startlbn+0,%dx
	call	print_hex
	call	print_msgi
		.byte	13,10,0
	mov	$LOADSEG,%ax
	mov	%ax,%es
	call	read_it
#
# Print a load complete message
#
	call	print_msgi
		.ascii	" done"
		.byte	0

#
# Dump out 1st 16 bytes
#
##	mov	si,#0x0400 ## (LOADSEG-BOOTSEG)*16
##	mov	di,#16
dump_loop:
##	mov	dl,(si)
##	inc	si
##	call	print_hexb
##	mov	ax,#0x0E20
##	xor	bx,bx
##	int	0x10
##	dec	di
##	jne	dump_loop
dump_done:
##	jmp	dump_done
#
# Jump to the 16-bit pre-loader
#
# All it assumes we do is pass drivetype in ax and driveno in dl,
# set the cs to LOADSEG (and thus the IP is zero)
#
	movb	driveno,%dl	# get driveno in dl for the pre-loader
	movl	paramlbn,%ecx	# get parameter block lbn
	movb	$'F',%al	# assume it is a floppy disk
	movb	$'D',%ah
	testb	%dl,%dl
	jns	preloader
	movb	$'H',%al	# it's a hard disk
preloader:
	ljmp	$LOADSEG,$0	# jump to pre-loader

#
# This routine reads disk sectors into memory
#
# It tries to read as much as it can per int 13
# Int 13 can read from one only one track per int 13
# Int 13 cannot cross 64k memory boundaries per int 13
#
#   Input:
#
#	es = starting segment to read into (must be on a block boundary)
#	blockcount = number of blocks (16 bits)
#	startlbn   = starting block number (32 bits)
#
#	BLOCKSIZE = bytes per sector
#	secpertrk = sectors per track (range 1..63)
#	trkpercyl_m1 = tracks per cylinder -1 (range 0..255)
#
#	driveno = drive number
#
#   Output:
#
#	data read in to memory
#	blockcount = zero
#	startlbn   = incremented to end
#	es = incremented to end
#
#   Scratch:
#
#	eax,ebx,cx,dx
#
read_it:
	movw	$0x0E2E,%ax		# output a dot
	xorw	%bx,%bx
	int	$0x10
	movw	blockcount,%dx		# output blocks to go
	call	print_hex
#
# Determine cylinder, track and sector to start reading at
#
	movb	secpertrk,%cl		# calculate number of sectors per cylinder
	movb	trkpercyl_m1,%al	# (al = tracks per cylinder - 1 in range 0..255)
	mulb	%cl			# (calculate ax = al * cl)
	addb	%cl,%al			# add one of these to result because we used tpc-1
	adcb	$0,%ah			# ... to get sectors per cylinder
	movw	%ax,%bx			# save result in bx
	movw	startlbn+0,%ax		# get starting block number in dx:ax
	movw	startlbn+2,%dx
	divw	%bx			# divide block number by sectors per cylinder to get cylinder number
					# ax = quotient = cylinder number
					# dx = remainder = sector within cylinder
	movw	%ax,%bx			# save cylinder number in bx
	movw	%dx,%ax			# put sector within cylinder in ax
	divb	%cl			# divide sector within cylinder by sectors per track to get track number within cylinder
					# al = quotient = track number in cylinder
					# ah = remainder = sector number in track
	movb	%al,%dh			# move track number where it will be needed for the int 13 call
#
# ah = sector number within the track (0..62)
# bx = cylinder number (0..1023)
# cl = sectors per track (1..63)
# dh = track number within the cylinder (0..255)
# es = segment to read into
#
# Determine how many sectors we can read given that we can't cross a track boundary, 
# we can't cross a 64k memory boundary, and we don't want to go past the eof
#
	movb	%cl,%al			# start with the number of sectors in a track
	subb	%ah,%al			# subtract out the starting sector number within the track
# al = number of sectors left in track
	movw	%es,%cx			# get the segment we will start loading at
	andw	$0x0FFF,%cx		# get the click (16 byte) offset within the current 64k memory block
	subw	$0x1000,%cx		# see how many clicks are left to the next 64k boundary
	negw	%cx
	shrw	$L2BLOCKSIZE-4,%cx	# see how many sectors are left to the next 64k boundary
	subb	%al,%cl			# compare with how many sectors left in track
	jnc	wont_pass_64k
	addb	%cl,%al			# if too many sectors, reduce by the overage
wont_pass_64k:
# al = min (number of sectors left in track, number of sectors to next 64k boundary)
	movw	blockcount,%cx		# get block count remaining to be loaded
	testb	%ch,%ch			# see if we have more than 255 sectors to go
	jnz	have_sec_count		# if so, al contains the sector count as is
	subb	%al,%cl			# see if actual count remaining is less than what we can do
	jnc	have_sec_count		# if not, al contains the sector count as is
	addb	%cl,%al			# if so, sector count = number of sectors left
	jmp	have_sec_count
read_more1: jmp	read_it
have_sec_count:
##	movb	$1,%al			## temp - just read one at a time
#
# al = number of sectors to read = min (number of sectors left in track, number of sectors to next 64k boundary, number of sectors left in file)
# ah = sector number within the track
# bx = cylinder number
# dh = track number within the cylinder
#
# Now finally read the sectors into memory
#
	movb	%bh,%cl			# get cylinder bits <8..15>
	shlb	$6,%cl			# put cylinder bits <8..9> in cl bits <6..7>
	orb	%ah,%cl			# get sector number in cl <0..5> (assumes sector < 63)
	incb	%cl			# offset sector number by 1
	movb	%bl,%ch			# get clyinder bits <0..7> in ch bits <0..7>
					# track number already in dh <0..7> (assumes track < 256)
	movb	driveno,%dl		# get drive number in dl
	movb	$2,%ah			# get read function code in ah
	xorw	%bx,%bx			# zero offset in es segment
	pushw	%es			# save registers for error message
	pushw	%dx
	pushw	%cx
	pushw	%ax
	int	$0x13			# read disk
	jc	read_error		# check for error
	popw	%ax			# restore registers
	popw	%cx
	popw	%dx
	popw	%es
	xorl	%ebx,%ebx		# get number of sectors that were read in (32 bits)
	movb	%al,%bl
	movw	%bx,%ax			# get amount to increment segment number by in ax
	shlw	$L2BLOCKSIZE-4,%ax
	movw	%es,%cx			# increment segment number for next time around loop
	addw	%ax,%cx
	movw	%cx,%es
	addl	%ebx,startlbn		# increment starting block number by number of sectors read
	subw	%bx,blockcount		# decrement number of sectors to read by number of sectors read
	jne	read_more1		# repeat if there is more to read
	ret				# return
read_more2: jmp read_more1
#
# Read error, output message and wait for the ANY key, then retry
#
read_error:
	pushw	startlbn+0		# push lbn on stack
	pushw	startlbn+2
	pushw	%ax			# save error code
	leaw	error_msg,%si
read_error_loop:
	call	print_msg		# output message
	popw	%dx			# output corresponding value
	call	print_hex
	movb	(%si),%al		# see if more to do
	testb	%al,%al
	jns	read_error_loop
	movw	%dx,%es			# final pop was es restore
##	call	pause			# wait for the ANY key
	jmp	read_more2
#
error_msg:	.byte	13,10
		.ascii	"err "
		.byte	0
		.ascii	" lbn="
		.byte	0
		.byte	0
		.ascii	" ax="
		.byte	0
		.ascii	" cx="
		.byte	0
		.ascii	" dx="
		.byte	0
		.ascii	" es="
		.byte	0,-1

#
# Print all registers
#
#   Input:
#
#	registers = to be printed
#	rtn address = null terminated prefix string
#
#   Scratch:
#
#	none
#
##print_all:
##	push	di			# push registers we print
##	push	si
##	push	dx
##	push	cx
##	push	bx
##	push	ax
##	call	print_msgi		# print a CR/LF
##		.byte	13,10,0
##	mov	di,sp			# point to registers on stack
##	mov	si,12(di)		# get return address
##	call	print_msg		# print message that follows call
##	mov	12(di),si		# update the return address
##	mov	cx,#6			# going to print 6 registers from stack
##print_all_loop:
##	push	cx			# save counter on stack
##	mov	ax,#0x0E20		# print a space
##	xor	bx,bx
##	int	$0x10
##	mov	dx,(di)			# get register value word to print
##	call	print_hex		# print out the word
##	inc	di			# increment to next word on stack
##	inc	di
##	pop	cx			# get the counter
##	dec	cx			# see if there are more to print
##	jne	print_all_loop		# if so, continue printing
##	pop	ax			# pop saved registers
##	pop	bx
##	pop	cx
##	pop	dx
##	pop	si
##	pop	di
##	ret				# all done

#
# Print inline message
#
#   Input:
#
#	null terminated message follows call inline
#
#   Scratch:
#
#	none
#
print_msgi:
	pushaw				# save all registers
	movw	%sp,%di			# point to stack
	movw	16(%di),%si		# si = return address = message pointer
	call	print_msg		# print it out
	movw	%si,16(%di)		# save new return address (following the null byte)
	popaw				# restore all registers
	ret				# return to point following the null
#
# Print message pointed to by ds:si
#
#   Input:
#
#	ds:si =  points to null terminated message
#
#   Output:
#
#	si = incremented to point just past the null
#
#   Scratch:
#
#	ax,bx
#
print_msg:
	lodsb				# al <- (ds:si)+
	testb	%al,%al			# test al
	je	print_msg_done		# done if zero
	movb	$0x0E,%ah		# put 0x0E in top = print char
	xor	%bx,%bx			# clear page number
	int	$0x10			# print it
	jmp	print_msg		# try for more
print_msg_done:
	ret
#
#  Print the word in dx in hexadecimal
#
#   Input:
#
#	dx = word to be printed
#
#   Scratch:
#
#	ax,bx,cx
#
#
print_hexb:
	movw	$2,%cx		# 2 hex digits
	rolw	$8,%dx
	jmp	print_hexx
print_hex:
	movw	$4,%cx		# 4 hex digits
print_hexx:
	xorw	%bx,%bx		# clear page number
print_digit:
	rolw	$4,%dx		# rotate so that lowest 4 bits are used
	movw	$0x0E0F,%ax	# ah = request, al = mask for nybble
	andb	%dl,%al
	addb	$0x90,%al	# convert al to ascii hex (four instructions)
	daa
	adcb	$0x40,%al
	daa
	int	$0x10
	loop	print_digit
	ret
#
# Pause (prints a '>' and waits for the ANY key)
#
##pause:
##	push	bx
##	push	ax
##	mov	ax,#0xe3e		# output a '>'
##	xor	bx,bx
##	int	$0x10
##	xor	ax,ax			# read a keystroke
##	int	$0x16
##	pop	ax
##	pop	bx
##	ret

		.org	BLOCKSIZE-16
#
paramlbn:	.long	0		# lbn of param block in boot file		## 496
#
		.byte	0								## 500
driveno:	.byte	0		# drive number					## 501
#
# The rest of the data is set up by the bootblock writing utility and should not be moved
#
# - Disk geometry
#
#   Even though the bootblock writing utility sets these values, we ignore 
#   what it wrote and get them via INT 0x13 with AH=8
#
secpertrk:	.byte	0		# sectors per track (code assumes < 63)		## 502
trkpercyl_m1:	.byte	0		# tracks per cylinder minus 1			## 503
#
# - Loader image descriptor
#
#   The initial values here are used when the loader image is copied directly to a device, not via the bootblock writing utility
#   Thus, startlbn=1 means the loader starts immediately following the bootblock (which is at lbn 0)
#   The blockcount=1216 is the largest loader we can load (goes from 0x08000 to 0x9FFFF)
#
#   If the bootblock was written via the bootblock writing utility, the actual values are patched in here
#
startlbn:	.long	1		# starting block number of loader image		## 504
blockcount:	.word	1216		# number of blocks in loader image		## 508
					# (code assumes startlbn + blockcount < 63*256*1024 = 16,515,072 or approx 7.8Gb)
					# (sectors are numbered 1..63, tracks 0..255, cylinders 0..1023)
#
# This number is required by the bios to be the last word in the bootblock
#
boot_flag:	.word	0xAA55		# magic number for bios				## 510
