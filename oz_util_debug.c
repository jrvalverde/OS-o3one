//+++2004-06-11
//    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
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
//---2004-06-11

/************************************************************************/
/*									*/
/*  Symbolic Debugger							*/
/*									*/
/*	debug [-nokernel] [-resume] <thread-or-process-id>		*/
/*									*/
/************************************************************************/

#include <ozone.h>
#include <oz_io_console.h>
#include <oz_io_fs.h>
#include <oz_knl_boot.h>
#include <oz_knl_hw.h>
#include <oz_knl_status.h>
#include <oz_sys_event.h>
#include <oz_sys_exhand.h>
#include <oz_sys_handle.h>
#include <oz_sys_process.h>
#include <oz_sys_thread.h>
#include <oz_util_start.h>

#include <elf.h>
#include <stab.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGARGS_MAX 16

#define MALLOC(size) malloc (size)
#define FREE(buff) free (buff)
#define PRINT oz_sys_io_fs_printf (dc -> h_output,
#define READMEM(size,addr,buff) readmem (dc, size, addr, buff)
#define WRITEMEM(size,buff,addr) writemem (dc, size, buff, addr)

#define LEGALVARCHAR(c) (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '_'))

typedef OZ_Pointer Value;			// generic arithmetic value
						// - should at least be the size of a pointer

typedef struct Bpt Bpt;
typedef struct Dc Dc;
typedef struct Exefile Exefile;
typedef struct Frame Frame;
typedef struct Module Module;
typedef struct Thread Thread;
typedef struct Var Var;

struct Bpt { Bpt *next;				// next in dc->bpts list
             int number;			// breakpoint number
             int enabled;			// clr: disabled; set: enabled
             OZ_Pointer address;		// breakpoint's address
             char string[1];			// symbolic address string
           };

struct Dc { Exefile *exefiles;			// list of executables we are operating on
            OZ_Handle h_input;			// input handle
            OZ_Handle h_output;			// output handle
            OZ_Handle h_haltevent;		// event flag gets set when attached thread gets signal
            OZ_Processid processid;		// this process' id number
            OZ_Handle h_process;		// handle to process
            int ignorectrlc;			// 0: control-C will halt all threads; 1: control-C does nothing
            uLong threadqseq;			// incremented when thread list changes
            Thread *threads;			// list of threads we know about
            Thread *curthread;			// current thread being debugged
            Bpt *bpts;				// list of defined breakpoints
            int ninsertedbpts;			// number of inserted breakpoints
            OZ_Process_Breaklist *insertedbpts;	// list of inserted breakpoints
            int exiting;			// set when exiting to defeat exithandler
            char cmdbuf[4096];			// current command line buffer
          };

struct Exefile { Exefile *next;			// next in dc->exefiles list
                 void const *data;		// pointer to mmap'ed file contents
                 OZ_Pointer baseaddr;		// base address (0 for static images, else base for dynamic)
                 Elf32_Shdr const *dynsym;	// points to .dynsym section header
                 Elf32_Shdr const *dynstr;	// points to .dynstr section header
                 Elf32_Shdr const *symtab;	// points to .symtab section header
                 Elf32_Shdr const *strtab;	// points to .strtab section header
                 Elf32_Shdr const *stab;	// points to .stab section header
                 Elf32_Shdr const *stabstr;	// points to .stabstr section header
                 int nmodules;			// number of modules described by .stab section
                 Module *modules;		// list of modules described by .stab section
                 char name[1];			// executable file name (must be last)
               };

struct Frame { Frame *next;			// next (upper) level
               int level;			// what level this is
               Exefile *exefile;		// executable halted in
               int modindx;			// module index halted in
               char const *srcname;		// source file halted in
               int srcindx;			// index in stab of source
               int srcline;			// source line number halted in
						//  0: haven't tried to look yet
						// -1: looked but couldn't find
               OZ_Mchargs *mchargs_adr;		// pointer to where mchargs_cpy is in target memory (NULL if not)
               OZ_Mchargx *mchargx_adr;		// pointer to where mchargx_cpy is in target memory (NULL if not)
               OZ_Mchargs mchargs_cpy;		// local copy of mchargs at that point
               OZ_Mchargx mchargx_cpy;		// local copy of mchargx at that point
             };

struct Module { char const *name;		// module name
                int stabcoun;			// count of .stab entries for the module
                int stabindx;			// index of first .stab entry for the module
                char const *stabstrs;		// strings in .stabstr section for this module's .stab strings
                uLong textsize;			// size (in bytes) of module's .text section
                OZ_Pointer textaddr;		// starting address of module's .text section (includes baseaddr)
              };

struct Thread { Thread *next;				// next in dc->threads list
                OZ_Threadid threadid;			// this thread's id number
                OZ_Handle h_thread;			// handle to thread
                OZ_Handle h_waitevent;			// event flag it waits on when halted
                int exited;				// set when we know thread has exited
                int letitrun;				// set to let it run
                int haltqueued;				// set to indicate an halt was queued to it
							// cleared when we get OZ_HALTED signal
                OZ_Processid volatile memprocid;	// process id when it halted (for accessing memory)
							//   zero indicates thread is running
							//   non-zero indicates thread is halted
                OZ_Sigargs const *volatile sigargs_adr;	// sigargs in target when it halted
                OZ_Mchargs *volatile mchargs_adr;	// mchargs in target when it halted
                OZ_Mchargx *volatile mchargx_adr;	// mchargx in target when it halted
                OZ_Sigargs sigargs_cpy[SIGARGS_MAX];	// pointer to local copy of sigargs when it halted
                uLong haltsts;				// halt status from sigargs
                Frame *frames;				// list of frames where we're halted at (points to level 0 frame)
                Frame *curframe;			// current frame
              };

struct Var { OZ_Pointer v_addr;			// address in target memory (see v_hasa)
             int v_bito;			// bit offset from v_addr
             int v_bitl;			// length of variable (in bits)
             Value v_valu;			// variable's value (if v_addr==0)
             char const *v_styp;		// stab type string
             int v_star;			// number of asterisks to add onto v_styp
             int v_hasa;			// 0: variable has no address (v_addr not valid)
						// 1: variable has an address (v_addr is valid)
           };

static char waiteventname[OZ_EVENT_NAMESIZE];
static char *pn = "oz_util_debug";
static enum { ENUM_DUMMY } const enum_dummy;

static int getimages (Dc *dc, OZ_Handle h_process);
static int readprompt (Dc *dc, char const *prompt, int size, char *buff);
static void exithandler (void *dcv, uLong status);
static void ctrlc_ast (void *dcv, uLong aststs, OZ_Mchargs *mchargs);
static int openandmap (char const *name, uLong *size_r, void **addr_r);
static int parseinstraddr (Dc *dc, char const *inbuf, OZ_Pointer *bptaddr, char **descr);
static void haltthreads (Dc *dc, int all);
static Thread *attachthread (Dc *dc, Thread **lthread, OZ_Threadid threadid, OZ_Handle h_thread);
static void startthread (Dc *dc, Thread *thread);
static Thread *waitforanyhalt (Dc *dc);
static uLong waitforthreadtohalt (Dc *dc, Thread *thread);
static int setcurthread (Dc *dc, Thread *thread);
static int readmem (Dc *dc, uLong size, OZ_Pointer addr, void *buff);
static int writemem (Dc *dc, uLong size, void const *buff, OZ_Pointer addr);
static int insertbreakpoints (Dc *dc);
static int removebreakpoints (Dc *dc);
static int writethreadmchargs (Dc *dc, Thread *thread);
static void printframespot (Dc *dc, Thread *thread, Frame *frame);
static int findsourceline (Dc *dc, 
                           OZ_Pointer addr, 
                           Exefile **exefile_r, 
                           int *modindx_r, 
                           char const **srcname_r, 
                           int *srcindx_r, 
                           int *srcline_r);
static int findfunctionname (Dc *dc, OZ_Pointer addr, char const **funname_r, OZ_Pointer *funaddr_r, 
                             Exefile **funexec_r, int *funmodi_r, int *funindx_r);
static char const *findfuncparam (Dc *dc, 
                                  Exefile *exefile, 
                                  int modindx, 
                                  int funindx, 
                                  int prmindx, 
                                  OZ_Mchargs *mchargs_cpy, 
                                  OZ_Pointer mchargs_adr, 
                                  Var *var_r);
static int findsrclineaddr (Dc *dc, Exefile *exefile, int modindx, int srcline, OZ_Pointer *addr_r);
static int findfunctionaddr (Dc *dc, Exefile *exefile, int modindx, int funnamel, char const *funname, OZ_Pointer *addr_r);
static int getexpression (int level, 
                          int fake, 
                          Dc *dc, 
                          Exefile *exefile, 
                          int modindx, 
                          OZ_Mchargs *mchargs_cpy, 
                          OZ_Pointer mchargs_adr, 
                          int bufl, 
                          char const *buff, 
                          Var *var_r);
static int isstyparith (char const *varstyp, int varstar);
static int stypsize (char const *varstyp, int varstar);
static int stypsizebits (char const *varstyp, int varstar);
static int getoperand (int fake, 
                       Dc *dc, 
                       Exefile *exefile, 
                       int modindx, 
                       OZ_Mchargs *mchargs_cpy, 
                       OZ_Pointer mchargs_adr, 
                       int bufl, 
                       char const *buff, 
                       Var *var_r);
static int findvariable (Dc *dc, 
                         Exefile *exefile, 
                         int modindx, 
                         int namel, 
                         char const *name, 
                         OZ_Mchargs *mchargs_cpy, 
                         OZ_Pointer mchargs_adr, 
                         Var *var_r);
static int computevaraddr (Dc *dc, 
                           Exefile *exefile, 
                           int modindx, 
                           int namel, 
                           char const *name, 
                           OZ_Mchargs *mchargs_cpy, 
                           OZ_Pointer mchargs_adr, 
                           Var *var_r);
static Elf32_Sym const *findglobalsymbol (Exefile *exefile, int namel, char const *name);
static int findstypstr (Dc *dc, 
                        Exefile *exefile, 
                        int modindx, 
                        int namel, 
                        char const *name, 
                        OZ_Mchargs *mchargs_cpy, 
                        char const **stypstr_r);
static char *findctypstr (Dc *dc, Exefile *exefile, int modindx, char const *varstyp, int varstar);
static int findfinalstyp (Dc *dc, Exefile *exefile, int modindx, char const *stypstr, char const **stypstr_r);
static int getstructfield (char const *varstyp, int fnamel, char const *fname, Var *var_r);
static int printvariable (Dc *dc, Exefile *exefile, int modindx, int incltype, Var var);
static int derefpointer (Dc *dc, Exefile *exefile, int modindx, Var *var);
static char const *arraysubscript (Dc *dc, char const *stypstr, int *lolim_r, int *hilim_r);
static int readvarvalue (Dc *dc, Var *var);
static int writevarvalue (Dc *dc, Var *var);
static int readmembits (Dc *dc, int varbitl, OZ_Pointer varaddr, uLong varbito, int size, void *buff);
static int openexefile (Dc *dc, char const *name, uLong size, void const *data, OZ_Pointer baseaddr);
static int qsort_modules (void const *m1v, void const *m2v);
static char const *rangebits (char const *p, int *bits);

/************************************************************************/
/*									*/
/*  Machine-dependent macros for addressing variables			*/
/*									*/
/************************************************************************/

#ifdef OZ_HW_TYPE_486
#include "oz_util_debug_486.c"
#endif

/************************************************************************/
/*									*/
/*  Extended help texts							*/
/*									*/
/************************************************************************/

static char const xhlp_break[] = 
"	break [<n>] - displays breakpoint(s)\n"
"	break [<n>] <expression> - sets breakpoint at indicated address\n"
"	break <n> clear - clears indicated breakpoint\n"
"	break <n> disable - disables the breakpoint\n"
"	break <n> enable - enables the breakpoint\n"
"\n"
"	<expression> has the form:\n"
"\n"
"		[[executable] sourcefile] linenumber\n"
"		example: oz_util_top.c 231\n"
"\n"
"	or:\n"
"\n"
"		*<general expression giving address>\n"
"		example: *0x800124C\n";

static char const xhlp_continue[] =
"	continue - resumes execution of all threads and waits for one to halt\n";

static char const xhlp_down[] =
"	down [<number>] - go down by 'number' levels into call stack, defaults to 1\n";

static char const xhlp_file[] = 
"	file - with no parameters lists out known executables\n"
"	file <executable>[:<baseaddress>] ... - adds the listed files to known executable list\n";

static char const xhlp_instruction[] =
"	instruction [<begin_address>[..<end_address>]]\n"
"\n"
"	disassembles instruction(s), defaults to the next instruction of the current thread\n"
"	if just begin_address given, that one instruction is disassembled\n"
"	if both begin_address and end_address given, the range is disassembled\n"
"\n"
"	<addresses> have the form:\n"
"\n"
"		[[executable] sourcefile] linenumber\n"
"		example: oz_util_top.c 231\n"
"\n"
"	or:\n"
"\n"
"		*<general expression giving address>\n"
"		example: *0x800124C\n";

static char const xhlp_print[] =
"	print <expression> [,<expression>...] - print value of expression(s)\n"
"\n"
"	<expression> is a C-language style expression\n";

static char const xhlp_tcontinue[] =
"	tcontinue - resumes execution of current thread, other threads stay halted\n";

static char const xhlp_thread[] = 
"	thread - with no parameters lists out known threads\n"
"	thread <idnumber> - selects the thread as the current thread\n";

static char const xhlp_tnext[] =
"	Steps the current thread to the next source line.  If there are \n"
"	are other threads that the debugger knows about, they will remain \n"
"	halted.  If the current source line calls a subroutine, it will \n"
"	run at full speed.  Otherwise, instructions in the current line \n"
"	will be singlestepped.\n";

static char const xhlp_tstep[] =
"	Steps the current thread to the next source line.  If there are \n"
"	are other threads that the debugger knows about, they will remain \n"
"	halted.  If the current source line calls a subroutine, it will \n"
"	step into the subroutine and will halt there.\n";

static char const xhlp_tstepi[] =
"	Steps the current thread to the next instruction.  If there are \n"
"	are other threads that the debugger knows about, they will remain \n"
"	halted.  If the current instruction calls a subroutine, it will \n"
"	step into the subroutine and will halt there.\n";

static char const xhlp_up[] =
"	up [<number>] - go up by 'number' levels out of call stack, defaults to 1\n";

/************************************************************************/
/*									*/
/*  Command table							*/
/*									*/
/************************************************************************/

static int cmd_break (Dc *dc, char *inbuf);
static int cmd_continue (Dc *dc, char *inbuf);
static int cmd_down (Dc *dc, char *inbuf);
static int cmd_file (Dc *dc, char *inbuf);
static int cmd_help (Dc *dc, char *inbuf);
static int cmd_instruction (Dc *dc, char *inbuf);
static int cmd_print (Dc *dc, char *inbuf);
static int cmd_quit (Dc *dc, char *inbuf);
static int cmd_tcontinue (Dc *dc, char *inbuf);
static int cmd_thread (Dc *dc, char *inbuf);
static int cmd_tnext (Dc *dc, char *inbuf);
static int cmd_tstep (Dc *dc, char *inbuf);
static int cmd_tstepi (Dc *dc, char *inbuf);
static int cmd_up (Dc *dc, char *inbuf);
static int cmd_where (Dc *dc, char *inbuf);

static struct { int mini; char const *name; int (*call) (Dc *dc, char *inbuf); int okex; char const *xhlp; char const *help; } const cmdtbl[] = {
	 1, "break",       cmd_break,       0, xhlp_break,       "manage breakpoints", 
	 1, "continue",    cmd_continue,    1, xhlp_continue,    "continue all threads", 
	 1, "down",        cmd_down,        0, xhlp_down,        "go down some call levels", 
	 1, "file",        cmd_file,        1, xhlp_file,        "manage executable files", 
	 1, "help",        cmd_help,        1, NULL,             "print general help message", 
	 1, "instruction", cmd_instruction, 0, xhlp_instruction, "disassemble instructions", 
	 1, "print",       cmd_print,       0, xhlp_print,       "print data", 
	 1, "quit",        cmd_quit,        1, NULL,             "quit out of debugger", 
	 2, "tcontinue",   cmd_tcontinue,   0, xhlp_tcontinue,   "continue current thread", 
	 2, "thread",      cmd_thread,      1, xhlp_thread,      "manage threads", 
	 2, "tnext",       cmd_tnext,       0, xhlp_tnext,       "step current thread by source line over calls", 
	 2, "tstep",       cmd_tstep,       0, xhlp_tstep,       "step current thread by source line", 
	 6, "tstepi",      cmd_tstepi,      0, xhlp_tstepi,      "step current thread by instruction", 
	 1, "up",          cmd_up,          0, xhlp_up,          "go up some call levels", 
	 2, "where",       cmd_where,       0, NULL,             "print out call frame trace", 
	 0, NULL,          NULL,            0, NULL,             NULL };

uLong oz_util_main (int argc, char **argv)

{
  char const *srcname;
  char c, *cmdpnt, *p, prompt[16], *threadidpnt;
  Dc *dc, dcb;
  Exefile *exefile;
  int i, kernel_flag, resume_flag, s, usedup;
  Long eventvalue;
  OZ_Handle h_sysproc, h_thread;
  OZ_IO_console_ctrlchar console_ctrlchar;
  OZ_Threadid threadid;
  Thread *thread;
  uLong size, sts;
  void *addr;

  if (argc > 0) pn = argv[0];

  memset (&dcb,  0, sizeof dcb);
  dc = &dcb;
  dc -> h_input  = oz_util_h_input;
  dc -> h_output = oz_util_h_output;

  kernel_flag = 1;
  resume_flag = 0;
  threadidpnt = NULL;
  for (i = 1; i < argc; i ++) {
    if (strcasecmp ("-nokernel", argv[i]) == 0) {
      kernel_flag = 0;
      continue;
    }
    if (strcasecmp ("-resume", argv[i]) == 0) {
      resume_flag = 1;
      continue;
    }
    if (threadidpnt == NULL) {
      threadidpnt = argv[i];
      continue;
    }
    goto usage;
  }
  if (threadidpnt == NULL) goto usage;

  /* Make up event flag name so when a thread is halted because of us, it is easy to tell */

  sts = oz_sys_thread_getid (0, &threadid);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  p = strrchr (pn, '/');
  if (p == NULL) p = pn;
  i = strlen (p);
  if (i > sizeof waiteventname - 10) p += i - sizeof waiteventname + 10;
  sts = oz_sys_sprintf (sizeof waiteventname, waiteventname, "%s %u", pn, threadid);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Get handle to process to be debugged */

  dc -> processid = oz_hw_atoi (threadidpnt, &usedup);
  if (threadidpnt[usedup] != 0) goto usage;
  threadid = 0;
  sts = oz_sys_process_getbyid (dc -> processid, &(dc -> h_process));			// maybe they gave process-id
  if (sts != OZ_SUCCESS) {
    OZ_Handle_item h_thread_to_h_process[2] = { OZ_HANDLE_CODE_PROCESS_HANDLE, sizeof dc -> h_process, &(dc -> h_process), NULL, 
                                                OZ_HANDLE_CODE_PROCESS_ID,     sizeof dc -> processid, &(dc -> processid), NULL };

    sts = oz_sys_thread_getbyid (dc -> processid, &h_thread);				// if not, maybe it's a thread-id
    if (sts != OZ_SUCCESS) {
      PRINT "main: error %u opening thread or process %u\n", sts, dc -> processid);
      return (-1);
    }
    threadid = dc -> processid;
    sts = oz_sys_handle_getinfo (h_thread, 2, h_thread_to_h_process, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (!resume_flag) oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
    sts = oz_sys_process_getid (OZ_PROCMODE_KNL, dc -> h_process, &(dc -> processid));
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  } else if (resume_flag) {
    PRINT "main: -resume only legal with thread-id, not process-id\n");
    return (OZ_BADPARAM);
  }

  /* Open its executable(s) to get symbols */

  if (!getimages (dc, dc -> h_process)) return (-1);

  /* Also get symbols from kernel */

  if (kernel_flag) {
    sts = oz_sys_process_getbyid (OZ_KNL_BOOT_SYSPROCID, &h_sysproc);
    if (sts != OZ_SUCCESS) PRINT "main: error %u getting system process handle\n", sts);
    else {
      sts = getimages (dc, h_sysproc);
      if (sts != OZ_SUCCESS) PRINT "main: error %u getting kernel symbol table\n", sts);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_sysproc);
    }
  }

  /* This is the event flag we use to wait for threads to halt */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "waiting for thread(s) to halt", &(dc -> h_haltevent));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Set up exit handler */

  sts = oz_sys_exhand_create (OZ_PROCMODE_KNL, exithandler, dc);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* If -resume, attach to the thread immediately, queue an halt to it, then resume it */

  if (resume_flag) {
    thread = attachthread (dc, &(dc -> threads), threadid, h_thread);
    if (!(thread -> exited)) {
      thread -> haltqueued = 1;
      sts = oz_sys_thread_halt (thread -> h_thread, OZ_PROCMODE_KNL);
      if (sts != OZ_SUCCESS) {
        PRINT "main: error %u halting thread %u\n", sts, thread -> threadid);
        return (sts);
      }
      sts = oz_sys_thread_resume (h_thread);
      if ((sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR)) {
        PRINT "main: error %u resuming thread %u\n", sts, thread -> threadid);
        return (sts);
      }
    }
  }

  /* Fake like we got a control-C to halt all the threads then re-arm for another control-C */

  ctrlc_ast (dc, OZ_SUCCESS, NULL);

  /* Set the initial 'current thread' */

  if (threadid != 0) {									// see if command line had thread-id
    for (thread = dc -> threads; thread != NULL; thread = thread -> next) {		// if so, look for it
      if (thread -> threadid == threadid) {
        setcurthread (dc, thread);							// found, select it
        break;
      }
    }
    if (thread == NULL) setcurthread (dc, dc -> threads);				// not found, use first one
  }
  if (threadid == 0) setcurthread (dc, dc -> threads);					// process-id, use first one

  /* Read and process commands */

  dc -> cmdbuf[0] = 0;
  while (1) {

    /* Read command */

    thread = dc -> curthread;
    if (thread -> curframe -> level == 0) oz_sys_sprintf (sizeof prompt, prompt, "%u> ", thread -> threadid);
    else oz_sys_sprintf (sizeof prompt, prompt, "%u.%d> ", thread -> threadid, thread -> curframe -> level);
    c = dc -> cmdbuf[0];					// save first char in case of simple return to repeat last command
    if (!readprompt (dc, prompt, sizeof dc -> cmdbuf, dc -> cmdbuf)) { // prompt and read command line
      dc -> cmdbuf[0] = 0;					// if EOF, do a 'quit' command
      cmd_quit (dc, dc -> cmdbuf);
      continue;
    }
    if (dc -> cmdbuf[0] == 0) dc -> cmdbuf[0] = c;		// if simple return, replace first char and repeat last command

    /* Remove leading, trailing and redundant spaces */

    p = dc -> cmdbuf;
    s = 1;
    for (i = 0; (c = dc -> cmdbuf[i]) != 0; i ++) {
      if (c > ' ') {
        s = 0;
        *(p ++) = c;
      } else if (!s) {
        *(p ++) = ' ';
        s = 1;
      }
    }
    if ((p > dc -> cmdbuf) && (p[-1] == ' ')) -- p;
    if (p == dc -> cmdbuf) continue;
    *p = 0;

    /* Get length of command keyword */

    cmdpnt = dc -> cmdbuf;
decode:
    p = strchr (cmdpnt, ' ');
    if (p != NULL) s = p - cmdpnt;
    else s = strlen (cmdpnt);

    /* Scan command table */

    for (i = 0; cmdtbl[i].name != NULL; i ++) {
      if ((s >= cmdtbl[i].mini) && (strncasecmp (cmdtbl[i].name, cmdpnt, s) == 0)) break;
    }

    /* Call processing routine */

    if (cmdtbl[i].name == NULL) PRINT "main: unknown command <%*.*s> (try 'help' for help)\n", s, s, cmdpnt);
    else {
      for (p = cmdpnt; (p = strstr (++ p, " ;")) != NULL;) if (p[2] != ';') break;	// break command on ' ;' but not ' ;;'
      if (p != NULL) *p = 0;								// null terminate on the space
      if (cmdpnt[s] == ' ') s ++;
      if (cmdpnt[s] == '?') {								// see if looking for extended help
        if (cmdtbl[i].xhlp != NULL) PRINT "%s", cmdtbl[i].xhlp);			// if so, print it
        else PRINT "main: no extended help available (try 'help' for help)\n");
      } else {
        if (cmdtbl[i].okex || !(dc -> curthread -> exited)) {				// if not, execute command
          if ((*(cmdtbl[i].call)) (dc, cmdpnt + s) <= 0) p = NULL;			// if it failed, don't do any compound
        } else {
          PRINT "main: command not valid when currently selected thread has exited (try 'thread ?')\n");
          p = NULL;
        }
      }
      if (p != NULL) {									// see if compound command present
        *p = ' ';									// restore ' ' in case of repeat
        p += 2;										// skip over the ' ;'
        if (*p == ' ') p ++;								// maybe there's a space after the ';'
        cmdpnt = p;									// point to next command
        goto decode;									// go decode it
      }
    }
  }

