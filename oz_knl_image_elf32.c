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
/*  Load Elf32 image in memory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	imageargs = image load arguments				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_image_elf32 = OZ_SUCCESS : successfully loaded		*/
/*	                 OZ_BADIMAGEFMT : not an elf32 image		*/
/*	                           else : load error			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_IMAGE_C

#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

#include "elf.h"

#define PRINTERROR oz_sys_io_fs_printerror

/* Image extension structure */

typedef struct { Elf32_Word *hash_addr;		/* hash table address */
                 uLong symtab_size;		/* symbol table size (number of entries in the symtab) */
                 Elf32_Sym *symtab_addr;	/* symbol table address */
                 uLong strtab_size;		/* number of bytes in string table */
                 char *strtab_addr;		/* string table address */
                 int phnum;			/* number of phdata elements */
                 Elf32_Phdr phdata[1];		/* program header array */
               } Imagex;

uLong oz_knl_image_elf32_load (OZ_Image_Args *imageargs);
static int elf32_lookup (void *imagexv, char *symname, OZ_Pointer *symvalu);
static void elf32_unload (void *imagexv, OZ_Procmode mprocmode);
static void *elf32_symscan (void *imagexv, void *lastsym, char **symname_r, OZ_Pointer *symvalu_r);

globaldef const OZ_Image_Hand oz_knl_image_elf32 = { ".elf", oz_knl_image_elf32_load, elf32_lookup, elf32_unload, elf32_symscan };

static int procrel_table (uLong rel_siz, Elf32_Rel *rel_pnt, 
                          int numsections, OZ_Mapsecparam *mapsecparams, OZ_Pointer dynaddroffs, 
                          OZ_Image_Args *imageargs, Imagex *imagex);
static int procrelatable (uLong relasiz, Elf32_Rela *relapnt, 
                          int numsections, OZ_Mapsecparam *mapsecparams, OZ_Pointer dynaddroffs, 
                          OZ_Image_Args *imageargs, Imagex *imagex);
static uLong valid_address (int numsections, OZ_Mapsecparam *mapsecparams, void *vaddr, int writable);

uLong oz_knl_image_elf32_load (OZ_Image_Args *imageargs)

