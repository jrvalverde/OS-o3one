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
/*  We don't have a bootblock to write					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_hw_bootblock.h"
#include "oz_knl_status.h"

uLong oz_hw_bootblock_nblocks (OZ_IO_disk_getinfo1 *disk_getinfo1, OZ_Iochan *diskiochan, OZ_Dbn *bb_nblocks, OZ_Dbn *bb_logblock)

{
  *bb_nblocks = 1;
  *bb_logblock = 0;
  return (OZ_SUCCESS);
}

uLong oz_hw_bootblock_modify (uByte *bootblock, 
                              OZ_Dbn ldr_nblocks, 
                              OZ_Dbn ldr_logblock, 
                              OZ_Dbn part_logblock, 
                              OZ_IO_fs_writeboot *fs_writeboot, 
                              OZ_IO_disk_getinfo1 *disk_getinfo1, 
                              OZ_IO_disk_getinfo1 *host_getinfo1, 
                              OZ_Iochan *diskiochan)

{
  return (OZ_NOTIMPLEMENTED);
}