usage:
  PRINT "usage: %s [-nokernel] [-resume] <thread-or-process-id>\n", pn);
  return (OZ_BADPARAM);
}

/************************************************************************/
/*									*/
/*  Get target process' images and load them in our symbol table	*/
/*									*/
/************************************************************************/

#define IMAGENAMESIZE 256

static int getimages (Dc *dc, OZ_Handle h_process)

{
  char *imagenames, *name;
  OZ_Handle h_file, h_section;
  OZ_IO_fs_open fs_open;
  OZ_Mempage npagem, svpage;
  OZ_Process_Imagelist *imagelist;
  uLong i, imagelistsize, imagelistused, sts;

  /* Get list of images loaded in target process space */

  for (imagelistsize = 8;; imagelistsize += 8) {
    imagelist  = MALLOC (imagelistsize * sizeof *imagelist);
    imagenames = MALLOC (imagelistsize * IMAGENAMESIZE);
    memset (imagelist, 0, imagelistsize * sizeof *imagelist);
    for (i = 0; i < imagelistsize; i ++) {
      imagelist[i].namesize = IMAGENAMESIZE;
      imagelist[i].namebuff = imagenames + i * IMAGENAMESIZE;
    }
    sts = oz_sys_process_imagelist (h_process, imagelistsize, imagelist, &imagelistused);
    if (sts != OZ_SUCCESS) {
      for (i = 0; i < imagelistused; i ++) {
        oz_sys_handle_release (OZ_PROCMODE_KNL, imagelist[i].h_iochan);
      }
      PRINT "error %u getting process image list\n", sts);
      return (0);
    }
    if (imagelistused < imagelistsize) break;
    FREE (imagelist);
    FREE (imagenames);
  }

  /* Map them to memory and read their symbol table stuff */

  for (i = 0; i < imagelistused; i ++) {
    h_file = imagelist[i].h_iochan;
    name   = imagelist[i].namebuff;

    /* Map it to memory */

    sts = oz_sys_section_create (OZ_PROCMODE_KNL, h_file, 0, 1, 0, &h_section);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
    if (sts != OZ_SUCCESS) {
      printf ("error %u creating section for %s\n", sts, name);
      break;
    }
    npagem = 0;
    svpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_USRHEAP_VA);
    sts = oz_sys_process_mapsection (OZ_PROCMODE_KNL, h_section, &npagem, &svpage, OZ_HW_DVA_USRHEAP_AT, OZ_HW_PAGEPROT_UR);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_section);
    if (sts != OZ_SUCCESS) {
      printf ("error %u mapping section for %s\n", sts, name);
      break;
    }

    /* Read in its symbols, etc */

    if (!openexefile (dc, name, npagem << OZ_HW_L2PAGESIZE, OZ_HW_VPAGETOVADDR (svpage), (OZ_Pointer)(imagelist[i].baseaddr))) {
      PRINT "error reading executable %s\n", name);
      break;
    }

    PRINT "image %s at base address %p\n", name, imagelist[i].baseaddr);
  }

  /* Free buffers and we're done */

  FREE (imagelist);
  FREE (imagenames);
  return (i == imagelistused);
}

/************************************************************************/
/*									*/
/*  Read command line							*/
/*									*/
/************************************************************************/

static int readprompt (Dc *dc, char const *prompt, int size, char *buff)

{
  OZ_IO_fs_readrec fs_readrec;
  uLong rlen, sts;

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = size - 1;
  fs_readrec.buff = buff;
  fs_readrec.rlen = &rlen;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.pmtsize = strlen (prompt);
  fs_readrec.pmtbuff = prompt;

  sts = oz_sys_io (OZ_PROCMODE_KNL, dc -> h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  if (sts == OZ_SUCCESS) buff[rlen] = 0;
  else if (sts != OZ_ENDOFFILE) PRINT "error %u reading command\n", sts);
  return (sts == OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Exit handler called when we exit					*/
/*									*/
/************************************************************************/

static void exithandler (void *dcv, uLong status)

{
  Dc *dc;
  Thread *thread;
  uLong sts;

  dc = dcv;
  if (dc -> exiting) return;

  /* Try to detach and resume all attached threads */

  for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
    if (!(thread -> exited) && TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
      CLRSINGSTEP (thread -> frames -> mchargs_cpy);
      sts = oz_sys_process_poke (dc -> h_process, OZ_PROCMODE_KNL, sizeof thread -> frames -> mchargs_cpy, &(thread -> frames -> mchargs_cpy), thread -> mchargs_adr);
      if (sts != OZ_SUCCESS) PRINT "exithandler: error %u writing %u mchargs at %p\n", sts, thread -> threadid, thread -> mchargs_adr);
    }
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, thread -> h_waitevent, OZ_THREAD_HALT_RESUME + 1, NULL);
    if (sts != OZ_SUCCESS) PRINT "exithandler: error %u setting %u waitevent\n", sts, thread -> threadid);
  }
}

/************************************************************************/
/*									*/
/*  Ast routine called when someone presses di control-C		*/
/*									*/
/************************************************************************/

static void ctrlc_ast (void *dcv, uLong aststs, OZ_Mchargs *mchargs)

{
  Dc *dc;
  OZ_IO_console_ctrlchar console_ctrlchar;
  uLong sts;

  dc = dcv;

  if (aststs != OZ_SUCCESS) {
    PRINT "ctrlc_ast: control-C error status %u\n", aststs);
    return;
  }

  /* Halt all threads */

  if (!(dc -> ignorectrlc)) {
    dc -> ignorectrlc = 1;
    PRINT "ctrlc_ast: halting threads\n");
    haltthreads (dc, 1);
    PRINT "ctrlc_ast: threads halted\n");
  }

  /* Re-arm control-C ast */

  memset (&console_ctrlchar, 0, sizeof console_ctrlchar);
  console_ctrlchar.mask[0] = 8;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_console, &ctrlc_ast, 0, ctrlc_ast, dc, 
                         OZ_IO_CONSOLE_CTRLCHAR, sizeof console_ctrlchar, &console_ctrlchar);
  if (sts != OZ_STARTED) PRINT "ctrlc_ast: error %u arming control-C ast\n", sts);

  /* Reset repeating and compound commands */

  memset (dc -> cmdbuf, 0, sizeof dc -> cmdbuf);
}

/************************************************************************/
/*									*/
/*  break - manipulate breakpoints					*/
/*									*/
/*	break [<n>] - displays breakpoint(s)				*/
/*	break [<n>] <expression> - breakpoints at indicated address	*/
/*	break <n> clear - clears indicated breakpoint			*/
/*	break <n> disable - disables the breakpoint			*/
/*	break <n> enable - enables the breakpoint			*/
/*									*/
/************************************************************************/

static int cmd_break (Dc *dc, char *inbuf)

{
  Bpt *bpt, **lbpt;
  char *descr, *p;
  int i, number;
  OZ_Pointer bptaddr;

  /* Save and strip breakpoint number from beginning of line */

  number = strtol (inbuf, &p, 10);
  if ((p == inbuf) || (*p != ' ')) number = 0;
  else if (number > 0) inbuf = ++ p;
  else {
    PRINT "invalid breakpoint number %d\n", number);
    return (-1);
  }

  /* If there's nothing else, list out breakpoint(s) */

  if (inbuf[0] == 0) {
    for (bpt = dc -> bpts; bpt != NULL; bpt = bpt -> next) {
      if ((number != 0) && (bpt -> number != number)) continue;
      PRINT "%3d: %8s  %p  %s\n", bpt -> number, bpt -> enabled ? "enabled" : "disabled", bpt -> address, bpt -> string);
    }
    return (1);
  }

  /* Check for 'clear', 'disable' and 'enable' subcommands */

  i = strlen (inbuf);
  if (strncasecmp (inbuf, "clear", i) == 0) {
    for (lbpt = &(dc -> bpts); (bpt = *lbpt) != NULL; lbpt = &(bpt -> next)) {
      if (bpt -> number == number) break;
    }
    if (bpt == NULL) {
      PRINT "no such breakpoint %d\n", number);
      return (0);
    }
    *lbpt = bpt -> next;
    FREE (bpt);
    return (1);
  }

  if (strncasecmp (inbuf, "disable", i) == 0) {
    for (lbpt = &(dc -> bpts); (bpt = *lbpt) != NULL; lbpt = &(bpt -> next)) {
      if (bpt -> number == number) break;
    }
    if (bpt == NULL) {
      PRINT "no such breakpoint %d\n", number);
      return (0);
    }
    bpt -> enabled = 0;
    return (1);
  }

  if (strncasecmp (inbuf, "enable", i) == 0) {
    for (lbpt = &(dc -> bpts); (bpt = *lbpt) != NULL; lbpt = &(bpt -> next)) {
      if (bpt -> number == number) break;
    }
    if (bpt == NULL) {
      PRINT "no such breakpoint %d\n", number);
      return (0);
    }
    bpt -> enabled = 1;
    return (1);
  }

  /* Add a new breakpoint */

  i = parseinstraddr (dc, inbuf, &bptaddr, &descr);
  if (i <= 0) return (i);
  if (inbuf[i] != 0) {
    if (descr != NULL) FREE (descr);
    PRINT "extra stuff at end of line %s\n", inbuf + i);
    return (-1);
  }

  /* If no number given, make up a number higher than any we have */

  if (number == 0) {
    for (lbpt = &(dc -> bpts); (bpt = *lbpt) != NULL; lbpt = &(bpt -> next)) {
      if (number < bpt -> number) number = bpt -> number;
    }
    number ++;
  }

  /* Otherwise, find insertion spot by ascending number, deleting any like-numbered breakpoint */

  else {
    for (lbpt = &(dc -> bpts); (bpt = *lbpt) != NULL; lbpt = &(bpt -> next)) {
      if (number == bpt -> number) {
        *lbpt = bpt -> next;
        FREE (bpt);
        break;
      }
      if (number < bpt -> number) break;
    }
  }

  /* Insert new breakpoint in list */

  if (descr != NULL) inbuf = descr;
  bpt = MALLOC (strlen (inbuf) + sizeof *bpt);
  bpt -> next    = *lbpt;
  bpt -> number  = number;
  bpt -> address = bptaddr;
  bpt -> enabled = 1;
  strcpy (bpt -> string, inbuf);
  *lbpt = bpt;
  if (descr != NULL) FREE (descr);

  PRINT "breakpoint %d at %p; %s\n", bpt -> number, bpt -> address, bpt -> string);

  return (1);
}

/************************************************************************/
/*									*/
/*  continue - continue all threads					*/
/*									*/
/************************************************************************/

static int cmd_continue (Dc *dc, char *inbuf)

{
  Thread *thread;
  uLong sts;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  /* Arm control-C */

  dc -> ignorectrlc = 0;

  /* Set singlestep flag in current thread                                                       */
  /* Tell thread to run stepping it over possible breakpoint instruction and wait for it to halt */
  /* If halts for other than SINGLESTEP, we're done                                              */

  thread = dc -> curthread;
  if (!(thread -> exited)) {
    if (!TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
      SETSINGSTEP (thread -> frames -> mchargs_cpy);
      if (!writethreadmchargs (dc, thread)) return (0);
    }
    thread -> letitrun = 1;
    startthread (dc, thread);
    sts = waitforthreadtohalt (dc, thread);
    if (sts != OZ_SINGLESTEP) goto alldone;
  }

  /* Insert breakpoints */

  if (!insertbreakpoints (dc)) return (0);

  /* Clear singlestep flag in all threads and start them */

  for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
    if (!(thread -> exited)) {
      if (TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
        CLRSINGSTEP (thread -> frames -> mchargs_cpy);
        if (!writethreadmchargs (dc, thread)) return (0);
      }
      startthread (dc, thread);
    }
  }

  /* Wait for one thread to halt */

  thread = waitforanyhalt (dc);

  /* Halt all threads */

  haltthreads (dc, 1);

  /* Remove breakpoints */

  if (!removebreakpoints (dc)) return (0);

  /* Disable control-C; we're about to halt all threads anyway */

alldone:
  dc -> ignorectrlc = 1;

  /* Set current thread to set it all up and print new line number */

  setcurthread (dc, thread);

  /* Successful */

  return (1);
}

/************************************************************************/
/*									*/
/*  down [<count>] - go to lower call frame				*/
/*									*/
/************************************************************************/

static int cmd_down (Dc *dc, char *inbuf)

{
  char *p;
  Frame *frame;
  int count, level;

  count = 1;
  if (inbuf[0] != 0) {
    count = strtol (inbuf, &p, 0);
    if (*p == ' ') p ++;
    if (*p != 0) {
      PRINT "extra stuff at end of line %s\n", p);
      return (-1);
    }
    if (count <= 0) {
      PRINT "count %d must be greater than zero\n", count);
      return (-1);
    }
  }

  level = dc -> curthread -> curframe -> level - count;

  for (frame = dc -> curthread -> frames; frame -> next != NULL; frame = frame -> next) if (frame -> level >= level) break;
  dc -> curthread -> curframe = frame;

  return (cmd_where (dc, ""));
}

/************************************************************************/
/*									*/
/*  file - manage executable files					*/
/*									*/
/************************************************************************/

static int cmd_file (Dc *dc, char *inbuf)

{
  char *name, *p;
  Exefile *exefile;
  int i;
  OZ_Pointer baseaddr;
  uLong size;
  void *addr;

  /* If no parameter(s) given, list out known executables and modules */

  if (inbuf[0] == 0) {
    for (exefile = dc -> exefiles; exefile != NULL; exefile = exefile -> next) {
      PRINT "%s:\n", exefile -> name);
      for (i = 0; i < exefile -> nmodules; i ++) {
        PRINT "  %s 0x%X at 0x%X\n", exefile -> modules[i].name, exefile -> modules[i].textsize, exefile -> modules[i].textaddr);
      }
    }
    return (1);
  }

  /* Add the given executables */

  while ((inbuf != NULL) && (inbuf[0] != 0)) {			// loop while there are parameters left
    p = strchr (inbuf, ' ');					// they are separated by spaces
    name = inbuf;						// point to name string
    if (p != NULL) *(p ++) = 0;					// null terminate if space found
    inbuf = p;							// increment for next time through loop
    baseaddr = 0;						// assume no base address given
    p = strrchr (name, ':');					// see if there is :baseaddr on the end
    if (p != NULL) {
      *(p ++) = 0;						// ok, chop it off the name string
      baseaddr = oz_hw_atoz (p, &i);				// convert it to binary
      if (p[i] != 0) {
        PRINT "bad base address %s\n", p);
        return (-1);
      }
    }
    if (!openandmap (name, &size, &addr)) {			// open executable file, map it to our memory
      PRINT "error opening executable %s\n", name);
      return (0);
    }
    if (!openexefile (dc, name, size, addr, baseaddr)) {	// add it to list of executables we know about
      PRINT "error reading executable %s\n", name);
      return (0);
    }
  }
  return (1);
}

/************************************************************************/
/*									*/
/*  help - print general help message					*/
/*									*/
/************************************************************************/

static int cmd_help (Dc *dc, char *inbuf)

{
  char buf[32];
  int i, j;

  PRINT "\nThese are the available commands:\n\n");

  for (i = 0; cmdtbl[i].name != NULL; i ++) {
    strncpy (buf, cmdtbl[i].name, sizeof buf);
    for (j = 0; j < cmdtbl[i].mini; j ++) {
      if ((buf[j] >= 'a') && (buf[j] <= 'z')) buf[j] -= 'a' - 'A';
    }
    PRINT "%30s  %s\n", buf, cmdtbl[i].help);
  }

  PRINT "\nAll commands are case insensitive.  The caps indicate required characters, lower case are optional.\n");
  PRINT "For help on a command, enter the command name followed by a space and question mark.\n\n");
  PRINT "Last command may be repeated by pressing return (with no space).\n");
  PRINT "Separate multiple commands on one line with space-semicolon-space (' ; ').\n\n");
  return (1);
}

/************************************************************************/
/*									*/
/*  instr <address> [.. <endaddress>]					*/
/*									*/
/************************************************************************/

static int cmd_instruction (Dc *dc, char *inbuf)

{
  char const *srcname;
  char ob[64];
  Exefile *exefile;
  int i, modindx, srcindx, srcline;
  OZ_Pointer begaddr, endaddr;
  uByte ib[OZ_HW_MAXINSLEN];

  endaddr = begaddr = GETNXTINSAD (dc -> curthread -> curframe -> mchargs_cpy);		// default to next instr to execute

  if (inbuf[0] != 0) {									// see if any parameter(s) specified
    i = parseinstraddr (dc, inbuf, &begaddr, NULL);					// ok, parse instruction address
    if (i <= 0) return (i);

    endaddr = begaddr;									// default end=beg address
    if (inbuf[i] != 0) {								// see if second parameter specified
      inbuf += i;
      if ((inbuf[0] != '.') || (inbuf[1] != '.')) {					// it must begin with ..
        PRINT "extra stuff at end of line %s\n", inbuf);
        return (-1);
      }
      inbuf += 2;
      i = parseinstraddr (dc, inbuf, &endaddr, NULL);					// parse end address
      if (i <= 0) return (i);
      if (inbuf[i] != 0) {
        PRINT "extra stuff at end of line %s\n", inbuf + i);
        return (-1);
      }
    }
  }

  i = sizeof ib;
  do {
    if (!READMEM (i, begaddr + sizeof ib - i, ib + sizeof ib - i)) return (0);		// fill ib
    i = oz_sys_disassemble (sizeof ib, ib, (uByte const *)begaddr, sizeof ob, ob, NULL); // disassemble
    if (i <= 0) break;
    if (!findsourceline (dc, begaddr, &exefile, &modindx, &srcname, &srcindx, &srcline)) PRINT "%p: %s\n", begaddr, ob);
    else PRINT "%s %s %d %p: %s\n", exefile -> name, srcname, srcline, begaddr, ob);	// print resulting instruction
    if (begaddr + i <= begaddr) break;							// stop if addr overflow
    memmove (ib, ib + i, sizeof ib - i);						// shift down over used bytes
    begaddr += i;									// inc addr for next instr
  } while (begaddr <= endaddr);								// repeat if user wants more

  return (1);
}

/************************************************************************/
/*									*/
/*  print <expr>[,...]							*/
/*									*/
/************************************************************************/

static int cmd_print (Dc *dc, char *inbuf)

{
  Frame *curframe;
  int l;
  Var var;

  while (*inbuf != 0) {
    curframe = dc -> curthread -> curframe;
    l = getexpression (0, 0, dc, curframe -> exefile, curframe -> modindx, 
                       &(curframe -> mchargs_cpy), (OZ_Pointer)(curframe -> mchargs_adr), 
                       strlen (inbuf), inbuf, &var);
    if (l == 0) return (0);
    if ((inbuf[l] != ',') && (inbuf[l] != 0)) {
      PRINT "extra stuff at end of line %s\n", inbuf + l);
      return (-1);
    }
    PRINT "  %*.*s=", l, l, inbuf);
    if (!printvariable (dc, curframe -> exefile, curframe -> modindx, 1, var)) {
      PRINT "error printing value\n");
      return (-1);
    }
    PRINT "\n");
    inbuf += l;
    if (*inbuf == ',') inbuf ++;
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  quit - terminate debugging						*/
/*									*/
/************************************************************************/

static int cmd_quit (Dc *dc, char *inbuf)

{
  char buf[16];
  Thread *thread;
  uLong sts;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  while (readprompt (dc, "cmd_quit: abort threads, continue threads, don't quit (a/c/d)? ", sizeof buf, buf)) {
    switch (buf[0]) {

      /* Abort - just use normal thread abort.  The abort asts will queue but won't execute until we 'continue' the threads. */

      case 'A': case 'a': {
        for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
          oz_sys_thread_abort (thread -> h_thread, OZ_ABORTED);
        }
      }

      /* Continue - resume execution on each thread and detach from them */

      case 'C': case 'c': {
        dc -> exiting = 1;
        for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
          if (!(thread -> exited)) {
            CLRSINGSTEP (thread -> frames -> mchargs_cpy);
            sts = oz_sys_process_poke (dc -> h_process, OZ_PROCMODE_KNL, sizeof thread -> frames -> mchargs_cpy, &(thread -> frames -> mchargs_cpy), thread -> mchargs_adr);
            if (sts != OZ_SUCCESS) PRINT "cmd_quit: error %u writing %u mchargs at %p\n", sts, thread -> threadid, thread -> mchargs_adr);
          }
          sts = oz_sys_event_set (OZ_PROCMODE_KNL, thread -> h_waitevent, OZ_THREAD_HALT_RESUME + 1, NULL);
          if (sts != OZ_SUCCESS) PRINT "cmd_quit: error %u setting %u waitevent\n", sts, thread -> threadid);
        }
        oz_sys_thread_exit (OZ_SUCCESS);
      }

      /* Don't quit */

      case 'D': case 'd': return (1);
    }
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  tcontinue - continue the current thread (others stay halted)	*/
/*									*/
/************************************************************************/