{
  char *symbolname;
  Elf32_Dyn  *elf_dynpnt,*elf_dynpnte;
  Elf32_Ehdr *elf_ex;
  Elf32_Phdr *elf_phdata, *elf_phpnt;
  Elf32_Rel  *jmp_pnt, *rel_pnt;
  Elf32_Rela *relapnt;
  Elf32_Sym  *elf_sym;
  Imagex *imagex;
  int i, j, numsections;
  OZ_Dbn map_block, map_blockoffs, startvbn;
  OZ_Hw_pageprot pageprot;
  OZ_Image_Refd_image *refd_image;
  OZ_Image_Secload *secload;
  OZ_Mapsecparam *mapsecparams;
  OZ_Mempage basevpage, dynpageoffs, hipage, map_vpage, npagem, svpage;
  OZ_Hw_pageprot *pageprots;
  OZ_Pointer dynaddroffs;
  OZ_Section_type sectype;
  OZ_Section *section;
  uByte *svaddr;
  uLong blocksize, exact, jmp_siz, map_vaddroffs, map_vpageoffs, mapsecflags, rel_ent, rel_siz, relaent, relasiz, sts, stblockoffs, stt;
  void *address;

  blocksize    = imageargs -> fs_getinfo1 -> blocksize;
  imagex       = NULL;
  mapsecparams = NULL;
  pageprots    = NULL;

  /* Make sure imageargs -> hdrbuf contains the whole Elf32 header */

  if (sizeof *elf_ex > OZ_IMAGE_HDRBUFSIZE) {
    oz_crash ("oz_knl_image_elf32: OZ IMAGE HDRBUFSIZE (%d) too small, must be at least %d\n", OZ_IMAGE_HDRBUFSIZE, sizeof *elf_ex);
  }

  elf_ex = (void *)(imageargs -> hdrbuf);

  /* Check for Elf magic number */

  if ((elf_ex -> e_ident[0] != 0x7f) 
   || (elf_ex -> e_ident[1] != 'E') 
   || (elf_ex -> e_ident[2] != 'L') 
   || (elf_ex -> e_ident[3] != 'F')) return (OZ_UNKIMAGEFMT);

  if ((elf_ex -> e_type != ET_EXEC) && (elf_ex -> e_type != ET_DYN)) {
    PRINTERROR ("oz_knl_image_elf32: %s: type %u not executable or dynamic\n", imageargs -> imagename, elf_ex -> e_type);
    return (OZ_BADIMAGEFMT);
  }
  if (elf_ex -> e_machine != EM_386) {
    PRINTERROR ("oz_knl_image_elf32: %s: machine %u not for 386\n", imageargs -> imagename, elf_ex -> e_machine);
    return (OZ_BADIMAGEFMT);
  }

  /* Now read in the program headers into imagex struct */

  if (elf_ex -> e_phentsize != sizeof *elf_phdata) {
    PRINTERROR ("oz_knl_image_elf32: %s: phentsize %d doesn't match sizeof Elf32_Phdr %d\n", 
	imageargs -> imagename, elf_ex -> e_phentsize, sizeof *elf_phdata);
    return (OZ_BADIMAGEFMT);
  }

  imagex = oz_sys_pdata_malloc (imageargs -> mprocmode, elf_ex -> e_phnum * sizeof *elf_phdata + sizeof *imagex);
  if (imagex == NULL) goto exquotapgp;
  memset (imagex, 0, sizeof *imagex);						// zero out base part of struct
  imagex -> phnum = elf_ex -> e_phnum;						// get number of program header table entries
  elf_phdata = imagex -> phdata;						// point to where they will go
  i = elf_ex -> e_phnum * sizeof *elf_phdata;					// get size of table in bytes
  if (elf_ex -> e_phoff + i <= OZ_IMAGE_HDRBUFSIZE) {				// see if it is within the imageargs -> hdrbuf
    memcpy (elf_phdata, ((char *)elf_ex) + elf_ex -> e_phoff, i);		// if so, just memcpy it
  } else {
    sts = oz_knl_image_read2 (imageargs, 					// if not, read from image file
                              elf_ex -> e_phoff / blocksize + 1, 
                              elf_ex -> e_phoff % blocksize, 
                              i, 
                              elf_phdata);
    if (sts != OZ_SUCCESS) goto cleanup;
  }

  /* Create sections - there is typically a read-only section and a read/write section              */
  /* The read/write section may have memsize > filesize, in which case we tack on zero fill section */

  mapsecparams = oz_sys_pdata_malloc (imageargs -> mprocmode, elf_ex -> e_phnum * 2 * sizeof *mapsecparams); // *2 for zero fill sections
  if (mapsecparams == NULL) {
    PRINTERROR ("oz_knl_image_elf32: %s: error mallocing mapsecparams[%d] array\n", imageargs -> imagename, elf_ex -> e_phnum);
    goto exquotapgp;
  }
  pageprots = oz_sys_pdata_malloc (imageargs -> mprocmode, elf_ex -> e_phnum * 2 * sizeof *pageprots); // *2 for zero fill sections
  if (pageprots == NULL) {
    PRINTERROR ("oz_knl_image_elf32: %s: error mallocing pageprots[%d] array\n", imageargs -> imagename, elf_ex -> e_phnum);
    goto exquotapgp;
  }
  mapsecflags = OZ_MAPSECTION_EXACT;
  numsections = 0;
  for (i = 0; i < elf_ex -> e_phnum; i ++) {
    elf_phpnt = elf_phdata + i;								/* point to program header element */
    if (elf_phpnt -> p_type != PT_LOAD) continue;					/* skip if not this type */
    if (elf_phpnt -> p_memsz == 0) continue;						/* skip if no size */

    map_vpage = OZ_HW_VADDRTOVPAGE (elf_phpnt -> p_vaddr);				/* get starting virtual page number */
    map_block = elf_phpnt -> p_offset / blocksize + 1;					/* get starting disk block number */
    map_vaddroffs = (elf_phpnt -> p_vaddr & ((1 << OZ_HW_L2PAGESIZE) - 1));		/* get offset in the starting page */
    map_blockoffs = elf_phpnt -> p_offset % blocksize;					/* get offset in the starting disk block */

    if (elf_phpnt -> p_filesz > elf_phpnt -> p_memsz) {					/* disk image can't be longer than memory image */
      PRINTERROR ("oz_knl_image_elf32: %s: section file size %u, memory size %u\n", 
		imageargs -> imagename, elf_phpnt -> p_filesz, elf_phpnt -> p_memsz);
      goto badimagefmt;
    }

    if (map_blockoffs > map_vaddroffs) {						/* disk blocks cant be bigger than memory pages */
      PRINTERROR ("oz_knl_image_elf32: %s: disk block offset %u larger than virtual page offset %u\n", 
		imageargs -> imagename, map_blockoffs, map_vaddroffs);
      goto badimagefmt;
    }

    map_vaddroffs -= map_blockoffs;							/* calculate virtual address offset at beginning of disk block */
    if (map_vaddroffs % blocksize != 0) {						/* it better be an exact multiple of disk blocks */
      PRINTERROR ("oz_knl_image_elf32: %s: section at vaddr %x at offset %u doesn't line up on a disk block\n", 
		imageargs -> imagename, elf_phpnt -> p_vaddr, elf_phpnt -> p_offset);
      goto badimagefmt;
    }

    if (map_vaddroffs / blocksize >= map_block) {					/* make sure the next subtract stays > 0 */
      PRINTERROR ("oz_knl_image_elf32: %s: section at vaddr %x at offset %u starts before beginning of file\n", 
		imageargs -> imagename, elf_phpnt -> p_vaddr, elf_phpnt -> p_offset);
      goto badimagefmt;
    }
 
    map_block -= map_vaddroffs / blocksize;						/* get block number that maps beginning of first memory page */

    svpage = OZ_HW_VADDRTOVPAGE (elf_phpnt -> p_vaddr);					/* virtual memory page to start at */
    npagem = OZ_HW_VADDRTOVPAGE (elf_phpnt -> p_vaddr + elf_phpnt -> p_filesz + (1 << OZ_HW_L2PAGESIZE) - 1) - svpage; /* number of pages in file to map */
    if (imageargs -> sysimage != 2) {							/* don't bother though if reading kernel symbol table */
      sts = oz_knl_section_create (imageargs -> iochan, npagem, map_block, 0, imageargs -> secattr, &(mapsecparams[numsections].section));
      if (sts != OZ_SUCCESS) {
        PRINTERROR ("oz_knl_image_elf32: %s: error %u creating %u page section for vbn %u\n", imageargs -> imagename, sts, npagem, startvbn);
        goto cleanup;
      }
    }

    if (numsections == 0) basevpage = svpage;

    mapsecparams[numsections].npagem    = npagem;
    mapsecparams[numsections].svpage    = svpage;
    mapsecparams[numsections].ownermode = imageargs -> procmode;
    mapsecparams[numsections].pageprot  = OZ_HW_PAGEPROT_KW;

    if (basevpage == 0) {								/* check for dynamic image */
      if (imageargs -> sysimage != 0) {
        mapsecflags = OZ_HW_DVA_SIMAGES_AT;						/*   system dynamic images go here */
        mapsecparams[numsections].svpage += OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_SIMAGES_VA);
      } else {
        mapsecflags = OZ_HW_DVA_PIMAGES_AT;						/*   process dynamic images go here */
        mapsecparams[numsections].svpage += OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_PIMAGES_VA);
      }
    }

    pageprots[numsections] = OZ_HW_PAGEPROT_UR;
    if (elf_phpnt -> p_flags & PF_W) {
      pageprots[numsections] = imageargs -> sysimage ? OZ_HW_PAGEPROT_KW : OZ_HW_PAGEPROT_UW;
    }

    numsections ++;

    /* Maybe a zero-fill section is required */

    npagem = OZ_HW_VADDRTOVPAGE (elf_phpnt -> p_vaddr + elf_phpnt -> p_memsz + (1 << OZ_HW_L2PAGESIZE) - 1) - svpage;
    if (npagem > mapsecparams[numsections-1].npagem) {
      npagem -= mapsecparams[numsections-1].npagem;
      sts = oz_knl_section_create (NULL, npagem, 0, OZ_SECTION_TYPE_ZEROES, imageargs -> secattr, &(mapsecparams[numsections].section));
      if (sts != OZ_SUCCESS) {
        PRINTERROR ("oz_knl_image_elf32: %s: error %u creating %u page zero fill section\n", imageargs -> imagename, sts, npagem);
        goto cleanup;
      }

      mapsecparams[numsections].npagem    = npagem;
      mapsecparams[numsections].svpage    = mapsecparams[numsections-1].npagem + mapsecparams[numsections-1].svpage;
      mapsecparams[numsections].ownermode = imageargs -> procmode;
      mapsecparams[numsections].pageprot  = OZ_HW_PAGEPROT_KW;

      pageprots[numsections] = pageprots[numsections-1];

      numsections ++;
    }
  }

  /* Map the sections to memory preserving their relative addresses.  Don't bother */
  /* though if reading kernel symbols, as the loader already read the kernel in.   */

  if (imageargs -> sysimage != 2) {

    /* Map them to memory */

    sts = oz_knl_process_mapsections ((basevpage == 0) ? 0 : OZ_MAPSECTION_EXACT, numsections, mapsecparams);
    if (sts != OZ_SUCCESS) {
      PRINTERROR ("oz_knl_image_elf32: %s: error %u mapping sections to memory\n", imageargs -> imagename, sts);
      goto cleanup;
    }
    dynpageoffs = mapsecparams[0].svpage - basevpage;			/* see how far the image was moved */
    dynaddroffs = dynpageoffs << OZ_HW_L2PAGESIZE;

    /* For those sections that had zero filling, we must manually zero out the last part of the last file section page */

    for (i = 0; i < elf_ex -> e_phnum; i ++) {
      OZ_Pointer end_of_file_data, end_of_file_page, end_of_zero_data;

      elf_phpnt = elf_phdata + i;							// point to program header element
      if (elf_phpnt -> p_type != PT_LOAD) continue;					// skip if not this type
      if (elf_phpnt -> p_filesz >= elf_phpnt -> p_memsz) continue;			// skip if no filling to do
      end_of_file_data = elf_phpnt -> p_vaddr + dynaddroffs + elf_phpnt -> p_filesz;	// this is where file data ends
      end_of_zero_data = elf_phpnt -> p_vaddr + dynaddroffs + elf_phpnt -> p_memsz;	// this is where zeroes end
      end_of_file_page = (end_of_file_data + (1 << OZ_HW_L2PAGESIZE) - 1) & -(1 << OZ_HW_L2PAGESIZE); // get end of page containing file
      if (end_of_zero_data > end_of_file_page) end_of_zero_data = end_of_file_page;	// chop our zeroes off there
											// OZ_SECTION_TYPE_ZEROES will fill the rest
      memset ((void *)end_of_file_data, 0, end_of_zero_data - end_of_file_data);	// zero the last part of that page (after faulting in the whole page)
    }
  } else {
    dynpageoffs = OZ_HW_VADDRTOVPAGE (OZ_LDR_KNLBASEVA) - mapsecparams[0].svpage;	// this is how far kernel was relocated
    dynaddroffs = dynpageoffs << OZ_HW_L2PAGESIZE;
    for (i = 0; i < numsections; i ++) mapsecparams[i].svpage += dynpageoffs;
  }

  /* Build list of mapped sections */

  for (i = 0; i < numsections; i ++) {
    secload = oz_sys_pdata_malloc (imageargs -> mprocmode, sizeof *secload); /* put on list of sections that were loaded for the image */
    if (secload == NULL) goto exquotapgp;
    memset (secload, 0, sizeof *secload);
    secload -> next     = imageargs -> secloads;
    secload -> section  = mapsecparams[i].section;
    secload -> npages   = mapsecparams[i].npagem;
    secload -> svpage   = mapsecparams[i].svpage;
    secload -> writable = (pageprots[i] != OZ_HW_PAGEPROT_UR);
    imageargs -> secloads = secload;

    mapsecparams[i].section = NULL;					/* don't clean it up */

    svaddr = OZ_HW_VPAGETOVADDR (mapsecparams[i].svpage);		/* save base image address = lowest of any sections */
    if ((imageargs -> baseaddr == NULL) || (svaddr < ((uByte *)(imageargs -> baseaddr)))) imageargs -> baseaddr = svaddr;
  }

  /* Return start address */

  imageargs -> startaddr = NULL;
  if (elf_ex -> e_entry != 0) {
    imageargs -> startaddr = (void *)(elf_ex -> e_entry + dynaddroffs);
  }

  /* Set up symbol table lookup parameters */

  imagex -> hash_addr   = NULL;						/* location of hash table */
  imagex -> symtab_size = 0;						/* number of elements found in symtab */
  imagex -> symtab_addr = NULL;						/* address of symbol table */
  imagex -> strtab_size = 0;						/* number of bytes in strtab */
  imagex -> strtab_addr = NULL;						/* address of string table */

  for (i = 0; i < elf_ex -> e_phnum; i ++) {
    elf_phpnt = elf_phdata + i;						/* point to program header element */
    if (elf_phpnt -> p_type != PT_DYNAMIC) continue;			/* skip if not this type */
    elf_dynpnt = (void *)(elf_phpnt -> p_vaddr + dynaddroffs);		/* point to dynamic table */
    if (valid_address (numsections, mapsecparams, elf_dynpnt, 0) < elf_phpnt -> p_memsz) {
      PRINTERROR ("oz_knl_image_elf32: %s: PT_DYNAMIC vaddr %p size %u not valid\n", 
	imageargs -> imagename, elf_dynpnt, elf_phpnt -> p_memsz);
      goto badimagefmt;
    }
    elf_dynpnte = elf_dynpnt + (elf_phpnt -> p_memsz / sizeof *elf_dynpnt); /* point to end of table */
    for (; (elf_dynpnt < elf_dynpnte) && (elf_dynpnt -> d_tag != DT_NULL); elf_dynpnt ++) { /* loop through table */
      switch (elf_dynpnt -> d_tag) {
        case DT_HASH: {
          imagex -> hash_addr = (void *)(elf_dynpnt -> d_un.d_ptr + dynaddroffs); // starting virtual address of hash table
          break;
        }
        case DT_STRTAB: {
          imagex -> strtab_addr = (void *)(elf_dynpnt -> d_un.d_ptr + dynaddroffs); /* starting virtual address of string table */
          break;
        }
        case DT_STRSZ: {
          imagex -> strtab_size = elf_dynpnt -> d_un.d_val;		/* size in bytes of string table */
          break;
        }
        case DT_SYMENT: {
          if (elf_dynpnt -> d_un.d_val != sizeof *(imagex -> symtab_addr)) { /* size of a single symtab entry */
            PRINTERROR ("oz_knl_image_elf32: %s: syment size %d not %d\n", 
		imageargs -> imagename, elf_dynpnt -> d_un.d_val, sizeof *(imagex -> symtab_addr));
            goto badimagefmt;
          }
          break;
        }
        case DT_SYMTAB: {
          imagex -> symtab_addr = (void *)(elf_dynpnt -> d_un.d_ptr + dynaddroffs); /* starting virt address of symbol table address */
          break;
        }
      }
    }
  }

  if (valid_address (numsections, mapsecparams, imagex -> strtab_addr, 0) < imagex -> strtab_size) {
    PRINTERROR ("oz_knl_image_elf32: %s: strtab %p size %u not valid\n", 
	imageargs -> imagename, imagex -> strtab_addr, imagex -> strtab_size);
    goto badimagefmt;
  }

  /* We have to get size of symbol table from hash table */

  if (imagex -> hash_addr != NULL) {
    if (valid_address (numsections, mapsecparams, imagex -> hash_addr, 0) < 2 * sizeof *(imagex -> hash_addr)) {
      PRINTERROR ("oz_knl_image_elf32: %s: hash table %p not valid\n", 
	imageargs -> imagename, imagex -> hash_addr);
      goto badimagefmt;
    }
    imagex -> symtab_size = imagex -> hash_addr[1];
    if (valid_address (numsections, mapsecparams, imagex -> symtab_addr, 1) < imagex -> symtab_size * sizeof *(imagex -> symtab_addr)) {
      PRINTERROR ("oz_knl_image_elf32: %s: symtab at %p size %u not valid\n", 
	imageargs -> imagename, imagex -> symtab_addr, imagex -> symtab_size);
      goto badimagefmt;
    }
  }

  /* All done if reading kernel image symbol table */

  if (imageargs -> sysimage == 2) {
    OZ_Pointer symbolval;

    if (!elf32_lookup (imagex, "oz_knl_image_elf32_load", &symbolval)) {
      oz_crash ("oz_knl_image_elf32_load: can't find symbol oz_knl_image_elf32_load");
    }
    if (symbolval != (OZ_Pointer)oz_knl_image_elf32_load) {
      oz_crash ("oz_knl_image_elf32_load: oz_knl_image_elf32_load is %X, should be %X", symbolval, (OZ_Pointer)oz_knl_image_elf32_load);
    }
    if (!elf32_lookup (imagex, "oz_sys_pdata_array", &symbolval)) {
      oz_crash ("oz_knl_image_elf32_load: can't find symbol oz_sys_pdata_array");
    }
    if (symbolval != (OZ_Pointer)oz_sys_pdata_array) {
      oz_crash ("oz_knl_image_elf32_load: oz_sys_pdata_array is %X, should be %X", symbolval, (OZ_Pointer)oz_sys_pdata_array);
    }
    goto success;
  }

  /* Load any dynamic images needed by this image */

  for (i = 0; i < elf_ex -> e_phnum; i ++) {
    elf_phpnt = elf_phdata + i;								/* point to program header element */
    if (elf_phpnt -> p_type != PT_DYNAMIC) continue;					/* skip if not this type */
    elf_dynpnt  = (void *)(elf_phpnt -> p_vaddr + dynaddroffs);				/* point to dynamic table */
    elf_dynpnte = elf_dynpnt + (elf_phpnt -> p_memsz / sizeof *elf_dynpnt);		/* point to end of table */
    for (; (elf_dynpnt < elf_dynpnte) && (elf_dynpnt -> d_tag != DT_NULL); elf_dynpnt ++) {
      if (elf_dynpnt -> d_tag == DT_NEEDED) {
        if (elf_dynpnt -> d_un.d_val >= imagex -> strtab_size) {
          PRINTERROR ("oz_knl_image_elf32: %s: bad DT_NEEDED string offset %u\n", imageargs -> imagename, elf_dynpnt -> d_un.d_val);
          goto badimagefmt;
        }
        refd_image = oz_sys_pdata_malloc (imageargs -> mprocmode, sizeof *refd_image);
        if (refd_image == NULL) goto exquotapgp;
        sts = oz_knl_image_load (imageargs -> procmode, 				/* owned by same procmode as this image */
                                 imagex -> strtab_addr + elf_dynpnt -> d_un.d_val, 	/* name is in string table at the given offset */
                                 imageargs -> sysimage, 				/* it is system image same as this one */
                                 imageargs -> level + 1, 				/* it is one level deeper than this image */
                                 NULL, NULL, &(refd_image -> image));			/* all we want back is image pointer */
        if (sts != OZ_SUCCESS) {
          PRINTERROR ("oz_knl_image_elf32: %s: error %u loading image %s\n", imageargs -> imagename, sts, imagex -> strtab_addr + elf_dynpnt -> d_un.d_val);
          oz_sys_pdata_free (imageargs -> mprocmode, refd_image);
          goto cleanup;
        }
        refd_image -> next       = imageargs -> refd_images;				/* link it so we will free it on close */
        imageargs -> refd_images = refd_image;
      }
    }
  }

  /* Look up undefined symbols in dynamic images      */
  /* Also relocate my own symbols by load offset      */
  /* Basically, this converts all entries to absolute */

  for (i = 0; i < imagex -> symtab_size; i ++) {

    /* Point to symbol table array entry and get pointer to name string */

    elf_sym = imagex -> symtab_addr + i;
    if (elf_sym -> st_name >= imagex -> strtab_size) {
      PRINTERROR ("oz_knl_image_elf32: %s: symbol[%d] name index %u out of range\n", imageargs -> imagename, i, elf_sym -> st_name);
      goto badimagefmt;
    }
    if (elf_sym -> st_name == 0) continue;
    symbolname = imagex -> strtab_addr + elf_sym -> st_name;

    /* Undefined symbol - look for it in all our refd_images */

    if (elf_sym -> st_shndx == SHN_UNDEF) {
      for (refd_image = imageargs -> refd_images; refd_image != NULL; refd_image = refd_image -> next) {
        if (oz_knl_image_lookup (refd_image -> image, symbolname, (uLong *)&(elf_sym -> st_value))) break;
      }
      if (refd_image == NULL) {
        PRINTERROR ("oz_knl_image_elf32: %s: unable to resolve symbol %s\n", imageargs -> imagename, symbolname);
        goto badimagefmt;
      }
    }

    /* Shouldn't have any COMMON symbols - should have been resolved by linker */

    else if (elf_sym -> st_shndx == SHN_COMMON) {
      PRINTERROR ("oz_knl_image_elf32: %s: symbol %s type is COMMON\n", imageargs -> imagename, symbolname);
      goto badimagefmt;
    }

    /* Anything else that's not absolute is relocated by the loading offset */

    else if (elf_sym -> st_shndx != SHN_ABS) {
      elf_sym -> st_value += dynaddroffs;
    }
  }

  /* Now that all symbols are resolved, perform relocations */

  for (i = 0; i < elf_ex -> e_phnum; i ++) {
    elf_phpnt = elf_phdata + i;
    if (elf_phpnt -> p_type != PT_DYNAMIC) continue;
    elf_dynpnt  = (void *)(elf_phpnt -> p_vaddr + dynaddroffs);
    elf_dynpnte = elf_dynpnt + (elf_phpnt -> p_memsz / sizeof *elf_dynpnt);
    jmp_pnt = NULL;
    jmp_siz = 0;
    rel_pnt = NULL;
    rel_siz = 0;
    rel_ent = 0;
    relapnt = NULL;
    relasiz = 0;
    relaent = 0;
    for (; (elf_dynpnt < elf_dynpnte) && (elf_dynpnt -> d_tag != DT_NULL); elf_dynpnt ++) {

      /* Scan through table, looking for pairs (JMPREL,PLTRELSZ) and triplets (REL,RELSZ,RELENT or RELA,RELASZ,RELAENT) */

      switch (elf_dynpnt -> d_tag) {
        case DT_JMPREL: {
          jmp_pnt = (void *)(elf_dynpnt -> d_un.d_ptr + dynaddroffs);
          break;
        }
        case DT_PLTRELSZ: {
          jmp_siz = elf_dynpnt -> d_un.d_val;
          break;
        }
        case DT_REL: {
          rel_pnt = (void *)(elf_dynpnt -> d_un.d_ptr + dynaddroffs);
          break;
        }
        case DT_RELSZ: {
          rel_siz = elf_dynpnt -> d_un.d_val;
          break;
        }
        case DT_RELENT: {
          rel_ent = elf_dynpnt -> d_un.d_val;
          if (rel_ent != sizeof *rel_pnt) {
            PRINTERROR ("oz_knl_image_elf32: %s: DT_RELENT was %d not %d\n", imageargs -> imagename, rel_ent, sizeof *rel_pnt);
            goto cleanup;
          }
          break;
        }
        case DT_RELA: {
          relapnt = (void *)(elf_dynpnt -> d_un.d_ptr + dynaddroffs);
          break;
        }
        case DT_RELASZ: {
          relasiz = elf_dynpnt -> d_un.d_val;
          break;
        }
        case DT_RELAENT: {
          relaent = elf_dynpnt -> d_un.d_val;
          if (relaent != sizeof *relapnt) {
            PRINTERROR ("oz_knl_image_elf32: %s: DT_RELAENT was %d not %d\n", imageargs -> imagename, relaent, sizeof *relapnt);
            goto cleanup;
          }
          break;
        }
      }

      /* If we have found both of DT_JMPREL and DT_PLTRELSZ, process the table */

      if ((jmp_pnt != NULL) && (jmp_siz != 0)) {
        if (!procrel_table (jmp_siz, jmp_pnt, numsections, mapsecparams, dynaddroffs, imageargs, imagex)) goto badimagefmt;
        jmp_pnt = NULL;
        jmp_siz = 0;
      }

      /* If we have found all of DT_REL, DT_RELSZ, DT_RELENT, process the table */

      if ((rel_pnt != NULL) && (rel_siz != 0) && (rel_ent != 0)) {
        if (!procrel_table (rel_siz, rel_pnt, numsections, mapsecparams, dynaddroffs, imageargs, imagex)) goto badimagefmt;
        rel_pnt = NULL;
        rel_siz = 0;
      }

      /* If we have found all of DT_RELA, DT_RELASZ, DT_RELAENT, process the entry */

      if ((relapnt != NULL) && (relasiz != 0) && (relaent != 0)) {
        if (!procrelatable (relasiz, relapnt, numsections, mapsecparams, dynaddroffs, imageargs, imagex)) goto badimagefmt;
        relapnt = NULL;
        relasiz = 0;
      }
    }
  }

  /* Now set final page protections */

  for (i = 0; i < numsections; i ++) {
    if (pageprots[i] != OZ_HW_PAGEPROT_KW) {
      sts = oz_knl_section_setpageprot (mapsecparams[i].npagem, mapsecparams[i].svpage, pageprots[i], 0, NULL);
      if (sts != OZ_SUCCESS) {
        PRINTERROR ("oz_knl_image_elf32: %s: error %u setting pageprot of %X at %X to %u\n", 
		imageargs -> imagename, mapsecparams[i].npagem, mapsecparams[i].svpage, pageprots[i]);
        goto cleanup;
      }
    }
  }

  /* Successful */

