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
/*  Link Elf32 binaries into an image					*/
/*									*/
/*  There are two flaws in the standard ld when dealing with 		*/
/*  shareables.								*/
/*									*/
/*    One is if the shareable defines:					*/
/*									*/
/*		.globl	onetwothree					*/
/*		.type	onetwothree,@object				*/
/*		onetwothree = 0x123					*/
/*									*/
/*	Any referencing image will get a *relocated* value for 		*/
/*	onetwothree.							*/
/*									*/
/*	So in here, we export absolute symbols as SHN_ABS, any 		*/
/*	relocatable symbols get exported with appropriate shndx		*/
/*									*/
/*   The other is:							*/
/*									*/
/*		.globl	onetwothree					*/
/*		.type	onetwothree,@object				*/
/*	onetwothree: .long 0x123					*/
/*									*/
/*	Any referencing image has the reference run through the global 	*/
/*	offset table so it picks up a garbage value.  Plus, it seems, 	*/
/*	that if you have more than one (like a fourfivesix), they all 	*/
/*	'share' the same location in the got.				*/
/*									*/
/*	So in here, we output relocs for @objects in place instead of 	*/
/*	going through the got.						*/
/*									*/
/*   A bug is that it won't link dynamics at a fixed address, it gives 	*/
/*   some error like it requires 4 headers but only allocated 3.	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"

#include <ar.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PS   (1 << OZ_HW_L2PAGESIZE)
#define PSM1 (PS - 1)
#define NPS  (-PS)

typedef enum { OT_REL, OT_LIB, OT_DYN, OT_DIN } Ot;
typedef enum { SV_ABS, SV_REL, SV_DYN } Symbolval;

typedef struct Comsym Comsym;
typedef struct Export Export;
typedef struct Global Global;
typedef struct Got Got;
typedef struct Mapdat Mapdat;
typedef struct Objfile Objfile;
typedef struct Secinfo Secinfo;
typedef struct Stringtable Stringtable;

	/* Symbols in '.common' */

struct Comsym { Comsym *next;
                char const *namestr;
                Elf32_Addr addr;	// assigned address within .common section
                Elf32_Addr align;	// required alignment
                Elf32_Addr size;	// space required
              };

	/* One entry in the global_array per global symbol definition */

struct Global { char const *namestr;	// points to name string
                Objfile *objfile;	// which object file it comes from
                Elf32_Half hdrindx;	// index of that object file's symbol table header
                Elf32_Half symindx;	// index of the symbol within that section
                Elf32_Sym *sym_ent;	// pointer to symbol entry in object file
                int dynsymi;		// where it was put in dynsym_array (or 0 if not)
                int got_index;		// index in GOT (0 if none yet)
                unsigned char symbind;	// ELF32_ST_BIND value (STB_GLOBAL or STB_WEAK)
                unsigned char refd;	// 0: not referenced by an undefined; 1: referenced by an undefined
              };

	/* One entry in global offset table */

#ifdef OZ_HW_TYPE_486
struct Got { unsigned char opcode;
             unsigned char disp32[4];
             unsigned char filler[3];
           };
#define GOT_RELTYPE R_386_PC32
#define GOT_FIELD disp32
static Got const got_init = { 0xE9, -4, -1, -1, -1, 0x90, 0x90, 0x90 };	// jmp . ; nop ; nop ; nop
#endif

	/* Used to print sections in map file */

typedef enum { MD_SEC, MD_GBL } Mapdattype;

struct Mapdat { Mapdattype md;
                union { struct { Objfile *objfile;
                                 int shndx;
                                 Elf32_Shdr *shdr;
                               } sec;
                        struct { Global *global_entry;
                               } gbl;
                      } u;
              };

	/* One entry in the objects list per input file                        */
	/* Also, entries are added for each library module that gets included  */
	/* Also, an entry is added on the front for the output program headers */
	/* Also, an entry is added on the end for the output section headers   */

struct Objfile { Objfile *next;			// next in 'objfiles' list; NULL to terminate
                 char const *name;		// file's name
                 int fd;			// object file
                 struct stat statbuf;		// stat of the file
                 Ot ot;				// object file type: individual, library, dynamic (excluded, included)
                 void *data;			// mmap address of beginning of file
                 Secinfo *si;			// array parallels the file's section header table (not in OT_LIB)
                 int dtneeded;			// index of DT_NEEDED entry, if any
               };

struct Secinfo { Elf32_Word si_outshndx;	// output section header index it's assigned to
                 Elf32_Off  si_outsoffs;	// offset in the output section data
               };

	/* Generic string table */

struct Stringtable { int index;			// currently used amount
                     int alloc;			// allocated amount
                     char *array;		// character array
                   };

	/* Internal object file - This is used to input stuff to the linking engine */

#define INTSHDROUTOFFS(shdr) \
   outshdr_array[intobjfile.si[&(shdr)-intobjdata.shdrs].si_outshndx].sh_offset \
 + intobjfile.si[&(shdr)-intobjdata.shdrs].si_outsoffs

static Objfile intobjfile;

static struct { Elf32_Ehdr ehdr;		// elf header
                Elf32_Shdr shdrs[12];		// section headers
              } intobjdata;

#define shdr_null intobjdata.shdrs[0]		// - null

#define shdr_header intobjdata.shdrs[1]		// - .header   : the 'fhout' structure, must end up at output file offset zero
						//               also, define_intobjsym assumes it is input section 1

#define shdr_hash intobjdata.shdrs[2]		// - .hash     : hash table for dynsym
  static Elf32_Word *hashtable;

#define shdr_dynsym intobjdata.shdrs[3]		// - .dynsym   : exported and imported symbols
  static Elf32_Sym *dynsym_array = NULL;		// array of output dynamic symbols
  static int dynsym_count = 0;				// number of elements in dynsym_array

#define shdr_dynstr intobjdata.shdrs[4]		// - .dynstr   : strings for dynsym
  static Stringtable dynstr;				// stringtable

#define shdr_dynamic intobjdata.shdrs[5]	// - .dynamic  : dynamic loading parameter table
  static Elf32_Dyn *dynamic_array = NULL;		// array of output dynamic entries
  static int dynamic_alloc = 0;				// number of elements allocated to dynamic_array
  static int dynamic_count = 0;				// number of elements used in dynamic_array

#define shdr_reldyn intobjdata.shdrs[6]		// - .rel.dyn  : relocation entries
  static Elf32_Rel *dynreloc_array = NULL;
  static int dynreloc_alloc = 0;
  static int dynreloc_count = 0;

#define shdr_got intobjdata.shdrs[7]		// - .got      : global offset table
  static Got *got_array = NULL;
  static int got_alloc = 0;
  static int got_count = 0;

#define shdr_shstrtab intobjdata.shdrs[8]	// - .shstrtab : strings for the above section names + all others we output
  static Stringtable shstrtab;				// string table for output section headers (.shstrtab)

#define shdr_common intobjdata.shdrs[9]		// - .common   : data allocated to our common

#define shdr_symtab intobjdata.shdrs[10]	// - .symtab   : defined symbols
  static Elf32_Sym *intobjsymtab_array;
  static int intobjsymtab_alloc = 0;
  static int intobjsymtab_count = 0;

#define shdr_symstr intobjdata.shdrs[11]	// - .symstr   : defined symbol strings
  static Stringtable intobjsymstr;

	/* This gets written at the beginning of the output file             */
	/* It contains the Elf header and the three-and-only program headers */

static struct { Elf32_Ehdr ehdr;		// standard elf header
                Elf32_Phdr phdr_ro;		// readonly program header
                Elf32_Phdr phdr_dyn;		// dynamic program header
                Elf32_Phdr phdr_rw;		// readwrite program header
              } fhout;

	/* Misc static data */

static char const *entryname = "_start";	// entrypoint symbol name
static char const *pn = "ldelf32";		// program name for error messages
static Comsym *comsyms;				// list of globals symbols that are defined as SHN_COMMON
static Elf32_Addr baseaddr;			// base address:  0=dynamic; else fixed
static int flag_base = 0;			// set if -base
static int flag_dynamic = 0;			// set if -dynamic
static int flag_strip = 0;			// set if -strip
static int showstopper = 0;			// if set, don't output anything
static Objfile **lobjfile, *objfiles;		// list of object files in command-line order + selected library modules on end

static int outsize;				// size of output array
static void *outdata;				// points to output file array

static Elf32_Shdr *outshdr_array;		// array of output section headers
static int outshdr_alloc;			// number of elements allocated to outshdr_array
static int outshdr_count;			// number of elements used in outshdr_array
static Elf32_Off outshdr_offs;			// starting offset in file

static Global *global_array;			// global symbol array
static int global_count;			// global symbol count

static char const *outname;			// output filename
static int outfd;				// output file descriptor

static char const *mapname;			// map filename (or NULL if none wanted)
static FILE *mapfile;

static Elf32_Off fileoffs;			// offset in file being assigned
static Elf32_Addr addroffs;			// address in image being assigned

static Objfile *mmapobjfile (char const *name);
static int search_library (Objfile *libfile, char const *symname);
static char *arfilename (struct ar_hdr const *arhdr, int filenamel, char const *filenames);
static int search_dynamic (Objfile *dynfile, char const *symname);
static void checkmultdef (Global *g1, Global *g2);
static void relocpass (int finalpass);
static void emit_dynreloc (Elf32_Addr rvad, int reloctype, char const *symname);
static Elf32_Addr assign_addresses (Elf32_Word (*selector) (Objfile *objfile, int shndx), int pagesplit);
static Elf32_Word aasel_allocro (Objfile *objfile, int shndx);
static Elf32_Word aasel_allocrw_copy (Objfile *objfile, int shndx);
static Elf32_Word aasel_allocrw_zfil (Objfile *objfile, int shndx);
static Elf32_Word aasel_symtab (Objfile *objfile, int shndx);
static Elf32_Word aasel_else (Objfile *objfile, int shndx);
static int find_equiv_outshdr (Objfile *objfile, Elf32_Shdr *shdr_tbl, Elf32_Shdr *shdr);
static Symbolval symbolval (Objfile *objfile, int symshndx, int symindex, Elf32_Addr *value_r, Symbolval ifrelbased);
static char const *sectioname (Objfile *objfile, int shndx);
static char const *symbolname (Objfile *objfile, int shndx, int symndx);
static int compare_globals (void const *gv1, void const *gv2);
static int compare_relocs (void const *rv1, void const *rv2);
static int compare_mapdats (void const *mdv1, void const *mdv2);
static int define_intobjsym (char const *name, Elf32_Addr value, int shndx);
static int output_dynamic (Elf32_Sword dtag, Elf32_Addr dval);
static int append_string (Stringtable *stringtable, char const *string);
static void fixsymtab (Objfile *objfile, int shndx);
static void writesymtab (Objfile *objfile, int shndx);
static void writeout (void *buff, int size, int offs);
static void compress_strtab (int tblindx, int strindx);

#define MALLOC(b) malloc (b) // wrap_malloc (__LINE__, b)
#define REALLOC(p,b) realloc (p, b) // wrap_realloc (__LINE__, p, b)
#define FREE(p) free (p) // wrap_free (__LINE__, p)

static void *wrap_malloc (int line, int bytes)

{
  uLong *p;

  p = malloc (bytes);
  printf ("wrap_malloc*: %d: %p = malloc (%d)  %u\n", line, p, bytes, p[-1] & -4);
  return (p);
}

static void *wrap_realloc (int line, void *oldmem, int bytes)

{
  uLong *p;

  p = realloc (oldmem, bytes);
  printf ("wrap_realloc*: %d: %p = realloc (%p, %d)  %u\n", line, p, oldmem, bytes, p[-1] & -4);
  return (p);
}

static void wrap_free (int line, void *p)

{
  free (p);
  printf ("wrap_free*: %d: free (%p)\n", line, p);
}

/************************************************************************/
/*									*/
/*  Linking consists of these steps, most of which have to occur in 	*/
/*  the order shown, as many things are interdependent.			*/
/*									*/
/*   1) init internal datafile struct					*/
/*   2) parse command line options, open object files			*/
/*   3) set up internal sections and symbols				*/
/*   4) select library modules and dynamic images, build global symbol table
/*   5) prune global symbol table and check for conflicts		*/
/*   6) create import/export symbol table (.dynsym)			*/
/*   7) output DT_NEEDED records					*/
/*   8) allocate slots for other DT_... records				*/
/*   9) create dynsym's hash table					*/
/*  10) create list of .common symbols & allocate space			*/
/*  11) reloc pass 1 to get size of .dyn.rel and .got tables		*/
/*  12) fill in internal object file section headers			*/
/*  13) assign address to all sections, fill in program headers		*/
/*  14) finalize internal object file symbols				*/
/*  15) purge out zero-sized sections					*/
/*  16) finalize import/export symbol table (.dynsym)			*/
/*  17) reloc pass 2 to generate .dyn.rel and .got table contents	*/
/*  18) finalize elf header contents					*/
/*  19) finalize DT_... record contents					*/
/*  20) create output file						*/
/*  21) write elf and program headers					*/
/*  22) write section contents						*/
/*  23) write section headers						*/
/*  24) close output file						*/
/*  25) write map file							*/
/*									*/
/************************************************************************/

int main (int argc, char *argv[])