static int cmd_tcontinue (Dc *dc, char *inbuf)

{
  Thread *thread;
  uLong sts;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  /* Set singlestep flag in target thread */

  thread = dc -> curthread;

  if (!TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
    SETSINGSTEP (thread -> frames -> mchargs_cpy);
    if (!writethreadmchargs (dc, thread)) return (0);
  }

  /* Arm control-C */

  dc -> ignorectrlc = 0;

  /* Tell thread to run stepping it over possible breakpoint instruction and wait for it to halt */

  thread -> letitrun = 1;
  startthread (dc, thread);
  sts = waitforthreadtohalt (dc, thread);

  /* If halted for other than SINGLESTEP, we're done */

  if (sts != OZ_SINGLESTEP) goto alldone;

  /* Clear singlestep flag in target thread */

  if (TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
    CLRSINGSTEP (thread -> frames -> mchargs_cpy);
    if (!writethreadmchargs (dc, thread)) return (0);
  }

  /* Insert breakpoints */

  if (!insertbreakpoints (dc)) return (0);

  /* Start thread going and wait for it to halt */

  startthread (dc, thread);
  sts = waitforthreadtohalt (dc, thread);

  /* Remove breakpoints */

  if (!removebreakpoints (dc)) return (0);

  /* Disable control-C; we're about to halt all threads anyway */

alldone:
  dc -> ignorectrlc = 1;

  /* Halt any newly created threads */

  haltthreads (dc, 1);

  /* Set current thread to set it all up and print new line number */

  setcurthread (dc, thread);

  /* Successful */

  return (1);
}

/************************************************************************/
/*									*/
/*  thread - manage threads						*/
/*									*/
/************************************************************************/

static int cmd_thread (Dc *dc, char *inbuf)

{
  char const *funname, *p;
  char cur;
  Frame *frame;
  int l;
  OZ_Pointer funaddr, nxtinsad;
  OZ_Threadid threadid;
  Thread *thread;
  uLong sts;

  /* Make sure our thread list is up-to-date */

  haltthreads (dc, 1);

  /* If no parameter, list out the threads we know about */

  if (inbuf[0] == 0) {
    for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
      cur = ' ';
      if (thread == dc -> curthread) cur = '*';
      if (thread -> exited) {
        PRINT "%c%u: exited, status %u\n", cur, thread -> threadid, thread -> haltsts);
      } else {
        frame = thread -> frames;
        printframespot (dc, thread, frame);
      }
    }
    return (1);
  }

  /* Parameter given, select the thread-id */

  threadid = oz_hw_atoi (inbuf, &l);
  if (inbuf[l] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf + l);
    return (-1);
  }

  for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
    if (thread -> threadid == threadid) {
      setcurthread (dc, thread);
      return (1);
    }
  }

  PRINT "unknown thread %u\n", threadid);
  return (0);
}

/************************************************************************/
/*									*/
/*  tnext - step the current thread to next source line in same 	*/
/*          function (other threads stay halted)			*/
/*									*/
/************************************************************************/

static int cmd_tnext (Dc *dc, char *inbuf)

{
  Bpt sobpt;
  char const *funname, *oldfunname, *oldsrcname, *srcname;
  Exefile *exefile, *oldexefile;
  Frame *frame0;
  int err, modindx, oldmodindx, oldsrcindx, oldsrcline, solen, srcindx, srcline;
  OZ_Pointer funaddr, oldfunaddr;
  Thread *thread;
  typeof (FRAMEPTR (frame0 -> mchargs_cpy)) callframe, oldframeptr;
  uLong sts;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  /* Save our starting point */

  thread = dc -> curthread;
  frame0 = thread -> frames;

  if (!findsourceline (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &oldexefile, 
                       &oldmodindx, &oldsrcname, &oldsrcindx, &oldsrcline)) return (0);

  oldframeptr = FRAMEPTR (frame0 -> mchargs_cpy);
  findfunctionname (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &oldfunname, &oldfunaddr, NULL, NULL, NULL);

  /* Set singlestep flag in target thread */

  if (!TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
    SETSINGSTEP (frame0 -> mchargs_cpy);
    if (!writethreadmchargs (dc, thread)) return (0);
  }

  /* Tell whoever cares that we want this thread to run */
  /* Also arm control-C so user can force it to halt    */

  thread -> letitrun = 1;
  dc -> ignorectrlc  = 0;

  do {
    do {

      /* If we are about to call a subroutine, set a temp breakpoint */
      /* at the return point and run the subroutine at full speed.   */

      solen = STEPOVER (frame0 -> mchargs_cpy);
      if (solen > 0) {
        memset (&sobpt, 0, sizeof sobpt);
        sobpt.next = dc -> bpts;
        sobpt.enabled = 1;
        sobpt.address = GETNXTINSAD (frame0 -> mchargs_cpy) + solen;
        dc -> bpts = &sobpt;
        callframe = FRAMEPTR (frame0 -> mchargs_cpy);
        while (1) {
          err = 1;
          CLRSINGSTEP (frame0 -> mchargs_cpy);					// turn off single-stepping
          if (!writethreadmchargs (dc, thread)) break;
          if (!insertbreakpoints (dc)) break;					// insert breakpoints
          startthread (dc, thread);						// execute subroutine
          sts = waitforthreadtohalt (dc, thread);
          if (!removebreakpoints (dc)) break;					// remove breakpoints
          if (sts != OZ_BREAKPOINT) break;					// see if stopped at a breakpoint
          if (GETNXTINSAD (frame0 -> mchargs_cpy) != sobpt.address) break;	// stopped at some other breakpoint
          SETSINGSTEP (frame0 -> mchargs_cpy);					// turn stepping back on
          if (!writethreadmchargs (dc, thread)) break;
          err = 0;
          if (FRAMEPTR (frame0 -> mchargs_cpy) == callframe) break;		// if same call frame, we're all done
          startthread (dc, thread);						// step over breakpointed instruction
          sts = waitforthreadtohalt (dc, thread);
          if (sts != OZ_SINGLESTEP) break;
        }
        dc -> bpts = sobpt.next;						// done, unlink temp breakpoint
        if (err || (sts != OZ_BREAKPOINT) || (GETNXTINSAD (frame0 -> mchargs_cpy) != sobpt.address)) goto stopped;
      }

      /* Not calling a subroutine, singlestep over the instruction */

      else {
        startthread (dc, thread);
        sts = waitforthreadtohalt (dc, thread);
        if (sts != OZ_SINGLESTEP) goto stopped;
      }

      /* If we're not in outer frame or we're in a different function, keep going */

      if (OUTFRAME (frame0 -> mchargs_cpy, oldframeptr)) goto stopped;
      findfunctionname (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &funname, &funaddr, NULL, NULL, NULL);
    } while (funname != oldfunname);

    /* Same function and frame */

    /* Repeat as long as we're on original source line.  If we can't tell what line we're on now, pretend we're still */
    /* on original line.  This happens like when we're jumping through the .got or starting a system service call.    */

  } while (!findsourceline (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &exefile, &modindx, &srcname, &srcindx, &srcline) 
        || ((exefile == oldexefile) && (modindx == oldmodindx) && (srcname == oldsrcname) && (srcline == oldsrcline)));

  /* We don't want it to run anymore, and disarm control-C */

stopped:
  thread -> letitrun = 0;
  dc -> ignorectrlc  = 1;

  /* Halt any newly created threads */

  haltthreads (dc, 1);

  /* Set current thread to set it all up and print new line number */

  setcurthread (dc, thread);

  /* Successful */

  return (1);
}

/************************************************************************/
/*									*/
/*  tstep - step the current thread to next source line (other threads 	*/
/*          stay halted)						*/
/*									*/
/************************************************************************/

static int cmd_tstep (Dc *dc, char *inbuf)

{
  char const *oldsrcname, *srcname;
  Exefile *exefile, *oldexefile;
  Frame *frame0;
  int modindx, oldmodindx, oldsrcindx, oldsrcline, srcindx, srcline;
  Thread *thread;
  uLong sts;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  /* Save our starting point */

  thread = dc -> curthread;
  frame0 = thread -> frames;

  if (!findsourceline (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &oldexefile, 
                       &oldmodindx, &oldsrcname, &oldsrcindx, &oldsrcline)) return (0);

  /* Set singlestep flag in target thread */

  if (!TSTSINGSTEP (frame0 -> mchargs_cpy)) {
    SETSINGSTEP (frame0 -> mchargs_cpy);
    if (!writethreadmchargs (dc, thread)) return (0);
  }

  /* Tell whoever cares that we want this thread to run */
  /* Also arm control-C so user can force it to halt    */

  thread -> letitrun = 1;
  dc -> ignorectrlc  = 0;

  do {

    /* Tell thread to start */

    startthread (dc, thread);

    /* Wait for thread to halt */

    sts = waitforthreadtohalt (dc, thread);

    /* If it halted for other than SINGLESTEP, stop looping */

    if (sts != OZ_SINGLESTEP) break;

    /* Repeat as long as we're on original source line.  If we can't tell what line we're on now, pretend we're still */
    /* on original line.  This happens like when we're jumping through the .got or starting a system service call.    */

  } while (!findsourceline (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &exefile, &modindx, &srcname, &srcindx, &srcline) 
        || ((exefile == oldexefile) && (modindx == oldmodindx) && (srcname == oldsrcname) && (srcline == oldsrcline)));

  /* We don't want it to run anymore, and disarm control-C */

  thread -> letitrun = 0;
  dc -> ignorectrlc  = 1;

  /* Halt any newly created threads */

  haltthreads (dc, 1);

  /* Set current thread to set it all up and print new line number */

  setcurthread (dc, thread);

  /* Successful */

  return (1);
}

/************************************************************************/
/*									*/
/*  tstepi - step the current thread one machine instruction		*/
/*           (other threads stay halted)				*/
/*									*/
/************************************************************************/

static int cmd_tstepi (Dc *dc, char *inbuf)

{
  Thread *thread;
  uLong sts;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  /* Set singlestep flag in target thread */

  thread = dc -> curthread;

  if (!TSTSINGSTEP (thread -> frames -> mchargs_cpy)) {
    SETSINGSTEP (thread -> frames -> mchargs_cpy);
    if (!writethreadmchargs (dc, thread)) return (0);
  }

  /* Tell whoever cares that we want this thread to run */
  /* Also arm control-C so user can force it to halt    */

  thread -> letitrun = 1;
  dc -> ignorectrlc  = 0;

  /* Tell thread to start */

  startthread (dc, thread);

  /* Wait for thread to halt */

  waitforthreadtohalt (dc, thread);

  /* We don't want it to run anymore, and disarm control-C */

  thread -> letitrun = 0;
  dc -> ignorectrlc  = 1;

  /* Halt any newly created threads */

  haltthreads (dc, 1);

  /* Set current thread to set it all up and print new line number */

  setcurthread (dc, thread);

  /* Successful */

  return (1);
}

/************************************************************************/
/*									*/
/*  where - display call frame trace					*/
/*									*/
/************************************************************************/

static int cmd_where (Dc *dc, char *inbuf)

{
  Frame *frame;

  if (inbuf[0] != 0) {
    PRINT "extra stuff at end of line %s\n", inbuf);
    return (-1);
  }

  for (frame = dc -> curthread -> frames; frame != NULL; frame = upperframe (dc, dc -> curthread, frame)) {
    printframespot (dc, dc -> curthread, frame);
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  up [<count>] - go to upper call frame				*/
/*									*/
/************************************************************************/

static int cmd_up (Dc *dc, char *inbuf)

{
  char *p;
  Frame *frame;
  int count;

  count = 1;
  if (inbuf[0] != 0) {
    count = strtol (inbuf, &p, 0);
    if (*p == ' ') p ++;
    if (*p != 0) {
      PRINT "extra stuff at end of line %s\n", p);
      return (-1);
    }
    if (count <= 0) {
      PRINT "count %d must be greater than zero\n", count);
      return (-1);
    }
  }

  while (-- count >= 0) {
    frame = upperframe (dc, dc -> curthread, dc -> curthread -> curframe);
    if (frame == NULL) break;
    dc -> curthread -> curframe = frame;
  }

  return (cmd_where (dc, ""));
}

/************************************************************************/
/*									*/
/*  Open a file and map it to memory					*/
/*									*/
/************************************************************************/

static int openandmap (char const *name, uLong *size_r, void **addr_r)

{
  OZ_Handle h_file, h_section;
  OZ_IO_fs_open fs_open;
  OZ_Mempage npagem, svpage;
  uLong sts;

  /* Open file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = name;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_file);
  if (sts != OZ_SUCCESS) {
    printf ("error %u opening %s\n", sts, name);
    return (0);
  }

  /* Map it to memory */

  sts = oz_sys_section_create (OZ_PROCMODE_KNL, h_file, 0, 1, 0, &h_section);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
  if (sts != OZ_SUCCESS) {
    printf ("error %u creating section for %s\n", sts, name);
    return (0);
  }
  npagem = 0;
  svpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_USRHEAP_VA);
  sts = oz_sys_process_mapsection (OZ_PROCMODE_KNL, h_section, &npagem, &svpage, OZ_HW_DVA_USRHEAP_AT, OZ_HW_PAGEPROT_UR);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_section);
  if (sts != OZ_SUCCESS) {
    printf ("error %u mapping section for %s\n", sts, name);
    return (0);
  }

  *size_r = npagem << OZ_HW_L2PAGESIZE;
  *addr_r = OZ_HW_VPAGETOVADDR (svpage);
  return (1);
}

/************************************************************************/
/*									*/
/*  Parse address string for 'break' command				*/
/*									*/
/*    Input:								*/
/*									*/
/*	inbuf = points to input string					*/
/*									*/
/*    Output:								*/
/*									*/
/*	parseinstraddr <= 0 : parse error				*/
/*	               else : number of chars of inbuf parsed		*/
/*	*bptaddr = resultant address					*/
/*	*descr = NULL : use original input for description		*/
/*	         else : points to malloc'd buff with description	*/
/*									*/
/************************************************************************/

static int parseinstraddr (Dc *dc, char const *inbuf, OZ_Pointer *bptaddr, char **descr)

{
  char const *p, *saveinbuf, *srcname;
  char *q;
  Exefile *ef, *exefile;
  Frame *curframe;
  int i, lineno, modindx, numfound, srcindx, srcline;
  Var var;

  if (descr != NULL) *descr = NULL;

  curframe = dc -> curthread -> curframe;

  /* - '*'address */

  if (*inbuf == '*') {
    inbuf ++;
    i = getexpression (0, 0, dc, curframe -> exefile, curframe -> modindx,
                       &(curframe -> mchargs_cpy), (OZ_Pointer)(curframe -> mchargs_adr),
                       strlen (inbuf), inbuf, &var);
    if (!readvarvalue (dc, &var)) return (0);
    *bptaddr = var.v_valu;
    return (++ i);
  }

  /* - [executablename] [sourcefilename] function name or line number */

  saveinbuf = inbuf;

  /* Parse out executablename */

  exefile = curframe -> exefile;
  p = strchr (inbuf, ' ');
  if (p != NULL) {
    i = p - inbuf;
    q = strchr (inbuf, '.');
    if ((q != NULL) && (q < p)) {
      for (exefile = dc -> exefiles; exefile != NULL; exefile = exefile -> next) {
        if ((exefile -> name[i] == 0) && (memcmp (exefile -> name, inbuf, i) == 0)) break;
      }
      if (exefile != NULL) inbuf = ++ p;
      else exefile = curframe -> exefile;
    }
  }

  /* Parse out sourcefilename */

  modindx = curframe -> modindx;
  p = strchr (inbuf, ' ');
  if (p != NULL) {
    i = p - inbuf;
    q = strchr (inbuf, '.');
    if ((q != NULL) && (q < p)) {
      for (modindx = exefile -> nmodules; -- modindx >= 0;) {
        if ((exefile -> modules[modindx].name[i] == 0) && (memcmp (exefile -> modules[modindx].name, inbuf, i) == 0)) break;
      }
      if (modindx >= 0) inbuf = ++ p;
      else modindx = curframe -> modindx;
    }
  }
  srcname = exefile -> modules[modindx].name;

  /* The rest is function name or line number in that module */

  if (*inbuf == ' ') inbuf ++;
  for (p = inbuf; LEGALVARCHAR (*p); p ++) {}

  lineno = strtol (inbuf, &q, 10);
  if (q == p) {
    if (!findsrclineaddr (dc, exefile, modindx, lineno, bptaddr)) {
      PRINT "parseinstraddr: cannot find address of source line %s %s %d\n", exefile -> name, srcname, lineno);
      return (-1);
    }
  } else {

    /* Look for function in given or default executable and sourcefile */

    if (!findfunctionaddr (dc, exefile, modindx, p - inbuf, inbuf, bptaddr)) {

      /* Can't find function in given or default executable and sourcefile */
      /* Look for it in all modules of given or default executable         */

      numfound = 0;
      for (i = 0; i < exefile -> nmodules; i ++) {
        if (findfunctionaddr (dc, exefile, i, p - inbuf, inbuf, bptaddr)) {
          modindx = i;
          numfound ++;
        }
      }

      /* If found more than once, print out all occurences so user can select one */

      if (numfound > 1) {
        for (i = 0; i < exefile -> nmodules; i ++) {
          if (findfunctionaddr (dc, exefile, i, p - inbuf, inbuf, bptaddr)) {
            PRINT "parseinstraddr: found %s %s %*.*s at 0x%X\n", exefile -> name, exefile -> modules[i].name, p - inbuf, p - inbuf, inbuf, *bptaddr);
          }
        }
        return (-1);
      }

      /* If not found at all, search all modules in all executables */

      if (numfound == 0) {
        for (ef = dc -> exefiles; ef != NULL; ef = ef -> next) {
          for (i = 0; i < ef -> nmodules; i ++) {
            if (findfunctionaddr (dc, ef, i, p - inbuf, inbuf, bptaddr)) {
              exefile = ef;
              modindx = i;
              numfound ++;
            }
          }
        }
      }

      /* If found more than once, print out all occurences so user can select one */

      if (numfound > 1) {
        for (ef = dc -> exefiles; ef != NULL; ef = ef -> next) {
          for (i = 0; i < exefile -> nmodules; i ++) {
            if (findfunctionaddr (dc, ef, i, p - inbuf, inbuf, bptaddr)) {
              PRINT "parseinstraddr: found %s %s %*.*s at 0x%X\n", ef -> name, ef -> modules[i].name, p - inbuf, p - inbuf, inbuf, *bptaddr);
            }
          }
        }
        return (-1);
      }

      /* If none found, too bad */

      if (numfound == 0) {
        PRINT "parseinstraddr: cannot find address of function <%*.*s> anywhere\n", p - inbuf, p - inbuf, inbuf);
        return (-1);
      }
    }
  }

  /* Find actual corresponding source and line number in case we skipped ahead */

  if (descr != NULL) {
    if (findsourceline (dc, *bptaddr, &exefile, &modindx, &srcname, &srcindx, &srcline)) {
      *descr = MALLOC (strlen (exefile -> name) + strlen (srcname) + 20);
      sprintf (*descr, "%s %s %d", exefile -> name, srcname, srcline);
    }
  }

  /* Return number of input chars parsed */

  if (*p == ' ') p ++;
  return (p - saveinbuf);
}

/************************************************************************/
/*									*/
/*  Halt threads							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc -> threads = list of threads to halt				*/
/*	all = 0 : don't halt the 'letitrun' threads			*/
/*	      1 : halt everything, clear all letitrun flags		*/
/*									*/
/*    Output:								*/
/*									*/
/*	dc -> threads = updated to include newly created threads	*/
/*									*/
/************************************************************************/

static void haltthreads (Dc *dc, int all)

{
  OZ_Handle h_thread, h_nexthread;
  OZ_Threadid threadid;
  Thread **lthread, *thread;
  uLong sts, threadqseq;

  OZ_Handle_item firsthread[1] = { OZ_HANDLE_CODE_THREAD_FIRST, sizeof h_thread,    &h_thread,    NULL };
  OZ_Handle_item nexthread[2]  = { OZ_HANDLE_CODE_THREAD_ID,    sizeof threadid,    &threadid,    NULL, 
                                   OZ_HANDLE_CODE_THREAD_NEXT,  sizeof h_nexthread, &h_nexthread, NULL };

  while (1) {

    /* Any threads we know about that we haven't tried to halt yet, queue an halt to them */

    for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
      if (all) thread -> letitrun = 0;
      if (!(thread -> exited) && !(thread -> letitrun) && (thread -> memprocid == 0) && !(thread -> haltqueued)) {
        thread -> haltqueued = 1;
        sts = oz_sys_thread_halt (thread -> h_thread, OZ_PROCMODE_KNL);
        if (sts != OZ_SUCCESS) PRINT "haltthreads: error %u halting thread %u\n", sts, thread -> threadid);
      }
    }

    /* Wait for all threads we know about to halt.  They indicate halted state by setting memprocid non-zero. */

    for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
      if (!(thread -> letitrun)) waitforthreadtohalt (dc, thread);
    }

    /* If not all, don't bother checking for new threads.  Possibly the     */
    /* one(s) left running are continually creating and destroying threads. */

    if (!all) break;

    /* If no threads have been added to the process, we're done */

    sts = oz_sys_process_getthreadqseq (OZ_PROCMODE_KNL, dc -> h_process, &threadqseq);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (threadqseq == dc -> threadqseq) break;
    dc -> threadqseq = threadqseq;

    /* There may be new threads, add to the list */

    sts = oz_sys_handle_getinfo (dc -> h_process, 1, firsthread, NULL);		// get first thread in the process
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    while (h_thread != 0) {
      sts = oz_sys_handle_getinfo (h_thread, 2, nexthread, NULL);		// find out about thread
      if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
      for (lthread = &(dc -> threads); (thread = *lthread) != NULL; lthread = &(thread -> next)) {
        if (thread -> threadid >= threadid) break;				// see if it's already in list
      }
      if ((thread == NULL) || (thread -> threadid > threadid)) {
        attachthread (dc, lthread, threadid, h_thread);				// if not, create new list element
      } else {
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);			// if so, toss temp handle
      }
      h_thread = h_nexthread;							// anyway, maybe there are more to check
    }
  }
}

