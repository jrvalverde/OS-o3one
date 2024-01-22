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
/*  Machine-dependent code for addressing variables			*/
/*									*/
/************************************************************************/

#define CLRSINGSTEP(__mchargs) (__mchargs).eflags &= ~0x100
#define SETSINGSTEP(__mchargs) (__mchargs).eflags |=  0x100
#define TSTSINGSTEP(__mchargs) ((__mchargs).eflags & 0x100)
#define GETNXTINSAD(__mchargs) (__mchargs).eip
#define FRAMEPTR(__mchargs) (__mchargs).ebp
#define OUTFRAME(__mchargs,__oldframe) ((__mchargs).ebp > (__oldframe))
#define ARGPOINTER(__mchargs) (__mchargs).ebp
#define VARPOINTER(__mchargs) (__mchargs).ebp
#define MCHARGSREG(__mchargs,__rn) (OZ_Pointer)mchargsreg ((OZ_Mchargs *)(__mchargs),__rn)
#define STEPOVER(__mchargs) stepover (dc, &(__mchargs))

/* Return register's address in target machine args */

static uLong *mchargsreg (OZ_Mchargs *mchargs, int rn)

{
  switch (rn & 7) {
    case 0: return &(mchargs -> eax);
    case 1: return &(mchargs -> ecx);
    case 2: return &(mchargs -> edx);
    case 3: return &(mchargs -> ebx);
    case 4: return &(mchargs -> esp);
    case 5: return &(mchargs -> ebp);
    case 6: return &(mchargs -> esi);
    case 7: return &(mchargs -> edi);
  }
  return (NULL);
}

/* Return pointer to next upper frame struct */

static Frame *upperframe (Dc *dc, Thread *thread, Frame *oldframe)

{
  Frame *frame;

  /* If we already know outer frame, return pointer */

  if (oldframe -> next != NULL) return (oldframe -> next);

  /* Set up next outer frame = old frame with incremented level                           */
  /* Also, the outer frames are just our imagination, so they don't have a target address */

  frame = MALLOC (sizeof *frame);
  *frame = *oldframe;
  frame -> level ++;
  frame -> mchargs_adr = NULL;
  frame -> mchargx_adr = NULL;

  /* We haven't tried to look for its source yet */

  frame -> srcline = 0;

  /* All we can update is the stack pointer, instruction pointer and frame pointer */

  frame -> mchargs_cpy.esp = frame -> mchargs_cpy.ebp + 8;
  if (!READMEM (4, frame -> mchargs_cpy.ebp + 4, &(frame -> mchargs_cpy.eip))) goto readerr;
  if (!READMEM (4, frame -> mchargs_cpy.ebp + 0, &(frame -> mchargs_cpy.ebp))) goto readerr;

  /* If new frame is same or lower than older it's no good - this will stop frame loops and stop on null terminator */

  if (frame -> mchargs_cpy.ebp <= oldframe -> mchargs_cpy.ebp) goto readerr;

  /* Link it and return pointer */

  oldframe -> next = frame;
  return (frame);

  /* Can't read frame, pretend it doesn't exist */

readerr:
  FREE (frame);
  return (NULL);
}

/* If instr pointed to by mchargs is a call instr, return number of bytes it takes, else return 0 */

static int stepover (Dc *dc, OZ_Mchargs *mchargs)

{
  int extra;
  uByte modrm, opcode;

  if (!READMEM (1, mchargs -> eip, &opcode)) return (0);
  if (opcode == 0xE8) return (5);									// call near rel32
  if ((opcode == 0xFF) && READMEM (1, mchargs -> eip + 1, &modrm) && ((modrm & 0x38) == 0x10)) {	// call near indirect
    switch (modrm >> 6) {
      case 0: extra = 0; if ((modrm & 7) == 5) extra = 4; break;
      case 1: extra = 8; break;
      case 2: extra = 4; break;
      case 3: return (2);
    }
    if ((modrm & 7) == 4) extra ++;
    return (extra + 2);
  }

  return (0);
}