success:
  imageargs -> imagex = imagex;			/* return pointer to image extension struct */
  sts = OZ_SUCCESS;				/* return successful status */
  goto cleanup_temp;

  /* Error exits */

r_offset_invalid:
  PRINTERROR ("oz_knl_image_elf32: %s: reloc target %p not valid\n", imageargs -> imagename, address);
  if ((rel_pnt != NULL) && (rel_siz != 0) && (rel_ent != 0)) {
    PRINTERROR ("oz_knl_image_elf32: %s: rel_pnt %p -> r_offset %X, r_info %X\n", imageargs -> imagename, rel_pnt, rel_pnt -> r_offset, rel_pnt -> r_info);
  }
  if ((relapnt != NULL) && (relasiz != 0) && (relaent != 0)) {
    PRINTERROR ("oz_knl_image_elf32: %s: relapnt %p -> r_offset %X, r_info %X, r_addend %X\n", imageargs -> imagename, relapnt, relapnt -> r_offset, relapnt -> r_info, relapnt -> r_addend);
  }
badimagefmt:
  sts = OZ_BADIMAGEFMT;
  goto cleanup;
exquotapgp:
  sts = OZ_EXQUOTAPGP;
  goto cleanup;

  /* Clean up all temp and failed data */

cleanup:
  while ((refd_image = imageargs -> refd_images) != NULL) {
    imageargs -> refd_images = refd_image -> next;
    oz_knl_image_increfc (refd_image -> image, -1);
  }
  for (i = 0; i < numsections; i ++) {
    if (mapsecparams[i].section != NULL) {
      oz_knl_section_increfc (mapsecparams[i].section, -1);
    }
  }
  if (imagex != NULL) oz_sys_pdata_free (imageargs -> mprocmode, imagex);