/************************************************************************/
/*									*/
/*  Attach to a given thread						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc = debug context block pointer				*/
/*	lthread  = last thread in list					*/
/*	threadid = thread to be attached				*/
/*	h_thread = handle to that thread				*/
/*									*/
/*    Output:								*/
/*									*/
/*	thread block added to dc->threads list				*/
/*									*/
/************************************************************************/

static Thread *attachthread (Dc *dc, Thread **lthread, OZ_Threadid threadid, OZ_Handle h_thread)

{
  Thread *thread;
  uLong sts;

  PRINT "(attachthread: attaching thread %u)\n", threadid);

  thread = MALLOC (sizeof *thread);
  memset (thread, 0, sizeof *thread);
  thread -> next = *lthread;
  thread -> threadid = threadid;
  thread -> h_thread = h_thread;
  *lthread = thread;

  thread -> frames = thread -> curframe = MALLOC (sizeof *(thread -> frames));
  memset (thread -> frames, 0, sizeof *(thread -> frames));

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, waiteventname, &(thread -> h_waitevent));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  sts = oz_sys_thread_attach (h_thread, OZ_PROCMODE_KNL, thread -> h_waitevent, dc -> h_haltevent, &(thread -> sigargs_adr), 
                              &(thread -> mchargs_adr), &(thread -> mchargx_adr), &(thread -> memprocid));
  if (sts != OZ_SUCCESS) {
    thread -> exited = 1;
    thread -> memprocid = dc -> processid;
    if (sts != OZ_THREADEAD) PRINT "(attachthread: error %u attaching thread %u)\n", sts, threadid);
    else {
      sts = oz_sys_thread_getexitsts (thread -> h_thread, &(thread -> haltsts));
      if (sts == OZ_SUCCESS) goto rtn;
      PRINT "attachthread: error %u fetching exit status from thread %u\n", sts, threadid);
    }
    thread -> haltsts = sts;
  }
rtn:
  return (thread);
}

/************************************************************************/
/*									*/
/*  Start thread							*/
/*									*/
/************************************************************************/

static void startthread (Dc *dc, Thread *thread)

{
  uLong sts;

  if (!(thread -> exited)) {

    /* Clear memprocid.  The target thread will set it when it is about to halt. */

    thread -> memprocid = 0;

    /* Set h_waitevent.  This causes the thread to resume execution at whatever is in target's memory for mchargs,mchargx, etc. */

    sts = oz_sys_event_set (OZ_PROCMODE_KNL, thread -> h_waitevent, OZ_THREAD_HALT_ATTACH + OZ_THREAD_HALT_RESUME + 1, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }
}

/************************************************************************/
/*									*/
/*  Wait for any thread to halt						*/
/*									*/
/*    Output:								*/
/*									*/
/*	waitforanyhalt = thread that halted				*/
/*									*/
/************************************************************************/

static Thread *waitforanyhalt (Dc *dc)

{
  Thread *thread;
  uLong sts;

  while (1) {
    for (thread = dc -> threads; thread != NULL; thread = thread -> next) {
      if (!(thread -> exited) && (thread -> memprocid != 0)) return (thread);
    }
    oz_sys_event_wait (OZ_PROCMODE_KNL, dc -> h_haltevent, 0);
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, dc -> h_haltevent, 0, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }
}

/************************************************************************/
/*									*/
/*  Wait for thread to halt						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to wait for					*/
/*									*/
/*    Output:								*/
/*									*/
/*	waitforthreadtohalt = thread -> haltsts = halt status:		*/
/*		OZ_HALTED = halted from call to haltthreads		*/
/*		OZ_SINGLESTEP = halted from singlestep trace bit	*/
/*		OZ_BREAKPOINT = halted from breakpoint instruction	*/
/*		OZ_THREADEAD = thread has exited			*/
/*		else = halted from other signal				*/
/*	its sigargs, mchargs, mchargx read from target and frame cache 	*/
/*	  reset								*/
/*									*/
/*    Note:								*/
/*									*/
/*	For OZ_BREAKPOINT, the hardware dependent part of the kernel 	*/
/*	has already backed up the 'next instruction address' to that 	*/
/*	of the breakpoint.						*/
/*									*/
/************************************************************************/

static uLong waitforthreadtohalt (Dc *dc, Thread *thread)

{
  Bpt *bpt;
  Frame *frame0;
  uLong exitsts, i, j, n, sts;

  /* Maybe we know we've exited */

  if (thread -> exited) return (OZ_THREADEAD);

  /* Release any old upper frames and clear the frame0 struct */

  while ((frame0 = thread -> frames -> next) != NULL) {				// see if there is an upper level frame
    thread -> frames -> next = frame0 -> next;					// if so, unlink it
    FREE (frame0);								// free it off
  }
  frame0 = thread -> frames;							// point to level 0 frame
  thread -> curframe = frame0;							// make sure curframe points to something valid

waitforit:
  memset (frame0, 0, sizeof *frame0);						// we know it isn't right as thread is running

  /* Thread signifies it has halted by setting memprocid then setting h_haltevent */

  while (thread -> memprocid == 0) {						// repeat until it says it is halted
    oz_sys_event_wait (OZ_PROCMODE_KNL, dc -> h_haltevent, 0);			// wait for something to halt
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, dc -> h_haltevent, 0, NULL);	// reset in case we need to wait again
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }
  if (thread -> memprocid != dc -> processid) {					// this better match or it's a royal puque
    PRINT "waitforthreadtohalt: thread %u memprocid %u, main procid %u\n", thread -> threadid, thread -> memprocid, dc -> processid);
    oz_sys_condhand_signal (2, OZ_BUGCHECK, 0);
  }

  /* Now that thread has halted, clear 'haltqueued' flag to allow another halt to be queued to it */

  thread -> haltqueued = 0;

  /* If sigargs pointer is NULL, it means the thread exited, so get its exit status */

  if (thread -> sigargs_adr == NULL) {
    if (thread -> mchargx_adr == NULL) {
      sts = oz_sys_thread_getexitsts (thread -> h_thread, &exitsts);
      if (sts != OZ_SUCCESS) PRINT "waitforthreadtohalt: error %u fetching exit status\n", sts);
    } else {
      sts = oz_sys_process_peek (dc -> h_process, OZ_PROCMODE_KNL, sizeof exitsts, thread -> mchargx_adr, &exitsts);
      if (sts != OZ_SUCCESS) PRINT "waitforthreadtohalt: error %u reading exit status from %X\n", sts, thread -> mchargx_adr);
    }
    if (sts == OZ_SUCCESS) sts = exitsts;
    PRINT "waitforthreadtohalt: thread %u exited, status %u\n", thread -> threadid, sts);
    goto exitsts;
  }

  /* Read mchargs and sigargs to get halt status */

  sts = oz_sys_process_peek (dc -> h_process, OZ_PROCMODE_KNL, sizeof frame0 -> mchargs_cpy, thread -> mchargs_adr, &(frame0 -> mchargs_cpy));
  if (sts != OZ_SUCCESS) {
    PRINT "waitforthreadtohalt: error %u fetching mchargs at %p\n", sts, thread -> mchargs_adr);
    goto exitsts;
  }

  sts = oz_sys_process_peek (dc -> h_process, OZ_PROCMODE_KNL, sizeof frame0 -> mchargx_cpy, thread -> mchargx_adr, &(frame0 -> mchargx_cpy));
  if (sts != OZ_SUCCESS) {
    PRINT "waitforthreadtohalt: error %u fetching mchargx at %p\n", sts, thread -> mchargx_adr);
    goto exitsts;
  }

  frame0 -> mchargs_adr = thread -> mchargs_adr;
  frame0 -> mchargx_adr = thread -> mchargx_adr;

  sts = oz_sys_process_peek (dc -> h_process, OZ_PROCMODE_KNL, sizeof thread -> sigargs_cpy[0], thread -> sigargs_adr, thread -> sigargs_cpy + 0);
  if (sts != OZ_SUCCESS) {
    PRINT "waitforthreadtohalt: error %u reading sigargs[0] at %p\n", sts, thread -> sigargs_cpy + 0);
    goto exitsts;
  }
  exitsts = thread -> sigargs_cpy[0];
  if (exitsts > SIGARGS_MAX - 1) exitsts = SIGARGS_MAX - 1;
  sts = oz_sys_process_peek (dc -> h_process, OZ_PROCMODE_KNL, exitsts * sizeof thread -> sigargs_cpy[0], thread -> sigargs_adr + 1, thread -> sigargs_cpy + 1);
  if (sts != OZ_SUCCESS) {
    PRINT "waitforthreadtohalt: error %u reading sigargs[1..%u] at %p\n", sts, exitsts, thread -> sigargs_cpy + 0);
    goto exitsts;
  }

  /* Analyze sigargs.  HALT is the lowest priority, followed by SINGLESTEP then BREAKPOINT.  If there */
  /* is anything else in there, stop on the first such thing found and use it as the halt status.     */

  sts = OZ_HALTED;
  for (n = 1; n <= exitsts; n += thread -> sigargs_cpy[n] + 1) {
    switch (thread -> sigargs_cpy[n]) {
      case OZ_HALTED: {			// if HALTED, leave sts = HALTED, SINGLESTEP or BREAKPOINT as it is
        break;
      }
      case OZ_SINGLESTEP: {		// if ZINGLESCHTEP, change possible sts = HALTED to SINGLESTEP
        if (sts == OZ_HALTED) sts = OZ_SINGLESTEP;
        break;
      }
      case OZ_BREAKPOINT: {		// if BREAKPOINT, change possible sts = HALTED or SINGLESTEP to BREAKPOINT
        for (bpt = dc -> bpts; bpt != NULL; bpt = bpt -> next) {
          if ((bpt -> address == GETNXTINSAD (frame0 -> mchargs_cpy)) && (bpt -> number > 0)) {
            PRINT "waitforthreadtohalt: thread %u halted on breakpoint %u: %s\n", thread -> threadid, bpt -> number, bpt -> string);
          }
        }
        sts = OZ_BREAKPOINT;
        break;
      }
      default: {				// anything else (including WATCHPOINT), save it and print all sigargs
        sts = thread -> sigargs_cpy[n];
        PRINT "waitforthreadtohalt: thread %u halted on signal %u:", thread -> threadid, sts);
        for (i = 0; ++ i <= exitsts;) {
          PRINT " %u", thread -> sigargs_cpy[i]);
          if (++ i < exitsts) {
            if (thread -> sigargs_cpy[i] > 0) {
              PRINT "(0x%X", thread -> sigargs_cpy[i+1]);
              for (j = 1; (++ j <= thread -> sigargs_cpy[i]) && (i + j <= exitsts);) {
                PRINT " 0x%X", thread -> sigargs_cpy[i+j]);
              }
              PRINT ")");
            }
            i += j;
          }
        }
        PRINT "\n");
        goto gothaltsts;
      }
    }
    if (n >= exitsts) break;
    if (thread -> sigargs_cpy[++n] >= exitsts) break;
  }

  /* If status HALTED, it may be from an old halt ast.  So if caller wants it to keep running, restart it then wait again. */

  if ((sts == OZ_HALTED) && (thread -> letitrun)) {
    startthread (dc, thread);
    goto waitforit;
  }
gothaltsts:
  thread -> haltsts = sts;
  return (sts);

  /* Thread actually exited or we were unable to read sigargs/mchargs/mchargx */

exitsts:
  thread -> exited   = 1;
  thread -> letitrun = 0;
  thread -> haltsts  = sts;
  return (OZ_THREADEAD);
}

/************************************************************************/
/*									*/
/*  Set current thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc = debugger context						*/
/*	thread = thread to make current					*/
/*									*/
/*    Output:								*/
/*									*/
/*	thread made current						*/
/*									*/
/************************************************************************/

static int setcurthread (Dc *dc, Thread *thread)

{
  Frame *frame0;
  uLong sts;

  dc -> curthread = thread;

  /* If thread has exited, print a simple message */

  if (thread -> exited) {
    PRINT "setcurthread: %u exited, status %u\n", thread -> threadid, thread -> haltsts);
    return (0);
  }

  /* Thread still going, print where it is halted */

  frame0 = thread -> frames;

  PRINT "setcurthread: %u halted at %p, sigargs %p, mchargs %p, mchargx %p\n", 
	thread -> threadid, GETNXTINSAD (frame0 -> mchargs_cpy), 
	thread -> sigargs_adr, thread -> mchargs_adr, thread -> mchargx_adr);

  /* Figure out what source line it's on */

  if (findsourceline (dc, GETNXTINSAD (frame0 -> mchargs_cpy), &(frame0 -> exefile), &(frame0 -> modindx), 
                      &(frame0 -> srcname), &(frame0 -> srcindx), &(frame0 -> srcline))) {
    PRINT "setcurthread: exefile %s, srcname %s, srcline %d\n", frame0 -> exefile -> name, frame0 -> srcname, frame0 -> srcline);
  } else {
    PRINT "setcurthread: cannot find source line at %p\n", GETNXTINSAD (frame0 -> mchargs_cpy));
    frame0 -> srcline = -1;
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Read and Write the target process' memory				*/
/*									*/
/************************************************************************/

static int readmem (Dc *dc, uLong size, OZ_Pointer addr, void *buff)

{
  uLong sts;

  sts = oz_sys_process_peek (dc -> h_process, OZ_PROCMODE_KNL, size, (void const *)addr, buff);
  if (sts != OZ_SUCCESS) PRINT "error %u reading %s%X byte%s at 0x%X\n", 
                               sts, (size > 9) ? "0x" : "", size, (size == 1) ? "" : "s", addr);
  return (sts == OZ_SUCCESS);
}

static int writemem (Dc *dc, uLong size, void const *buff, OZ_Pointer addr)

{
  uLong sts;

  sts = oz_sys_process_poke (dc -> h_process, OZ_PROCMODE_KNL, size, buff, (void *)addr);
  if (sts != OZ_SUCCESS) PRINT "error %u writing %s%X byte%s at 0x%X\n", 
                               sts, (size > 9) ? "0x" : "", size, (size == 1) ? "" : "s", addr);
  return (sts == OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Insert breakpoints							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc -> h_process = process to insert breakpoints into		*/
/*	dc -> bpts = list of breakpoints to insert			*/
/*									*/
/*    Output:								*/
/*									*/
/*	insertbreakpoints = 0 : failure					*/
/*	                    1 : success					*/
/*	dc -> ninsertedbpts = number of breakpoints set			*/
/*	dc -> insertedbpts  = where they were set and old opcode there	*/
/*									*/
/************************************************************************/

static int insertbreakpoints (Dc *dc)

{
  Bpt *bpt;
  int i, j, nbreaks;
  OZ_Process_Breaklist *breaklist;
  uLong sts;

  /* Count number of breakspoints that are enabled */

  nbreaks = 0;
  for (bpt = dc -> bpts; bpt != NULL; bpt = bpt -> next) if (bpt -> enabled) nbreaks ++;
  if (nbreaks == 0) return (1);

  /* Fill in array with unique breakpoint addresses */

  breaklist = MALLOC (nbreaks * sizeof *breaklist);
  nbreaks = 0;
  for (bpt = dc -> bpts; bpt != NULL; bpt = bpt -> next) {
    if (bpt -> enabled) {
      for (i = nbreaks; -- i >= 0;) if (breaklist[i].addr == (OZ_Breakpoint *)(bpt -> address)) break;
      if (i < 0) {
        breaklist[nbreaks].addr = (OZ_Breakpoint *)(bpt -> address);	// address for the breakpoint
        breaklist[nbreaks].opcd = OZ_OPCODE_BPT;			// the breakpoint's opcode
        breaklist[nbreaks].stat = 0;					// zero out the write status
        nbreaks ++;
      }
    }
  }

  /* Write breakpoints to process */

  sts = oz_sys_process_setbreaks (OZ_PROCMODE_KNL, dc -> h_process, nbreaks, breaklist);
  if (sts != OZ_SUCCESS) {
    PRINT "error %u inserting breakpoints\n", sts);
    return (0);
  }

  /* Check all write statuses.  If any failed, remove those that succeeded. */

  j = 0;
  for (i = 0; i < nbreaks; i ++) {
    if (breaklist[i].stat == OZ_SUCCESS) breaklist[j++] = breaklist[i];
    else PRINT "error %u setting breakpoint at %p\n", breaklist[i].stat, breaklist[i].addr);
  }
  if (j < i) {
    sts = oz_sys_process_setbreaks (OZ_PROCMODE_KNL, dc -> h_process, j, breaklist);
    if (sts != OZ_SUCCESS) PRINT "error %u removing breakpoints\n", sts);
    FREE (breaklist);
    return (0);
  }

  /* All successful, remember what we did */

  dc -> ninsertedbpts = nbreaks;
  dc -> insertedbpts  = breaklist;
  return (1);
}

/************************************************************************/
/*									*/
/*  Remove breakpoints							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc -> h_process = process to remove breakpoints from		*/
/*	dc -> ninsertedbpts = number of breakpoints set			*/
/*	dc -> insertedbpts  = where they were set and old opcode there	*/
/*									*/
/*    Output:								*/
/*									*/
/*	removebreakpoints = 0 : failure					*/
/*	                    1 : success					*/
/*	dc -> ninsertedbpts = 0						*/
/*	dc -> insertedbpts  = NULL					*/
/*									*/
/************************************************************************/

static int removebreakpoints (Dc *dc)

{
  int i, j, nbreaks;
  OZ_Process_Breaklist *breaklist;
  uLong sts;

  /* Write original opcodes back to process */

  nbreaks   = dc -> ninsertedbpts;
  breaklist = dc -> insertedbpts;
  if (nbreaks == 0) return (1);
  dc -> ninsertedbpts = 0;
  dc -> insertedbpts  = NULL;

  sts = oz_sys_process_setbreaks (OZ_PROCMODE_KNL, dc -> h_process, nbreaks, breaklist);
  if (sts != OZ_SUCCESS) {
    PRINT "error %u removing breakpoints\n", sts);
    FREE (breaklist);
    return (0);
  }

  /* Check all write statuses and that the opcode is OZ_OPCODE_BPT */

  j = 0;
  for (i = 0; i < nbreaks; i ++) {
    if (breaklist[i].stat != OZ_SUCCESS) PRINT "error %u removing breakpoint at %p\n", breaklist[i].stat, breaklist[i].addr);
    else if (breaklist[i].opcd != OZ_OPCODE_BPT) PRINT "opcode %X was not breakpoint at %p\n", breaklist[i].opcd, breaklist[i].addr);
    else j ++;
  }
  FREE (breaklist);
  return (i == j);
}

/************************************************************************/
/*									*/
/*  Write a thread's frame 0 machine arguments				*/
/*									*/
/************************************************************************/

static int writethreadmchargs (Dc *dc, Thread *thread)

{
  uLong sts;

  if (!(thread -> exited)) {
    sts = oz_sys_process_poke (dc -> h_process, OZ_PROCMODE_KNL, sizeof thread -> frames -> mchargs_cpy, 
                               &(thread -> frames -> mchargs_cpy), thread -> mchargs_adr);
    if (sts == OZ_SUCCESS) return (1);
    PRINT "writethreadmchargs: error %u writing mchargs at %p\n", sts, thread -> mchargs_adr);
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Print our spot in a frame						*/
/*									*/
/************************************************************************/

static void printframespot (Dc *dc, Thread *thread, Frame *frame)

{
  char const *funname, *name, *p, *sep;
  char cur;
  Exefile *exefile;
  int funindx, i, l, modindx, namel;
  OZ_Pointer funaddr, nxtinsad;
  Var var;

  /* If we haven't yet, try to get source line */

  nxtinsad = GETNXTINSAD (frame -> mchargs_cpy);
  if (frame -> srcline == 0) {
    if (!findsourceline (dc, nxtinsad, &(frame -> exefile), &(frame -> modindx), 
                         &(frame -> srcname), &(frame -> srcindx), &(frame -> srcline))) {
      frame -> srcline = -1;
    }
  }

  /* Set up 'current frame' marker */

  cur = ' ';
  if (frame == dc -> curthread -> curframe) cur = '*';

  /* Print out frame number and next instruction address */

  PRINT "%c%u.%u: %p", cur, thread -> threadid, frame -> level, nxtinsad);

  /* If we know the source line, print the info out */

  if (frame -> srcline >= 0) PRINT "; %s %s %d", frame -> exefile -> name, frame -> srcname, frame -> srcline);

  /* If we can find it, print out the function name and its arguments */

  if (!findfunctionname (dc, nxtinsad, &funname, &funaddr, &exefile, &modindx, &funindx)) PRINT "\n");
  else {

    /* Print function name and offset within the function */

    p = strchr (funname, ':');
    if (p != NULL) l = p - funname;
    else l = strlen (funname);
    if (funaddr == nxtinsad) PRINT " %*.*s (", l, l, funname);
    else PRINT " %*.*s+0x%X (", l, l, funname, nxtinsad - funaddr);

    /* Print out function parameter names and values */

    sep = "";
    for (i = 0; (name = findfuncparam (dc, exefile, modindx, funindx, ++ i, &(frame -> mchargs_cpy), (OZ_Pointer)(frame -> mchargs_adr), &var)) != NULL;) {
      p = strchr (name, ':');
      if (p != NULL) namel = p - name;
      else namel = strlen (name);
      PRINT "%s%*.*s=", sep, namel, namel, name);
      printvariable (dc, exefile, modindx, 0, var);
      sep = ", ";
    }
    PRINT ")\n");
  }
}

/*************************************************************************/
/**									**/
/**	Stab library starts here					**/
/**									**/
/*************************************************************************/

typedef struct Stab Stab;

struct Stab { unsigned int n_strx;
              unsigned char n_type;
              unsigned char n_other;
              unsigned short n_desc;
              unsigned int n_value;
            };

#define intstyp (intstypstr + 10)				// stab-like type to assign to an integer constant
static char const intstypstr[] = "\000const:t0=r0;0;0;";	// has to be complete stab string incl leading null

#define voidstyp (voidstypstr + 9)				// stab-like type for C-type 'void'
static char const voidstypstr[] = "\000void:t0=@s0;r0;0;0;";	// it's an integer of size zero

/************************************************************************/
/*									*/
/*  Open executable file and parse its .stab				*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = executable file name					*/
/*	size = size of executable image					*/
/*	data = where it is loaded in local memory			*/
/*	baseaddr = load offset in target memory				*/
/*									*/
/*    Output:								*/
/*									*/
/*	openexefile = 0 : failed					*/
/*	              1 : successful					*/
/*									*/
/************************************************************************/

static int openexefile (Dc *dc, char const *name, uLong size, void const *data, OZ_Pointer baseaddr)

