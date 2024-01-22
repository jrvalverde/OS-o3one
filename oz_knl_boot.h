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

#ifndef _OZ_KNL_BOOT_H
#define _OZ_KNL_BOOT_H

#define OZ_KNL_BOOT_SYSPROCID 1

#include "oz_knl_hw.h"
#include "oz_knl_malloc.h"

void oz_knl_boot_firstcpu (OZ_Mempage ffvirtpage, OZ_Mempage ffphyspage);
void oz_knl_boot_anothercpu (void);
#endif