cleanup_temp:
  if (mapsecparams != NULL) oz_sys_pdata_free (imageargs -> mprocmode, mapsecparams);
  if (pageprots    != NULL) oz_sys_pdata_free (imageargs -> mprocmode, pageprots);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Process relocation table						*/
/*									*/
/*    Input:								*/
/*									*/
/*	rel_siz = size of table (in bytes)				*/
/*	rel_pnt = points to table					*/
/*	numsections = number of entries in mapsecparams			*/
/*	mapsecparams = address limits of image sections			*/
/*	dynaddroffs = dynamic image address offset			*/
/*	imageargs = image argument list					*/
/*	imagex = image argument list extension				*/
/*									*/
/*    Output:								*/
/*									*/
/*	procrel_table = 0 : failed, bad image format			*/
/*	                1 : success					*/
/*	relocations processed						*/
/*									*/
/************************************************************************/

static int procrel_table (uLong rel_siz, Elf32_Rel *rel_pnt, 
                          int numsections, OZ_Mapsecparam *mapsecparams, OZ_Pointer dynaddroffs, 
                          OZ_Image_Args *imageargs, Imagex *imagex)

{
  char *symbolname;
  Elf32_Sym  *elf_sym;
  int j;
  OZ_Image_Refd_image *refd_image;
  void *address;

  rel_siz /= sizeof *rel_pnt;							// convert byte size to number of entries
  while (rel_siz > 0) {								// repeat while there are entries to process
    address = (void *)(dynaddroffs + rel_pnt -> r_offset);			// get address to be wacked
    j = ELF32_R_SYM (rel_pnt -> r_info);					// get symbol table index
    if (j >= imagex -> symtab_size) {
      PRINTERROR ("oz_knl_image_elf32: %s: reloc %p sybol index %d bad\n", imageargs -> imagename, address, j);
      goto badimagefmt;
    }
    elf_sym = imagex -> symtab_addr + j;					// point to symbol table entry
    switch (ELF32_R_TYPE (rel_pnt -> r_info)) {					// dispatch on relocation type

      /* Nop */

      case R_386_NONE: {
        break;
      }

      /* Add symbol's value to memory location */

      case R_386_32: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address += elf_sym -> st_value;
        break;
      }

      /* Add symbol's value to memory location minus the address */

      case R_386_PC32: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address += elf_sym -> st_value - (OZ_Pointer)address;
        break;
      }

      /* Store symbol's value in the memory location */

      case R_386_COPY: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address  = elf_sym -> st_value;
        break;
      }

      /* Relocate by dynamic image load offset */

      case R_386_RELATIVE: {
        if (dynaddroffs != 0) {
          if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
          *(uLong *)address += dynaddroffs;
        }
        break;
      }

      /* Reference to symbol in subimage - Note that the symbol also has a definition in this image as a pointer */
      /* to the 'jmp *' instruction.  When we find it in subimage, we replace current symbol's value with        */
      /* subimages as the subimage one is equivalent and would be faster for any images subsequently loaded.     */

      case R_386_JMP_SLOT: {

        /* Make sure address being relocated is valid */

        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;

        /* Point to symbol name string in string table */

        symbolname = imagex -> strtab_addr + elf_sym -> st_name;

        /* Scan all refd_images for the symbol */

        for (refd_image = imageargs -> refd_images; refd_image != NULL; refd_image = refd_image -> next) {
          if (oz_knl_image_lookup (refd_image -> image, symbolname, (uLong *)&(elf_sym -> st_value))) break;
        }

        /* Too bad if not found */

        if (refd_image == NULL) {
          PRINTERROR ("oz_knl_image_elf32: %s: unable to resolve symbol %s\n", imageargs -> imagename, symbolname);
          goto badimagefmt;
        }

        /* Store new value in table */

        *(uLong *)address = elf_sym -> st_value;
        break;
      }

      /* Who knows what */

      default: {
        PRINTERROR ("oz_knl_image_elf32: %s: %p unknown reloc type %u\n", imageargs -> imagename, address, ELF32_R_TYPE (rel_pnt -> r_info));
        goto badimagefmt;
      }
    }
    rel_siz --;
    rel_pnt ++;
  }
  return (1);