{
  char const *p, *stabstraddr;
  Elf32_Ehdr const *ehdr;
  Elf32_Phdr const *phdr_tbl;
  Elf32_Shdr const *shdr, *shdr_tbl, *shstrshdr;
  Exefile *exefile;
  int i, j;
  OZ_Handle h_file;
  OZ_IO_fs_open fs_open;
  OZ_Pointer begofimage, endofimage;
  Stab const *stab, *stab_tbl;
  uLong pendstrtaboffs, sts;

  /* Set up data structure */

  exefile = MALLOC (sizeof *exefile + strlen (name));
  memset (exefile, 0, sizeof *exefile);
  strcpy (exefile -> name, name);
  exefile -> data = data;
  ehdr = exefile -> data;
  if ((memcmp (ehdr -> e_ident, ELFMAG, SELFMAG) != 0) 
   || (ehdr -> e_ident[EI_CLASS]   != ELFCLASS32) 
   || (ehdr -> e_ident[EI_DATA]    != ELFDATA2LSB) 
   || (ehdr -> e_ident[EI_VERSION] != EV_CURRENT) 
   || (ehdr -> e_type              != ET_DYN)) {
    FREE (exefile);
    PRINT "unknown executable file format %s\n", name);
    return (0);
  }

  /* Check base address */

  phdr_tbl = exefile -> data + ehdr -> e_phoff;
  begofimage = (OZ_Pointer)(-1);
  for (j = 0; j < ehdr -> e_phnum; j ++) {
    if (phdr_tbl[j].p_type == PT_LOAD) {
      if ((begofimage == (OZ_Pointer)(-1)) || (begofimage > phdr_tbl[j].p_vaddr)) {
        begofimage = phdr_tbl[j].p_vaddr;
      }
    }
  }

  if (begofimage != 0) {				// see if linked at a fixed address
    if ((baseaddr != 0) && (baseaddr != begofimage)) {	// if so, load address better match
      PRINT "image %s linked at %X but loaded at %X\n", name, begofimage, baseaddr);
      FREE (exefile);
      return (0);
    }
    baseaddr = 0;					// ok, we don't need to offset anything
  }
  exefile -> baseaddr = baseaddr;

  /* Find symbol sections */

  shdr_tbl  = exefile -> data + ehdr -> e_shoff;	// point to section header table
  shstrshdr = shdr_tbl + ehdr -> e_shstrndx;		// this section contains section header name strings
  for (i = 0; i < ehdr -> e_shnum; i ++) {		// loop through all section headers
    shdr = shdr_tbl + i;
    p = exefile -> data + shstrshdr -> sh_offset + shdr -> sh_name;
    if (strcmp (p, ".dynsym")  == 0) exefile -> dynsym  = shdr;
    if (strcmp (p, ".dynstr")  == 0) exefile -> dynstr  = shdr;
    if (strcmp (p, ".symtab")  == 0) exefile -> symtab  = shdr;
    if (strcmp (p, ".strtab")  == 0) exefile -> strtab  = shdr;
    if (strcmp (p, ".stab")    == 0) exefile -> stab    = shdr;
    if (strcmp (p, ".stabstr") == 0) exefile -> stabstr = shdr;
  }

  if ((exefile -> dynsym == NULL) || (exefile -> dynstr == NULL) 
   || (exefile -> symtab == NULL) || (exefile -> strtab == NULL) 
   || (exefile -> stab == NULL)  || (exefile -> stabstr == NULL)) {
    FREE (exefile);
    PRINT "missing .dynsym, .dynstr, .symtab, .strtab, .stab and/or .stabstr section in %s\n", name);
    return (0);
  }

  /* Get module list from stab section */

  stab_tbl = exefile -> data + exefile -> stab -> sh_offset;

  exefile -> nmodules = 0;
  for (i = 0; i < exefile -> stab -> sh_size / sizeof *stab_tbl; i ++) {
    if (stab_tbl[i].n_type == 0) exefile -> nmodules ++;
  }

  if (exefile -> nmodules != 0) {
    exefile -> modules = MALLOC (exefile -> nmodules * sizeof *(exefile -> modules));
    stabstraddr = exefile -> data + exefile -> stabstr -> sh_offset;
    pendstrtaboffs = 0;
    j = -1;
    for (i = 0; i < exefile -> stab -> sh_size / sizeof *stab_tbl; i ++) {
      stab = stab_tbl + i;
      switch (stab -> n_type) {

        /* Beginning of new module: n_desc=number of descriptors to follow; n_value=size of stabstr used */

        case 0: {
          j ++;
          stabstraddr += pendstrtaboffs;			// increment stabstr address by size used by last module
          pendstrtaboffs = stab -> n_value;			// save stabstr size used by this module
          exefile -> modules[j].stabcoun = stab -> n_desc;
          exefile -> modules[j].stabindx = i + 1;
          exefile -> modules[j].stabstrs = stabstraddr;
          exefile -> modules[j].textsize = (OZ_Pointer)(-1);
          exefile -> modules[j].textaddr = 0;
          break;
        }

        /* Main source file: n_strx=filename string; n_value=start addr of text section */

        case N_SO: {
          if (*(stabstraddr + stab -> n_strx) == 0) {		// there is a null filename at end of (some) modules
            exefile -> modules[j].textsize = stab -> n_value + exefile -> baseaddr - exefile -> modules[j].textaddr;
          } else {
            exefile -> modules[j].name     = stabstraddr + stab -> n_strx;
            exefile -> modules[j].textaddr = stab -> n_value + exefile -> baseaddr;
          }
          break;
        }
      }
    }

    /* Sort by ascending address (probably already is) */

    qsort (exefile -> modules, exefile -> nmodules, sizeof *(exefile -> modules), qsort_modules);

    /* Fix textsize entries for those modules that didn't have N_SO with null filename */

    for (j = 0; j < exefile -> nmodules - 1; j ++) {
      if (exefile -> modules[j].textsize == (OZ_Pointer)(-1)) {
        exefile -> modules[j].textsize = exefile -> modules[j+1].textaddr - exefile -> modules[j].textaddr;
      }
    }
    if (exefile -> modules[j].textsize == (OZ_Pointer)(-1)) {
      phdr_tbl = exefile -> data + ehdr -> e_phoff;
      endofimage = (OZ_Pointer)(-1);
      for (j = 0; j < ehdr -> e_phnum; j ++) {
        if (phdr_tbl[j].p_type == PT_LOAD) {
          if ((endofimage == (OZ_Pointer)(-1)) || (endofimage < phdr_tbl[j].p_vaddr + phdr_tbl[j].p_memsz)) {
            endofimage = phdr_tbl[j].p_vaddr + phdr_tbl[j].p_memsz;
          }
        }
      }
      exefile -> modules[j].textsize = endofimage - exefile -> modules[j].textaddr;
    }
  }

  /* File is good, link to list */

  exefile -> next = dc -> exefiles;
  dc -> exefiles = exefile;

  return (1);
}

static int qsort_modules (void const *m1v, void const *m2v)

{
  Module const *m1, *m2;

  m1 = m1v;
  m2 = m2v;

  if (m1 -> textaddr < m2 -> textaddr) return (-1);
  if (m1 -> textaddr > m2 -> textaddr) return  (1);
  return (0);
}

/************************************************************************/
/*									*/
/*  Find source file line for a given address				*/
/*									*/
/*    Input:								*/
/*									*/
/*	addr = address of some instruction				*/
/*									*/
/*    Output:								*/
/*									*/
/*	*exefile_r = executable file it was found in			*/
/*	*modindx_r = module index within executable file		*/
/*	*srcname_r = points to source file name				*/
/*	*srcindx_r = index in stab for source line record		*/
/*	             ... relative to beg of module			*/
/*	*srcline_r = line number within the source file			*/
/*									*/
/************************************************************************/

static int findsourceline (Dc *dc, 
                           OZ_Pointer addr, 
                           Exefile **exefile_r, 
                           int *modindx_r, 
                           char const **srcname_r, 
                           int *srcindx_r, 
                           int *srcline_r)

{
  char const *srcname, *stabstraddr;
  Elf32_Ehdr const *ehdr;
  Elf32_Phdr const *phdr, *phdr_tbl;
  Elf32_Shdr const *shdr, *shdr_tbl;
  Exefile *exefile;
  int i, j, k;
  OZ_Pointer lastfunaddr;
  Stab const *stab;

  *exefile_r = NULL;
  *modindx_r = 0;
  *srcname_r = NULL;
  *srcindx_r = 0;
  *srcline_r = 0;

  /* Find executable by looking at the program headers */

  for (exefile = dc -> exefiles; exefile != NULL; exefile = exefile -> next) {
    ehdr = exefile -> data;
    phdr = exefile -> data + ehdr -> e_phoff;		// point to program header table
    for (i = ehdr -> e_phnum; -- i >= 0;) {		// loop through all program headers
      if (addr < exefile -> baseaddr + phdr -> p_vaddr) continue;
      if (addr - exefile -> baseaddr - phdr -> p_vaddr < phdr -> p_memsz) goto found_exefile;
      phdr ++;
    }
  }
  return (0);

found_exefile:
  *exefile_r = exefile;

  /* Find module within the executable */

  addr -= exefile -> baseaddr;

  i = 0;
  k = exefile -> nmodules;
  while (k > i) {
    j = (i + k) / 2;
    if (addr < exefile -> modules[j].textaddr) k = j;
    else if (addr - exefile -> modules[j].textaddr < exefile -> modules[j].textsize) goto found_module;
    else i = ++ j;
  }
  return (0);
found_module:
  *modindx_r = j;

  /* Scan module for desired source line */

  lastfunaddr = 0;						// reset last func address (.s file SLINEs have absolute addrs)
  srcname     = NULL;						// no source module name found yet

  stab  = exefile -> data + exefile -> stab -> sh_offset;
  stab += exefile -> modules[j].stabindx;
  stabstraddr = exefile -> modules[j].stabstrs;
  for (i = 0; i < exefile -> modules[j].stabcoun; i ++) {
    switch (stab -> n_type) {

      /* Main source file: n_strx=filename string; n_value=start addr of text section */

      case N_SO: {
        srcname = stabstraddr + stab -> n_strx;			// point to filename string
        if (srcname[0] == 0) goto found_sline;			// there is a null filename at end of source modules
        break;
      }

      /* Sub-source file: n_strx=filename string */

      case N_SOL: {
        srcname = stabstraddr + stab -> n_strx;
        break;
      }

      /* Beginning of function: n_strx=function name string; n_value=absolute address */

      case N_FUN: {
        lastfunaddr = stab -> n_value;
        break;
      }

      /* Source line: n_desc=line number; n_value=address relative to last N_FUN address */

      case N_SLINE: {
        if (stab -> n_value + lastfunaddr > addr) goto found_sline; // if line is beyond what we want, last one must be it
        *srcname_r = srcname;					// otherwise, assume this one will match
        *srcindx_r = i;
        *srcline_r = stab -> n_desc;
        break;
      }
    }

    stab ++;
  }

found_sline:
  return ((*srcname_r != NULL) && (**srcname_r != 0));
}

/************************************************************************/
/*									*/
/*  Find funciton name for a given address				*/
/*									*/
/*    Input:								*/
/*									*/
/*	addr = address of some instruction				*/
/*									*/
/*    Output:								*/
/*									*/
/*	*funname_r = function name					*/
/*	*funaddr_r = function entrypoint				*/
/*									*/
/************************************************************************/

static int findfunctionname (Dc *dc, OZ_Pointer addr, char const **funname_r, OZ_Pointer *funaddr_r, 
                             Exefile **funexec_r, int *funmodi_r, int *funindx_r)

{
  char const *stabstraddr;
  Elf32_Ehdr const *ehdr;
  Elf32_Phdr const *phdr, *phdr_tbl;
  Elf32_Shdr const *shdr, *shdr_tbl;
  Exefile *exefile;
  int i, j, k;
  Stab const *stab;

  *funname_r = NULL;
  *funaddr_r = 0;

  /* Find executable by looking at the program headers */

  for (exefile = dc -> exefiles; exefile != NULL; exefile = exefile -> next) {
    ehdr = exefile -> data;
    phdr = exefile -> data + ehdr -> e_phoff;		// point to program header table
    for (i = ehdr -> e_phnum; -- i >= 0;) {		// loop through all program headers
      if (addr < exefile -> baseaddr + phdr -> p_vaddr) continue;
      if (addr - exefile -> baseaddr - phdr -> p_vaddr < phdr -> p_memsz) goto found_exefile;
      phdr ++;
    }
  }
  return (0);
found_exefile:
  if (funexec_r != NULL) *funexec_r = exefile;

  /* Find module within the executable */

  addr -= exefile -> baseaddr;

  i = 0;
  k = exefile -> nmodules;
  while (k > i) {
    j = (i + k) / 2;
    if (addr < exefile -> modules[j].textaddr) k = j;
    else if (addr - exefile -> modules[j].textaddr < exefile -> modules[j].textsize) goto found_module;
    else i = ++ j;
  }
  return (0);
found_module:
  if (funmodi_r != NULL) *funmodi_r = j;

  /* Scan module for function at or just below address */

  stab  = exefile -> data + exefile -> stab -> sh_offset;
  stab += exefile -> modules[j].stabindx;
  stabstraddr = exefile -> modules[j].stabstrs;
  for (i = 0; i < exefile -> modules[j].stabcoun; i ++) {
    switch (stab -> n_type) {

      /* Beginning of function: n_strx=function name string; n_value=absolute address */

      case N_FUN: {
        if (stab -> n_value > addr) goto rtn;
        if (stabstraddr[stab->n_strx] != 0) {
          *funname_r = stabstraddr + stab -> n_strx;
          *funaddr_r = exefile -> baseaddr + stab -> n_value;
          if (funindx_r != NULL) *funindx_r = i;
        }
        break;
      }
    }

    stab ++;
  }
rtn:
  return (*funname_r != NULL);
}

/************************************************************************/
/*									*/
/*  Find function parameter						*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile,modindx,funindx = as return by findfunctionname		*/
/*	prmindx = parameter index (1..?)				*/
/*	mchargs_cpy = local copy of mchargs				*/
/*	mchargs_adr = target address of mchargs				*/
/*									*/
/*    Output:								*/
/*									*/
/*	findfuncparam = NULL : no more parameters			*/
/*	                else : points to colon terminated string	*/
/*	*var_r = filled in with parameter info				*/
/*									*/
/************************************************************************/

static char const *findfuncparam (Dc *dc, 
                                  Exefile *exefile, 
                                  int modindx, 
                                  int funindx, 
                                  int prmindx, 
                                  OZ_Mchargs *mchargs_cpy, 
                                  OZ_Pointer mchargs_adr, 
                                  Var *var_r)

{
  char const *p, *prmname, *stabstraddr;
  Stab const *stab;

  memset (var_r, 0, sizeof *var_r);

  /* Point to symbol table for the module */

  stab  = exefile -> data + exefile -> stab -> sh_offset;
  stab += exefile -> modules[modindx].stabindx;
  stabstraddr = exefile -> modules[modindx].stabstrs;

  /* The parameter records directly follow the function record */

  stab += funindx + prmindx;				// point to parameter symbol record
  if (stab -> n_type != N_PSYM) return (NULL);		// return error if went off end

  prmname = stabstraddr + stab -> n_strx;		// point to parameter name string
  p = strchr (prmname, ':');				// name should be followed by a colon
  if (p == NULL) return (NULL);

  var_r -> v_styp = p + 1;				// point to raw type string
  var_r -> v_addr = stab -> n_value;			// get symbol's raw value

  if (!computevaraddr (dc, exefile, modindx, p - prmname, prmname, mchargs_cpy, mchargs_adr, var_r)) return (NULL);
  return (prmname);
}

/************************************************************************/
/*									*/
/*  Find address of a given source file line				*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile = executable to look in					*/
/*	modindx = module in that executable				*/
/*	srcline = source line to look for				*/
/*									*/
/*    Output:								*/
/*									*/
/*	findsrclineaddr = 0 : line not found				*/
/*	                  1 : successful				*/
/*	*addr_r = address of the source line				*/
/*									*/
/************************************************************************/

static int findsrclineaddr (Dc *dc, Exefile *exefile, int modindx, int srcline, OZ_Pointer *addr_r)

