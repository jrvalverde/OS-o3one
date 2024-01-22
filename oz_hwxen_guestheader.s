##########################################################################
##									##
##  This header file data is the first 12 bytes of an Xeno guest OS 	##
##  image file.								##
##									##
##  The way this is set up is link it at the same VA as the guest OS 	##
##  image is linked, as raw binary.  This will set the .long _start to 	##
##  be the starting VA of the image.					##
##									##
##########################################################################

	.text
	.globl	_start
_start:
#
# The following 12 bytes get read by the loader but do not get saved in memory
#
	.ascii	"XenoGues"	# read_kernel_header (tools/xc/lib/xc_linux_build.c)
	.long	_start		# - the kernel's load address
#
# The following 24 bytes replace the first 24 bytes of the kernel image's ELF header
#
ehdr:
	pushl	_start+24	# push e_entry on stack
	ret			# jump to it
	.org	ehdr+24		# skip over rest of useless header stuff