r_offset_invalid:
  PRINTERROR ("oz_knl_image_elf32: %s: reloc target %p not valid\n", imageargs -> imagename, address);
  PRINTERROR ("oz_knl_image_elf32: %s: rel_pnt %p -> r_offset %X, r_info %X\n", imageargs -> imagename, rel_pnt, rel_pnt -> r_offset, rel_pnt -> r_info);
badimagefmt:
  return (0);
}

/************************************************************************/
/*									*/
/*  Same as procrel_table, but for RELA style pointers			*/
/*									*/
/************************************************************************/

static int procrelatable (uLong relasiz, Elf32_Rela *relapnt, 
                          int numsections, OZ_Mapsecparam *mapsecparams, OZ_Pointer dynaddroffs, 
                          OZ_Image_Args *imageargs, Imagex *imagex)

{
  char *symbolname;
  Elf32_Sym  *elf_sym;
  int j;
  OZ_Image_Refd_image *refd_image;
  void *address;

  relasiz /= sizeof *relapnt;							// convert byte size to number of entries
  while (relasiz > 0) {								// repeat while there are entries to process
    address = (void *)(dynaddroffs + relapnt -> r_offset);			// get address to be wacked
    j = ELF32_R_SYM (relapnt -> r_info);					// get symbol table index
    if (j >= imagex -> symtab_size) {
      PRINTERROR ("oz_knl_image_elf32: %s: reloc at %p sybol index %d bad\n", imageargs -> imagename, address, j);
      goto badimagefmt;
    }
    elf_sym = imagex -> symtab_addr + j;					// point to symbol table entry
    switch (ELF32_R_TYPE (relapnt -> r_info)) {					// dispatch on relocation type

      /* Nop */

      case R_386_NONE: {
        break;
      }

      /* Add symbol's value to memory location */

      case R_386_32: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address = elf_sym -> st_value + relapnt -> r_addend;
        break;
      }

      /* Add symbol's value to memory location minus the address */

      case R_386_PC32: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address = elf_sym -> st_value - (OZ_Pointer)address + relapnt -> r_addend;
        break;
      }

      /* Store symbol's value in the memory location */

      case R_386_COPY: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address = elf_sym -> st_value;
        break;
      }

      /* Relocate by dynamic image load offset */

      case R_386_RELATIVE: {
        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;
        *(uLong *)address += dynaddroffs + relapnt -> r_addend;
        break;
      }

      /* Reference to symbol in subimage - Note that the symbol also has a definition in this image as a pointer */
      /* to the 'jmp *' instruction.  When we find it in subimage, we replace current symbol's value with        */
      /* subimages as the subimage one is equivalent and would be faster for any images subsequently loaded.     */

      case R_386_JMP_SLOT: {

        /* Make sure address being relocated is valid */

        if (valid_address (numsections, mapsecparams, address, 1) < sizeof (uLong)) goto r_offset_invalid;

        /* Point to symbol name string in string table */

        symbolname = imagex -> strtab_addr + elf_sym -> st_name;

        /* Scan all refd_images for the symbol */

        for (refd_image = imageargs -> refd_images; refd_image != NULL; refd_image = refd_image -> next) {
          if (oz_knl_image_lookup (refd_image -> image, symbolname, (uLong *)&(elf_sym -> st_value))) break;
        }

        /* Too bad if not found */

        if (refd_image == NULL) {
          PRINTERROR ("oz_knl_image_elf32: %s: unable to resolve symbol %s\n", imageargs -> imagename, symbolname);
          goto badimagefmt;
        }

        /* Store new value in table */

        *(uLong *)address = elf_sym -> st_value;
        break;
      }

      /* Who knows what */

      default: {
        PRINTERROR ("oz_knl_image_elf32: %s: %p unknown reloc type %u\n", imageargs -> imagename, address, ELF32_R_TYPE (relapnt -> r_info));
        goto badimagefmt;
      }
    }
    relasiz --;
    relapnt ++;
  }
  return (1);