{
  char const *srcname, *stabstraddr;
  int i;
  OZ_Pointer lastfunaddr;
  Stab const *stab;

  /* Scan module for desired source line */

  lastfunaddr = 0;							// reset last func address (.s file SLINEs have absolute addrs)
  srcname     = "";							// no source module name found yet

  stab  = exefile -> data + exefile -> stab -> sh_offset;
  stab += exefile -> modules[modindx].stabindx;
  stabstraddr = exefile -> modules[modindx].stabstrs;
  for (i = 0; i < exefile -> modules[modindx].stabcoun; i ++) {
    switch (stab -> n_type) {

      /* Main source file: n_strx=filename string; n_value=start addr of text section */

      case N_SO: {
        srcname = stabstraddr + stab -> n_strx;				// point to filename string
        if (srcname[0] == 0) return (0);				// there is a null filename at end of source modules
        break;
      }

      /* Sub-source file: n_strx=filename string */

      case N_SOL: {
        srcname = stabstraddr + stab -> n_strx;
        break;
      }

      /* Beginning of function: n_strx=function name string; n_value=absolute address */

      case N_FUN: {
        lastfunaddr = stab -> n_value;
        break;
      }

      /* Source line: n_desc=line number; n_value=address relative to last N_FUN address */

      case N_SLINE: {
        if ((stab -> n_desc >= srcline) 				// stop if it's .ge. requested line number
         && (strcmp (srcname, exefile -> modules[modindx].name) == 0)) { // ... and it's the main source file
          *addr_r = exefile -> baseaddr + lastfunaddr + stab -> n_value;
          return (1);
        }
        break;
      }
    }

    stab ++;
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Find address of a given function					*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile = executable to look in					*/
/*	modindx = module in that executable				*/
/*	funname = function in that module				*/
/*									*/
/*    Output:								*/
/*									*/
/*	findfunctionaddr = 0 : function not found			*/
/*	                   1 : successful				*/
/*	*addr_r = address of the function				*/
/*									*/
/************************************************************************/

static int findfunctionaddr (Dc *dc, Exefile *exefile, int modindx, int funnamel, char const *funname, OZ_Pointer *addr_r)

{
  char const *fname, *p, *sname, *stabstraddr;
  int i;
  Stab const *stab;

  /* Scan module for desired funciton name */

  sname = "";								// no source module name found yet
  stab  = exefile -> data + exefile -> stab -> sh_offset;		// point to start of stabs for this module
  stab += exefile -> modules[modindx].stabindx;
  stabstraddr = exefile -> modules[modindx].stabstrs;			// point to stab strings for this module
  for (i = 0; i < exefile -> modules[modindx].stabcoun; i ++) {		// loop through this module's stabs
    switch (stab -> n_type) {

      /* Main source file: n_strx=filename string; n_value=start addr of text section */

      case N_SO: {
        sname = stabstraddr + stab -> n_strx;				// point to filename string
        if (sname[0] == 0) return (0);					// there is a null filename at end of source modules
        break;
      }

      /* Sub-source file: n_strx=filename string */

      case N_SOL: {
        sname = stabstraddr + stab -> n_strx;
        break;
      }

      /* Beginning of function: n_strx=function name string; n_value=absolute address */

      case N_FUN: {
        fname = stabstraddr + stab -> n_strx;
        p = strchr (fname, ':');
        if (p == NULL) p = fname + strlen (fname);
        if ((p - fname == funnamel) && (memcmp (fname, funname, funnamel) == 0) // stop if function name matches
         && (strcmp (sname, exefile -> modules[modindx].name) == 0)) goto foundfunc; // ... and it's the main source file
        break;
      }
    }

    stab ++;
  }
  return (0);

  /* Found function.  Return its address for now, but find next source line and break on that, if any, so we skip the prolog. */

foundfunc:
  *addr_r = exefile -> baseaddr + stab -> n_value;

  while (++ i < exefile -> modules[modindx].stabcoun) {
    ++ stab;
    switch (stab -> n_type) {

      /* Main source file: n_strx=filename string; n_value=start addr of text section */

      case N_SO: {
        sname = stabstraddr + stab -> n_strx;				// point to filename string
        if (sname[0] == 0) return (1);					// there is a null filename at end of source modules
        break;
      }

      /* Sub-source file: n_strx=filename string */

      case N_SOL: {
        sname = stabstraddr + stab -> n_strx;
        break;
      }

      /* Beginning of function: n_strx=function name string; n_value=absolute address */

      case N_FUN: return (1);						// if starting another func, just return address of old one

      /* Source line: n_desc=line number; n_value=address relative to last N_FUN address */

      case N_SLINE: {
        if ((stab -> n_value != 0) 					// don't do the function itself or its '{'
         && (strcmp (sname, exefile -> modules[modindx].name) == 0)) {	// stop if it's in the main source file
          *addr_r += stab -> n_value;					// return its address instead to skip prolog
          return (1);
        }
        break;
      }
    }
  }

  /* Didn't find a SLINE after the function, just stick with function entrypoint address then */

  return (1);
}

/************************************************************************/
/*									*/
/*  Get expression from string						*/
/*									*/
/*    Input:								*/
/*									*/
/*	level       = operator precedence level (start at zero)		*/
/*	fake        = 0 : normal processing				*/
/*	           else : just parse, don't evaluate			*/
/*	dc          = debug context pointer				*/
/*	exefile     = executable file the operand is in			*/
/*	modindx     = module within that executable where operand is	*/
/*	mchargs_cpy = local copy of mchargs we're stopped at		*/
/*	mchargs_adr = target address where mchargs are			*/
/*	bufl        = length of string					*/
/*	buff        = address of string					*/
/*									*/
/*    Output:								*/
/*									*/
/*	getexpression = 0 : failed (message output)			*/
/*	             else : number of chars of buff processed		*/
/*	*var_r = filled in (v_styp is finalized)			*/
/*									*/
/************************************************************************/

#define BOP_ADD 1
#define BOP_AND 2
#define BOP_CON 3
#define BOP_DIV 4
#define BOP_DOT 5
#define BOP_EQ 6

#define BOP_GE 7
#define BOP_GT 8
#define BOP_IDX 9
#define BOP_LAN 10
#define BOP_LE 11
#define BOP_LOR 12
#define BOP_LT 13
#define BOP_MOD 14
#define BOP_MUL 15

#define BOP_NE 16
#define BOP_OR 17
#define BOP_PTR 18
#define BOP_SET 19
#define BOP_SHL 20
#define BOP_SHR 21
#define BOP_SUB 22
#define BOP_TRM 23
#define BOP_XOR 24

static struct { int levl; int size; char const *name; int code; } const boptbl[] = {
	10, 2, "..", BOP_TRM, 
	 9, 2, "->", BOP_PTR,
	 6, 2, "<<", BOP_SHL,
	 6, 2, ">>", BOP_SHR,
	 5, 2, "<=", BOP_LE,
	 5, 2, ">=", BOP_GE,
	 4, 2, "==", BOP_EQ,
	 4, 2, "!=", BOP_NE,
	 2, 2, "&&", BOP_LAN,
	 2, 2, "||", BOP_LOR,

	 9, 1, "[",  BOP_IDX,
	 9, 1, ".",  BOP_DOT,
	 8, 1, "*",  BOP_MUL,
	 8, 1, "/",  BOP_DIV,
	 8, 1, "%",  BOP_MOD,
	 7, 1, "+",  BOP_ADD,
	 7, 1, "-",  BOP_SUB,
	 5, 1, "<",  BOP_LT,
	 5, 1, ">",  BOP_GT,
	 3, 1, "&",  BOP_AND,
	 3, 1, "^",  BOP_XOR,
	 3, 1, "|",  BOP_OR,
	 1, 1, "?",  BOP_CON,
	 0, 1, "=",  BOP_SET,

	-1, 0, NULL, 0 };

#define UNARYLVL 9
#define MAXLEVEL 11

static int getexpression (int level, 
                          int fake, 
                          Dc *dc, 
                          Exefile *exefile, 
                          int modindx, 
                          OZ_Mchargs *mchargs_cpy, 
                          OZ_Pointer mchargs_adr, 
                          int bufl, 
                          char const *buff, 
                          Var *var_r)

{
  char c;
  int bufi, code, hilim, i, lolim;
  Var idx, var, var2;

  if (level == MAXLEVEL) return (getoperand (fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, bufl, buff, var_r));

  /* Get first operand */

  bufi = getexpression (level + 1, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, bufl, buff, &var);
  if (bufi == 0) return (0);

  /* See if binary operator follows first operand */

chkbop:
  if ((bufi < bufl) && (buff[bufi] == ' ')) bufi ++;					// skip leading space
  if (bufi < bufl) {									// see if end of input string
    for (i = 0; boptbl[i].size != 0; i ++) {						// loop through table
      if ((bufi + boptbl[i].size <= bufl) 						// make sure we have enough string
       && (memcmp (buff + bufi, boptbl[i].name, boptbl[i].size) == 0)) break;		// see if string matches
    }
  }
  if ((bufi >= bufl) || (boptbl[i].levl != level)) goto rtn;

  code = boptbl[i].code;
  switch (code) {

    /* Add: arithmetic or pointer + arithmetic */

    case BOP_ADD: {
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "invalid addition operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi ++;										// skip over the '+'
      i = getexpression (level + 1, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, // get right-hand operand
                         bufl - bufi, buff + bufi, &var2);
      if (i == 0) return (0);
      if (!isstyparith (var2.v_styp, var2.v_star)) {					// it must be arithmetic
        PRINT "invalid arithmetic operand %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      bufi += i;
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
      var.v_hasa = 0;									// result has no memory address

      if (var.v_star == 0) var.v_valu += var2.v_valu;					// if left is arithemetic, do simple add
      else var.v_valu += stypsize (var.v_styp, var.v_star - 1) * var2.v_valu;		// left is pointer, do scaled add
      break;
    }

    /* Conditional: pointer or arithmetic ? anything : anything */

    case BOP_CON: {
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "invalid conditional operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi ++;										// skip over the '?'
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      i = getexpression (level, fake || (var.v_valu == 0), dc, exefile, modindx, mchargs_cpy, mchargs_adr, 
                         bufl - bufi, buff + bufi, &var2);				// get 'if true' operand
      if (i == 0) return (0);
      if (buff[bufi+i] != ':') {							// there should be a colon here
        PRINT "missing : in conditional %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      bufi += ++ i;
      i = getexpression (level, fake || (var.v_valu != 0), dc, exefile, modindx, mchargs_cpy, mchargs_adr,
                         bufl - bufi, buff + bufi, &var2);				// get 'if false' operand
      if (i == 0) return (0);
      bufi += i;
      var = var2;									// save whichever we got
      break;
    }

    /* Pointer: pointer -> struct field */

    case BOP_PTR: {
      if (!derefpointer (dc, exefile, modindx, &var)) {
        PRINT "getexpression: error dereferencing %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      // fall through to BOP_DOT processing
    }

    /* Dot: struct . struct field */

    case BOP_DOT: {
      bufi += boptbl[i].size;								// skip over the '.' or '->'
      if (var.v_star > 0) {								// make sure there aren't any *'s left in there
        PRINT "getexpression: cannot take field of a pointer %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      if ((bufi < bufl) && (buff[bufi] == ' ')) bufi ++;				// skip trailing space
      for (i = 0; (bufi + i) < bufl; i ++) {						// get length of field name string
        c = buff[bufi+i];
        if (!LEGALVARCHAR (c)) break;
      }
      var2.v_bito = var.v_bito;								// save current bit offset
      if (!getstructfield (var.v_styp, i, buff + bufi, &var)) {				// get struct/union field
        PRINT "getexpression: bad field name <%*.*s>\n", i, i, buff + bufi);
        return (0);
      }
      var.v_bito += var2.v_bito;							// add bit offsets
      var.v_star  = findfinalstyp (dc, exefile, modindx, var.v_styp, &var.v_styp);	// find final type
      if (var.v_star < 0) return (0);
      bufi += i;									// increment past field name string
      break;
    }

    /* Index: pointer [ index ] */

    case BOP_IDX: {

      /* Set up var as an address to first element of array (with v_hasa=1 and v_addr=address of first element) */

      if (!derefpointer (dc, exefile, modindx, &var)) {
        PRINT "getexpression: error dereferencing %*.*s\n", bufi, bufi, buff);
        return (0);
      }

      /* Get index value from input string */

      bufi ++;										// skip over '['
      i = getexpression (0, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, bufl - bufi, buff + bufi, &idx);
      if (i == 0) return (0);
      if ((bufi + i >= bufl) || (buff[bufi+i] != ']')) {				// make sure index ended on ']'
        PRINT "array index did not end on ] %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      if (!isstyparith (idx.v_styp, idx.v_star)) {					// make sure it's arithmetic (int, long, etc)
        PRINT "array index not arithmetic %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      bufi += ++ i;									// skip past ']'
      if (!readvarvalue (dc, &idx)) return (0);						// read index value

      /* Apply index value to address (it might be an array of bits) */

      var.v_bitl = stypsizebits (var.v_styp, var.v_star);				// get element size
      if (var.v_bitl & 7) var.v_bito += var.v_bitl * idx.v_valu;			// increment bit offset by that much
               else var.v_addr += (var.v_bitl / 8) * idx.v_valu;			// ... or array address by this much
      break;
    }

    /* Logical And: pointer or arithmetic && pointer or arithmetic */

    case BOP_LAN: {
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "invalid arithmetic operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi += 2;									// skip over the '&&'
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      i = getexpression (level + 1, fake || (var.v_valu == 0), dc, exefile, modindx, mchargs_cpy, mchargs_adr, 
                         bufl - bufi, buff + bufi, &var2);				// get right-hand operand
      if (i == 0) return (0);
      if ((var2.v_star == 0) && !isstyparith (var2.v_styp, 0)) {
        PRINT "invalid arithmetic operand %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      bufi += i;
      if (var.v_valu != 0) {
        if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
        var.v_valu = (var2.v_valu != 0);
      }
      var.v_hasa = 0;									// result has no memory address
      var.v_styp = intstyp;								// result is always type 'int'
      var.v_star = 0;
      break;
    }

    /* Logical Or: pointer or arithmetic || pointer or arithmetic */

    case BOP_LOR: {
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "invalid arithmetic operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi += 2;									// skip over the '||'
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      i = getexpression (level + 1, fake || (var.v_valu != 0), dc, exefile, modindx, mchargs_cpy, mchargs_adr, 
                         bufl - bufi, buff + bufi, &var2);				// get right-hand operand
      if (i == 0) return (0);
      if ((var2.v_star == 0) && !isstyparith (var2.v_styp, 0)) {
        PRINT "invalid arithmetic operand %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      bufi += i;
      if (var.v_valu != 0) var.v_valu = 1;
      else {
        if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
        var.v_valu = (var2.v_valu != 0);
      }
      var.v_hasa = 0;									// result has no memory address
      var.v_styp = intstyp;								// result is always type 'int'
      var.v_star = 0;
      break;
    }

    /* Various comparisons:  pointer or arithmetic (op) pointer or arithmetic */

    case BOP_EQ:
    case BOP_GE:
    case BOP_GT:
    case BOP_LE:
    case BOP_LT:
    case BOP_NE: {
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "invalid arithmetic operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi += boptbl[i].size;								// skip over the (op)
      i = getexpression (level + 1, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, // get right-hand operand
                         bufl - bufi, buff + bufi, &var2);
      if (i == 0) return (0);
      bufi += i;
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
      switch (code) {									// perform comparison
        case BOP_EQ: var.v_valu = (var.v_valu == var2.v_valu); break;
        case BOP_GE: var.v_valu = (var.v_valu >= var2.v_valu); break;
        case BOP_GT: var.v_valu = (var.v_valu >  var2.v_valu); break;
        case BOP_LE: var.v_valu = (var.v_valu <= var2.v_valu); break;
        case BOP_LT: var.v_valu = (var.v_valu <  var2.v_valu); break;
        case BOP_NE: var.v_valu = (var.v_valu != var2.v_valu); break;
      }
      var.v_hasa = 0;									// result has no memory address
      var.v_styp = intstyp;								// result is always type 'int'
      var.v_star = 0;
      break;
    }

    /* Set: pointer or arithmetic = same thing */

    case BOP_SET: {
      if (!var.v_hasa) {
        PRINT "left of = has no address %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "target of = must be pointer or arithmetic %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi ++;										// skip over the '='
      i = getexpression (level, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, 	// get right-hand operand
                         bufl - bufi, buff + bufi, &var2);				// RIGHT associativity, so don't dec level
      if (i == 0) return (0);
      bufi += i;
      if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
      var.v_valu = var2.v_valu;								// write left-hand value
      if (!writevarvalue (dc, &var)) return (0);
      break;
    }

    /* Subtract: arithmetic or pointer - arithmetic or pointer */

    case BOP_SUB: {
      if ((var.v_star == 0) && !isstyparith (var.v_styp, 0)) {
        PRINT "invalid subtraction operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi ++;										// skip over the '-'
      i = getexpression (level + 1, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr, // get right-hand operand
                         bufl - bufi, buff + bufi, &var2);
      if (i == 0) return (0);
      if ((var2.v_star == 0) && !isstyparith (var2.v_styp, 0)) {
        PRINT "invalid subtraction operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi += i;
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
      var.v_hasa = 0;									// result has no memory address

      if (var2.v_star == 0) {								// see if right is arithmetic
        if (var.v_star == 0) var.v_valu -= var2.v_valu;					// if left is also, do simple subtract
        else var.v_valu -= stypsize (var.v_styp, var.v_star - 1) * var2.v_valu;		// left is pointer, do scaled subtract
        break;
      }

      i = stypsize (var.v_styp, var.v_star - 1);					// subtracting two pointers
      if (i != stypsize (var2.v_styp, var2.v_star - 1)) {				// all we want is consistent size
        PRINT "left and right of subtraction are pointers to different sized objects\n"); // of what they point to
        return (0);
      }
      if (i == 0) i = 1;								// and definitely non-zero!
      var.v_valu = (var.v_valu - var2.v_valu) / i;					// find scaled difference of pointers
      var.v_hasa = 0;
      var.v_bitl = 8 * sizeof (int);							// result is an (int)
      var.v_styp = intstyp;
      var.v_star = 0;
      break;
    }

    /* Anything that's a valid terminator that would otherwise be mistaken for an operator */
    /* Like ".." is a terminator, but would be mistaken for "." and a bad field name       */

    case BOP_TRM: goto rtn;

    /* Various that take arithmetic operands */

    case BOP_AND:
    case BOP_DIV:
    case BOP_MOD:
    case BOP_MUL:
    case BOP_OR:
    case BOP_SHL:
    case BOP_SHR:
    case BOP_XOR: {
      if (!isstyparith (var.v_styp, var.v_star)) {					// make sure left-hand is arithmetic
        PRINT "invalid arithmetic operand %*.*s\n", bufi, bufi, buff);
        return (0);
      }
      bufi ++;										// skip over the '&'
      i = getexpression (level + 1, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr,  // get right-hand operand
                         bufl - bufi, buff + bufi, &var2);
      if (i == 0) return (0);
      if (!isstyparith (var2.v_styp, var2.v_star)) {					// it must be arithmetic
        PRINT "invalid arithmetic operand %*.*s\n", i, i, buff + bufi);
        return (0);
      }
      if (!readvarvalue (dc, &var)) return (0);						// read left-hand value
      if (!readvarvalue (dc, &var2)) return (0);					// read right-hand value
      var.v_hasa = 0;									// result has no memory address
      switch (code) {									// perform computation
        case BOP_AND: var.v_valu  &= var2.v_valu; break;
        case BOP_MUL: var.v_valu  *= var2.v_valu; break;
        case BOP_OR:  var.v_valu  |= var2.v_valu; break;
        case BOP_SHL: var.v_valu <<= var2.v_valu; break;
        case BOP_SHR: var.v_valu >>= var2.v_valu; break;
        case BOP_SUB: var.v_valu  -= var2.v_valu; break;
        case BOP_XOR: var.v_valu  ^= var2.v_valu; break;
        case BOP_DIV: {
          if (var2.v_valu == 0) {
            PRINT "divide by zero %*.*s\n", i, i, buff + bufi);
            return (0);
          }
          var.v_valu /= var2.v_valu;
          break;
        }
        case BOP_MOD: {
          if (var2.v_valu == 0) {
            PRINT "divide by zero %*.*s\n", i, i, buff + bufi);
            return (0);
          }
          var.v_valu %= var2.v_valu;
          break;
        }
      }
      bufi += i;
      break;
    }

    /* Don't know how we got here */

    default: {
      PRINT "unsupported operator\n");
      return (0);
    }
  }
  goto chkbop;

rtn:
  if (!fake) *var_r = var;								// no match, return variable
  return (bufi);									// return terminator's index
}

/* See if finalized type is arithmetic (int, long, etc) */

static int isstyparith (char const *varstyp, int varstar)

{
  char const *p;

  if (varstar > 0) return (0);		// if any pointer levels, it's not arithmetic

  if (*varstyp == '@') {		// see if '@s'size';' present
    p = strchr (varstyp, ';');		// if so, skip over it
    if (p == NULL) return (0);
    varstyp == ++ p;
  }

  return (*varstyp == 'r');		// integer types begin with 'r'
}

/* Find size of finalized type (in bytes) */

static int stypsize (char const *varstyp, int varstar)

{
  int size;

  size = stypsizebits (varstyp, varstar);
  return ((size + 7) / 8);
}

/* Find size of finalized type (in bits) */

static int stypsizebits (char const *varstyp, int varstar)

{
  int arrayelements, lolim, hilim;

  /* A pointer to anything is a pointer */

  if (varstar > 0) return (8 * sizeof (OZ_Pointer));

  arrayelements = 1;
decode:
  switch (*varstyp) {

    /* Array subscript - get limits and multiply result by them */

    case 'a': {
      varstyp = arraysubscript (NULL, ++ varstyp, &lolim, &hilim);
      if (varstyp == NULL) return (0);
      arrayelements *= ++ hilim - lolim;
      goto decode;
    }

    /* We might have '@s'sizeinbits';' */

    case '@': if (*(++ varstyp) == 's') return (arrayelements * atoi (++ varstyp));

    /* Maybe it's an enum */

    case 'e': return (arrayelements * 8 * sizeof enum_dummy);

    /* Integer range: r(sometype);lowlimit;highlimit; */

    case 'r': {
      int hibits, lobits;

      if (memcmp (varstyp, "r(0,2);0;127;", 13) == 0) return (arrayelements * 8); // otherwise, we get chars=7 bits

      varstyp = strchr (varstyp, ';');					// point to ; just before low_limit
      if (varstyp != NULL) {
        varstyp = rangebits (++ varstyp, &lobits);			// decode the low limit
        if (*varstyp == ';') varstyp = rangebits (++ varstyp, &hibits);	// decode the high limit
        if (*varstyp == ';') {
          if (lobits < hibits) lobits = hibits;
          return (arrayelements * lobits);
        }
      }
    }

    /* Structs and Unions have the size in bytes as a decimal number following the 's' or 'u' */

    case 's': case 'u': return (arrayelements * 8 * atoi (++ varstyp));
  }

  /* Who knows */

  return (0);
}

/************************************************************************/
/*									*/
/*  Get operand from string						*/
/*									*/
/*    Input:								*/
/*									*/
/*	fake        = 0 : normal processing				*/
/*	           else : just parse, don't evaluate			*/
/*	dc          = debug context pointer				*/
/*	exefile     = executable file the operand is in			*/
/*	modindx     = module within that executable where operand is	*/
/*	mchargs_cpy = local copy of mchargs we're stopped at		*/
/*	mchargs_adr = target address where mchargs are			*/
/*	bufl        = length of string					*/
/*	buff        = address of string					*/
/*									*/
/*    Output:								*/
/*									*/
/*	getoperand = 0 : failed (message output)			*/
/*	          else : number of chars of buff processed		*/
/*	*var_r = filled in (v_styp is finalized)			*/
/*									*/
/************************************************************************/

static int getoperand (int fake, 
                       Dc *dc, 
                       Exefile *exefile, 
                       int modindx, 
                       OZ_Mchargs *mchargs_cpy, 
                       OZ_Pointer mchargs_adr, 
                       int bufl, 
                       char const *buff, 
                       Var *var_r)

{
  char c, *p;
  char const *casstyp, *expstyp;
  int bufi, casstar, expstar, i, j;
  Var var;

  memset (&var, 0, sizeof var);

  bufi = 0;									// start at beginning of input string
  while ((bufi < bufl) && ((c = buff[bufi]) == ' ')) bufi ++;			// skip over leading spaces

  /* If prefix operator present, get following operand, apply prefix operator then return */

  if ((c == '~') || (c == '!') || (c == '&') || (c == '*') || (c == '-')) {	// check for prefix operator
    bufi ++;									// skip over prefix operator
    i = getexpression (UNARYLVL, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr,  // get the following operand, 
                       bufl - bufi, buff + bufi, &var);				// ... including ->, ., [] operators
    if (i == 0) return (0);
    switch (c) {								// apply prefix operator
      case '~': {								// binary complement
        if (!isstyparith (var.v_styp, var.v_star)) {
          PRINT "cannot complement non-arithmetic value %*.*s\n", i, i, buff + bufi);
          return (0);
        }
        if (!readvarvalue (dc, &var)) return (0);				// read its value
        var.v_valu = ~var.v_valu;						// complement value
        var.v_hasa = 0;								// it doesn't have an address anymore
        break;
      }
      case '!': {								// logical not
        if ((var.v_star == 0) && !isstyparith (var.v_styp, var.v_star)) {
          PRINT "cannot logical not non-arithmetic value %*.*s\n", i, i, buff + bufi);
          return (0);
        }
        if (!readvarvalue (dc, &var)) return (0);				// read its value
        var.v_valu = !var.v_valu;						// not the value
        var.v_hasa = 0;								// it doesn't have an address anymore
        var.v_bito = 0;
        var.v_bitl = 8 * sizeof (int);						// the result is always an integer
        var.v_styp = intstyp;
        var.v_star = 0;
        break;
      }
      case '&': {								// address of operand
        if ((var.v_bito | var.v_bitl) & 7) {					// can't take address of a bit field
          PRINT "cannot take address of bitfield %*.*s\n", i, i, buff + bufi);
          return (0);
        }
        if (!(var.v_hasa)) {							// can't take address of a constant, etc.
          PRINT "operand does not have an address %*.*s\n", i, i, buff + bufi);
          return (0);
        }
        var.v_valu = var.v_addr + (var.v_bito / 8);
        var.v_bito = 0;
        var.v_bitl = 8 * sizeof var.v_addr;
        var.v_hasa = 0;
        var.v_star ++;								// there shall be one more * after type
        break;									//   eg, &(int) becomes (int *)
      }
      case '*': {								// indirect
        if (!derefpointer (dc, exefile, modindx, &var)) {			// make *(int *) become (int) & read pointer
          PRINT "getoperand: error dereferencing %*.*s\n", i, i, buff + bufi);
          return (0);
        }
        var.v_bitl = stypsizebits (var.v_styp, var.v_star);			// get new length
        break;
      }
      case '-': {
        if (!isstyparith (var.v_styp, var.v_star)) {
          PRINT "cannot negate non-arithmetic value %*.*s\n", i, i, buff + bufi);
          return (0);
        }
        if (!readvarvalue (dc, &var)) return (0);
        var.v_valu = -var.v_valu;
        var.v_hasa = 0;
        break;
      }
    }
    bufi += i;
    goto rtn;
  }

  /* Check for (type) cast prefix */

  casstyp = NULL;								// assume no type prefix
  casstar = 0;									// .. and it has no trailing asterisks
  if ((bufi < bufl) && (c == '(')) {						// see if we have a '('
    for (i = j = bufi; ++ i < bufl;) {						// ok, skip chars than can be part of type name
      c = buff[i];								// ... including spaces and digits
      if (!LEGALVARCHAR (c) && (c != ' ')) break;
      if (c != ' ') j = i;							// save index of last non-blank char
    }
    while ((i < bufl) && (((c = buff[i]) == ' ') || (c == '*'))) {		// there may be trailing asterisks
      casstar += (c == '*');							// ... amongst spaces
      i ++;
    }
    if ((i < bufl) && (c == ')')) {						// see if it terminated on a ')'
      while ((++ i < bufl) && ((c = buff[i]) == ' ')) {}			// if so, skip any following spaces
      if ((i < bufl) && (LEGALVARCHAR (c) || (c == '('))) {			// see if followed by varname, constant or '('
        expstar = findstypstr (dc, exefile, modindx, j - bufi, buff + bufi + 1, mchargs_cpy, &casstyp);
        if (expstar < 0) {
          PRINT "failed to find type %*.*s\n", j - bufi, j - bufi, buff + bufi + 1);
          return (0);
        }
        casstar += expstar;
        bufi = i;
      }
    }
  }

  /* Check for (expr) */

  if ((bufi < bufl) && (buff[bufi] == '(')) {
    bufi ++;									// skip over open parenthesis
    i = getexpression (0, fake, dc, exefile, modindx, mchargs_cpy, mchargs_adr,  // get the enclosed expression
                       bufl - bufi, buff + bufi, &var);
    if (i == 0) return (0);
    if ((bufi + i < bufl) && (buff[bufi+i] == ' ')) i ++;			// make sure it ended on a close paren
    if ((bufi + i >= bufl) || (buff[bufi+i] != ')')) {
      PRINT "missing ) after %*.*s\n", i, i, buff + bufi);
      return (0);
    }
    bufi += ++ i;								// advance past the close paren
  }

  /* Else, check for varname or numeric constant */

  else if (LEGALVARCHAR (c)) {							// see if 0..9, a..z, etc
    if ((c >= '0') && (c <= '9')) {						// ok, see if digit
      var.v_addr = var.v_bito = var.v_hasa = 0;					// digit, constants don't have an address
      var.v_bitl = 8 * sizeof var.v_valu;					// this is how many bits it has
      var.v_valu = strtoul (buff + bufi, &p, 0);				// get the value
      var.v_styp = intstyp;							// this is it's stab type as far as we care
      bufi = ((char const *)p) - buff;						// advance to terminator
    } else {
      for (i = bufi; ++ i < bufl;) {						// get number of chars in varname
        c = buff[i];
        if (!LEGALVARCHAR (c)) break;
      }
      if (!findvariable (dc, exefile, modindx, i - bufi, buff + bufi, mchargs_cpy, mchargs_adr, &var)) {
        PRINT "getoperand: cannot find variable <%*.*s>\n", i - bufi, i - bufi, buff + bufi);
        return (0);
      }
      // if (var.v_hasa) PRINT "getoperand*: %*.*s: v_addr %X, v_bitl %d, v_star %d, v_styp %s\n", 
      // 		i - bufi, i - bufi, buff + bufi, var.v_addr, var.v_bitl, var.v_star, var.v_styp);
      // else PRINT "getoperand*: %*.*s: v_valu %X, v_bitl %d, v_star %d, v_styp %s\n", 
      // 		i - bufi, i - bufi, buff + bufi, var.v_valu, var.v_bitl, var.v_star, var.v_styp);
      bufi = i;
    }
  }

  /* Else, we don't know what it is */

  else {
    PRINT "invalid operand at %*.*s\n", bufl, bufl, buff + bufi);
    return (0);
  }

  /* Apply type cast, if any */

  if (casstyp != NULL) {
    var.v_styp = casstyp;
    var.v_star = casstar;
    var.v_bitl = stypsizebits (casstyp, casstar);
  }

  /* Operand OK, return values */