{
  char const *cp;
  char *dynstrbuff, *p;
  Comsym *comsym;
  Elf32_Addr lastaddr, rdwraddr, zeroaddr;
  Elf32_Ehdr *ehdr;
  Elf32_Off prerwfileoffs;
  Elf32_Shdr *shdr, *shdr_tbl;
  Elf32_Sym *sym_ent;
  Global *global_entry, global_key;
  int addedsomething, i, j, rc;
  Objfile *libfile, *objfile;
  Secinfo *si;

  int dt_hash, dt_strtab, dt_symtab, dt_strsz, dt_rel, dt_relsz;
  int intobjsym_baseaddr, intobjsym_lastaddr, intobjsym_rdwraddr, intobjsym_zeroaddr;
  int symtab_index, strtab_index, stab_index, stabstr_index;

  if (argc > 0) {
    pn = argv[0];
    cp = strrchr (pn, '/');
    if (cp != NULL) pn = ++ cp;
  }

  /* Set up internal object file - this is used to house the sections that we create with symbol tables and relocations, etc */

  memset (&intobjfile, 0, sizeof intobjfile);
  intobjfile.name = "*INTERNAL*";
  intobjfile.fd   = -1;
  intobjfile.ot   = OT_REL;
  intobjfile.data = &intobjdata;
  intobjfile.si   = MALLOC (sizeof intobjdata.shdrs / sizeof intobjdata.shdrs[0] * sizeof *intobjfile.si);
  memset (intobjfile.si, 0, sizeof intobjdata.shdrs / sizeof intobjdata.shdrs[0] * sizeof *intobjfile.si);

  memset (&intobjdata, 0, sizeof intobjdata);
  intobjdata.ehdr.e_shoff     = (void *)intobjdata.shdrs - (void *)&intobjdata;
  intobjdata.ehdr.e_shentsize = sizeof intobjdata.shdrs[0];
  intobjdata.ehdr.e_shnum     = sizeof intobjdata.shdrs / sizeof intobjdata.shdrs[0];
  intobjdata.ehdr.e_shstrndx  = &shdr_shstrtab - intobjdata.shdrs;

  shdr_symtab.sh_name      = append_string (&shstrtab, ".symtab");	// Elf files may only have one SHT_SYMTAB
  shdr_symtab.sh_type      = SHT_SYMTAB;				// ... so use customary name here
  shdr_symtab.sh_link      = &shdr_symstr - intobjdata.shdrs;
  shdr_symtab.sh_addralign = 4;
  shdr_symtab.sh_entsize   = sizeof (Elf32_Sym);

  shdr_symstr.sh_name      = append_string (&shstrtab, ".strtab");	// ... and this must be customary, too
  shdr_symstr.sh_type      = SHT_STRTAB;
  shdr_symstr.sh_addralign = 1;

  objfiles = &intobjfile;
  lobjfile = &intobjfile.next;

  /* Collect filenames from command line */

  outname  = NULL;					// don't have output filename yet
  baseaddr = OZ_HW_PROCLINKBASE;			// default base address for normal executable
  mapname  = NULL;					// assume no map wanted

  for (i = 1; i < argc; i ++) {
    if (strcasecmp (argv[i], "-base") == 0) {		// -base specifies an alternate base address
      if (++ i >= argc) goto usage;
      flag_base = 1;
      baseaddr = strtoul (argv[i], &p, 16);
      if (*p != 0) goto usage;
      continue;
    }
    if (strcasecmp (argv[i], "-dynamic") == 0) {	// -dynamic also means default base of zero
      flag_dynamic = 1;
      if (!flag_base) baseaddr = 0;
      continue;
    }
    if (strcasecmp (argv[i], "-map") == 0) {		// -map specifies map file
      if (++ i >= argc) goto usage;
      mapname = argv[i];
      continue;
    }
    if (strcasecmp (argv[i], "-strip") == 0) {		// -strip omits output symbol tables
      flag_strip = 1;
      continue;
    }
    if (strcasecmp (argv[i], "-undef") == 0) {		// -undef specifies undefined symbol to pull in object module from library
      if (++ i >= argc) goto usage;
      define_intobjsym (argv[i], 0, SHN_UNDEF);
      continue;
    }
    if (argv[i][0] == '-') goto usage;			// no other options allowed
    if (outname == NULL) {				// see if we have output filename yet
      outname = argv[i];				// if not, save it
      continue;
    }
    objfile = mmapobjfile (argv[i]);			// all others are input files
    if (objfile == NULL) return (-1);
    *lobjfile = objfile;				// link on end of list
    lobjfile = &(objfile -> next);
  }
  *lobjfile = NULL;					// terminate input file list
  if (objfiles -> next == NULL) goto usage;		// make sure we got something (also guarantees we have output name)

  /* Define some internal symbols */

  intobjsym_baseaddr = define_intobjsym ("OZ_IMAGE_BASEADDR", 0, 1);	// set to image base address
  intobjsym_lastaddr = define_intobjsym ("OZ_IMAGE_LASTADDR", 0, 1);	// set to last address (inclusive)
  intobjsym_rdwraddr = define_intobjsym ("OZ_IMAGE_RDWRADDR", 0, 1);	// set to start of read/write section
  intobjsym_zeroaddr = define_intobjsym ("OZ_IMAGE_ZEROADDR", 0, 1);	// set to start of zeroed section

  /* Convert OT_LIBs and OT_RELs to possibly multiple OT_RELs as needed to resolve undefined symbols */
  /* This is how we select modules from libraries and include dynamic sub-images                     */

  global_array = NULL;
  do {

    /* Count the number of defined global symbols that we know about so far */

    global_count = 0;
    for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
      if ((objfile -> ot != OT_REL) && (objfile -> ot != OT_DIN)) continue;		// only look at 'individual' files
											// ... and included library modules
											// ... and included dynamic images
      ehdr = objfile -> data;								// point to obj file's elf header
      shdr_tbl = objfile -> data + ehdr -> e_shoff;					// point to obj file's section header table
      for (i = 0; i < ehdr -> e_shnum; i ++) {						// loop through each section header
        shdr = shdr_tbl + i;
        if (((objfile -> ot == OT_REL) && (shdr -> sh_type == SHT_SYMTAB)) 		// look for appropriate symbol table header
         || ((objfile -> ot == OT_DIN) && (shdr -> sh_type == SHT_DYNSYM))) {
          for (j = shdr -> sh_entsize; j < shdr -> sh_size; j += shdr -> sh_entsize) {	// loop through symbols
            sym_ent = objfile -> data + shdr -> sh_offset + j;				// point to symbol table entry
            if (sym_ent -> st_shndx == SHN_UNDEF) continue;				// skip undefined symbols
            if ((ELF32_ST_BIND (sym_ent -> st_info) == STB_GLOBAL) || (ELF32_ST_BIND (sym_ent -> st_info) == STB_WEAK)) {
              global_count ++;								// count if global or weak definition
            }
          }
        }
      }
    }

    /* Make an array of those globals for fast searching */

    global_array = REALLOC (global_array, global_count * sizeof *global_array);
    global_count = 0;
    for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
      if ((objfile -> ot != OT_REL) && (objfile -> ot != OT_DIN)) continue;		// only look at 'individual' files
											// ... and included library modules
											// ... and included dynamic images
      ehdr = objfile -> data;								// point to obj file's elf header
      shdr_tbl = objfile -> data + ehdr -> e_shoff;					// point to obj file's section header table
      for (i = 0; i < ehdr -> e_shnum; i ++) {						// loop through each section header
        shdr = shdr_tbl + i;
        if (((objfile -> ot == OT_REL) && (shdr -> sh_type == SHT_SYMTAB)) 		// look for appropriate symbol table header
         || ((objfile -> ot == OT_DIN) && (shdr -> sh_type == SHT_DYNSYM))) {
          for (j = shdr -> sh_entsize; j < shdr -> sh_size; j += shdr -> sh_entsize) {	// loop through symbols
            sym_ent = objfile -> data + shdr -> sh_offset + j;				// point to symbol table entry
            if (sym_ent -> st_shndx == SHN_UNDEF) continue;				// skip undefined symbols
            if ((ELF32_ST_BIND (sym_ent -> st_info) == STB_GLOBAL) || (ELF32_ST_BIND (sym_ent -> st_info) == STB_WEAK)) {
              global_array[global_count].objfile = objfile;				// fill in global defintion
              global_array[global_count].hdrindx = i;
              global_array[global_count].symindx = j / shdr -> sh_entsize;
              global_array[global_count].sym_ent = sym_ent;
              global_array[global_count].symbind = ELF32_ST_BIND (sym_ent -> st_info);
              global_array[global_count].namestr = symbolname (objfile, i, j / shdr -> sh_entsize);
              global_array[global_count].refd    = 0;
              global_count ++;
            }
          }
        }
      }
    }

    /* Sort it for quick searching */

    qsort (global_array, global_count, sizeof *global_array, compare_globals);

    /* For each unresolved undefined, look for it in libraries and dynamics                        */
    /* If found in a library, a new entry is made in the 'objects' list that looks like the module */
    /* If found in a dynamic, the dynamic's type is changed from OT_DYN to OT_DIN                  */

    addedsomething = 0;									// we haven't added any new globals so far this pass
    for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
      if ((objfile -> ot != OT_REL) && (objfile -> ot != OT_DIN)) continue;		// only look at 'individual' files
											// ... and included library modules
											// ... and included dynamic images
      ehdr = objfile -> data;								// point to obj file's elf header
      shdr_tbl = objfile -> data + ehdr -> e_shoff;					// point to obj file's section header table
      for (i = 0; i < ehdr -> e_shnum; i ++) {						// loop through each section header
        shdr = shdr_tbl + i;
        if (((objfile -> ot != OT_DIN) || (shdr -> sh_type != SHT_DYNSYM)) 		// skip if not 'symbol table'
         && ((objfile -> ot != OT_REL) || (shdr -> sh_type != SHT_SYMTAB))) continue;
        for (j = shdr -> sh_entsize * shdr -> sh_info; j < shdr -> sh_size; j += shdr -> sh_entsize) { // loop through non-local syms
          sym_ent = objfile -> data + shdr -> sh_offset + j;				// point to symbol table entry
          if (sym_ent -> st_shndx != SHN_UNDEF) continue;				// skip if not undefined
          global_key.namestr = symbolname (objfile, i, j / shdr -> sh_entsize);
          global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
          if (global_entry != NULL) {
            global_entry -> refd = 1;							// already defined, mark global as referenced
            continue;									//   then skip it
          }
          for (libfile = objfiles; libfile != NULL; libfile = libfile -> next) {	// try to find in dynamic or library
            rc = 0;
            switch (libfile -> ot) {
              case OT_LIB: {
                rc = search_library (libfile, global_key.namestr);			// library, search it
                if (rc > 0) addedsomething = 1;						// if a new module included, rescan to get all its globals
                break;
              }
              case OT_DIN: {
                rc = search_dynamic (libfile, global_key.namestr);			// included dynamic, search it
                break;
              }
              case OT_DYN: {
                rc = search_dynamic (libfile, global_key.namestr);			// excluded dynamic, search it
                addedsomething |= rc;							// if it's now included, rescan so we get all its globals
                break;
              }
            }
            if (rc != 0) break;
          }
        }
      }
    }

    /* Repeat if we added new globals (and potentially new undefineds) */
    /* This is how we pick up inter-library dependencies               */

  } while (addedsomething);

  /* Prune out weak symbols and check for multiple definitions */

  for (i = global_count; -- i > 0;) {

    /* Purge unreferenced symbols from dynamic images */

    if ((global_array[i].objfile -> ot == OT_DIN) && !global_array[i].refd) goto prune_i;

    /* Skip if names of this and the one before don't match */

    if (strcmp (global_array[i].namestr, global_array[i-1].namestr) != 0) continue;

    /* If one is common and the other isn't, trash the common definition                                  */
    /* This happens when there is an 'extern int abc;' in one module, but 'int const abc=123;' in another */

    if ((global_array[i].sym_ent -> st_shndx != SHN_COMMON) && (global_array[i-1].sym_ent -> st_shndx == SHN_COMMON)) goto prune_im1;
    if ((global_array[i].sym_ent -> st_shndx == SHN_COMMON) && (global_array[i-1].sym_ent -> st_shndx != SHN_COMMON)) goto prune_i;

    /* If there are two common definitions of the same symbol, and both are either weak or global, just save one of them */

    if ((global_array[i].sym_ent -> st_shndx == SHN_COMMON) && (global_array[i-1].sym_ent -> st_shndx == SHN_COMMON) && (global_array[i].symbind == global_array[i-1].symbind)) {
      if (global_array[i-1].sym_ent -> st_value < global_array[i].sym_ent -> st_value) global_array[i-1].sym_ent -> st_value = global_array[i].sym_ent -> st_value;
      goto prune_i;
    }

    /* If one is global and the other weak, purge the weak definition */

    if ((global_array[i].symbind == STB_GLOBAL) && (global_array[i-1].symbind == STB_WEAK)) goto prune_im1;
    if ((global_array[i].symbind == STB_WEAK) && (global_array[i-1].symbind == STB_GLOBAL)) goto prune_i;

    /* If one defined by a dynamic image and the other by an object or library, ditch the dynamic image one */

    if ((global_array[i].objfile -> ot == OT_REL) && (global_array[i-1].objfile -> ot == OT_DIN)) goto prune_im1;
    if ((global_array[i].objfile -> ot == OT_DIN) && (global_array[i-1].objfile -> ot == OT_REL)) goto prune_i;

    /* Check for conflicting definition */

    checkmultdef (global_array + i, global_array + i - 1);

    /* Purge out the global_array[i-1] entry */

prune_im1: // (keep the wise cracks to yourself)
    global_array[i].refd |= global_array[i-1].refd;
    memmove (global_array + i - 1, global_array + i, (global_count - i) * sizeof *global_array);
    -- global_count;
    continue;

    /* Purge out the global_array[i] entry */

prune_i:
    -- global_count;
    global_array[i+1].refd |= global_array[i].refd;
    memmove (global_array + i, global_array + i + 1, (global_count - i) * sizeof *global_array);
  }

  /* Fill in dynsym output - just the names for now (we don't know the values yet)                        */
  /* 1) if -dynamic, symbols to export, ie, any that are globally defined by object modules               */
  /* 2) symbols we import, ie, those that are defined by sub-images that are used to satisfy an undefined */

  dynsym_count = 1;								// count number of symbols
  dynstr.alloc = 1;								// count length of all strings
  for (i = 0; i < global_count; i ++) {
    if (((global_array[i].objfile -> ot == OT_REL) && flag_dynamic) 		// - will be defined by this image
     || ((global_array[i].objfile -> ot == OT_DIN) && global_array[i].refd)) {	// - undef by this image, defined by sub-dynamic
      dynsym_count ++;
      dynstr.alloc += strlen (global_array[i].namestr) + 1;
    }
  }
  dynsym_array = MALLOC (dynsym_count * sizeof *dynsym_array);			// allocate the arrays
  dynstr.array = MALLOC (dynstr.alloc);
  dynsym_count = 1;								// fill the arrays in
  dynstr.index = 1;
  memset (dynsym_array, 0, sizeof *dynsym_array);				// null their first elements
  dynstr.array[0] = 0;
  for (i = 0; i < global_count; i ++) {						// step through global array
    global_array[i].dynsymi = 0;						// doesn't have a dynsym_array index yet
    if (((global_array[i].objfile -> ot == OT_REL) && flag_dynamic) 
     || ((global_array[i].objfile -> ot == OT_DIN) && global_array[i].refd)) {
      dynsym_array[dynsym_count].st_name  = dynstr.index;			// fill in name string index
      dynsym_array[dynsym_count].st_value = i;					// save global_array index it came from
      global_array[i].dynsymi = dynsym_count;					// save where it went in dynsym array
      j = strlen (global_array[i].namestr) + 1;
      memcpy (dynstr.array + dynstr.index, global_array[i].namestr, j);
      dynsym_count ++;
      dynstr.index += j;
    }
  }

  /* Make 'DT_NEEDED' records for dynamic images we use */

  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {	// loop through input files
    Elf32_Dyn *pdyn;
    Elf32_Phdr *phdr;
    Elf32_Addr pdyn_soname, pdyn_strtab;

    objfile -> dtneeded = 0;							// in case we don't do it
    if (objfile -> ot != OT_DIN) continue;					// only consider included dynamic images
    ehdr = objfile -> data;							// point to its elf header
    phdr = objfile -> data + ehdr -> e_phoff;					// point to its program header array
    pdyn_strtab = -1;								// haven't found its DT_STRTAB entry yet
    pdyn_soname = -1;								// haven't found its DT_SONAME entry yet
    for (i = 0; i < ehdr -> e_phnum; i ++) {					// scan its program header array
      if (phdr[i].p_type == PT_DYNAMIC) {					// look for the PT_DYNAMIC load parameter array
        pdyn = objfile -> data + phdr[i].p_offset;				// point to it
        while (pdyn -> d_tag != DT_NULL) {					// scan it
          if (pdyn -> d_tag == DT_STRTAB) pdyn_strtab = pdyn -> d_un.d_ptr;	// maybe we found DT_STRTAB entry
          if (pdyn -> d_tag == DT_SONAME) pdyn_soname = pdyn -> d_un.d_val;	// maybe we found DT_SONAME entry
          pdyn ++;
        }
        break;
      }
    }
    cp = strrchr (objfile -> name, '/');					// assume we didn't find both DT_STRTAB,DT_SONAME
    if (cp != NULL) cp ++;							// ... and use filename after last '/'
    else cp = objfile -> name;
    if ((pdyn_strtab != (typeof (pdyn_strtab))(-1)) && (pdyn_soname != (typeof (pdyn_soname))(-1))) {
      for (i = 0; i < ehdr -> e_phnum; i ++) {					// ok, look for DT_STRTAB's load section
        if ((phdr[i].p_type == PT_LOAD) && (phdr[i].p_vaddr <= pdyn_strtab) && (phdr[i].p_vaddr + phdr[i].p_filesz > pdyn_strtab)) {
          cp = objfile -> data 							//   address of beginning of file
             + phdr[i].p_offset 						// + offset to beginning of load section
             + pdyn_strtab - phdr[i].p_vaddr 					// + offset to beginning of string table
             + pdyn_soname;							// + offset to string of interest
          break;
        }
      }
    }
    i = append_string (&dynstr, cp);						// put name string in .dynstr
    objfile -> dtneeded = output_dynamic (DT_NEEDED, i);			// output a DT_NEEDED record
  }

  /* Now that DT_NEEDEDs are done, reserve slots in dynamic_array we need for other stuff */

  dt_hash   = output_dynamic (DT_HASH,   0);			// fill in with va of hashtable
  dt_strtab = output_dynamic (DT_STRTAB, 0);			// fill in with va of dynstr.array
  dt_symtab = output_dynamic (DT_SYMTAB, 0);			// fill in with va of dynsym_array
  dt_strsz  = output_dynamic (DT_STRSZ,  0);			// fill in with dynstr.index
              output_dynamic (DT_SYMENT, sizeof (Elf32_Sym));

  cp = strrchr (outname, '/');					// fill in SONAME with output filename after last '/'
  if (cp != NULL) cp ++;
  else cp = outname;
              output_dynamic (DT_SONAME, append_string (&dynstr, cp));

  dt_rel    = output_dynamic (DT_REL,    0);			// fill in with va of reloc_array
  dt_relsz  = output_dynamic (DT_RELSZ,  0);			// fill in with size of reloc_array
              output_dynamic (DT_RELENT, sizeof (Elf32_Rel));
              output_dynamic (DT_NULL,   0);			// this is always last

  /* Initialize hash table */

  i = dynsym_count / 7;							// seven symbols per bookay
  if (i < 16) i = 16;							// but at least 16
  if (i > dynsym_count) i = dynsym_count;				// but never more than symbols

  hashtable = MALLOC ((2 + i + dynsym_count) * sizeof *hashtable);	// MALLOC hash table
  if (STN_UNDEF != 0) abort ();
  memset (hashtable, 0, (2 + i + dynsym_count) * sizeof *hashtable);	// init to STN_UNDEF
  hashtable[0] = i;							// save 'nbuckets'
  hashtable[1] = dynsym_count;						// save 'nchain'

  /* Put all the common symbols in a list so we will have a common linking between them */

  comsyms = NULL;
  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
    if (objfile -> ot != OT_REL) continue;				// only look at 'individual' files
									// ... and included library modules
    ehdr = objfile -> data;						// point to obj file's elf header
    shdr_tbl = objfile -> data + ehdr -> e_shoff;			// point to obj file's section header table
    for (i = 0; i < ehdr -> e_shnum; i ++) {				// loop through each section header
      shdr = shdr_tbl + i;						// point to section header
      if (shdr -> sh_type != SHT_SYMTAB) continue;			// skip if not symbol table
      for (j = 0; j < shdr -> sh_size; j += shdr -> sh_entsize) {	// loop through all symbols
        sym_ent = objfile -> data + shdr -> sh_offset + j;		// point to symbol table entry
        if (sym_ent -> st_shndx != SHN_COMMON) continue;		// skip if not common
        global_key.namestr = symbolname (objfile, i, j / shdr -> sh_entsize); // see if defined as a non-common global somewhere
        global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
        if ((global_entry != NULL) && (global_entry -> sym_ent -> st_shndx != SHN_COMMON)) {
          sym_ent -> st_shndx = SHN_UNDEF;				// ok, convert reference to 'undefined' so it will link
          continue;
        }
        for (comsym = comsyms; comsym != NULL; comsym = comsym -> next) { // see if already defined as a common
          if (strcmp (comsym -> namestr, global_key.namestr) == 0) break;
        }
        if (comsym == NULL) {
          comsym = MALLOC (sizeof *comsym);				// if not, define it
          comsym -> next    = comsyms;
          comsym -> namestr = global_key.namestr;
          comsym -> align   = sym_ent -> st_value;
          comsym -> size    = sym_ent -> st_size;
          comsyms = comsym;
        } else {
          if (comsym -> align < sym_ent -> st_value) comsym -> align = sym_ent -> st_value;
          if (comsym -> size  < sym_ent -> st_size)  comsym -> size  = sym_ent -> st_size;
        }
      }
    }
  }

  /* Scan through to see how many relocation entries and global offset entries we will need */

  relocpass (0);
  if (showstopper) return (-1);								// stop if error so we don't get two sets of undefined symbol messages

  /* Make a section header for the header table - it must end up at file offset zero */

  shdr_header.sh_name      = append_string (&shstrtab, ".header.ldelf32");		// put name in shstrtab
  shdr_header.sh_type      = SHT_SHLIB;							// section header type = headers
  shdr_header.sh_flags     = SHF_ALLOC;							// it occupies readonly memory
  shdr_header.sh_offset    = (void *)&fhout - (void *)&intobjdata;			// offset to fhout
  shdr_header.sh_size      = sizeof fhout;						// size of fhout
  shdr_header.sh_addralign = 4;

  /* Make a section header for the hash table */

  shdr_hash.sh_name      = append_string (&shstrtab, ".hash");				// put name in shstrtab
  shdr_hash.sh_type      = SHT_HASH;							// section header type = hash table
  shdr_hash.sh_flags     = SHF_ALLOC;							// it occupies readonly memory
  shdr_hash.sh_offset    = (void *)hashtable - (void *)&intobjdata;			// offset to table (don't move it!)
  shdr_hash.sh_size      = (hashtable[0] + hashtable[1] + 2) * sizeof *hashtable;	// size of table
  shdr_hash.sh_link      = &shdr_dynsym - intobjdata.shdrs;				// header index of hashed symbol table
  shdr_hash.sh_addralign = sizeof *hashtable;						// align to entry size
  shdr_hash.sh_entsize   = sizeof *hashtable;						// entry size

  /* Make a section header for the dynamic symbol (export + import) table */

  shdr_dynsym.sh_name      = append_string (&shstrtab, ".dynsym");			// put name in shstrtab
  shdr_dynsym.sh_type      = SHT_DYNSYM;						// section header type = dynamic symbols
  shdr_dynsym.sh_flags     = SHF_ALLOC;							// it occupies readonly memory
  shdr_dynsym.sh_offset    = (void *)dynsym_array - (void *)&intobjdata;		// offset to table (don't move it!)
  shdr_dynsym.sh_size      = dynsym_count * sizeof (Elf32_Sym);				// size of table
  shdr_dynsym.sh_link      = &shdr_dynstr - intobjdata.shdrs;				// header index of string table
  shdr_dynsym.sh_addralign = 4;								// alignment
  shdr_dynsym.sh_entsize   = sizeof (Elf32_Sym);					// entry size

  /* Make a section header for the dynamic string (export + import) table */

  shdr_dynstr.sh_name      = append_string (&shstrtab, ".dynstr");			// put name in shstrtab
  shdr_dynstr.sh_type      = SHT_STRTAB;						// this is a string table
  shdr_dynstr.sh_flags     = SHF_ALLOC;							// it occupies readonly memory
  shdr_dynstr.sh_offset    = (void *)dynstr.array - (void *)&intobjdata;		// offset to table (don't move it!)
  shdr_dynstr.sh_size      = dynstr.index;						// size of table
  shdr_dynstr.sh_addralign = 1;

  /* Make a section header for the dynamic linking parameter table */

  shdr_dynamic.sh_name      = append_string (&shstrtab, ".dynamic");			// put name in shstrtab
  shdr_dynamic.sh_type      = SHT_DYNAMIC;						// this is the dynamic load param table
  shdr_dynamic.sh_flags     = SHF_ALLOC;						// it occupies readonly memory
  shdr_dynamic.sh_offset    = (void *)dynamic_array - (void *)&intobjdata;		// offset to table
  shdr_dynamic.sh_size      = dynamic_count * sizeof (Elf32_Dyn);			// table size
  shdr_dynamic.sh_link      = &shdr_dynstr - intobjdata.shdrs;				// corresponding string table
  shdr_dynamic.sh_addralign = 4;							// alignment
  shdr_dynamic.sh_entsize   = sizeof (Elf32_Dyn);					// entry size

  /* Make a section header for the dynamic relocation table */
  /* The link points to the DYNSYM table                    */

  shdr_reldyn.sh_name      = append_string (&shstrtab, ".rel.dyn");
  shdr_reldyn.sh_type      = SHT_REL;
  shdr_reldyn.sh_flags     = SHF_ALLOC;
  shdr_reldyn.sh_offset    = (void *)dynreloc_array - (void *)&intobjdata;
  shdr_reldyn.sh_size      = dynreloc_count * sizeof (Elf32_Rel);
  shdr_reldyn.sh_link      = &shdr_dynsym - intobjdata.shdrs;
  shdr_reldyn.sh_addralign = 4;
  shdr_reldyn.sh_entsize   = sizeof (Elf32_Rel);

  /* Make a section header for the global offset table         */
  /* Our GOT simply consists of JMP instructions every 8 bytes */
  /* The loader will do all fixups as part of relocation       */
  /* There is one GOT entry per imported function.  Imported   */
  /* objects are relocated in place.                           */

  shdr_got.sh_name      = append_string (&shstrtab, ".got.ldelf32");
  shdr_got.sh_type      = SHT_PROGBITS;
  shdr_got.sh_flags     = SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR; // write enable because we know we will copy-on-write fault it  
								// anyway, so might as well put it with other read/write stuff
  shdr_got.sh_offset    = (void *)got_array - (void *)&intobjdata;
  shdr_got.sh_size      = got_count * sizeof (Got);
  shdr_got.sh_addralign = sizeof (Got);
  shdr_got.sh_entsize   = sizeof (Got);

  /* Make a section header for common symbols   */
  /* Assign all the common symbols a spot in it */

  shdr_common.sh_name      = append_string (&shstrtab, ".common.ldelf32");
  shdr_common.sh_type      = SHT_NOBITS;			// we don't need any file space, it's all zero filled
  shdr_common.sh_flags     = SHF_ALLOC | SHF_WRITE;		// but allocate virtual read/write memory for it
  shdr_common.sh_addralign = 1;

  for (comsym = comsyms; comsym != NULL; comsym = comsym -> next) {
    if (shdr_common.sh_addralign < comsym -> align) shdr_common.sh_addralign = comsym -> align;	// maximize section alignment
    shdr_common.sh_size = (shdr_common.sh_size + comsym -> align - 1) & -(comsym -> align);	// align its size
    comsym -> addr = shdr_common.sh_size;							// assign offset within section
    shdr_common.sh_size += comsym -> size;							// increment size for next one
  }

  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {	// loop through all object files
    if (objfile -> ot != OT_REL) continue;					// must be an object module
    ehdr = objfile -> data;							// point to elf header
    shdr_tbl = objfile -> data + ehdr -> e_shoff;				// point to beg of section header table
    for (i = 1; i < ehdr -> e_shnum; i ++) {					// loop through all section headers
      shdr = shdr_tbl + i;							// point to section header
      if (shdr -> sh_type != SHT_SYMTAB) continue;				// skip if not symbol table
      for (j = 0; j < shdr -> sh_size; j += shdr -> sh_entsize) {		// loop through all symbols
        sym_ent = objfile -> data + shdr -> sh_offset + j;			// point to symbol table entry
        if (sym_ent -> st_shndx != SHN_COMMON) continue;			// skip if not common
        if (ELF32_ST_BIND (sym_ent -> st_info) != STB_LOCAL) continue;		// skip if not local
        if (shdr_common.sh_addralign < sym_ent -> st_value) shdr_common.sh_addralign = sym_ent -> st_value;
        if (sym_ent -> st_value > 0) {
          shdr_common.sh_size = (shdr_common.sh_size + sym_ent -> st_value - 1) & -(sym_ent -> st_value );
        }
        sym_ent -> st_value  = shdr_common.sh_size;				// assign offset within section
        shdr_common.sh_size += sym_ent -> st_size;				// increment size for next one
      }
    }
  }

  /* Make a section header for the section header name strings */

  shdr_shstrtab.sh_name   = append_string (&shstrtab, ".shstrtab");
  shdr_shstrtab.sh_type   = SHT_STRTAB;
  shdr_shstrtab.sh_offset = (void *)shstrtab.array - (void *)&intobjdata;
  shdr_shstrtab.sh_size   = shstrtab.index;

  /* At this point, we have the size and type of everything that goes in the output file */
  /* Figure out where to put it all                                                      */

  outshdr_alloc = 16;
  outshdr_array = MALLOC (outshdr_alloc * sizeof *outshdr_array);
  memset (outshdr_array, 0, sizeof *outshdr_array);			// put null header at beginning
  outshdr_count = 1;

  memset (&fhout, 0, sizeof fhout);

  i = outshdr_count;
  fileoffs = 0;
  addroffs = baseaddr;
  assign_addresses (aasel_allocro, 0);					// readonly sections

  if (fileoffs > 0) {
    fhout.phdr_ro.p_type   = PT_LOAD;
    fhout.phdr_ro.p_offset = outshdr_array[i].sh_offset;
    fhout.phdr_ro.p_vaddr  = outshdr_array[i].sh_addr;
    fhout.phdr_ro.p_filesz = fileoffs - outshdr_array[i].sh_offset;
    fhout.phdr_ro.p_memsz  = fileoffs - outshdr_array[i].sh_offset;
    fhout.phdr_ro.p_flags  = PF_R | PF_X;
    fhout.phdr_ro.p_align  = PS;
  }

  i = outshdr_count;
  prerwfileoffs = fileoffs;
  rdwraddr = assign_addresses (aasel_allocrw_copy, 1);			// readwrite sections that are copied

  fhout.phdr_rw.p_type   = PT_LOAD;
  fhout.phdr_rw.p_offset = outshdr_array[i].sh_offset;
  fhout.phdr_rw.p_vaddr  = outshdr_array[i].sh_addr;
  fhout.phdr_rw.p_filesz = fileoffs - outshdr_array[i].sh_offset;
  fhout.phdr_rw.p_flags  = PF_R | PF_X | PF_W;
  fhout.phdr_rw.p_align  = PS;

  zeroaddr = assign_addresses (aasel_allocrw_zfil, -1);			// readwrite sections that are zero-filled

  fhout.phdr_rw.p_memsz  = fileoffs - outshdr_array[i].sh_offset;

  lastaddr = fileoffs + addroffs - 1;					// that's the last in memory

  prerwfileoffs -= fileoffs;
  fileoffs = fhout.phdr_rw.p_filesz + outshdr_array[i].sh_offset;	// 're-use' the zero-filled area of output file

  if (prerwfileoffs == 0) memset (&fhout.phdr_rw, 0, sizeof fhout.phdr_rw); // if no rw stuff, zero program header

  addroffs = 0;
  assign_addresses (aasel_symtab, 0);					// symbol tables
  addroffs = 0;
  assign_addresses (aasel_else, 0);					// everything else

  fileoffs     = (fileoffs + 3) & -4;
  outshdr_offs = fileoffs;
  fileoffs    += outshdr_count + sizeof *outshdr_array;

  fhout.phdr_dyn.p_type   = PT_DYNAMIC;
  fhout.phdr_dyn.p_offset = INTSHDROUTOFFS (shdr_dynamic);
  fhout.phdr_dyn.p_vaddr  = shdr_dynamic.sh_addr;
  fhout.phdr_dyn.p_filesz = shdr_dynamic.sh_size;
  fhout.phdr_dyn.p_memsz  = shdr_dynamic.sh_size;
  fhout.phdr_dyn.p_flags  = PF_R;
  fhout.phdr_dyn.p_align  = shdr_dynamic.sh_addralign;

  /* Now we know where everything goes! */

  /* Finalize internal object file symbols */

  intobjsymtab_array[intobjsym_baseaddr].st_value = 0;
  intobjsymtab_array[intobjsym_lastaddr].st_value = lastaddr - baseaddr;
  intobjsymtab_array[intobjsym_rdwraddr].st_value = rdwraddr - baseaddr;
  intobjsymtab_array[intobjsym_zeroaddr].st_value = zeroaddr - baseaddr;

  /* Eliminate zero sized output sections (except for null section at beginning) */

  for (i = outshdr_count; -- i > 0;) {
    if (outshdr_array[i].sh_size == 0) {

      /* Zap out the zero sized section */

      -- outshdr_count;
      memmove (outshdr_array + i, outshdr_array + i + 1, (outshdr_count - i) * sizeof *outshdr_array);

      /* Decrement all references to sutff after it by input files */

      for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
        if (objfile -> ot != OT_REL) continue;
        ehdr = objfile -> data;
        for (j = ehdr -> e_shnum; -- j > 0;) {
          if (objfile -> si[j].si_outshndx > i) objfile -> si[j].si_outshndx --;
        }
      }

      /* Decrement sh_link and sh_info references to stuff after it */

      for (j = outshdr_count; -- j > 0;) {
        shdr = outshdr_array + j;
        if (shdr -> sh_link > i) shdr -> sh_link --;
        if ((shdr -> sh_info > i) && ((shdr -> sh_type == SHT_REL) || (shdr -> sh_type == SHT_RELA))) shdr -> sh_info --;
      }
    }
  }

  /* Fill in export/import symbol values                          */
  /* For exported relocatable values, the value includes baseaddr */
  /* All imported values are marked as undefineds                 */

  for (i = dynsym_count; -- i > 0;) {							// loop through all but the null entry
    j = dynsym_array[i].st_value;							// get global_array index
    global_entry = global_array + j;							// point to global entry
    sym_ent = global_entry -> sym_ent;							// point to it's original defining symbol table entry
    dynsym_array[i].st_size  = sym_ent -> st_size;					// copy to dynsym_array
    dynsym_array[i].st_info  = sym_ent -> st_info;
    dynsym_array[i].st_other = 0;
    if (sym_ent -> st_shndx == SHN_COMMON) {						// see if it is common (street trash)
      cp = dynstr.array + dynsym_array[i].st_name;
      for (comsym = comsyms; comsym != NULL; comsym = comsym -> next) { 		// if so, get its value from common table
        if (strcmp (comsym -> namestr, cp) == 0) break;
      }
      if (comsym == NULL) abort ();
      dynsym_array[i].st_shndx = intobjfile.si[&shdr_common-intobjdata.shdrs].si_outshndx;
      dynsym_array[i].st_value = shdr_common.sh_addr + comsym -> addr;
    } else {
      objfile = global_entry -> objfile;						// point to object file that defines the symbol
      switch (objfile -> ot) {
        case OT_REL: {									// defined by some module
          if (sym_ent -> st_shndx == SHN_ABS) {
            dynsym_array[i].st_shndx = SHN_ABS;						// - absolute section index
            dynsym_array[i].st_value = sym_ent -> st_value;				// - just copy the value as is
          } else {
            dynsym_array[i].st_shndx = objfile -> si[sym_ent->st_shndx].si_outshndx;	// - output section number
            dynsym_array[i].st_value = sym_ent -> st_value 				// - symbol offset in input section
                                     + objfile -> si[sym_ent->st_shndx].si_outsoffs 	//   input section offset in output section
                                     + outshdr_array[dynsym_array[i].st_shndx].sh_addr;	//   address of output section (incl baseaddr)
          }
          break;
        }
        case OT_DIN: {
          dynsym_array[i].st_shndx = SHN_UNDEF;						// it's defined in a dynamic, so it's undefined by us
          dynsym_array[i].st_value = objfile -> dtneeded + 1;				// ... not in spec, but put in index+1 of DT_NEEDED entry
          break;									// (a loader could use it to know what image it's defined in)
        }
        default: abort ();
      }
    }
  }

  /* Output final relocations */

  i = dynreloc_count;			// save number of entries beforehand
  relocpass (1);			// perform final relocation
  if (dynreloc_count > i) abort ();	// number should never increase, may decrease due to satisfied undefineds
