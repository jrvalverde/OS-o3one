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

#ifndef _OZ_SYS_EXHAND_H
#define _OZ_SYS_EXHAND_H

#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_thread.h"

OZ_HW_SYSCALL_DCL_3 (exhand_create, OZ_Procmode, procmode, OZ_Exhand_entry, entry, void *, param)
OZ_HW_SYSCALL_DCL_3 (exhand_delete, OZ_Procmode, procmode, OZ_Exhand_entry, entry, void *, param)

#endif