rtn:
  i = findfinalstyp (dc, exefile, modindx, var.v_styp, &var.v_styp);
  if (i < 0) return (0);
  var.v_star += i;
  if (!fake) *var_r = var;
  return (bufi);
}

/************************************************************************/
/*									*/
/*  Find a variable in stab for module					*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile = the executable to look in				*/
/*	modindx = the module it is in					*/
/*	namel/name  = name of variable to find				*/
/*	mchargs_cpy = local copy of mchargs at the 'srcindx'		*/
/*	mchargs_adr = address of mchargs in target			*/
/*									*/
/*    Output:								*/
/*									*/
/*	findvariable = 0 : can't find it				*/
/*	               1 : variable found				*/
/*	*var_r = filled in (v_styp/v_star have been finalized)		*/
/*									*/
/************************************************************************/

static int findvariable (Dc *dc, 
                         Exefile *exefile, 
                         int modindx, 
                         int namel, 
                         char const *name, 
                         OZ_Mchargs *mchargs_cpy, 
                         OZ_Pointer mchargs_adr, 
                         Var *var_r)

{
  char const *stabstraddr, *string;
  Elf32_Sym const *sym_ent;
  int niidx, rbracecount, sdidx;
  OZ_Pointer lastfuncaddr, nxtinsad;
  Stab const *stab, *stab_tbl;

  memset (var_r, 0, sizeof *var_r);

  nxtinsad = (OZ_Pointer)GETNXTINSAD ((*mchargs_cpy));		// this is instruction pointer we're at

  stab_tbl  = exefile -> data + exefile -> stab -> sh_offset;	// point to beginning of .stab table for this module
  stab_tbl += exefile -> modules[modindx].stabindx;

  /* Set 'niidx' to the index of the right-brace record for the next instruction to be executed */

  lastfuncaddr = 0;						// haven't seen any function yet
  rbracecount  = 1;						// assume we will stop between { and }

  for (niidx = 0; niidx < exefile -> modules[modindx].stabcoun; niidx ++) { // step through the module's .stab records
    stab = stab_tbl + niidx;
    switch (stab -> n_type) {
      case N_FUN: {						// if function definition(s) present, 
        lastfuncaddr = exefile -> baseaddr + stab -> n_value;	// ... RBRAC are relative to most recent one
        break;
      }
      case N_RBRAC: {
        if (stab -> n_value + lastfuncaddr >= nxtinsad) goto gotit; // stop at appropriate right brace
        break;
      }
    }
  }
  rbracecount = 0;						// we went all the way to end of module
								// we're not between { and } so use count zero
gotit:

  /* Scan backward from that point to find the symbol name */

  stabstraddr = exefile -> modules[modindx].stabstrs;		// point to beginning of string table for this module

  for (sdidx = niidx; -- sdidx >= 0;) {
    stab = stab_tbl + sdidx;
    switch (stab -> n_type) {

      /* Left and Right brace                                                     */
      /* If we have a positive right brace count, we ignore any variables therein */

	//  int main ()
	//  {
	//     int x;			<- this would have rbracecount=0
	//     {
	//       int x;			<- this would have rbracecount=1
	//       some jibberish
	//     }
	//     eip is pointing here, 
	//       we want x with rbracecount=0
	//  }				<- started scanning on this brace

      case N_LBRAC: {
        if (rbracecount > 0) rbracecount --;
        break;
      }
      case N_RBRAC: {
        rbracecount ++;
        break;
      }

      /* Symbol names */

      case N_FUN:
      case N_GSYM:
      case N_LSYM:
      case N_LCSYM:
      case N_PSYM:
      case N_RSYM:
      case N_STSYM: {
        string = stabstraddr + stab -> n_strx;
        if ((rbracecount == 0) 
         && (string[namel] == ':') 
         && (memcmp (string, name, namel) == 0) 
         && (strchr ("(-0123456789FGPRSfgprs", string[namel+1]) != NULL)) goto foundsym;
        break;
      }
    }
  }

  /* Scanned all the way back to beginning of module - try scanning at the end of module for static symbol definitions */

  for (sdidx = exefile -> modules[modindx].stabcoun; -- sdidx > niidx;) {
    stab = stab_tbl + sdidx;
    switch (stab -> n_type) {

      /* These end the scan */

      case N_FUN: goto notonend;	// end of a function definition
      case N_RBRAC: goto notonend;	// either of the braces
      case N_LBRAC: goto notonend;

      /* Symbol names */

      case N_GSYM:
      case N_LSYM:
      case N_LCSYM:
      case N_PSYM:
      case N_RSYM:
      case N_STSYM: {
        string = stabstraddr + stab -> n_strx;
        if ((rbracecount == 0) 
         && (string[namel] == ':') 
         && (memcmp (string, name, namel) == 0) 
         && (strchr ("(-0123456789FGPRSfgprs", string[namel+1]) != NULL)) goto foundsym;
        break;
      }
    }
  }
notonend:

  /* Check image's symtab table in case it's a global.  If it is, we scan the whole executable for a matching GSYM record. */

  sym_ent = findglobalsymbol (exefile, namel, name);			// look for it in symtab table
  if (sym_ent != NULL) {						// if not found, it's not found
    for (modindx = exefile -> nmodules; -- modindx >= 0;) {		// loop through all modules
      stab_tbl  = exefile -> data + exefile -> stab -> sh_offset;	// point to beginning of .stab table for this module
      stab_tbl += exefile -> modules[modindx].stabindx;
      stabstraddr = exefile -> modules[modindx].stabstrs;		// point to beginning of string table for this module
      for (sdidx = exefile -> modules[modindx].stabcoun; -- sdidx >= 0;) { // loop through all symbols in module
        stab = stab_tbl + sdidx;
        if (stab -> n_type == N_GSYM) {					// we only care about global symbol entries
          string = stabstraddr + stab -> n_strx;			// point to the string
          if ((string[namel] == ':') 					// must have a ':' right after the name
           && (memcmp (string, name, namel) == 0)) goto foundsym;
        }
      }
    }
  }

  /* Symbol not found */

notfound:
  PRINT "findvariable: symbol <%*.*s> not found in module %s\n", namel, namel, name, exefile -> modules[modindx].name);
  return (0);

  /* Compute the variable's address */

foundsym:
  var_r -> v_styp = string + namel + 1;
  var_r -> v_addr = stab -> n_value;
  return (computevaraddr (dc, exefile, modindx, namel, name, mchargs_cpy, mchargs_adr, var_r));
}

/************************************************************************/
/*									*/
/*  Compute variable's address						*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile,modindx = module the variable is defined in		*/
/*	namel = length of variable's name				*/
/*	name  = variable's name						*/
/*	var_r -> v_styp = raw type					*/
/*	var_r -> v_addr = from stab -> n_value				*/
/*									*/
/*    Output:								*/
/*									*/
/*	computevaraddr = 0 : error					*/
/*	                 1 : success					*/
/*	*var_r = finialized						*/
/*									*/
/************************************************************************/

static int computevaraddr (Dc *dc, 
                           Exefile *exefile, 
                           int modindx, 
                           int namel, 
                           char const *name, 
                           OZ_Mchargs *mchargs_cpy, 
                           OZ_Pointer mchargs_adr, 
                           Var *var_r)

{
  char const *p, *q;
  Elf32_Sym const *sym_ent;
  int hilim, i, lolim;

  switch (var_r -> v_styp[0]) {

    /* Function - value is function entrypoint */

    case 'F':
    case 'f': {
      var_r -> v_valu = var_r -> v_addr + exefile -> baseaddr;
      var_r -> v_styp ++;
      break;
    }

    /* Global variable - address is global's address */

    case 'G':
    case 'g': {
      sym_ent = findglobalsymbol (exefile, namel, name);
      if (sym_ent == NULL) {
        PRINT "computevaraddr: cannot find global <%*.*s> in image symbol table\n", namel, namel, name);
        return (0);
      }
      var_r -> v_addr = exefile -> baseaddr + sym_ent -> st_value;
      var_r -> v_hasa = 1;
      var_r -> v_styp ++;
      break;
    }

    /* Function parameter - address is parameter's address */

    case 'P':
    case 'p': {
      var_r -> v_addr += ARGPOINTER (*mchargs_cpy);
      var_r -> v_hasa  = 1;
      var_r -> v_styp ++;
      break;
    }

    /* Register variable - address is its address in mchargs */

    case 'R':
    case 'r': {
      PRINT "computevaraddr: variable <%*.*s> is kept in a register and is not reliable\n", namel, namel, name);
      var_r -> v_addr = MCHARGSREG (mchargs_adr, var_r -> v_addr);
      var_r -> v_hasa = 1;
      var_r -> v_styp ++;
      break;
    }

    /* An address in the image (static variable) */

    case 'S':
    case 's': {
      var_r -> v_addr += exefile -> baseaddr;
      var_r -> v_hasa  = 1;
      var_r -> v_styp ++;
      break;
    }

    /* Otherwise, it's a local variable */

    default: {
      var_r -> v_addr += VARPOINTER (*mchargs_cpy);
      var_r -> v_hasa  = 1;
      break;
    }
  }

  /* Finalize type, ie, skip through equivalences and indirects until final type found */

finalize:
  i = findfinalstyp (dc, exefile, modindx, var_r -> v_styp, &(var_r -> v_styp));
  if (i < 0) return (0);
  var_r -> v_star += i;

  /* Set up variable's size (in bits) */

  var_r -> v_bitl = stypsizebits (var_r -> v_styp, var_r -> v_star);
  return (1);
}

/************************************************************************/
/*									*/
/*  Find symbol in image's symtbl array					*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile = executable to look in					*/
/*	namel   = length of symbol name					*/
/*	name    = symbol name						*/
/*									*/
/*    Output:								*/
/*									*/
/*	findglobalsymbol = NULL : symbol not found			*/
/*	                   else : pointer to symtab entry		*/
/*									*/
/************************************************************************/

static Elf32_Sym const *findglobalsymbol (Exefile *exefile, int namel, char const *name)

{
  char const *str_ent, *str_tbl;
  Elf32_Sym const *sym_ent;
  int i;

  i = exefile -> symtab -> sh_size / exefile -> symtab -> sh_entsize;	// get number of symbols in table
  sym_ent = exefile -> data + exefile -> symtab -> sh_offset;		// point to base of symbol table
  str_tbl = exefile -> data + exefile -> strtab -> sh_offset;		// get string table address

  while (-- i >= 0) {
    if ((ELF32_ST_BIND (sym_ent -> st_info) == STB_GLOBAL) || (ELF32_ST_BIND (sym_ent -> st_info) == STB_WEAK)) {
      str_ent = str_tbl + sym_ent -> st_name;
      if ((strlen (str_ent) == namel) && (memcmp (name, str_ent, namel) == 0)) return (sym_ent);
    }
    sym_ent ++;
  }
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Locate stab's type string for a given C-language type		*/
/*									*/
/*    Input:								*/
/*									*/
/*	exefile = the executable to look in				*/
/*	modindx = the module it is in					*/
/*	namel/name  = C-language type to find				*/
/*	mchargs_cpy = local copy of mchargs at the 'srcindx'		*/
/*	mchargs_adr = address of mchargs in target			*/
/*									*/
/*    Output:								*/
/*									*/
/*	findstypstr < 0 : not found					*/
/*	           else : number of asterisks to add on			*/
/*	*stypstr_r = stab type string					*/
/*									*/
/************************************************************************/

static int findstypstr (Dc *dc, 
                        Exefile *exefile, 
                        int modindx, 
                        int namel, 
                        char const *name, 
                        OZ_Mchargs *mchargs_cpy, 
                        char const **stypstr_r)

{
  char c, subtypechr, typechr;
  char const *p, *stabstraddr, *string;
  int niidx, rbracecount, sdidx;
  OZ_Pointer lastfuncaddr, nxtinsad;
  Stab const *stab, *stab_tbl;

  subtypechr = 0;						// look for <typename>:t records
  typechr = 't';
  if ((namel > 7) && (memcmp (name, "struct ", 7) == 0)) {
    subtypechr = 's';						// ... but maybe <structname>:T instead
    typechr = 'T';
    namel  -= 7;
    name   += 7;
  } else if ((namel > 6) && (memcmp (name, "union ", 6) == 0)) {
    subtypechr = 'u';						// ... but maybe <unionname>:T instead
    typechr = 'T';
    namel  -= 6;
    name   += 6;
  } else if ((namel > 5) && (memcmp (name, "enum ", 5) == 0)) {
    subtypechr = 'e';						// ... but maybe <enumname>:T instead
    typechr = 'T';
    namel  -= 5;
    name   += 5;
  }

  nxtinsad = (OZ_Pointer)GETNXTINSAD ((*mchargs_cpy));		// this is instruction pointer we're at

  stab_tbl  = exefile -> data + exefile -> stab -> sh_offset;	// point to beginning of .stab table for this module
  stab_tbl += exefile -> modules[modindx].stabindx;

  /* Set 'niidx' to the index of the rbrac for the next instruction to be executed */

  lastfuncaddr = 0;						// haven't seen any function yet

  for (niidx = 0; niidx < exefile -> modules[modindx].stabcoun; niidx ++) { // step through the module's .stab records
    stab = stab_tbl + niidx;
    switch (stab -> n_type) {
      case N_FUN: {						// if function definition(s) present, 
        lastfuncaddr = stab -> n_value;				// ... RBRAC are relative to most recent one
        break;
      }
      case N_RBRAC: {
        if (stab -> n_value + lastfuncaddr >= nxtinsad) goto gotit; // stop at appropriate right brace
        break;
      }
    }
  }
gotit:

  /* Scan backward from that point to find the type name */

  rbracecount = 1;						// haven't seen any braces yet, but symbols 
								//   come just before corresponding left brace
  stabstraddr = exefile -> modules[modindx].stabstrs;		// point to beginning of string table for this module

  for (sdidx = niidx; -- sdidx >= 0;) {
    stab = stab_tbl + sdidx;

    switch (stab -> n_type) {

      /* Left and Right brace                                                     */
      /* If we have a positive right brace count, we ignore any variables therein */

	//  int main ()
	//  {
	//     int x;			<- this would have rbracecount=0
	//     {
	//       int x;			<- this would have rbracecount=1
	//       some jibberish
	//     }
	//     eip is pointing here, 
	//       we want x with rbracecount=0
	//  }				<- started scanning on this brace

      case N_LBRAC: {
        if (rbracecount > 0) rbracecount --;
        break;
      }
      case N_RBRAC: {
        rbracecount ++;
        break;
      }

      /* Symbol names */

      /* <namestring>:t<stabtypenumber>=<definition>             */
      /* <namestring>:T<stabtypenumber>=<subtypechr><definition> */

      case N_LSYM: {
        if (rbracecount == 0) {
          string = stabstraddr + stab -> n_strx;
          if ((string[namel] == ':') && (string[namel+1] == typechr) && (memcmp (string, name, namel) == 0)) {
            if ((subtypechr == 0) || (((p = strchr (string + namel, '=')) != NULL) && (*(++ p) == subtypechr))) {
              return (findfinalstyp (dc, exefile, modindx, string + namel + 2, stypstr_r));
            }
          }
        }
        break;
      }
    }
  }

  /* Scanned all the way back to beginning of module - type not found */

  return (-1);
}

/************************************************************************/
/*									*/
/*  Find C-language type corresponding to stab type			*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc = debug context						*/
/*	exefile = executable file the types are defined in		*/
/*	modindx = module within that executable				*/
/*	varstyp = stab type string					*/
/*	varstar = number of *'s to put on end				*/
/*									*/
/*    Output:								*/
/*									*/
/*	findctypstr = NULL : couldn't find C-language type def		*/
/*	              else : pointer to malloc'd string buffer		*/
/*									*/
/************************************************************************/

static char *findctypstr (Dc *dc, Exefile *exefile, int modindx, char const *varstyp, int varstar)

{
  char const *begln, *class, *colon;
  char *ctypstr;
  int i, l;

look:
  i = findfinalstyp (dc, exefile, modindx, varstyp, &varstyp);
  if (i < 0) return (NULL);
  varstar += i;

  /* If it begins with 'x', it's a dangling enum/struct/union name */

  if (varstyp[0] == 'x') {
    class = NULL;
    if (varstyp[1] == 'e') class = "enum";
    if (varstyp[1] == 's') class = "struct";
    if (varstyp[1] == 'u') class = "union";
    if (class != NULL) {
      i = strlen (class);
      l = strlen (varstyp + 3);			// get length of C-type string (without trailing colon)
      ctypstr = MALLOC (i + l + varstar + 3);	// malloc output string buffer
      memcpy (ctypstr, class, i);		// put in class string
      ctypstr[i++] = ' ';			// a space
      memcpy (ctypstr + i, varstyp + 2, l);	// copy type name string (without trailing colon)
      i += l;
    } else {
      i = strlen (varstyp);			// can't decode, just output as is
      ctypstr = MALLOC (l + varstar + 2);
      memcpy (ctypstr, varstyp, i);
    }
    goto putstars;
  }

  /* Back up to colon and beginning of line */

  for (colon = varstyp; *(-- colon) != ':';) if (*colon == 0) return (NULL);
  for (begln = colon; begln[-1] != 0; -- begln) {}

  /* If char after colon is 't', then C-type name is from beg-of-line up to colon */

  if (colon[1] == 't') {
    i = colon - begln;				// get length of C-type string
    ctypstr = MALLOC (i + varstar + 2);		// malloc output string buffer
    memcpy (ctypstr, begln, i);			// copy C-type string
    goto putstars;
  }

  /* If char after colon is 'T', it's a struct, union or enum */

  if (colon[1] == 'T') {
    class = NULL;
    if (*varstyp == 'e') class = "enum";
    if (*varstyp == 's') class = "struct";
    if (*varstyp == 'u') class = "union";
    if (class != NULL) {
      i = strlen (class);
      l = colon - begln;			// get length of C-type string
      ctypstr = MALLOC (i + l + varstar + 3);	// malloc output string buffer
      memcpy (ctypstr, class, i);		// put in class string
      ctypstr[i++] = ' ';			// a space
      memcpy (ctypstr + i, begln, l);		// copy class name string
      i += l;
      goto putstars;
    }
  }

  /* If original type begins with '*', count it as a star and look for pointed-to type */

  if (*varstyp == '*') {
    varstar ++;
    varstyp ++;
    goto look;
  }

  return (NULL);

putstars:
  if (varstar > 0) {				// maybe it needs some asterisks on the end
    ctypstr[i++] = ' ';
    memset (ctypstr + i, '*', varstar);
    i += varstar;
  }
  ctypstr[i] = 0;				// null terminate
  return (ctypstr);				// return pointer
}

/************************************************************************/
/*									*/
/*  Find final definition of type					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc      = debug context						*/
/*	exefile = executable the type is defined in			*/
/*	modindx = module the type is defined in				*/
/*	stypstr = input stab type					*/
/*									*/
/*    Output:								*/
/*									*/
/*	findfinalstyp < 0 : not found					*/
/*	             else : number of indirect levels			*/
/*	*stypstr_r = final stab type					*/
/*									*/
/************************************************************************/

static int findfinalstyp (Dc *dc, Exefile *exefile, int modindx, char const *stypstr, char const **stypstr_r)

{
  char c;
  char const *p, *q, *stabstraddr, *string;
  int i, stars, styplen;
  Stab const *stab, *stab_tbl;

  stab_tbl  = exefile -> data + exefile -> stab -> sh_offset;		// point to beginning of .stab table for this module
  stab_tbl += exefile -> modules[modindx].stabindx;
  stabstraddr = exefile -> modules[modindx].stabstrs;			// point to beginning of string table for this module

  stars = 0;

  /* If it's a number or '('something'), it's an equivalency */
  /* If it's an '*' it's an indirect level                   */

  while (((c = *stypstr) == '*') || (c == '(') || ((c >= '0') && (c <= '9'))) {
    if (c == '*') {							// check for an indirect level
      stars ++;
      stypstr ++;
      continue;
    }
    if (c == '(') {							// check for '('something')' format
      p = strchr (stypstr, ')');					// ... it ends with a ')'
      if (p == NULL) return (-1);
      p ++;								// point past ')'
    } else {
      for (p = stypstr; (c = *(++ p)) != 0;) if ((c < '0') || (c > '9')) break; // number, point past last digit
    }
    if ((*p == '=') && (p[1] != 'x')) {					// see if another equivalency follows
      i = p - stypstr;							// ... but if we have like (0,20)=(0,20)
      if ((memcmp (p + 1, stypstr, i) == 0) && !LEGALVARCHAR (p[i+1])) goto itisvoid; // ... it is 'void'
      stypstr = ++ p;							// skip past the '='
      continue;								// ... and decode the following type
    }
    styplen = p - stypstr;						// if not, find this type's definition

    /* Look through the whole module for the type definition, could be anywhere             */
    /* It may be defined by a type record or by an equivalency in another variable's record */

    for (i = 0; i < exefile -> modules[modindx].stabcoun; i ++) {
      stab = stab_tbl + i;
      switch (stab -> n_type) {
        case N_FUN:
        case N_GSYM:
        case N_LSYM:
        case N_PSYM:
        case N_RSYM:
        case N_STSYM: {
          string = stabstraddr + stab -> n_strx;			// point to corresponding string
          p = strchr (string, ':');					// it must have a colon in it
          if (p != NULL) {
            while ((q = strchr (p, '=')) != NULL) {			// find the following =
              if (q[1] != 'x') {					// ignore '=x'struct/union_name
                if (((q[-styplen-1] < '0') || (q[-styplen-1] > '9')) 	// see if name before = matches
                 && (memcmp (q - styplen, stypstr, styplen) == 0)) {
                  if ((q[styplen+1] >= '0') && (q[styplen+1] <= '9')) goto foundtype;
                  if (memcmp (q + 1, stypstr, styplen) != 0) goto foundtype; // if = something else, go find it
                  goto itisvoid;					// if = itself, it is void
                }
              }
              p = ++ q;							// not that one, look for another =
            }
          }
          break;
        }
      }
    }
    if ((stypstr[styplen+0] == '=') && (stypstr[styplen+1] == 'x')) {	// maybe it's a dangling struct/union reference
      *stypstr_r = stypstr + styplen + 1;				// if so return pointer to 'xsStructname:' or whatever
      return (stars);
    }
    PRINT "findfinalstyp: type <%s> unresolved\n", stypstr);		// can't find it at all
itisvoid:
    *stypstr_r = voidstyp;						// ... use type 'void'
    return (stars);

foundtype:
    stypstr = ++ q;
  }

  *stypstr_r = stypstr;
  return (stars);
}