#if R_386_NULL != 0
  error : code assumes R 386 NULL is zero
#endif
  memset (dynreloc_array + dynreloc_count, 0, (i - dynreloc_count) * sizeof *dynreloc_array);
  qsort (dynreloc_array, dynreloc_count, sizeof *dynreloc_array, compare_relocs);

  /* Fill in the Elf header */

  memcpy (fhout.ehdr.e_ident, ELFMAG, SELFMAG);
  fhout.ehdr.e_ident[EI_CLASS]   = ELFCLASS32;
  fhout.ehdr.e_ident[EI_DATA]    = ELFDATA2LSB;
  fhout.ehdr.e_ident[EI_VERSION] = EV_CURRENT;
  fhout.ehdr.e_type      = ET_DYN;
  fhout.ehdr.e_machine   = EM_386;
  fhout.ehdr.e_version   = EV_CURRENT;
  fhout.ehdr.e_phoff     = sizeof fhout.ehdr;
  fhout.ehdr.e_shoff     = outshdr_offs;
  fhout.ehdr.e_ehsize    = sizeof fhout.ehdr;
  fhout.ehdr.e_phentsize = sizeof (Elf32_Phdr);
  fhout.ehdr.e_phnum     = 3;
  fhout.ehdr.e_shentsize = sizeof (Elf32_Shdr);
  fhout.ehdr.e_shnum     = outshdr_count;

  global_key.namestr = entryname;
  global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
  if ((global_entry != NULL) && (global_entry -> objfile -> ot == OT_REL)) { // don't use _start defined by a dynamic image
    if (symbolval (global_entry -> objfile, global_entry -> hdrindx, global_entry -> symindx, &fhout.ehdr.e_entry, SV_REL) != SV_REL) {
      fprintf (stderr, "%s: %s not relative to image base, unsuitable for use as entrypoint\n", pn, entryname);
      showstopper = 1;
    }
  }

  for (i = 0; i < outshdr_count; i ++) {
    if (strcmp (shstrtab.array + outshdr_array[i].sh_name, ".shstrtab") == 0) fhout.ehdr.e_shstrndx = i;
  }

  /* Fill in missing dt_... stuff */

  dynamic_array[dt_hash].d_un.d_ptr   = shdr_hash.sh_addr;
  dynamic_array[dt_strtab].d_un.d_ptr = shdr_dynstr.sh_addr;
  dynamic_array[dt_symtab].d_un.d_ptr = shdr_dynsym.sh_addr;
  dynamic_array[dt_strsz].d_un.d_val  = shdr_dynstr.sh_size;
  dynamic_array[dt_rel].d_un.d_ptr    = shdr_reldyn.sh_addr;
  dynamic_array[dt_relsz].d_un.d_val  = dynreloc_count * sizeof (Elf32_Rel);

  /* Fix symbol tables, ie, relocate all st_value's, and wipe out any undefined references to now-defined symbols */

  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
    if (objfile -> ot != OT_REL) continue;				// only look at 'individual' files
									// ... and included library modules
    ehdr = objfile -> data;						// point to obj file's elf header
    shdr_tbl = objfile -> data + ehdr -> e_shoff;			// point to obj file's section header table
    for (i = 1; i < ehdr -> e_shnum; i ++) {				// loop through each section header (except null)
      if (shdr_tbl[i].sh_type == SHT_SYMTAB) fixsymtab (objfile, i);	// fix symbol table
    }
  }

  /* If show stopper, don't output anything */

  if (showstopper) return (-1);

  /* Malloc output buffer */

  outsize = outshdr_offs + outshdr_count * sizeof *outshdr_array;
  outdata = malloc (outsize);

  /* Write elf header and program headers */

  writeout (&fhout, sizeof fhout, 0);

  /* Output section contents */

  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
    if (objfile -> ot != OT_REL) continue;				// only look at 'individual' files
									// ... and included library modules
    ehdr = objfile -> data;						// point to obj file's elf header
    shdr_tbl = objfile -> data + ehdr -> e_shoff;			// point to obj file's section header table
    for (i = 1; i < ehdr -> e_shnum; i ++) {				// loop through each section header (except null)
      shdr = shdr_tbl + i;
      j = objfile -> si[i].si_outshndx;					// get output section index
      if (j == 0) continue;						// reloc sections aren't written at all
      if (shdr -> sh_type == SHT_NOBITS) continue;			// nobits (.bss,.common) sections aren't written at all
      if (shdr -> sh_type == SHT_SYMTAB) writesymtab (objfile, i);	// symbol tables are written funny
      else writeout (objfile -> data + shdr -> sh_offset, 		// - where it is in our memory
                     shdr -> sh_size, 					// - how many bytes
                     outshdr_array[j].sh_offset + objfile -> si[i].si_outsoffs); // - where to write it
    }
  }

  /* Output section headers */

  writeout (outshdr_array, outshdr_count * sizeof *outshdr_array, outshdr_offs);

  /* outsize/outdata contain a completely working executable at this point */

  /* Compress .stabstr and .strtab sections, if any */