r_offset_invalid:
  PRINTERROR ("oz_knl_image_elf32: %s: reloc target %p not valid\n", imageargs -> imagename, address);
  PRINTERROR ("oz_knl_image_elf32: %s: relapnt %p -> r_offset %X, r_info %X\n", imageargs -> imagename, relapnt, relapnt -> r_offset, relapnt -> r_info);
badimagefmt:
  return (0);
}

/* Return length of data available at the given address */

static uLong valid_address (int numsections, OZ_Mapsecparam *mapsecparams, void *vaddr, int writable)

{
  Elf32_Phdr *phpnt;
  int i;
  OZ_Mempage spage, vpage;

  vpage = OZ_HW_VADDRTOVPAGE (vaddr);						// get page number the address is in
  for (i = numsections; -- i >= 0;) {						// loop through all we mapped to memory
    spage  = mapsecparams[i].svpage;						// get the starting page number
    if (spage > vpage) continue;						// if it starts after the address, skip it
    spage += mapsecparams[i].npagem;						// add number of pages mapped
    if (spage <= vpage) continue;						// if it ends before the address, skip it
    if (writable && (mapsecparams[i].pageprot == OZ_HW_PAGEPROT_UR)) break;	// if we need writable and it's read-only, it fails
    return ((OZ_Pointer)(OZ_HW_VPAGETOVADDR (spage)) - (OZ_Pointer)vaddr);	// ok, calc how many bytes from address to its end
  }
  return (0);									// nothing matched, return a zero length
}