/************************************************************************/
/*									*/
/*  Given an struct/union address and type, get a field therein		*/
/*									*/
/*    Input:								*/
/*									*/
/*	varstyp = struct's stab type					*/
/*	fnamel/fname = field name to extract from struct		*/
/*									*/
/*    Output:								*/
/*									*/
/*	getstructfield = 0 : not a struct/union or field not found	*/
/*	                 1 : successful					*/
/*	*var = filled in:						*/
/*	  v_addr = untouched						*/
/*	  v_hasa = untouched						*/
/*	  v_bito = bit offset of field within struct			*/
/*	  v_bitl = length of field in bits				*/
/*	  v_styp = type of field (not finalized)			*/
/*	  v_star = 0							*/
/*									*/
/************************************************************************/

static int getstructfield (char const *varstyp, int fnamel, char const *fname, Var *var)

{
  char c, *p, *q, *r;

  /* [s/u]<structsizeinbytes><fieldname>:<fieldtype..lotsofcrap>,<bitoffs>,<bitsize>;<fieldname>:<fieldtype..lotsofcrap>,<bitoffs>,<bitsize>;...;; */

  /* First char must be 's' for struct; 'u' for union */

  c = *(varstyp ++);
  if ((c != 's') && (c != 'u')) return (0);

  /* Skip over struct/union size (string of decimal digits) */

  while (((c = *varstyp) >= '0') && (c <= '9')) varstyp ++;

  /* Find the field caller wants */

  p = strchr (varstyp, ':');
  if (p != NULL) {
    if ((p - varstyp == fnamel) && (memcmp (varstyp, fname, fnamel) == 0)) goto foundit;
    while ((p = strchr (++ p, ':')) != NULL) {
      if ((p[-fnamel-1] == ';') && (memcmp (p - fnamel, fname, fnamel) == 0)) goto foundit;
    }
  }
  return (0);

  /* Decipher the various parts (p points to the colon following fieldname) */

foundit:
  var -> v_styp = ++ p;				// field type directly follows the colon
  var -> v_star = 0;
  q = strstr (p, ";;");				// it either ends on a ";;"
  if (q == NULL) q = p + strlen (p);
  r = strchr (p, ':');				// ... or by another field name
  if (r == NULL) r = p + strlen (p);
  if (r < q) for (q = r; *(-- q) != ';';) {}	//     get q pointing to ';' after length
  while (*(-- q) != ',') {}			// back up to next-to-last comma
  while (*(-- q) != ',') {}
  var -> v_bito = strtol (++ q, &q, 0);		// get bit offset from first number
  var -> v_bitl = strtol (++ q, &q, 0);		// get bit length from second number
  return (1);					// successful
}

/************************************************************************/
/*									*/
/*  PRINT variable contents						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc       = debug context					*/
/*	exefile  = executable file containing variable definition	*/
/*	modindx  = module the variable is defined in			*/
/*	incltype = 0 : don't print variable's type			*/
/*	           1 : print variable's type				*/
/*	var      = variable to print					*/
/*									*/
/************************************************************************/

static int printvariable (Dc *dc, Exefile *exefile, int modindx, int incltype, Var var)

{
  char c, cbuf, cstr[64], *varctyp;
  char const *p, *q;
  int i, inarray, j, l;

  inarray = 0;

  /* Finalize type */

finalize:
  i = findfinalstyp (dc, exefile, modindx, var.v_styp, &var.v_styp);
  if (i < 0) {
    PRINT "printvariable: cannot finalize type <%s>\n", var.v_styp);
    return (0);
  }
  var.v_star += i;

  /* If it has *'s on the end, print out the pointer value */

  if (var.v_star > 0) {
    if (!readvarvalue (dc, &var)) return (0);				// read pointer's value
    varctyp = findctypstr (dc, exefile, modindx, var.v_styp, var.v_star); // see if it has a C-language type
    if (varctyp == NULL) {
      if (!incltype) PRINT "0x%X", var.v_valu);				// if not, PRINT without type
      else PRINT "(? *)0x%X", var.v_valu);	
    } else {
      if ((var.v_valu != 0) && (strcmp ("char *", varctyp) == 0)) {	// maybe it's a C-language string
        for (i = 0; i < (sizeof cstr) - 1; i ++) {
          if (!READMEM (1, var.v_valu + i, &cbuf)) break;
          if (cbuf == 0) break;
          cbuf &= 127;
          if ((cbuf < 32) || (cbuf > 126)) cbuf = '.';
          cstr[i] = cbuf;
        }
        cstr[i] = 0;
        if (!incltype) PRINT "0x%X='%s'", var.v_valu, cstr);		// if so, print with first 63 chars
        else PRINT "(char *)0x%X='%s'", var.v_valu, cstr);
      } else {
        if (!incltype) PRINT "0x%X", var.v_valu);
        else PRINT "(%s)0x%X", varctyp, var.v_valu);			// not C-string, PRINT with returned type
      }
      FREE (varctyp);							// free C-type buffer
    }
    return (1);								// successful
  }

  /* Decode type specification string */

  switch (*var.v_styp) {

    /* '*' - pointer to what follows */

    case '*': {
      var.v_star = 1;
      var.v_styp ++;
      goto finalize;
    }

    /* '@' - variable size */
    /* '@s'size_in_bits';' */

    case '@': {
      if (var.v_styp[1] == 's') {					// '@' has to be follwed by 's'
        var.v_styp += 2;						// skip over '@s'
        var.v_bitl  = oz_hw_atoi (var.v_styp, &j);			// get size in bits as a decimal integer string
        if (var.v_styp[j] == ';') {					// it must terminate with a ';'
          var.v_styp += ++ j;						// ok, increment over everything including ';'
          goto finalize;						// decode the rest as a legit type
        }
        var.v_styp -= 2;
        PRINT "printvariable: bad size prefix <%s>\n", var.v_styp);
        return (0);
      }
      break;
    }

    /* 'a' - array */

    case 'a': {
      int hilim, lolim;

      var.v_styp = arraysubscript (dc, ++ var.v_styp, &lolim, &hilim);	// parse subscript
      if (var.v_styp == NULL) return (0);
      if (lolim == 0) PRINT "[%d]", hilim + 1);				// print subscript limits
      else PRINT "[%d..%d]", lolim, hilim);
      inarray = 1;
      goto finalize;							// go process array element type
    }

    /* 'e' - enum definition */

    case 'e': {
      if (inarray) goto printarraytype;
      if (var.v_bitl == 0) var.v_bitl = 8 * sizeof enum_dummy;		// default size
      if (!readvarvalue (dc, &var)) return (0);
      var.v_styp ++;							// skip over the 'e'
      while (*var.v_styp != 0) {					// scan string for value
        q = strchr (var.v_styp, ':');					// there has to be a colon in there
        if (q == NULL) break;
        p = strchr (++ q, ',');						// followed by a comma
        if (p == NULL) break;						// no more commas mean no more values
        if (atoi (q) == var.v_valu) {
          PRINT "%*.*s", p - var.v_styp, p - var.v_styp, var.v_styp);	// found it, print it out
          return (1);
        }
        var.v_styp = ++ p;						// no match, try after next 
      }
      PRINT "%d", var.v_valu);						// not found, print out numeric value
      return (1);
    }

    /* 'r' - range (integer) */
    /* 'r'same_type_number;low_limit;high_limit; */

    case 'r': {
      if (inarray) goto printarraytype;
      if (!readvarvalue (dc, &var)) return (0);
      varctyp = findctypstr (dc, exefile, modindx, var.v_styp, var.v_star); // see if it has a C-language type
      cstr[0] = 0;
      if (incltype && (varctyp != NULL)) strcpy (cstr, "(%s)");		// maybe output type string
      strcat (cstr, "%u");						// always print unsigned decimal
      if (var.v_valu & -16) strcat (cstr, "=0x%X");			// only do hex if more than one higit
      if (((varctyp == NULL) || (strstr (varctyp, "unsigned") == NULL)) // only do signed decimal if var is not unsigned
       && (((signed)var.v_valu) < 0)) strcat (cstr, "=%d");		// ... and its value is negative
      if ((varctyp != NULL) && (strcmp (varctyp, "char") == 0) && (var.v_valu >= 32) && (var.v_valu < 127)) {
        strcat (cstr, "='%c'");						// maybe it's a printable char
      }
      if (cstr[0] != '(') PRINT cstr, var.v_valu, var.v_valu, var.v_valu, var.v_valu);
      else PRINT cstr, varctyp, var.v_valu, var.v_valu, var.v_valu, var.v_valu);
      if (varctyp != NULL) FREE (varctyp);				// free C-type string buffer
      return (1);
    }

    /* 's' - structure definition ; 'u' - union definition */
    /* [s/u]<structsizeinbytes><fieldname>:<fieldtype>,<bitoffs>,<bitsize>;<fieldname>:<fieldtype>,<bitoffs>,<bitsize>;... */

    case 's':
    case 'u': {
      char const *sep;
      Var field;

      if (inarray) goto printarraytype;
      varctyp = NULL;							// maybe print struct/union's C-language type
      if (incltype) varctyp = findctypstr (dc, exefile, modindx, var.v_styp, var.v_star);
      if (varctyp == NULL) PRINT "{ ");
      else {
        PRINT "(%s){ ", varctyp);
        FREE (varctyp);
      }

      while (((c = *(++ var.v_styp)) >= '0') && (c <= '9')) {}		// skip over struct size
      sep = "";
      while (*var.v_styp != 0) {					// scan through the struct fields
        p = strchr (var.v_styp, ';');					// each ends with a ';'
        if (p == NULL) break;						// no more ';'s mean no more fields
        if (p == var.v_styp) break;					// also, ';;' means the end
        q = strchr (var.v_styp, ':');					// find colon after field name
        if (q != NULL) {
          PRINT "%s%*.*s=", sep, q - var.v_styp, q - var.v_styp, var.v_styp);
          var.v_styp = ++ q;						// field's type follows colon
          for (q = p; (q > var.v_styp) && (*(-- q) != ',');) {}
          while ((q > var.v_styp) && (*(-- q) != ',')) {}
          if (q > var.v_styp) {
            field.v_styp = var.v_styp;					// point to field type string
            field.v_star = 0;						// no extra indirect levels
            field.v_addr = var.v_addr;					// same address
            field.v_bito = var.v_bito + oz_hw_atoi (++ q, &j);		// ... but at this bit offset
            field.v_hasa = 1;						// it has an address
            q += j;
            field.v_bitl = oz_hw_atoi (++ q, &j);			// it has this number of bits
            printvariable (dc, exefile, modindx, 0, field);		// print it out
          }
          sep = ", ";
        }
        var.v_styp = ++ p;						// point to next field
      }
      PRINT " }");
      return (1);
    }
  }

  /* Who knows what it is */

  PRINT "printvariable: undecodable type <%s>\n", var.v_styp);
  return (0);

  /* It was an array, just print the data type */

printarraytype:
  if (incltype) {
    varctyp = findctypstr (dc, exefile, modindx, var.v_styp, var.v_star);
    if (varctyp != NULL) {
      PRINT "(%s)", varctyp);
      FREE (varctyp);
    }
  }
  if (var.v_hasa) PRINT "0x%X", var.v_addr);
  return (1);
}

/************************************************************************/
/*									*/
/*  Dereference a pointer variable					*/
/*									*/
/*    Input:								*/
/*									*/
/*	var = variable to dereference					*/
/*									*/
/*    Output:								*/
/*									*/
/*	derefpointer = 0 : failed, variable not a pointer (or array)	*/
/*	               1 : successful					*/
/*	var -> v_hasa = 1						*/
/*	var -> v_addr = address of variable				*/
/*	var -> v_styp/v_star = updated to pointed-to type (finalized)	*/
/*									*/
/************************************************************************/

static int derefpointer (Dc *dc, Exefile *exefile, int modindx, Var *var)

{
  int hilim, lolim;

  /* Being a pointer overrides being an array (ie, var might be pointer to an array) */

  if (var -> v_star > 0) {
    var -> v_star --;					// there shall be one less * after type
							// eg, (Mchargs *) becomes (Mchargs)
    if (!readvarvalue (dc, var)) return (0);		// read pointer value
    var -> v_addr = var -> v_valu;
    var -> v_hasa = 1;
    var -> v_bito = 0;
    return (1);
  }

  /* Not a pointer, must be an array then */

  if ((*var -> v_styp != 'a') || !(var -> v_hasa)) {
    PRINT "derefpointer: cannot dereference non-pointer/non-array\n");
    return (0);
  }

  /* Array, strip off a subscript from the type and that's all we do */

  var -> v_styp = arraysubscript (dc, ++ var -> v_styp, &lolim, &hilim);
  if (var -> v_styp == NULL) return (0);
  hilim = findfinalstyp (dc, exefile, modindx, var -> v_styp, &(var -> v_styp));
  if (hilim < 0) return (0);
  var -> v_star += hilim;
  return (1);
}

/************************************************************************/
/*									*/
/*  Parse an array subscript specification				*/
/*									*/
/*    Input:								*/
/*									*/
/*	stypstr = stab type string, just past the 'a'			*/
/*									*/
/*    Output:								*/
/*									*/
/*	arraysubscript = NULL : parse error				*/
/*	                 else : stab type string of element		*/
/*	*lolim_r = lower subscript limit (inclusive)			*/
/*	*hilim_r = higher subscript limit (inclusive)			*/
/*									*/
/*  Element lnm[3][4][5]:						*/
/*									*/
/*  Element:t(0,21)=(0,22)=s8xyz:(0,1),0,32;uvw:(0,1),32,32;;		*/
/*  lnm:(0,23)=ar(0,24)=r(0,24);0000000000000;0037777777777;;0;2;(0,25)=ar(0,24);0;3;(0,26)=ar(0,24);0;4;(0,21)
/*									*/
/************************************************************************/

static char const *arraysubscript (Dc *dc, char const *stypstr, int *lolim_r, int *hilim_r)

{
  char const *p, *q;
  int l;

  q = strchr (stypstr, ';');						// get next semi-colon
  if (q == NULL) q = stypstr + strlen (stypstr);
  while (1) {
    if (*stypstr != 'r') goto badsubscript;				// we only know how to do intger-type subscripts
    p = strchr (stypstr, '=');						// get next equal sign
    if (p == NULL) p = stypstr + strlen (stypstr);
    if (p > q) break;							// stop if semi-colon comes first
    stypstr = ++ p;							// equals comes first, point just past it
  }									// ... and process equivalent type
  *lolim_r = oz_hw_atoi (++ q, &l);					// get subscript low limit
  p = q + l;
  if (*p != ';') goto badsubscript;
  *hilim_r = oz_hw_atoi (++ p, &l);					// get subscript high limit
  q = p + l;
  if (*q != ';') goto badsubscript;
  if (q[1] == ';') {							// check for double-semicolon
    q ++;
    *lolim_r = oz_hw_atoi (++ q, &l);					// if so, get subscript stuff after that
    p = q + l;
    if (*p != ';') goto badsubscript;
    *hilim_r = oz_hw_atoi (++ p, &l);
    q = p + l;
    if (*q != ';') goto badsubscript;
  }
  return (++ q);							// datatype follows that last semi-colon we found

badsubscript:
  if (dc != NULL) PRINT "arraysubscript: bad array subscript descriptor <%s>\n", stypstr);
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Read variable's value from memory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	var = variable to read						*/
/*									*/
/*    Output:								*/
/*									*/
/*	readvarvalue = 0 : failed					*/
/*	               1 : successful					*/
/*	var -> v_valu = filled in					*/
/*									*/
/************************************************************************/

static int readvarvalue (Dc *dc, Var *var)

{
  if (!(var -> v_hasa)) return (1);		// hopefully v_valu is already filled in
  var -> v_valu = 0;				// it's not, zero it so it fills properly
  return (readmembits (dc, var -> v_bitl, var -> v_addr, var -> v_bito, sizeof var -> v_valu, &(var -> v_valu)));
}

/************************************************************************/
/*									*/
/*  Write variable's value to memory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	var = variable to write						*/
/*									*/
/*    Output:								*/
/*									*/
/*	writevarvalue = 0 : failed					*/
/*	                1 : successful					*/
/*									*/
/************************************************************************/

static int writevarvalue (Dc *dc, Var *var)

{
  Value data, mask;

  if (!(var -> v_hasa)) {
    PRINT "no address to write to\n");
    return (0);
  }

  if (var -> v_bitl > 8 * sizeof data) {
    PRINT "write length %d too big\n", var -> v_bitl);
    return (0);
  }

  var -> v_addr += var -> v_bito / 8;
  var -> v_bito &= 7;

  if (var -> v_bito == 0) {
    if ((var -> v_bitl & 7) == 0) return (WRITEMEM (var -> v_bitl / 8, &var -> v_valu, var -> v_addr));
    var -> v_addr += var -> v_bitl / 8;
    if (!READMEM ((var -> v_bitl + 7) / 8, var -> v_addr, &data)) return (0);
    mask = -1 << (var -> v_bitl & 7);
    data = (data & mask) | (var -> v_valu & ~mask);
    return (WRITEMEM ((var -> v_bitl + 7) / 8, &data, var -> v_addr));
  }

  if (var -> v_bito + var -> v_bitl <= 8 * sizeof data) {
    if (!READMEM ((var -> v_bito + var -> v_bitl + 7) / 8, var -> v_addr, &data)) return (0);
    mask = (~-1 << (var -> v_bitl & 7)) << var -> v_bito;
    data = (data & ~mask) | ((var -> v_valu & mask) << var -> v_bito);
    return (WRITEMEM ((var -> v_bito + var -> v_bitl + 7) / 8, &data, var -> v_addr));
  }

  PRINT "write offset %d length %d too big\n", var -> v_bito, var -> v_bitl);
  return (0);
}

/************************************************************************/
/*									*/
/*  Read memory bits from target					*/
/*									*/
/*    Input:								*/
/*									*/
/*	varbitl = bit length to read					*/
/*	varaddr = address to read from					*/
/*	varbito = bit offset from that address to start at		*/
/*	size = size of output buffer					*/
/*	buff = output buffer						*/
/*									*/
/*    Output:								*/
/*									*/
/*	readmembits = 0 : failure					*/
/*	              1 : success					*/
/*	*buff = filled in with data					*/
/*									*/
/************************************************************************/

static int readmembits (Dc *dc, int varbitl, OZ_Pointer varaddr, uLong varbito, int size, void *buff)

{
  uByte *pntr, tempb;
  uLong templ;

  pntr = buff;

  varaddr += varbito / 8;
  varbito &= 7;
  if (varbitl > size * 8) varbitl = size * 8;

  /* If it starts on a byte boundary, just read directly into caller's buffer */

  if (varbito == 0) {
    if (!READMEM ((varbitl + 7) / 8, varaddr, pntr)) return (0);
    if (varbitl & 7) pntr[varbitl/8] &= ~(0xFF << (varbitl & 7));
    return (1);
  }

  /* If it can be read as a single long (or less), just read, shift and write */

  if (varbito + varbitl <= 32) {
    if (!READMEM ((varbitl + 7) / 8, varaddr, &templ)) return (0);
    templ >>= varbito;
    templ  &= ~(0xFFFFFFFF << varbitl);
  }

  /* It takes at least 5 bytes of reading */

  else {
    if (!READMEM (1, varaddr, &tempb)) return (0);	// read first 8-varbito bits into tempb
    varaddr ++;
    tempb  >>= varbito;
    varbitl -= 8 - varbito;				// that many fewer input bits to read

    while (varbitl > 32) {				// repeat while whole longs of input to process
      if (!READMEM (4, varaddr, &templ)) return (0);	// read long into templ
      varaddr += 4;
      varbitl -= 32;
      *(uLong *)pntr = tempb | (templ << (8 - varbito)); // write long to output
      pntr    += 4;
      tempb    = templ >> 24 + varbito;			// shift over carry bits for next loop
    }

    if (!READMEM ((varbitl + 7) / 8, varaddr, &templ)) return (0); // read last bytes
    templ     &= ~(0xFFFFFFFF << varbitl);		// set upper bits to zeroes
    *(pntr ++) = tempb | (templ << (8 - varbito));	// store with carry bits
    templ    >>= varbito;				// shift junk out of bottom
    varbitl   -= varbito;				// this many input bits left to process
  }

  /* Write varbitl bits of templ to output */

  while (varbitl > 0) {					// repeat while there are input bits left to do
    *(pntr ++) = templ;					// store a bytes worth
    templ    >>= 8;					// shift for next byte
    varbitl   -= 8;					// see if any left to do
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Count bits in a range specification value				*/
/*									*/
/************************************************************************/

static char const *rangebits (char const *p, int *bits)

{
  char c;
  int b;
  Value v;

  v = 0;
  if (*p == '-') p ++;

  c = *p - '0';
  b = 10;
  if (c == 0) b = 8;
  do {
    v = v * b + c;
    c = *(++ p) - '0';
  } while ((c >= 0) && (c < b));

  *bits = 0;
  while (v != 0) {
    (*bits) ++;
    v /= 2;
  }

  return (p);
}