#if 000 // doesn't work
  symtab_index  = 0;							// haven't found .symtab yet
  strtab_index  = 0;							// haven't found .strtab yet
  stab_index    = 0;							// haven't found .stab yet
  stabstr_index = 0;							// haven't found .stabstr yet
  ehdr = outdata;							// point to output file's elf header
  shdr_tbl = outdata + ehdr -> e_shoff;					// point to output file's section headers
  cp = outdata + shdr_tbl[ehdr->e_shstrndx].sh_offset;			// point to output file's section name string table
  for (i = ehdr -> e_shnum; -- i >= 0;) {				// loop through output sections, starting at the end
    shdr = shdr_tbl + i;
    if (shdr -> sh_flags & SHF_ALLOC) break;				// we can't move anything occupying virtual addresses
    if ((shdr -> sh_type == SHT_SYMTAB)   && (strcmp (cp + shdr -> sh_name, ".symtab")  == 0)) symtab_index  = i;
    if ((shdr -> sh_type == SHT_STRTAB)   && (strcmp (cp + shdr -> sh_name, ".symtab")  == 0)) strtab_index  = i;
    if ((shdr -> sh_type == SHT_PROGBITS) && (strcmp (cp + shdr -> sh_name, ".stab")    == 0)) stab_index    = i;
    if ((shdr -> sh_type == SHT_STRTAB)   && (strcmp (cp + shdr -> sh_name, ".stabstr") == 0)) stabstr_index = i;
  }
  if ((stabstr_index != 0) && (stab_index != 0) && (shdr_tbl[stab_index].sh_link == stabstr_index)) {
    compress_strtab (stab_index, stabstr_index);			// compress .stab/.stabstr
  }
  if ((strtab_index != 0) && (symtab_index != 0) && (shdr_tbl[symtab_index].sh_link == strtab_index)) {
    compress_strtab (symtab_index, strtab_index);			// compress .symtab/.strtab
  }
#endif

  /* If -strip, strip out symbol table sections */

  if (flag_strip) {

    /* Only keep sections that allocate memory plus section header name string section */

    ehdr = outdata;							// point to output executable
    shdr_tbl = outdata + ehdr -> e_shoff;				// point to output file's section headers
    for (i = 1; i < ehdr -> e_shnum; i ++) {				// loop through output section headers
      if (i == ehdr -> e_shstrndx) continue;				// leave section header name string section as is
      shdr = shdr_tbl + i;						// point to section header
      if (shdr -> sh_flags & SHF_ALLOC) continue;			// if it has assigned addresses, leave it intact
      memset (shdr, 0, sizeof *shdr);					// don't want it, zap it
    }

    /* Remove null headers from end of table */

    while (-- i >= 0) if (shdr_tbl[i].sh_type != SHT_NULL) break;
    ehdr -> e_shnum = ++ i;

    /* Remove nulls just before sec hdr name str section */

    while ((ehdr -> e_shstrndx == ehdr -> e_shnum - 1) && (shdr_tbl[ehdr->e_shstrndx-1].sh_type == SHT_NULL)) {
      shdr_tbl[ehdr->e_shstrndx-1] = shdr_tbl[ehdr->e_shstrndx];
      ehdr -> e_shstrndx --;
      ehdr -> e_shnum --;
    }

    /* Calculate new output size from the 'alloc' sections */

    outsize = 0;
    for (i = 1; i < ehdr -> e_shnum; i ++) {
      if (i == ehdr -> e_shstrndx) continue;
      shdr = shdr_tbl + i;
      if (outsize < shdr -> sh_offset + shdr -> sh_size) outsize = shdr -> sh_offset + shdr -> sh_size;
    }

    /* Put section name strings directly after allocated memory data */

    shdr = shdr_tbl + ehdr -> e_shstrndx;				// point to section header name string section
    memmove (outdata + outsize, outdata + shdr -> sh_offset, shdr -> sh_size); // scrunch its contents down
    shdr -> sh_offset = outsize;					// save its new file offset
    outsize += shdr -> sh_size;						// point just past the strings

    /* Put section headers just after that */

    outsize  = (outsize + 3) & -4;					// align to make them nice
    memmove (outdata + outsize, outdata + ehdr -> e_shoff, ehdr -> e_shnum * sizeof *shdr);
    ehdr -> e_shoff = outsize;						// save new file offset
    outsize += ehdr -> e_shnum * sizeof *shdr;				// set up new file size
  }

  /* Create output file from outsize/outdata */

  outfd = open (outname, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (outfd < 0) {
    fprintf (stderr, "%s: error creating %s: %s\n", pn, outname, strerror (errno));
    return (-1);
  }

  rc = write (outfd, outdata, outsize);
  if (rc < 0) {
    fprintf (stderr, "%s: error writing %s: %s\n", pn, outname, strerror (errno));
    return (-1);
  }
  if (rc != outsize) {
    fprintf (stderr, "%s: only wrote %d of %u bytes to %s\n", pn, rc, outsize, outname);
    return (-1);
  }

  if (close (outfd) < 0) {
    fprintf (stderr, "%s: error closing %s: %s\n", pn, outname, strerror (errno));
    return (-1);
  }

  /* Maybe they want a map file, too */

  if (mapname != NULL) {
    int nmapdats;
    Mapdat *mapdats;
    Objfile *curobjfile;

    mapfile = fopen (mapname, "w");
    if (mapfile == NULL) {
      fprintf (stderr, "%s: error creating %s: %s\n", pn, mapname, strerror (errno));
      return (-1);
    }

    /* Fill mapdat array:                      */
    /* 1) Input and Output sections            */
    /* 2) Symbols (global abs, common & reloc) */

    nmapdats = 0;
    for (i = outshdr_count; -- i >= 0;) {
      shdr = outshdr_array + i;
      if ((shdr -> sh_flags & SHF_ALLOC) && (shdr -> sh_size != 0)) nmapdats ++;
    }
    for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
      if (objfile -> ot != OT_REL) continue;
      ehdr = objfile -> data;
      shdr_tbl = objfile -> data + ehdr -> e_shoff;
      for (i = ehdr -> e_shnum; -- i >= 0;) {
        shdr = shdr_tbl + i;
        if ((shdr -> sh_flags & SHF_ALLOC) && (shdr -> sh_size != 0)) nmapdats ++;
      }
    }
    for (i = global_count; -- i >= 0;) {
      if (global_array[i].objfile -> ot == OT_REL) nmapdats ++;
    }

    mapdats = MALLOC (nmapdats * sizeof *mapdats);

    nmapdats = 0;
    for (i = outshdr_count; -- i >= 0;) {
      shdr = outshdr_array + i;
      if ((shdr -> sh_flags & SHF_ALLOC) && (shdr -> sh_size != 0)) {
        mapdats[nmapdats].md            = MD_SEC;
        mapdats[nmapdats].u.sec.objfile = NULL;
        mapdats[nmapdats].u.sec.shndx   = i;
        mapdats[nmapdats].u.sec.shdr    = shdr;
        nmapdats ++;
      }
    }
    for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
      if (objfile -> ot != OT_REL) continue;
      ehdr = objfile -> data;
      shdr_tbl = objfile -> data + ehdr -> e_shoff;
      for (i = ehdr -> e_shnum; -- i >= 0;) {
        shdr = shdr_tbl + i;
        if ((shdr -> sh_flags & SHF_ALLOC) && (shdr -> sh_size != 0)) {
          mapdats[nmapdats].md            = MD_SEC;
          mapdats[nmapdats].u.sec.objfile = objfile;
          mapdats[nmapdats].u.sec.shndx   = i;
          mapdats[nmapdats].u.sec.shdr    = shdr;
          nmapdats ++;
        }
      }
    }
    for (i = global_count; -- i >= 0;) {
      global_entry = global_array + i;
      if (global_entry -> objfile -> ot == OT_REL) {
        mapdats[nmapdats].md                 = MD_GBL;
        mapdats[nmapdats].u.gbl.global_entry = global_entry;
        nmapdats ++;
      }
    }

    /* Sort it by (isnt_absolute, ascending_address, is_symbol, is_from_input) */

    qsort (mapdats, nmapdats, sizeof *mapdats, compare_mapdats);

    /* Print it out */

    if ((mapdats[0].md == MD_GBL) && (mapdats[0].u.gbl.global_entry -> sym_ent -> st_shndx == SHN_ABS)) {
      fprintf (mapfile, "\n                      >> *ABSOLUTE*\n");
    }

    curobjfile = NULL;
    for (i = 0; i < nmapdats; i ++) {
      switch (mapdats[i].md) {
        case MD_SEC: {
          objfile = mapdats[i].u.sec.objfile;
          j = mapdats[i].u.sec.shndx;
          shdr = mapdats[i].u.sec.shdr;
          if (objfile == NULL) {
            fprintf (mapfile, "\n%8X at %8X  >> %s\n", shdr -> sh_size, shdr -> sh_addr, shstrtab.array + shdr -> sh_name);
          } else {
            curobjfile = objfile;
            fprintf (mapfile, "%8X at %8X   > %s\n", shdr -> sh_size, shdr -> sh_addr, curobjfile -> name);
          }
          break;
        }
        case MD_GBL: {
          global_entry = mapdats[i].u.gbl.global_entry;
          if (global_entry -> objfile == curobjfile) {
            fprintf (mapfile, "            %8X     %s\n", global_entry -> sym_ent -> st_value, global_entry -> namestr);
          } else {
            fprintf (mapfile, "            %8X     %s  (%s)\n", global_entry -> sym_ent -> st_value, global_entry -> namestr, global_entry -> objfile -> name);
          }
          break;
        }
      }
    }

    fprintf (mapfile, "\nEnd.\n");
    fclose (mapfile);
  }

  return (0);

  /* Output usage message */