/************************************************************************/
/*									*/
/*  Look up a symbol in the symbol table				*/
/*									*/
/*    Input:								*/
/*									*/
/*	imagexv = image extension as returned by the load routine	*/
/*	symname = symbol name (null terminated string)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	elf32_lookup = 0 : symbol not found				*/
/*	               1 : symbol found					*/
/*	*symvalu = symbol value						*/
/*									*/
/************************************************************************/

static int elf32_lookup (void *imagexv, char *symname, OZ_Pointer *symvalu)

{
  char *strp;
  Elf32_Sym *symp;
  Imagex *imagex;
  uLong i, nameoffs;

  imagex = imagexv;					/* point to imagex struct */

  for (i = 0; i < imagex -> symtab_size; i ++) {	/* scan the symbol table (SYMTAB) */
    symp = imagex -> symtab_addr + i;			/* point to a SYMTAB entry */
    if ((symp -> st_info != ELF32_ST_INFO (STB_GLOBAL, STT_NOTYPE)) 
     && (symp -> st_info != ELF32_ST_INFO (STB_GLOBAL, STT_OBJECT)) 
     && (symp -> st_info != ELF32_ST_INFO (STB_GLOBAL, STT_FUNC))) continue;
    nameoffs = symp -> st_name;
    if (nameoffs >= imagex -> strtab_size) continue;
    strp = imagex -> strtab_addr + nameoffs;		/* point to corresponding string */
    if (strcmp (strp, symname) == 0) {			/* see if the string matches */
      *symvalu = symp -> st_value;			/* if so, return the value */
      return (1);					/* ... and a success status */
    }
  }
  return (0);						/* can't find the symbol, return failure */
}

