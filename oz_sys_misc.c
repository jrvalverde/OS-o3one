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

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_syscall.h"

/************************************************************************/
/*									*/
/*  Fill a buffer with random data					*/
/*									*/
/*   Input:								*/
/*									*/
/*	size = number of bytes to get					*/
/*	buff = where to put them					*/
/*									*/
/*   Output:								*/
/*									*/
/*	oz_sys_random_fill = OZ_SUCCESS : buffer filled			*/
/*	                           else : error status			*/
/*	*buff = filled with random data					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (random_fill, uLong, size, uByte *, buff)

{
  OZ_Seclock *seclock;
  uLong sts;

  sts = oz_knl_section_iolockw (cprocmode, size, buff, &seclock, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) {
    oz_hw_random_fill (size, buff);
    oz_knl_section_iounlk (seclock);
  }
  return (sts);
}