usage:
  fprintf (stderr, "usage: %s <outputfile> <inputfiles...>\n", pn);
  fprintf (stderr, "	[-base <address>]\n");
  fprintf (stderr, "	[-dynamic]\n");
  fprintf (stderr, "	[-map <mapfile>]\n");
  fprintf (stderr, "	[-strip]\n");
  fprintf (stderr, "	[-undef <symbol>]\n");
  return (-1);
}

/************************************************************************/
/*									*/
/*  Map object file to memory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = points to string containing file name			*/
/*									*/
/*    Output:								*/
/*									*/
/*	mmapobjfile = NULL : unable to open or unknown file type	*/
/*	              else : pointer to objfile struct			*/
/*									*/
/************************************************************************/

static Objfile *mmapobjfile (char const *name)

{
  Elf32_Ehdr *ehdr;
  Objfile *objfile;

  /* Open file and map to memory.  We want the memory such that we can write it but not write the file. */

  objfile = MALLOC (sizeof *objfile);
  objfile -> name = name;
  objfile -> fd = open (objfile -> name, O_RDONLY);
  if (objfile -> fd < 0) {
    fprintf (stderr, "%s: error opening %s: %s\n", pn, objfile -> name, strerror (errno));
    return (NULL);
  }

  if (fstat (objfile -> fd, &(objfile -> statbuf)) < 0) {
    fprintf (stderr, "%s: error statting %s: %s\n", pn, objfile -> name, strerror (errno));
    return (NULL);
  }
  objfile -> data = mmap (NULL, objfile -> statbuf.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, objfile -> fd, 0);
  if (objfile -> data == MAP_FAILED) {
    fprintf (stderr, "%s: error mmapping %s: %s\n", pn, objfile -> name, strerror (errno));
    return (NULL);
  }

  /* Determine file type - Individual, Library or Dynamic */

  if (objfile -> statbuf.st_size > sizeof (Elf32_Ehdr)) {
    ehdr = objfile -> data;
    if (memcmp (ehdr -> e_ident, ELFMAG, SELFMAG) == 0) {
      if ((ehdr -> e_ident[EI_CLASS]   != ELFCLASS32) 
       || (ehdr -> e_ident[EI_DATA]    != ELFDATA2LSB) 
       || (ehdr -> e_ident[EI_VERSION] != EV_CURRENT)) {
        fprintf (stderr, "%s: %s has unsupported elf class/data/version\n", pn, objfile -> name);
        return (NULL);
      }
      if (ehdr -> e_type == ET_REL) objfile -> ot = OT_REL;
      else if (ehdr -> e_type == ET_DYN) objfile -> ot = OT_DYN;
      else {
        fprintf (stderr, "%s: %s has unsupported elf e_type %d\n", pn, objfile -> name, ehdr -> e_type);
        return (NULL);
      }
      objfile -> si = MALLOC (ehdr -> e_shnum * sizeof *(objfile -> si));
      memset (objfile -> si, 0, ehdr -> e_shnum * sizeof *(objfile -> si));
      return (objfile);
    }
  }

  if (objfile -> statbuf.st_size > SARMAG + sizeof (struct ar_hdr)) {
    if (memcmp (objfile -> data, ARMAG, SARMAG) == 0) {
      objfile -> ot = OT_LIB;
      return (objfile);
    }
  }

  fprintf (stderr, "%s: %s unknown file type\n", pn, objfile -> name);
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Search a library for the given symbol				*/
/*									*/
/*    Input:								*/
/*									*/
/*	libfile = library file						*/
/*									*/
/*    Output:								*/
/*									*/
/*	search_library = 1 : symbol found in new element		*/
/*	                 0 : symbol not found				*/
/*	                -1 : symbol found in previous element		*/
/*									*/
/************************************************************************/

#define IXTABLENAME "/               "
#define FNTABLENAME "//              "

#define LIBULONG(od,ix) (((unsigned char *)od)[ix] << 24) + (((unsigned char *)od)[ix+1] << 16) + (((unsigned char *)od)[ix+2] << 8) + (((unsigned char *)od)[ix+3])

static int search_library (Objfile *libfile, char const *symname)

{
  char const *cp, *filenames, *strtab;
  char *arname, *objname;
  Elf32_Ehdr const *ehdr;
  Elf32_Shdr const *shdr, *shdr_tbl;
  Elf32_Sym const *symtab;
  int arsize, filenamel, i, j, offset, skipto;
  Objfile *objfile;
  struct ar_hdr *arhdr;
  void *objdata;

  skipto    = 0;
  filenamel = 0;									// haven't seen the filename file yet
  filenames = NULL;
  for (offset = SARMAG; offset + sizeof *arhdr < libfile -> statbuf.st_size; offset += sizeof *arhdr + arsize) {
    arhdr = libfile -> data + offset;							// point to archive element header
    if (memcmp (arhdr -> ar_fmag, ARFMAG, sizeof arhdr -> ar_fmag) != 0) goto badmagic;	// if magic not there, we're done
    arsize = atoi (arhdr -> ar_size);							// get size of data
    if (offset + sizeof *arhdr + arsize > libfile -> statbuf.st_size) goto overflow;	// if it overflows file, we're done
    objdata = libfile -> data + offset + sizeof *arhdr;					// point to data
    if (memcmp (arhdr -> ar_name, IXTABLENAME, sizeof arhdr -> ar_name) == 0) {		// see if index element
      j  = LIBULONG (objdata, 0);							//   number of names in index
      cp = objdata + (j + 1) * 4;							//   pointer to first name
      for (i = 1; i <= j; i ++) {
        if (strcmp (cp, symname) == 0) {						//   see if name matches
          skipto = LIBULONG (objdata, i * 4);						//     ok, skip to that element
          break;
        }
        cp += strlen (cp) + 1;
      }
      if (i > j) return (0);								//   not in index, not in library at all
    } else if (memcmp (arhdr -> ar_name, FNTABLENAME, sizeof arhdr -> ar_name) == 0) {	// see if filename element
      filenamel = arsize;								//   ok, save its length
      filenames = objdata;								//       and point to its data
    } else if (offset >= skipto) {
      ehdr = objdata;									// assume it's an object file
      if (memcmp (ehdr -> e_ident, ELFMAG, SELFMAG) == 0) {				// see if magic is there, skip if not
        shdr_tbl = objdata + ehdr -> e_shoff;						// point to section header table
        for (i = 0; i < ehdr -> e_shnum; i ++) {					// loop through all section headers
          shdr = shdr_tbl + i;								// point to this section header
          if (shdr -> sh_type == SHT_SYMTAB) {						// see if it's a symbol table
            strtab = objdata + shdr_tbl[shdr->sh_link].sh_offset;			// ok, point to string table
            for (j = 0; j < shdr -> sh_size; j += sizeof *symtab) {			// loop through symbol table
              symtab = objdata + shdr -> sh_offset + j;					// point to symbol table entry
              if (symtab -> st_shndx == SHN_UNDEF) continue;				// skip if it does not define anything
              if (ELF32_ST_BIND (symtab -> st_info) != STB_GLOBAL) continue;		// skip if not a global definition
              if (strcmp (strtab + symtab -> st_name, symname) == 0) goto found;	// see if symbol is found
            }
          }
        }
        if (skipto > 0) goto badindex;
      }
    }
    arsize = (arsize + 1) & -2;
  }

  return (0);

badmagic:
  arname = arfilename (arhdr, filenamel, filenames);
  fprintf (stderr, "%s: library %s module %s bad library header magic\n", pn, libfile -> name, arname);
  FREE (arname);
  showstopper = 1;
  return (0);

overflow:
  arname = arfilename (arhdr, filenamel, filenames);
  fprintf (stderr, "%s: library %s module %s overflows end-of-file\n", pn, libfile -> name, arname);
  FREE (arname);
  showstopper = 1;
  return (0);

badindex:
  arname = arfilename (arhdr, filenamel, filenames);
  fprintf (stderr, "%s: %s not found in library %s module %s (index %X)\n", pn, symname, libfile -> name, arname, skipto);
  FREE (arname);
  showstopper = 1;
  return (0);

found:
  if (arhdr -> ar_mode[0] == 0) return (-1);					// maybe we already selected it

  arname = arfilename (arhdr, filenamel, filenames);				// point to the module's filename
  objfile = MALLOC (sizeof *objfile);						// make up an object module struct so rest 
  memset (objfile, 0, sizeof *objfile);						//   of code just thinks it's an object file
  objname = MALLOC (strlen (libfile -> name) + strlen (arname) + 3);		// name it libname[modname]
  sprintf (objname, "%s[%s]", libfile -> name, arname);
  FREE (arname);
  objfile -> name             = objname;
  objfile -> fd               = -1;						// it doesn't have its own FD
  objfile -> statbuf.st_mtime = atoi (arhdr -> ar_date);			// save the module's mtime
  objfile -> statbuf.st_size  = arsize;						// save the module's size
  objfile -> ot               = OT_REL;						// it's a relocatable
  objfile -> data             = objdata;					// save pointer to its data

  objfile -> si = MALLOC (ehdr -> e_shnum * sizeof *(objfile -> si));		// set up an empty secinfo struct
  memset (objfile -> si, 0, ehdr -> e_shnum * sizeof *(objfile -> si));

  *lobjfile = objfile;								// link to list of input object files
  lobjfile = &(objfile -> next);

  arhdr -> ar_mode[0] = 0;							// remember we already selected it

  return (1);
}

/* Return mallocated null-terminated archive element filename string */

static char *arfilename (struct ar_hdr const *arhdr, int filenamel, char const *filenames)

{
  char *arname;
  char const *cp;
  int l;

  if ((arhdr -> ar_name[0] == '/') && ((l = atoi (arhdr -> ar_name + 1)) < filenamel)) { // see if /filenametableoffset
    filenames += l;									// ok, point to entry in filename table
    filenamel -= l;
    cp = memchr (filenames, '\n', filenamel);						// scan for terminating \n
    if (cp == NULL) cp = filenames + filenamel;
  } else {
    filenames = arhdr -> ar_name;							// if not, use name from entry itself
    cp = memchr (filenames, ' ', sizeof arhdr -> ar_name);				// terminated by a space
    if (cp == NULL) cp = filenames + sizeof arhdr -> ar_name;
  }
  if (cp[-1] == '/') -- cp;								// there may be an idiot '/' on the end
  l = cp - filenames;									// get length not including '/'
  arname = MALLOC (l + 1);								// MALLOC buffer for it
  memcpy (arname, filenames, l);							// copy it
  arname[l] = 0;									// null terminate it
  return (arname);									// return pointer
}

/************************************************************************/
/*									*/
/*  Search a dynamic shareable for the given symbol			*/
/*  If found, change the type to OT_DIN (included dynamic)		*/
/*									*/
/************************************************************************/

static int search_dynamic (Objfile *dynfile, char const *symname)

{
  char *stringtablebase;
  Elf32_Ehdr *elfheader;
  Elf32_Shdr *sectionheader, *sectionheadertable, *stringtableheader;
  Elf32_Sym *symbolentry;
  int i, j;

  elfheader = dynfile -> data;							// point to shareable's image header
  sectionheadertable = dynfile -> data + elfheader -> e_shoff;			// point to shareable's section header table
  for (i = 0; i < elfheader -> e_shnum; i ++) {					// loop through each section header
    sectionheader = sectionheadertable + i;					// point to section header
    if (sectionheader -> sh_type != SHT_DYNSYM) continue;			// only care about dynamic symbol headers
    stringtableheader = sectionheadertable + sectionheader -> sh_link;		// point to corresponding string table section header
    stringtablebase = dynfile -> data + stringtableheader -> sh_offset;		// point to corresponding string table base
    for (j = 0; j < sectionheader -> sh_size; j += sectionheader -> sh_entsize) { // loop through each symbol in dyn symbol section
      symbolentry = dynfile -> data + sectionheader -> sh_offset + j;		// point to the symbol entry
      if (strcmp (stringtablebase + symbolentry -> st_name, symname) == 0) {	// see if this is it
        dynfile -> ot = OT_DIN;							// ok, mark it 'included' in output
        return (1);
      }
    }
  }

  return (0);
}

/************************************************************************/
/*									*/
/*  Compare two global symbol definitions for conflict			*/
/*  We already know the names are the same				*/
/*  We already know they are both weak or both global			*/
/*									*/
/*    Output:								*/
/*									*/
/*	checkmultdef = 0 : not a conflicting definition			*/
/*	               1 : conflicting definition			*/
/*									*/
/************************************************************************/

static void checkmultdef (Global *g1, Global *g2)

{
  Elf32_Ehdr *ehdr1, *ehdr2;
  Elf32_Shdr *shdr1, *shdr2;
  Elf32_Sym  *sym1, *sym2;

  ehdr1  = g1 -> objfile -> data;			// point to elf header
  shdr1  = g1 -> objfile -> data + ehdr1 -> e_shoff;	// point to beginning of section header table
  shdr1 += g1 -> hdrindx;				// point to symbol header section
  sym1   = g1 -> objfile -> data + shdr1 -> sh_offset;	// point to beginning of symbol table
  sym1  += g1 -> symindx;				// point to the symbol we want

  ehdr2  = g2 -> objfile -> data;			// point to elf header
  shdr2  = g2 -> objfile -> data + ehdr2 -> e_shoff;	// point to beginning of section header table
  shdr2 += g2 -> hdrindx;				// point to symbol header section
  sym2   = g2 -> objfile -> data + shdr2 -> sh_offset;	// point to beginning of symbol table
  sym2  += g2 -> symindx;				// point to the symbol we want

  /* Two absolute definitions are OK as long as the values are the same */

  if ((sym1 -> st_shndx == SHN_ABS) && (sym2 -> st_shndx == SHN_ABS) && (sym1 -> st_value == sym2 -> st_value)) return;

  /* They are OK if both are common */

  if ((sym1 -> st_shndx == SHN_COMMON) && (sym2 -> st_shndx == SHN_COMMON)) return;

  /* Too bad, output error message */

  fprintf (stderr, "%s: %s defined by %s as %s+%X, but by %s as %s+%X\n", pn, g1 -> namestr, 
	g1 -> objfile -> name, sectioname (g1 -> objfile, sym2 -> st_shndx), sym1 -> st_value, 
	g2 -> objfile -> name, sectioname (g2 -> objfile, sym2 -> st_shndx), sym2 -> st_value);
  showstopper = 1;
}

/************************************************************************/
/*									*/
/*  Perform relocation							*/
/*									*/
/*    Output:								*/
/*									*/
/*	dynreloc_count = number of dynamic relocations needed		*/
/*	got_count = number of global offset table entries needed	*/
/*									*/
/************************************************************************/

static void relocpass (int finalpass)

{
  char const *symname, *symnamtbl;
  Elf32_Addr rvad, symvalu;
  Elf32_Ehdr *ehdr;
  Elf32_Rel *rel;
  Elf32_Shdr *shdr, *shdr_targ, *shdr_syms, *shdr_tbl;
  Elf32_Sym *symboltbl;
  Elf32_Word rval;
  Global *global_entry, global_key;
  int gi, relctr, shndx, si;
  Objfile *objfile;
  Symbolval valtype;
  void *rpnt;

  dynreloc_count = 0;
  got_count = 0;

  for (gi = global_count; -- gi >= 0;) global_array[gi].got_index = 0;

  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {	// loop through input files
    if (objfile -> ot != OT_REL) continue;					// only want object files, no libs or dyns
    ehdr = objfile -> data;							// point to its elf header
    shdr_tbl = objfile -> data + ehdr -> e_shoff;				// point to beg of section header table
    for (shndx = 0; shndx < ehdr -> e_shnum; shndx ++) {			// loop through all its sections
      shdr = shdr_tbl + shndx;							// point to section header
      if ((shdr -> sh_type == SHT_REL) && (shdr -> sh_info != 0)) {		// see if it is relocation type (and not .rel.dyn)

        /* Get stuff related to section being relocated and symbols used */

        shdr_syms = shdr_tbl + shdr -> sh_link;					// point to symbol table used for relocations
        symboltbl = objfile -> data + shdr_syms -> sh_offset;
        symnamtbl = objfile -> data + shdr_tbl[shdr_syms->sh_link].sh_offset;	// point to base of symbol name strings
        shdr_targ = shdr_tbl + shdr -> sh_info;					// point to target text/data to relocate

        /* Loop through the relocation table */

        rel = objfile -> data + shdr -> sh_offset;				// point to relocations
        for (relctr = shdr -> sh_size / shdr -> sh_entsize; -- relctr >= 0;) {	// count through them all

          /* Get parameters for a particular relocation */

          rvad = shdr_targ -> sh_addr + rel -> r_offset;			// virtual address of data to relocate
          rpnt = objfile -> data + shdr_targ -> sh_offset + rel -> r_offset;	// point to data in input file to relocate
          rval = *(Elf32_Word *)rpnt;						// get value to relocate
          si   = ELF32_R_SYM (rel -> r_info);					// get index in symbol table
          symname = "";
          valtype = SV_ABS;
          symvalu = 0;
          if (si != 0) {
            symname = symnamtbl + symboltbl[si].st_name;			// point to symbol name string
            valtype = symbolval (objfile, shdr -> sh_link, si, &symvalu, SV_ABS); // get symbol value and type
          }

          /* If setting up pointer to function in a dynamic image, maybe use GOT */

          if (valtype == SV_DYN) {
            global_key.namestr = symname;
            global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
            if ((global_entry != NULL) 
             && (ELF32_ST_TYPE (global_entry -> sym_ent -> st_info) == STT_FUNC) 
             && (global_entry -> objfile -> ot == OT_DIN)) {

              /* Make a new GOT entry if one doesn't already exist for this global */

              gi = global_entry -> got_index;					// see if global already has a GOT entry
              if (gi == 0) {
                if (got_alloc == got_count) {					// if not, make sure there's room for one more
                  got_alloc += 16;
                  got_array  = REALLOC (got_array, got_alloc * sizeof *got_array);
                }
                if (got_count == 0) {						// if very first, put a null at beginning
                  memset (got_array, 0, sizeof *got_array);			// ... to eat up index zero
                  got_count ++;
                }
                gi = got_count ++;						// save index and inc for next time
                got_array[gi] = got_init;					// fill in initial contents
                if (shdr_targ -> sh_flags & SHF_ALLOC) {
                  emit_dynreloc (shdr_got.sh_addr + got_array[gi].GOT_FIELD - (unsigned char *)got_array, GOT_RELTYPE, symname);
                } else {
                  fprintf (stderr, "%s: .got reloc for non-alloc section at %s+%X in %s\n", pn, 
                           sectioname (objfile, shndx), rvad, objfile -> name);
                  showstopper = 1;
                }
                global_entry -> got_index = gi;					// save GOT index for next ref on same global
              }

              /* Make up a fake symbol entry for relocation stuff to think the symbol points to the GOT entry */

              valtype = (baseaddr == 0) ? SV_REL : SV_ABS;			// is it rel or abs now
              symvalu = shdr_got.sh_addr 					// virtual address of start of GOT entry
                      + (unsigned char *)(got_array + gi) 
                      - (unsigned char *)got_array;
            }
          }

          /* Process relocation */

          switch (ELF32_R_TYPE (rel -> r_info)) {
            case R_386_NONE: break;

            /* Add symbol value to location */

            case R_386_32: {
              switch (valtype) {

                /* For absolute, just add the value and that's the end of it */

                case SV_ABS: {
                  rval += symvalu;
                  break;
                }

                /* For dynamic, pass the relocation on to the dynamic loader */

                case SV_DYN: {
                  if (shdr_targ -> sh_flags & SHF_ALLOC) emit_dynreloc (rvad, R_386_32, symname);
                  else {
                    fprintf (stderr, "%s: R_386_32 reloc for non-alloc section at %s+%X in %s\n", pn, 
                             sectioname (objfile, shndx), rvad, objfile -> name);
                    showstopper = 1;
                  }
                  break;
                }

                /* For relative, add the value and tell dynamic loader to add image base */

                case SV_REL: {
                  rval += symvalu;
                  if (shdr_targ -> sh_flags & SHF_ALLOC) emit_dynreloc (rvad, R_386_RELATIVE, NULL);
                  else {
                    // ?? somehow test for a .stab entry that is OK to leave unrelocated
                  }
                  break;
                }
              }
              break;
            }

            /* Add symbol value minus PC to location (eg, relative jump to symbol) */

            case R_386_PC32: {
              switch (valtype) {

                /* For absolute, add value to location and subtract PC.  If we're dynamic, tell loader to subtract PC. */

                case SV_ABS: {
                  rval += symvalu - rvad;
                  if (baseaddr == 0) {
                    if (shdr_targ -> sh_flags & SHF_ALLOC) emit_dynreloc (rvad, R_386_PC32, NULL);
                    else {
                      fprintf (stderr, "%s: R_386_32 reloc for non-alloc section at %s+%X in %s\n", pn, 
                               sectioname (objfile, shndx), rvad, objfile -> name);
                      showstopper = 1;
                    }
                  }
                  break;
                }

                /* For dynamic, pass the relocation on to the dynamic loader */

                case SV_DYN: {
                  if (shdr_targ -> sh_flags & SHF_ALLOC) emit_dynreloc (rvad, R_386_PC32, symname);
                  else {
                    fprintf (stderr, "%s: R_386_32 reloc for non-alloc section at %s+%X in %s\n", pn, 
                             sectioname (objfile, shndx), rvad, objfile -> name);
                    showstopper = 1;
                  }
                  break;
                }

                /* For relative, add the value minus PC and that's the end of it (eg, relative jump to relative symbol) */

                case SV_REL: {
                  rval += symvalu - rvad;
                  break;
                }
              }
              break;
            }

            /* Didn't know we got these from relocatable files */

            default: {
              fprintf (stderr, "%s: unknown reloc type %u at %s+%X in %s\n", pn, ELF32_R_TYPE (rel -> r_info), 
                       sectioname (objfile, shndx), rvad, objfile -> name);
              showstopper = 1;
              break;
            }
          }
          if (finalpass) *(Elf32_Word *)rpnt = rval;				// if final pass, write relocated value
          rel ++;								// increment to next relocation
        }
      }
    }
  }
}

static void emit_dynreloc (Elf32_Addr rvad, int reloctype, char const *symname)

{
  Global *global_entry, global_key;
  int dynsymi;

  /* Make sure there's room for another entry */

  if (dynreloc_count == dynreloc_alloc) {
    dynreloc_alloc += 64;
    dynreloc_array  = REALLOC (dynreloc_array, dynreloc_alloc * sizeof *dynreloc_array);
  }

  /* If there's an associated symbol, look for it in dynsym_array */

  dynsymi = 0;
  if (symname != NULL) {
    global_key.namestr = symname;
    global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
    if (global_entry == NULL) {
      fprintf (stderr, "%s: emit_dynreloc can't find %s in global array\n", pn, symname);
      showstopper = 1;
      return;
    }
    dynsymi = global_entry -> dynsymi;
    if (dynsymi == 0) {
      fprintf (stderr, "%s: emit_dynreloc can't find %s in dynsym array\n", pn, symname);
      showstopper = 1;
      return;
    }
  }

  /* Fill in relocation entry */

  dynreloc_array[dynreloc_count].r_offset = rvad;
  dynreloc_array[dynreloc_count].r_info   = (dynsymi << 8) + reloctype;

  dynreloc_count ++;
}

/************************************************************************/
/*									*/
/*  Assign addresses to sections					*/
/*									*/
/*    Input:								*/
/*									*/
/*	selector  = says wheter or not to include an object file's section
/*	pagesplit = 0 : not any boundary of note			*/
/*	            1 : ro->rw, so make sure we go to new page		*/
/*	           -1 : rw->zf, don't let sec cross first page bound	*/
/*									*/
/*	objfiles = list of object files to scan				*/
/*	fileoffs = next offset from beg of file to assign		*/
/*	addroffs = what to add to fileoffs to get address		*/
/*									*/
/*	outshdr_count = existing number of output sections		*/
/*	outshdr_array = array of existing output sections		*/
/*	outshdr_alloc = elements allocted to outshdr_array		*/
/*									*/
/*    Output:								*/
/*									*/
/*	fileoffs/addroffs = next offsets to assign			*/
/*	objfiles  sh_addr = assigned virtual address			*/
/*	      si_outshndx = output section header index			*/
/*	      si_outsoffs = output section offset			*/
/*	outshdr_* sh_addr = assigned virtual address			*/
/*	        sh_offset = offset from beg of output file		*/
/*	          sh_size = total section size				*/
/*	     sh_addralign = largest alignment				*/
/*									*/
/************************************************************************/

static Elf32_Addr assign_addresses (Elf32_Word (*selector) (Objfile *objfile, int shndx), int pagesplit)

{
  Elf32_Addr firstaddress;
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr, *shdr_tbl;
  Elf32_Word shsize;
  int anyassigned, assigned, i, j;
  Objfile *objfile;

  /* Scan 1 - accumulate section sizes */

  for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
    if (objfile -> ot != OT_REL) continue;				// only look at 'individual' files
									// ... and included library modules
    ehdr = objfile -> data;						// point to obj file's elf header
    shdr_tbl = objfile -> data + ehdr -> e_shoff;			// point to obj file's section header table
    for (i = 1; i < ehdr -> e_shnum; i ++) {				// loop through each section header (skip the null one)
      shsize = (*selector) (objfile, i);				// see if selectable and get the size needed
      if (shsize == 0) continue;					// skip if not selected
      shdr = shdr_tbl + i;
      j = find_equiv_outshdr (objfile, shdr_tbl, shdr);			// find or create equivalent output section
      if (shdr -> sh_addralign > 1) {
        outshdr_array[j].sh_size = (outshdr_array[j].sh_size + shdr -> sh_addralign - 1) & -(shdr -> sh_addralign);
      }
      outshdr_array[j].sh_size += shsize;
    }
  }

  /* Scan 2 - assign addresses */

  anyassigned = 0;
  firstaddress = 0;
  for (j = 0; j < outshdr_count; j ++) {				// loop through output sections
    assigned = 0;							// haven't assigned it an offset and address yet
    for (objfile = objfiles; objfile != NULL; objfile = objfile -> next) {
      if (objfile -> ot != OT_REL) continue;				// only look at 'individual' files
									// ... and included library modules
      ehdr = objfile -> data;						// point to obj file's elf header
      shdr_tbl = objfile -> data + ehdr -> e_shoff;			// point to obj file's section header table
      for (i = 1; i < ehdr -> e_shnum; i ++) {				// loop through each section header (skip the null one)
        shsize = (*selector) (objfile, i);				// see if selectable and get the size needed
        if (shsize == 0) continue;					// skip if not selected
        shdr = shdr_tbl + i;
        if (find_equiv_outshdr (objfile, shdr_tbl, shdr) != j) continue; // see if it is one we're doing now
        if (!assigned) {						// ok, see if output section has assigned address
          if (outshdr_array[j].sh_addralign > 1) {			//   align address
            fileoffs = (fileoffs + outshdr_array[j].sh_addralign - 1) & -(outshdr_array[j].sh_addralign);
          }
          if (pagesplit < 0) {						//   if it's the rw->zf split, ...
            if ((fileoffs & PSM1) + shsize >= PS) {			//     don't let section split on the rw->zf boundary so 
              pagesplit = 0;						//     oz_knl_section_iolock won't see a split buffer
              if ((fileoffs & PSM1) + shsize > PS) fileoffs = (fileoffs + PSM1) & NPS; 
            }
          }
          outshdr_array[j].sh_offset = fileoffs;			//   assign output file offset
          if (outshdr_array[j].sh_flags & SHF_ALLOC) {
            if (pagesplit > 0) {					//   if it's the ro->rw split, ...
              if (fileoffs & PSM1) addroffs += PS;			//     make sure we're on a new virt addr page
              pagesplit = 0;						//     and we're no longer on the split
            }
            outshdr_array[j].sh_addr = fileoffs + addroffs;		//   assign output section its virtual address
          }
          assigned = 1;							//   now it has been assigned file offset and virt address
        }
        if (!anyassigned) {						// make sure we have first address of the type defined
          anyassigned = 1;
          firstaddress = fileoffs + addroffs;
        }
        if (shdr -> sh_addralign > 1) fileoffs = (fileoffs + shdr -> sh_addralign - 1) & -(shdr -> sh_addralign);
        shdr -> sh_addr = fileoffs + addroffs;				// give input section it its virtual address
        objfile -> si[i].si_outshndx = j;				// remember which output section it belongs to
        objfile -> si[i].si_outsoffs = fileoffs - outshdr_array[j].sh_offset; // remember what offset in that section
        fileoffs += shsize;						// increment offset for next one
      }
    }
  }

  return (firstaddress);
}