/************************************************************************/
/*									*/
/*  Finish unloading an image						*/
/*  The sections have all been unmapped					*/
/*  All that remains is to clean up the imagex struct			*/
/*									*/
/************************************************************************/

static void elf32_unload (void *imagexv, OZ_Procmode mprocmode)

{
  if (imagexv != NULL) oz_sys_pdata_free (mprocmode, imagexv);
}

/************************************************************************/
/*									*/
/*  Scan the image's symbol table					*/
/*									*/
/*    Input:								*/
/*									*/
/*	imagexv = image extension as returned by the load routine	*/
/*	lastsym = NULL : start at the beginning				*/
/*	          else : value returned by last call			*/
/*									*/
/*    Output:								*/
/*									*/
/*	elf32_symscan = NULL : no more symbols				*/
/*	                else : scan context				*/
/*	*symname_r = points to null terminated symbol name string	*/
/*	*symvalu_r = symbol value					*/
/*									*/
/************************************************************************/

static void *elf32_symscan (void *imagexv, void *lastsym, char **symname_r, OZ_Pointer *symvalu_r)

{
  Elf32_Sym *symp;
  Imagex *imagex;
  uLong i, nameoffs;

  imagex = imagexv;

  i = 0;
  symp = lastsym;
  if (symp != NULL) i = symp - imagex -> symtab_addr + 1;

  for (; i < imagex -> symtab_size; i ++) {
    symp = imagex -> symtab_addr + i;
    if ((symp -> st_info != ELF32_ST_INFO (STB_GLOBAL, STT_NOTYPE)) 
     && (symp -> st_info != ELF32_ST_INFO (STB_GLOBAL, STT_OBJECT)) 
     && (symp -> st_info != ELF32_ST_INFO (STB_GLOBAL, STT_FUNC))) continue;
    nameoffs = symp -> st_name;
    if (nameoffs >= imagex -> strtab_size) continue;
    *symname_r = imagex -> strtab_addr + nameoffs;
    *symvalu_r = symp -> st_value;
    return (symp);
  }
  return (NULL);
}
