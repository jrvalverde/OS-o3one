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

/************************************************************************/
/*									*/
/*  Generic I/O function codes						*/
/*									*/
/************************************************************************/

#ifndef _OZ_IO_GEN_H
#define _OZ_IO_GEN_H

#define OZ_IO_GEN_BASE (0x00000000)
#define OZ_IO_GEN_MASK (0xFFFFFF00)

#include "oz_knl_hw.h"

/* Poll for device able to process requests */

#define OZ_IO_GEN_POLL OZ_IO_DN(OZ_IO_GEN_BASE,1)

#define OZ_IO_GEN_POLL_R 1
#define OZ_IO_GEN_POLL_W 2
#define OZ_IO_GEN_POLL_X 4

typedef struct { uLong mask;
               } OZ_IO_gen_poll;

#endif