/************************************************************************/
/*									*/
/*  Various section selectors for assign_addresses routine		*/
/*									*/
/*  After stepping through these, all section types must be covered, 	*/
/*  except for SHT_REL and SHT_RELA, which are omitted from the output 	*/
/*  file completely							*/
/*									*/
/*    Input:								*/
/*									*/
/*	objfile = input object file of type OT_REL			*/
/*	shndx = section within that object file to test			*/
/*									*/
/*    Output:								*/
/*									*/
/*	aasel_* = 0 : the section is not selected (or is empty anyway)	*/
/*	       else : size of the section will occupy in in the output	*/
/*									*/
/************************************************************************/

/* Select sections that occupy read-only virtual memory and contain data that needs to be copied to the output image */

static Elf32_Word aasel_allocro (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;

  ehdr  = objfile -> data;
  shdr  = objfile -> data + ehdr -> e_shoff;
  shdr += shndx;
  if ((shdr -> sh_flags & (SHF_WRITE | SHF_ALLOC)) != SHF_ALLOC) return (0);
  return (shdr -> sh_size);
}

/* Select sections that occupy read/write virtual memory and contain data that needs to be copied to the output image */

static Elf32_Word aasel_allocrw_copy (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;

  ehdr  = objfile -> data;
  shdr  = objfile -> data + ehdr -> e_shoff;
  shdr += shndx;
  if ((shdr -> sh_flags & (SHF_WRITE | SHF_ALLOC)) != (SHF_WRITE | SHF_ALLOC)) return (0);
  if (shdr -> sh_type == SHT_NOBITS) return (0);
  return (shdr -> sh_size);
}

/* Select sections that occupy read/write virtual memory but are zero filled */

static Elf32_Word aasel_allocrw_zfil (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;

  ehdr  = objfile -> data;
  shdr  = objfile -> data + ehdr -> e_shoff;
  shdr += shndx;
  if ((shdr -> sh_flags & (SHF_WRITE | SHF_ALLOC)) != (SHF_WRITE | SHF_ALLOC)) return (0);
  if (shdr -> sh_type != SHT_NOBITS) return (0);
  return (shdr -> sh_size);
}

/* Select Shdrbol table section */

static Elf32_Word aasel_symtab (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;
  Elf32_Sym *sym;
  Elf32_Word shsize;
  Global *global_entry, global_key;
  int i;

  ehdr  = objfile -> data;
  shdr  = objfile -> data + ehdr -> e_shoff;
  shdr += shndx;
  if (shdr -> sh_flags & SHF_ALLOC) return (0);
  if (shdr -> sh_type != SHT_SYMTAB) return (0);

  /* When copying a symtab to the output, we strip out all undefined references to   */
  /* module globals.  We leave in all undefined references to dynamic image globals. */
  /* See fixsymtab and writesymtab.                                                  */

  shsize = shdr -> sh_entsize;
  sym    = objfile -> data + shdr -> sh_offset;
  for (i = 1; i < shdr -> sh_size / sizeof *sym; i ++) {
    sym ++;
    if (sym -> st_shndx == SHN_UNDEF) {
      global_key.namestr = symbolname (objfile, shndx, i);
      global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
      if ((global_entry != NULL) && (global_entry -> objfile -> ot == OT_REL)) continue;
    }
    shsize += shdr -> sh_entsize;
  }

  return (shsize);
}

