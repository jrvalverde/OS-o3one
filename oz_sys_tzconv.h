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

#ifndef _OZ_SYS_TZCONV_H
#define _OZ_SYS_TZCONV_H

#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_sys_dateconv.h"

uLong oz_knl_tzconv_setdefault (const char *tzname);
OZ_HW_SYSCALL_DCL_6 (tzconv, OZ_Datebin_tzconv, tzconvtype, 
                                    OZ_Datebin, utcin, 
                                     OZ_Handle, h_tzfilein, 
                                  OZ_Datebin *, lclout, 
                                           int, tznameoutl, 
                                        char *, tznameout)

#endif