/* Select all other sections (except relocation info) */

static Elf32_Word aasel_else (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;

  ehdr  = objfile -> data;
  shdr  = objfile -> data + ehdr -> e_shoff;
  shdr += shndx;
  if (shdr -> sh_flags & SHF_ALLOC) return (0);
  if ((shdr -> sh_type == SHT_SYMTAB) || (shdr -> sh_type == SHT_REL) || (shdr -> sh_type == SHT_RELA)) return (0);
  return (shdr -> sh_size);
}

/************************************************************************/
/*									*/
/*  Find/Create equivalent output section				*/
/*									*/
/*    Input:								*/
/*									*/
/*	objfile = object file that input section header belongs to	*/
/*	shdr_tbl = beginning of that object file's section header table	*/
/*	shdr = section header within that table				*/
/*									*/
/*    Output:								*/
/*									*/
/*	find_equiv_outshdr = index in outshdr_array			*/
/*									*/
/*    Note:								*/
/*									*/
/*	output section will match on:					*/
/*		sh_name, sh_type, sh_link, sh_info, sh_entsize		*/
/*									*/
/*	output section will have maximized:				*/
/*		sh_flags, sh_addralign					*/
/*									*/
/************************************************************************/

static int find_equiv_outshdr (Objfile *objfile, Elf32_Shdr *shdr_tbl, Elf32_Shdr *shdr)

{
  char const *secname;
  int j, j_info, j_link;

  /* Hopefully find an existing equivalent */

  secname = sectioname (objfile, shdr - shdr_tbl);					// point to target's name string

  for (j = outshdr_count; -- j > 0;) {							// loop through existing output sections
    if (shdr -> sh_type != outshdr_array[j].sh_type) continue;				// it has to match type exactly
    if (shdr -> sh_entsize != outshdr_array[j].sh_entsize) continue;			// it has to match entry size exactly
    if (strcmp (outshdr_array[j].sh_name + shstrtab.array, secname) != 0) continue;	// it has to match name exactly
    if (shdr -> sh_link != 0) {								// it's linked sections have to be equiv
      j_link = find_equiv_outshdr (objfile, shdr_tbl, shdr_tbl + shdr -> sh_link);
      if (j_link != outshdr_array[j].sh_link) continue;
    }
    if ((shdr -> sh_info != 0) && ((shdr -> sh_type == SHT_REL) || (shdr -> sh_type == SHT_RELA))) { // same with info sections
      j_info = find_equiv_outshdr (objfile, shdr_tbl, shdr_tbl + shdr -> sh_info);
      if (j_info != outshdr_array[j].sh_info) continue;
    }
    outshdr_array[j].sh_flags |= shdr -> sh_flags;					// ok, maximize flags and address alignment
    if (outshdr_array[j].sh_addralign < shdr -> sh_addralign) outshdr_array[j].sh_addralign = shdr -> sh_addralign;
    return (j);										// return its index
  }

  /* Make sure there's room in the array for a new entry */

  if (outshdr_count == outshdr_alloc) {
    outshdr_alloc += 16;
    outshdr_array  = REALLOC (outshdr_array, outshdr_alloc * sizeof *outshdr_array);
  }

  /* Fill in new entry */

  j = outshdr_count ++;
  memset (outshdr_array + j, 0, sizeof outshdr_array[j]);
  outshdr_array[j].sh_name      = append_string (&shstrtab, secname);
  outshdr_array[j].sh_type      = shdr -> sh_type;
  outshdr_array[j].sh_flags     = shdr -> sh_flags;
  outshdr_array[j].sh_addralign = shdr -> sh_addralign;
  outshdr_array[j].sh_entsize   = shdr -> sh_entsize;

  if (shdr -> sh_link != 0) {
    outshdr_array[j].sh_link = find_equiv_outshdr (objfile, shdr_tbl, shdr_tbl + shdr -> sh_link);
  }

  if ((shdr -> sh_info != 0) && ((shdr -> sh_type == SHT_REL) || (shdr -> sh_type == SHT_RELA))) {
    outshdr_array[j].sh_info = find_equiv_outshdr (objfile, shdr_tbl, shdr_tbl + shdr -> sh_info);
  }

  /* Return index of new entry */

  return (j);
}

/************************************************************************/
/*									*/
/*  Determine symbol's ultimate value					*/
/*									*/
/*    Input:								*/
/*									*/
/*	objfile  = object file that defines symbol			*/
/*	symshndx = symbol table's section header index (SHT_SYMTAB)	*/
/*	symindex = symbol's index within the symbol table		*/
/*	ifrelbased = SV_ABS : if based, reloc syms return as SV_ABS	*/
/*	             SV_REL : if based, reloc syms return as SV_REL	*/
/*									*/
/*    Output:								*/
/*									*/
/*	symbolval = SV_ABS : value is absolute				*/
/*	            SV_REL : value still needs reloc by load base	*/
/*	            SV_DYN : defined by dynamic image at load time	*/
/*	*value_r = value						*/
/*									*/
/************************************************************************/

static Symbolval symbolval (Objfile *objfile, int symshndx, int symindex, Elf32_Addr *value_r, Symbolval ifrelbased)

{
  Comsym *comsym;
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr, *shdr_tbl;
  Elf32_Sym *sym_ent;
  Global *global_entry, global_key;

try_it:
  ehdr     = objfile -> data;					// point to obj file's elf header
  shdr_tbl = objfile -> data + ehdr -> e_shoff;			// point to obj file's section header table
  shdr     = shdr_tbl + symshndx;
  sym_ent  = objfile -> data + shdr -> sh_offset;		// point to symbol table entry
  sym_ent += symindex;

  *value_r = sym_ent -> st_value;				// return basic value

  switch (sym_ent -> st_shndx) {

    /* Absolute symbols always contain their final value */

    case SHN_ABS: return (SV_ABS);

    /* Common symbols should have their value defined in the comsym list now */
    /* Their st_value simply contains the required address aligment          */

    case SHN_COMMON: {
      if (ELF32_ST_BIND (sym_ent -> st_info) == STB_LOCAL) {			// if local, 
        *value_r += shdr_common.sh_addr;					// ... st_value contains offset in .common
        goto rtnabsrel;								// ... so just add base of .command
      }
      global_key.namestr = symbolname (objfile, symshndx, symindex);		// global, get symbol's name
      for (comsym = comsyms; comsym != NULL; comsym = comsym -> next) {		// scan the comsyms list
        if (strcmp (comsym -> namestr, global_key.namestr) == 0) {		// see if the name matches
          *value_r = shdr_common.sh_addr + comsym -> addr;			// ok, get the address
          goto rtnabsrel;							// return if it's abs or rel
        }
      }
      fprintf (stderr, "%s: couldn't find %s common symbol %s\n", pn, objfile -> name, global_key.namestr);
      showstopper = 1;
      return (SV_ABS);
    }

    /* Undefineds - look them up in global symbol table                       */
    /* Any unsatisfied undefineds should have already been checked for by now */

    case SHN_UNDEF: {
      global_key.namestr = symbolname (objfile, symshndx, symindex);		// look for it in global array
      global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
      if (global_entry == NULL) {						// make sure we found it somewhere
        fprintf (stderr, "%s: %s references undefined symbol %s\n", pn, objfile -> name, global_key.namestr);
        showstopper = 1;
        return (SV_ABS);
      }
      switch (global_entry -> objfile -> ot) {
        case OT_DIN: return (SV_DYN);						// if def by dynamic image, tell caller so
        case OT_REL: {
          objfile  = global_entry -> objfile;					// ok, use that value instead
          symshndx = global_entry -> hdrindx;
          symindex = global_entry -> symindx;
          goto try_it;
        }
        default: {								// can't possibly be another type
          fprintf (stderr, "%s: global symbol %s from objfile type %d\n", pn, global_key.namestr, global_entry -> objfile -> ot);
          abort ();
        }
      }
    }

    /* The remaining are genuine section indicies - add the section's base address to return value */

    default: {
      *value_r += shdr_tbl[sym_ent->st_shndx].sh_addr;
      break;
    }
  }

rtnabsrel:
  return ((baseaddr == 0) ? SV_REL : ifrelbased);
}

/************************************************************************/
/*									*/
/*  Given an OT_REL file and a section header index, return the name 	*/
/*  of the section							*/
/*									*/
/************************************************************************/

static char const *sectioname (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *sechdr, *shdrs, *symhdr;

  if (shndx == SHN_ABS) return ("*ABS*");
  if (shndx == SHN_COMMON) return ("*COMMON*");
  if (shndx == SHN_UNDEF) return ("*UNDEF*");

  ehdr   = objfile -> data;			// point to elf header
  shdrs  = objfile -> data + ehdr -> e_shoff;	// point to beginning of section header table
  sechdr = shdrs + shndx;			// point to section header of interest
  symhdr = shdrs + ehdr -> e_shstrndx;		// point to section name string table section header

  return (objfile -> data + symhdr -> sh_offset + sechdr -> sh_name);
}

/************************************************************************/
/*									*/
/*  Given an OT_REL file and a symbol index, return the name of the 	*/
/*  symbol								*/
/*									*/
/*    Input:								*/
/*									*/
/*	objfile = object file						*/
/*	shndx   = symbol table section index within the file		*/
/*	symndx  = symbol index within the section			*/
/*									*/
/*    Output:								*/
/*									*/
/*	symbolname = points to symbol name string			*/
/*									*/
/************************************************************************/

static char const *symbolname (Objfile *objfile, int shndx, int symndx)

{
  Elf32_Ehdr *elfheader;
  Elf32_Shdr *sectionheadertable, *stringheadersection, *symbolheadersection;
  Elf32_Sym *symbolentry, *symboltable;

  elfheader = objfile -> data;
  sectionheadertable = objfile -> data + elfheader -> e_shoff;
  symbolheadersection = sectionheadertable + shndx;
  if ((symbolheadersection -> sh_type != SHT_SYMTAB) && (symbolheadersection -> sh_type != SHT_DYNSYM)) abort ();
  stringheadersection = sectionheadertable + symbolheadersection -> sh_link;
  symboltable = objfile -> data + symbolheadersection -> sh_offset;
  symbolentry = symboltable + symndx;

  return (objfile -> data + stringheadersection -> sh_offset + symbolentry -> st_name);
}

/************************************************************************/
/*									*/
/*  Compare two Global array entry names				*/
/*  This routine is suitable for qsort and bsearch			*/
/*									*/
/************************************************************************/

static int compare_globals (void const *gv1, void const *gv2)

{
  return (strcmp (((Global *)gv1) -> namestr, ((Global *)gv2) -> namestr));
}

/************************************************************************/
/*									*/
/*  Use this to sort a relocation table					*/
/*									*/
/************************************************************************/

static int compare_relocs (void const *rv1, void const *rv2)

{
  if (((Elf32_Rel *)rv1) -> r_offset < ((Elf32_Rel *)rv2) -> r_offset) return (-1);
  if (((Elf32_Rel *)rv1) -> r_offset > ((Elf32_Rel *)rv2) -> r_offset) return  (1);
  return (0);
}

/************************************************************************/
/*									*/
/*  Use this to sort the map section listing by address			*/
/*									*/
/************************************************************************/

static int compare_mapdats (void const *mdv1, void const *mdv2)

{
  Elf32_Addr a1, a2;
  int abs1, abs2;
  Mapdat const *md1, *md2;

  md1 = mdv1;
  md2 = mdv2;

  /* Get element's virtual address */

  switch (md1 -> md) {
    case MD_SEC: a1 = md1 -> u.sec.shdr -> sh_addr; break;
    case MD_GBL: a1 = md1 -> u.gbl.global_entry -> sym_ent -> st_value; break;
  }

  switch (md2 -> md) {
    case MD_SEC: a2 = md2 -> u.sec.shdr -> sh_addr; break;
    case MD_GBL: a2 = md2 -> u.gbl.global_entry -> sym_ent -> st_value; break;
  }

  /* Absolute symbols come before everything else */

  abs1 = (md1 -> md == MD_GBL) && (md1 -> u.gbl.global_entry -> sym_ent -> st_shndx == SHN_ABS);
  abs2 = (md2 -> md == MD_GBL) && (md2 -> u.gbl.global_entry -> sym_ent -> st_shndx == SHN_ABS);

  if (abs1 && !abs2) return (-1);
  if (!abs1 && abs2) return  (1);

  if (abs1 && abs2) {
    if (a1 < a2) return (-1);
    if (a1 > a2) return  (1);
    return (strcmp (md1 -> u.gbl.global_entry -> namestr, md2 -> u.gbl.global_entry -> namestr));
  }

  /* Next key is the address */

  if (a1 < a2) return (-1);
  if (a1 > a2) return  (1);

  /* Next is that sections print before symbols */

  if (md1 -> md < md2 -> md) return (-1);
  if (md1 -> md > md2 -> md) return  (1);

  switch (md1 -> md) {
    case MD_SEC: {
      if (md1 -> u.sec.objfile < md2 -> u.sec.objfile) return (-1);	// should only be comparing a NULL (output file) with non-NULL (input file)
      if (md1 -> u.sec.objfile > md2 -> u.sec.objfile) return  (1);	// ... and we want the output file listing to come before any input file
      return (0);
    }
    case MD_GBL: {
      return (strcmp (md1 -> u.gbl.global_entry -> namestr, md2 -> u.gbl.global_entry -> namestr));
    }
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Define an internal object file symbol				*/
/*									*/
/*    Input:								*/
/*									*/
/*	name  = name to define symbol as				*/
/*	value = value to define it to					*/
/*	shndx = SHN_ABS : absolute value				*/
/*	              1 : relocatable value				*/
/*	      SHN_UNDEF : undefined					*/
/*									*/
/************************************************************************/

static int define_intobjsym (char const *name, Elf32_Addr value, int shndx)

{
  /* Make sure there's room in the array for a new symbol table entry */

  if (intobjsymtab_count == intobjsymtab_alloc) {
    intobjsymtab_alloc += 16;
    intobjsymtab_array  = REALLOC (intobjsymtab_array, intobjsymtab_alloc * sizeof *intobjsymtab_array);
    if (intobjsymtab_count == 0) {
      memset (intobjsymtab_array, 0, sizeof *intobjsymtab_array);
      intobjsymtab_count ++;
    }
  }

  /* Fill it in */

  memset (intobjsymtab_array + intobjsymtab_count, 0, sizeof *intobjsymtab_array);

  intobjsymtab_array[intobjsymtab_count].st_name  = append_string (&intobjsymstr, name);
  intobjsymtab_array[intobjsymtab_count].st_value = value;
  intobjsymtab_array[intobjsymtab_count].st_info  = STB_WEAK << 4;
  intobjsymtab_array[intobjsymtab_count].st_shndx = shndx;

  intobjsymtab_count ++;

  /* Update input object file's section headers to include new symbol */

  shdr_symtab.sh_offset = (void *)intobjsymtab_array - (void *)&intobjdata;
  shdr_symtab.sh_size   = intobjsymtab_count * sizeof *intobjsymtab_array;

  shdr_symstr.sh_offset = (void *)intobjsymstr.array - (void *)&intobjdata;
  shdr_symstr.sh_size   = intobjsymstr.index;

  /* Return index of new entry */

  return (intobjsymtab_count - 1);
}

/************************************************************************/
/*									*/
/*  Output something to the dynamic array				*/
/*									*/
/*    Input:								*/
/*									*/
/*	dtag,dval = element to add					*/
/*									*/
/*    Output:								*/
/*									*/
/*	output_dynamic = index associated with item			*/
/*									*/
/************************************************************************/

static int output_dynamic (Elf32_Sword dtag, Elf32_Addr dval)

{
  if (dynamic_count == dynamic_alloc) {							// see if it's filled up
    dynamic_alloc += 16;								// if so, allocate 16 more
    dynamic_array  = REALLOC (dynamic_array, dynamic_alloc * sizeof *dynamic_array);
  }
  dynamic_array[dynamic_count].d_tag = dtag;						// store new tag
  dynamic_array[dynamic_count].d_un.d_val = dval;					// store new value
  if (dynamic_array[dynamic_count].d_un.d_ptr != dval) abort ();			// make sure it works as a pointer, too

  return (dynamic_count ++);								// return index
}

/************************************************************************/
/*									*/
/*  Append a string to a string table					*/
/*									*/
/*    Input:								*/
/*									*/
/*	stringtable = table to append to				*/
/*	string = string to append					*/
/*									*/
/*    Output:								*/
/*									*/
/*	append_string = index of string					*/
/*									*/
/************************************************************************/

static int append_string (Stringtable *stringtable, char const *string)

{
  int idx, lnew, lold;

  lnew = strlen (string) + 1;								// see how much room needed, incl null

  for (idx = 1; idx <= stringtable -> index - lnew; idx += lold) {			// maybe it's already in there
    lold = strlen (stringtable -> array + idx) + 1;
    if ((lold == lnew) && (memcmp (stringtable -> array + idx, string, lnew) == 0)) return (idx);
  }

  if (stringtable -> alloc < stringtable -> index + lnew) {				// see if there's enough room
    stringtable -> alloc = stringtable -> index + lnew + 64;				// if not, allocate extra 64 bytes
    stringtable -> array = REALLOC (stringtable -> array, stringtable -> alloc);
    if (stringtable -> index == 0) stringtable -> array[stringtable->index++] = 0;	// make sure phony null at beginning
  }
  memcpy (stringtable -> array + stringtable -> index, string, lnew);			// copy incl null
  stringtable -> index += lnew;								// increment past the null

  if (stringtable == &shstrtab) {							// if writing section name strings, 
    shdr_shstrtab.sh_offset = (void *)shstrtab.array - (void *)&intobjdata;		// ... update shstrtab section header
    shdr_shstrtab.sh_size   = shstrtab.index;
  }
  if ((stringtable == &dynstr) && flag_dynamic) {					// similar with dynamic strings
    shdr_dynstr.sh_offset = (void *)dynstr.array - (void *)&intobjdata;
    shdr_dynstr.sh_size   = dynstr.index;
  }

  return (stringtable -> index - lnew);							// return index of start of string
}

/************************************************************************/
/*									*/
/*  Fix symbol table section						*/
/*									*/
/*    Input:								*/
/*									*/
/*	objfile = object file it came from				*/
/*	shndx   = section index of symbol table section			*/
/*									*/
/*    Output:								*/
/*									*/
/*	edited symbol table						*/
/*	- st_value altered to point to final virt addr (incl baseaddr)	*/
/*	- st_name altered to point to offset in output string table	*/
/*	- unwanted entries zeroed					*/
/*									*/
/*    Note:								*/
/*									*/
/*	We strip out all undefined references to module globals.  We 	*/
/*	leave in all undefined references to dynamic image globals.  	*/
/*	See aasel_symtab and writesymtab.				*/
/*									*/
/*	All relocatable addresses are converted to be addresses 	*/
/*	including baseaddr (so gdb will work nice).			*/
/*									*/
/*	We can't actually scrunch out the unwnated entries because it 	*/
/*	would mess up global_array[*].symindx and .sym_ent.		*/
/*									*/
/************************************************************************/

static void fixsymtab (Objfile *objfile, int shndx)

{
  Comsym *comsym;
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;
  Elf32_Sym *sym, *sym_tbl;
  Global *global_entry, global_key;
  int i, outoffs, outshndx;

  ehdr  = objfile -> data;						// point to elf header
  shdr  = objfile -> data + ehdr -> e_shoff;				// point to beg of its section header table
  shdr += shndx;							// point to symbol table's section header

  outshndx = objfile -> si[shndx].si_outshndx;				// get output file section header index
  outoffs  = outshdr_array[outshndx].sh_offset + objfile -> si[shndx].si_outsoffs; // get output file offset for this section

  sym_tbl = objfile -> data + shdr -> sh_offset;			// point to beg of symbol table
  for (i = 0; i < shdr -> sh_size / sizeof *sym; i ++) {		// loop through all symbols including null
    sym = sym_tbl + i;							// point to entry of interest

    /* If it's an undefined that references a global defined in this image, don't output it as it is no longer undefined */
    /* If it references a global in a dynamic image, output it as is as it is still undefined                            */
    
    if (sym -> st_shndx == SHN_UNDEF) {					// also catches the null entry
      global_key.namestr = symbolname (objfile, shndx, i);
      global_entry = bsearch (&global_key, global_array, global_count, sizeof *global_entry, compare_globals);
      if ((global_entry != NULL) && (global_entry -> objfile -> ot == OT_REL)) goto zapit;
    }

    /* If it's common, convert to output address and convert section to output common section */

    else if (sym -> st_shndx == SHN_COMMON) {
      if (ELF32_ST_BIND (sym -> st_info) != STB_LOCAL) {		// see if global (locals aready have st_value set up)
        global_key.namestr = symbolname (objfile, shndx, i);		// global, get common symbol's name
        for (comsym = comsyms; comsym != NULL; comsym = comsym -> next) { // look for it in global common list
          if (strcmp (comsym -> namestr, global_key.namestr) == 0) {
            sym -> st_value = comsym -> addr;				// found, get address within common
            break;
          }
        }
        if (comsym == NULL) abort ();
      }
      sym -> st_value += shdr_common.sh_addr;				// relocate by common base address
      sym -> st_shndx  = &shdr_common - intobjdata.shdrs;		// change section number to .common section number
    }

    /* If it's an general relocatable, convert to output address and convert section to output section */

    else if (sym -> st_shndx != SHN_ABS) {
      outshndx = objfile -> si[sym->st_shndx].si_outshndx;		// get corresponding output section
      sym -> st_value += objfile -> si[sym->st_shndx].si_outsoffs;	// add offset within the section
      sym -> st_value += outshdr_array[outshndx].sh_addr;		// add section base address
      sym -> st_shndx  = outshndx;					// change section number to output section number
    }

    /* Fix the st_name entry as the string tables have been merged */

    sym -> st_name += objfile -> si[shdr->sh_link].si_outsoffs;
    continue;

    /* Something we don't want, zap it out */

zapit:
    memset (sym, 0, sizeof *sym);
  }
}

/************************************************************************/
/*									*/
/*  Copy symbol table section to output file				*/
/*									*/
/*    Input:								*/
/*									*/
/*	objfile = object file it came from				*/
/*	shndx   = section index of symbol table section			*/
/*									*/
/*    Output:								*/
/*									*/
/*	edited symbol table written to output file			*/
/*									*/
/************************************************************************/

static void writesymtab (Objfile *objfile, int shndx)

{
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr;
  Elf32_Sym *sym, *sym_tbl;
  int i, outoffs, outshndx;

  ehdr  = objfile -> data;						// point to elf header
  shdr  = objfile -> data + ehdr -> e_shoff;				// point to beg of its section header table
  shdr += shndx;							// point to symbol table's section header

  outshndx = objfile -> si[shndx].si_outshndx;				// get output file section header index
  outoffs  = outshdr_array[outshndx].sh_offset + objfile -> si[shndx].si_outsoffs; // get output file offset for this section

  sym_tbl = objfile -> data + shdr -> sh_offset;			// point to beg of symbol table
  for (i = 0; i < shdr -> sh_size / sizeof *sym; i ++) {		// loop through all symbols including null
    sym = sym_tbl + i;							// point to entry of interest
    if ((i == 0) || (sym -> st_name != 0)) {
      writeout (sym, sizeof *sym, outoffs);				// if non-zero, write it out
      outoffs += sizeof *sym;
    }
  }
}

/************************************************************************/
/*									*/
/*  Write 'size' bytes from 'buff' at offset 'offs' in file 'outfd'	*/
/*									*/
/************************************************************************/

static void writeout (void *buff, int size, int offs)

{
  if (size + offs > outsize) {
    fprintf (stderr, "%s: writing %u at offs %u, max %u\n", size, offs, outsize);
    abort ();
  }

  memcpy (outdata + offs, buff, size);
}

/************************************************************************/
/*									*/
/*  Compress redundant strings from output file's string table		*/
/*									*/
/*    Input:								*/
/*									*/
/*	tblindx = .symtab or .stab table index				*/
/*	strindx = .strtab or .stabstr table index			*/
/*	outsize = current size of output file				*/
/*	outdata = current output file buffer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	*outdata = string table compressed				*/
/*	outsize  = altered as required					*/
/*									*/
/************************************************************************/

typedef struct { int   oldstrindex;
                 char *oldstraddrs;
                 int   oldstrlenp1;
                 int   newcspindex;
                 int   newstrindex;
                 char *newstraddrs;
               } Csp;

typedef struct { unsigned int n_strx;
                 unsigned char n_type;
                 unsigned char n_other;
                 unsigned short n_desc;
                 unsigned int n_value;
               } Stab;

static int compare_csp_names (void const *csp1, void const *csp2);
static int compare_csp_oldindxs (void const *csp1, void const *csp2);

static void compress_strtab (int tblindx, int strindx)

{
  Csp *newcsp, *oldcsp;
  Elf32_Ehdr *ehdr;
  Elf32_Shdr *shdr, *shdr_tbl, *strshdr, *tblshdr;
  Elf32_Sym *sym;
  int i, j, k, l, m, nnews, nolds, o;
  Stab *stab;

  ehdr = outdata;						// point to output file's elf header
  shdr_tbl = outdata + ehdr -> e_shoff;				// point to output file's section header table

  tblshdr = shdr_tbl + tblindx;					// point to output file symbol table header
  strshdr = shdr_tbl + strindx;					// point to output file string table header

  /* Count how many strings are in old table */

  nolds = 0;							// no strings found yet
  for (i = 1; i < strshdr -> sh_size; i += l) {			// loop through the existing table
    l = strlen (outdata + strshdr -> sh_offset + i) + 1;	// get length of a string including null
    nolds ++;							// one more string found
  }

  /* Fill in array with pointers to old strings */

  oldcsp = malloc (2 * nolds * sizeof *oldcsp);			// alloc array for all found strings
  newcsp = oldcsp + nolds;

  nolds = 0;							// no strings yet
  for (i = 1; i < strshdr -> sh_size; i += l) {			// loop through the existing table
    oldcsp[nolds].oldstrindex = i;				// save old string index
    oldcsp[nolds].oldstraddrs = outdata + strshdr -> sh_offset + i; // save old string address
    l = strlen (outdata + strshdr -> sh_offset + i) + 1;	// get length of a string including null
    oldcsp[nolds].oldstrlenp1 = l;				// save length plus 1
    nolds ++;							// one more string found
  }

  /* Sort so end of strings are sorted, 'cde' comes before 'abcde' */

  qsort (oldcsp, nolds, sizeof *oldcsp, compare_csp_names);

  /* Eliminate redundant strings */

  nnews = 0;							// don't have any new strings yet
  for (i = 0; i < nolds - 1; i ++) {				// loop through old strings except for last one
    oldcsp[i].newcspindex = nnews;				// save index from oldcsp entry to corresponding newcsp entry
    l = oldcsp[i].oldstrlenp1;					// get length of current string (eg, 'cde')
    m = oldcsp[i+1].oldstrlenp1;				// get length of next string (eg, 'abcde')
    if ((l > m) || (memcmp (oldcsp[i].oldstraddrs, oldcsp[i+1].oldstraddrs + m - l, l) != 0)) { // see of 'abcde' ends in 'cde'
      newcsp[nnews].oldstrindex = oldcsp[i].oldstrindex;	// it doesn't, save current as a new string
      newcsp[nnews].oldstraddrs = oldcsp[i].oldstraddrs;
      newcsp[nnews].oldstrlenp1 = oldcsp[i].oldstrlenp1;
      nnews ++;
    }
  }
  oldcsp[i].newcspindex = nnews;				// last entry is always unique
  newcsp[nnews].oldstrindex = oldcsp[i].oldstrindex;
  newcsp[nnews].oldstraddrs = oldcsp[i].oldstraddrs;
  newcsp[nnews].oldstrlenp1 = oldcsp[i].oldstrlenp1;
  nnews ++;

  /* Sort new table by ascending indices so we can compress without overlap */

  qsort (newcsp, nnews, sizeof *newcsp, compare_csp_oldindxs);

  /* Create new table by compressing out redundant strings */

  o = 1;							// output index (leave the initial null alone)
  for (i = 0; i < nnews; i ++) {				// loop through all new entries
    newcsp[i].newstrindex = o;					// assign it an index
    newcsp[i].newstraddrs = outdata + strshdr -> sh_offset  + o; // the corresponding address
    memmove (newcsp[i].newstraddrs, newcsp[i].oldstraddrs, newcsp[i].oldstrlenp1); // move string down
    o += newcsp[i].oldstrlenp1;					// increment offset for next one
  }

  /* Put new table back sorted by name so oldcsp[*].newcspindex will work */

  qsort (newcsp, nnews, sizeof *newcsp, compare_csp_names);

  /* Sort old table by ascending indices to make it easy to find stuff */

  qsort (oldcsp, nolds, sizeof *oldcsp, compare_csp_oldindxs);

  /* Update .symtab/.stab indicies to point to new strings */

  switch (tblshdr -> sh_type) {
    case SHT_SYMTAB: {
      sym = outdata + tblshdr -> sh_offset;							// point to beg of symbol table
      for (i = tblshdr -> sh_size / sizeof *sym; -- i > 0;) {					// loop through all but null entry
        sym ++;											// skip over null, inc to next
        for (j = 0; j < nolds - 1; j ++) if (sym -> st_name < oldcsp[j+1].oldstrindex) break;	// find old index
        if (sym -> st_name  < oldcsp[j].oldstrindex) abort ();
        if (sym -> st_name >= oldcsp[j].oldstrindex + oldcsp[j].oldstrlenp1) abort ();
        k = oldcsp[j].newcspindex;								// find corresponding newcsp entry
        sym -> st_name += newcsp[k].newstrindex + newcsp[k].oldstrlenp1 			// write new index
                        - oldcsp[j].oldstrindex + newcsp[k].oldstrlenp1;
      }
      break;
    }
    case SHT_PROGBITS: {
      stab = outdata + tblshdr -> sh_offset;							// point to beg of stab table
      for (i = tblshdr -> sh_size / sizeof *stab; -- i >= 0;) {
        for (j = 0; j < nolds - 1; j ++) if (stab -> n_strx < oldcsp[j+1].oldstrindex) break;	// find old index
        if (stab -> n_strx  < oldcsp[j].oldstrindex) abort ();
        if (stab -> n_strx >= oldcsp[j].oldstrindex + oldcsp[j].oldstrlenp1) abort ();
        k = oldcsp[j].newcspindex;								// find corresponding newcsp entry
        stab -> n_strx += newcsp[k].newstrindex + newcsp[k].oldstrlenp1 			// write new index
                        - oldcsp[j].oldstrindex + newcsp[k].oldstrlenp1;
        stab ++;
      }
      break;
    }
    default: abort ();
  }

  free (oldcsp);

  /* Subtract compression amount from section headers */

  l = strshdr -> sh_size;							// get old size
  strshdr -> sh_size = o;							// store new size

  for (i = 0; i < ehdr -> e_shnum; i ++) {					// loop through all output section headers
    shdr = shdr_tbl + i;							// point to one of them
    if (shdr -> sh_offset > strshdr -> sh_offset) shdr -> sh_offset -= l - o;	// if after the string section, fix its offset
  }
  ehdr -> e_shoff -= l - o;							// fix offsets to headers themselves

  /* Move everything after it down */

  memmove (outdata + strshdr -> sh_offset + o, outdata + strshdr -> sh_offset + l, outsize - strshdr -> sh_offset - l);
  outsize -= l - o;
}

static int compare_csp_names (void const *csp1, void const *csp2)

{
  char const *s1, *s2;
  int c, i, l1, l2;

  s1 = ((Csp *)csp1) -> oldstraddrs;
  s2 = ((Csp *)csp2) -> oldstraddrs;

  l1 = strlen (s1);
  l2 = strlen (s2);

  s1 += l1;
  s2 += l2;

  i = l1;
  if (i > l2) i = l2;

  while (-- i >= 0) {
    c = (int)*(-- s1) - (int)*(-- s2);
    if (c != 0) return (c);
  }
  return (l1 - l2);
}

static int compare_csp_oldindxs (void const *csp1, void const *csp2)

{
  return (((Csp *)csp1) -> oldstrindex - ((Csp *)csp2) -> oldstrindex);
}
