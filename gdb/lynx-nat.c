/* Native-dependent code for LynxOS.
   Copyright 1993, 1994 Free Software Foundation, Inc.

This file is part of GDB.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include "defs.h"
#include "frame.h"
#include "inferior.h"
#include "target.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/fpp.h>

static unsigned long registers_addr PARAMS ((int pid));

#define X(ENTRY)(offsetof(struct econtext, ENTRY))

#ifdef I386
/* Mappings from tm-i386v.h */

static int regmap[] =
{
  X(eax),
  X(ecx),
  X(edx),
  X(ebx),
  X(esp),			/* sp */
  X(ebp),			/* fp */
  X(esi),
  X(edi),
  X(eip),			/* pc */
  X(flags),			/* ps */
  X(cs),
  X(ss),
  X(ds),
  X(es),
  X(ecode),			/* Lynx doesn't give us either fs or gs, so */
  X(fault),			/* we just substitute these two in the hopes
				   that they are useful. */
};
#endif

#ifdef M68K
/* Mappings from tm-m68k.h */

static int regmap[] =
{
  X(regs[0]),			/* d0 */
  X(regs[1]),			/* d1 */
  X(regs[2]),			/* d2 */
  X(regs[3]),			/* d3 */
  X(regs[4]),			/* d4 */
  X(regs[5]),			/* d5 */
  X(regs[6]),			/* d6 */
  X(regs[7]),			/* d7 */
  X(regs[8]),			/* a0 */
  X(regs[9]),			/* a1 */
  X(regs[10]),			/* a2 */
  X(regs[11]),			/* a3 */
  X(regs[12]),			/* a4 */
  X(regs[13]),			/* a5 */
  X(regs[14]),			/* fp */
  offsetof (st_t, usp) - offsetof (st_t, ec), /* sp */
  X(status),			/* ps */
  X(pc),

  X(fregs[0*3]),		/* fp0 */
  X(fregs[1*3]),		/* fp1 */
  X(fregs[2*3]),		/* fp2 */
  X(fregs[3*3]),		/* fp3 */
  X(fregs[4*3]),		/* fp4 */
  X(fregs[5*3]),		/* fp5 */
  X(fregs[6*3]),		/* fp6 */
  X(fregs[7*3]),		/* fp7 */

  X(fcregs[0]),			/* fpcontrol */
  X(fcregs[1]),			/* fpstatus */
  X(fcregs[2]),			/* fpiaddr */
  X(ssw),			/* fpcode */
  X(fault),			/* fpflags */
};
#endif

#ifdef SPARC
/* Mappings from tm-sparc.h */

#define FX(ENTRY)(offsetof(struct fcontext, ENTRY))

static int regmap[] =
{
  -1,				/* g0 */
  X(g1),
  X(g2),
  X(g3),
  X(g4),
  -1,				/* g5->g7 aren't saved by Lynx */
  -1,
  -1,

  X(o[0]),
  X(o[1]),
  X(o[2]),
  X(o[3]),
  X(o[4]),
  X(o[5]),
  X(o[6]),			/* sp */
  X(o[7]),			/* ra */

  -1,-1,-1,-1,-1,-1,-1,-1,	/* l0 -> l7 */

  -1,-1,-1,-1,-1,-1,-1,-1,	/* i0 -> i7 */

  FX(f.fregs[0]),		/* f0 */
  FX(f.fregs[1]),
  FX(f.fregs[2]),
  FX(f.fregs[3]),
  FX(f.fregs[4]),
  FX(f.fregs[5]),
  FX(f.fregs[6]),
  FX(f.fregs[7]),
  FX(f.fregs[8]),
  FX(f.fregs[9]),
  FX(f.fregs[10]),
  FX(f.fregs[11]),
  FX(f.fregs[12]),
  FX(f.fregs[13]),
  FX(f.fregs[14]),
  FX(f.fregs[15]),
  FX(f.fregs[16]),
  FX(f.fregs[17]),
  FX(f.fregs[18]),
  FX(f.fregs[19]),
  FX(f.fregs[20]),
  FX(f.fregs[21]),
  FX(f.fregs[22]),
  FX(f.fregs[23]),
  FX(f.fregs[24]),
  FX(f.fregs[25]),
  FX(f.fregs[26]),
  FX(f.fregs[27]),
  FX(f.fregs[28]),
  FX(f.fregs[29]),
  FX(f.fregs[30]),
  FX(f.fregs[31]),

  X(y),
  X(psr),
  X(wim),
  X(tbr),
  X(pc),
  X(npc),
  FX(fsr),			/* fpsr */
  -1,				/* cpsr */
};
#endif

#ifdef SPARC

/* This routine handles some oddball cases for Sparc registers and LynxOS.
   In partucular, it causes refs to G0, g5->7, and all fp regs to return zero.
   It also handles knows where to find the I & L regs on the stack.  */

void
fetch_inferior_registers (regno)
     int regno;
{
  int whatregs = 0;

#define WHATREGS_FLOAT 1
#define WHATREGS_GEN 2
#define WHATREGS_STACK 4

  if (regno == -1)
    whatregs = WHATREGS_FLOAT | WHATREGS_GEN | WHATREGS_STACK;
  else if (regno >= L0_REGNUM && regno <= I7_REGNUM)
    whatregs = WHATREGS_STACK;
  else if (regno >= FP0_REGNUM && regno < FP0_REGNUM + 32)
    whatregs = WHATREGS_FLOAT;
  else
    whatregs = WHATREGS_GEN;

  if (whatregs & WHATREGS_GEN)
    {
      struct econtext ec;		/* general regs */
      char buf[MAX_REGISTER_RAW_SIZE];
      int retval;
      int i;

      errno = 0;
      retval = ptrace (PTRACE_GETREGS, inferior_pid, (PTRACE_ARG3_TYPE) &ec,
		       0);
      if (errno)
	perror_with_name ("Sparc fetch_inferior_registers(ptrace)");
  
      memset (buf, 0, REGISTER_RAW_SIZE (G0_REGNUM));
      supply_register (G0_REGNUM, buf);
      supply_register (TBR_REGNUM, (char *)&ec.tbr);

      memcpy (&registers[REGISTER_BYTE (G1_REGNUM)], &ec.g1,
	      4 * REGISTER_RAW_SIZE (G1_REGNUM));
      for (i = G1_REGNUM; i <= G1_REGNUM + 3; i++)
	register_valid[i] = 1;

      supply_register (PS_REGNUM, (char *)&ec.psr);
      supply_register (Y_REGNUM, (char *)&ec.y);
      supply_register (PC_REGNUM, (char *)&ec.pc);
      supply_register (NPC_REGNUM, (char *)&ec.npc);
      supply_register (WIM_REGNUM, (char *)&ec.wim);

      memcpy (&registers[REGISTER_BYTE (O0_REGNUM)], ec.o,
	      8 * REGISTER_RAW_SIZE (O0_REGNUM));
      for (i = O0_REGNUM; i <= O0_REGNUM + 7; i++)
	register_valid[i] = 1;
    }

  if (whatregs & WHATREGS_STACK)
    {
      CORE_ADDR sp;
      int i;

      sp = read_register (SP_REGNUM);

      target_xfer_memory (sp + FRAME_SAVED_I0,
			  &registers[REGISTER_BYTE(I0_REGNUM)],
			  8 * REGISTER_RAW_SIZE (I0_REGNUM), 0);
      for (i = I0_REGNUM; i <= I7_REGNUM; i++)
	register_valid[i] = 1;

      target_xfer_memory (sp + FRAME_SAVED_L0,
			  &registers[REGISTER_BYTE(L0_REGNUM)],
			  8 * REGISTER_RAW_SIZE (L0_REGNUM), 0);
      for (i = L0_REGNUM; i <= L0_REGNUM + 7; i++)
	register_valid[i] = 1;
    }

  if (whatregs & WHATREGS_FLOAT)
    {
      struct fcontext fc;		/* fp regs */
      int retval;
      int i;

      errno = 0;
      retval = ptrace (PTRACE_GETFPREGS, inferior_pid, (PTRACE_ARG3_TYPE) &fc,
		       0);
      if (errno)
	perror_with_name ("Sparc fetch_inferior_registers(ptrace)");
  
      memcpy (&registers[REGISTER_BYTE (FP0_REGNUM)], fc.f.fregs,
	      32 * REGISTER_RAW_SIZE (FP0_REGNUM));
      for (i = FP0_REGNUM; i <= FP0_REGNUM + 31; i++)
	register_valid[i] = 1;

      supply_register (FPS_REGNUM, (char *)&fc.fsr);
    }
}

/* This routine handles storing of the I & L regs for the Sparc.  The trick
   here is that they actually live on the stack.  The really tricky part is
   that when changing the stack pointer, the I & L regs must be written to
   where the new SP points, otherwise the regs will be incorrect when the
   process is started up again.   We assume that the I & L regs are valid at
   this point.  */

void
store_inferior_registers (regno)
     int regno;
{
  int whatregs = 0;

  if (regno == -1)
    whatregs = WHATREGS_FLOAT | WHATREGS_GEN | WHATREGS_STACK;
  else if (regno >= L0_REGNUM && regno <= I7_REGNUM)
    whatregs = WHATREGS_STACK;
  else if (regno >= FP0_REGNUM && regno < FP0_REGNUM + 32)
    whatregs = WHATREGS_FLOAT;
  else if (regno == SP_REGNUM)
    whatregs = WHATREGS_STACK | WHATREGS_GEN;
  else
    whatregs = WHATREGS_GEN;

  if (whatregs & WHATREGS_GEN)
    {
      struct econtext ec;		/* general regs */
      int retval;

      ec.tbr = read_register (TBR_REGNUM);
      memcpy (&ec.g1, &registers[REGISTER_BYTE (G1_REGNUM)],
	      4 * REGISTER_RAW_SIZE (G1_REGNUM));

      ec.psr = read_register (PS_REGNUM);
      ec.y = read_register (Y_REGNUM);
      ec.pc = read_register (PC_REGNUM);
      ec.npc = read_register (NPC_REGNUM);
      ec.wim = read_register (WIM_REGNUM);

      memcpy (ec.o, &registers[REGISTER_BYTE (O0_REGNUM)],
	      8 * REGISTER_RAW_SIZE (O0_REGNUM));

      errno = 0;
      retval = ptrace (PTRACE_SETREGS, inferior_pid, (PTRACE_ARG3_TYPE) &ec,
		       0);
      if (errno)
	perror_with_name ("Sparc fetch_inferior_registers(ptrace)");
    }

  if (whatregs & WHATREGS_STACK)
    {
      int regoffset;
      CORE_ADDR sp;

      sp = read_register (SP_REGNUM);

      if (regno == -1 || regno == SP_REGNUM)
	{
	  if (!register_valid[L0_REGNUM+5])
	    abort();
	  target_xfer_memory (sp + FRAME_SAVED_I0,
			      &registers[REGISTER_BYTE (I0_REGNUM)],
			      8 * REGISTER_RAW_SIZE (I0_REGNUM), 1);

	  target_xfer_memory (sp + FRAME_SAVED_L0,
			      &registers[REGISTER_BYTE (L0_REGNUM)],
			      8 * REGISTER_RAW_SIZE (L0_REGNUM), 1);
	}
      else if (regno >= L0_REGNUM && regno <= I7_REGNUM)
	{
	  if (!register_valid[regno])
	    abort();
	  if (regno >= L0_REGNUM && regno <= L0_REGNUM + 7)
	    regoffset = REGISTER_BYTE (regno) - REGISTER_BYTE (L0_REGNUM)
	      + FRAME_SAVED_L0;
	  else
	    regoffset = REGISTER_BYTE (regno) - REGISTER_BYTE (I0_REGNUM)
	      + FRAME_SAVED_I0;
	  target_xfer_memory (sp + regoffset, &registers[REGISTER_BYTE (regno)],
			      REGISTER_RAW_SIZE (regno), 1);
	}
    }

  if (whatregs & WHATREGS_FLOAT)
    {
      struct fcontext fc;		/* fp regs */
      int retval;

/* We read fcontext first so that we can get good values for fq_t... */
      errno = 0;
      retval = ptrace (PTRACE_GETFPREGS, inferior_pid, (PTRACE_ARG3_TYPE) &fc,
		       0);
      if (errno)
	perror_with_name ("Sparc fetch_inferior_registers(ptrace)");
  
      memcpy (fc.f.fregs, &registers[REGISTER_BYTE (FP0_REGNUM)],
	      32 * REGISTER_RAW_SIZE (FP0_REGNUM));

      fc.fsr = read_register (FPS_REGNUM);

      errno = 0;
      retval = ptrace (PTRACE_SETFPREGS, inferior_pid, (PTRACE_ARG3_TYPE) &fc,
		       0);
      if (errno)
	perror_with_name ("Sparc fetch_inferior_registers(ptrace)");
      }
}
#endif

#ifndef SPARC

/* Return the offset relative to the start of the per-thread data to the
   saved context block.  */

static unsigned long
registers_addr(pid)
     int pid;
{
  CORE_ADDR stblock;
  int ecpoff = offsetof(st_t, ecp);
  CORE_ADDR ecp;

  errno = 0;
  stblock = (CORE_ADDR) ptrace (PTRACE_THREADUSER, pid, (PTRACE_ARG3_TYPE)0,
				0);
  if (errno)
    perror_with_name ("registers_addr(PTRACE_THREADUSER)");

  ecp = (CORE_ADDR) ptrace (PTRACE_PEEKTHREAD, pid, (PTRACE_ARG3_TYPE)ecpoff,
			    0);
  if (errno)
    perror_with_name ("registers_addr(PTRACE_PEEKTHREAD)");

  return ecp - stblock;
}

/* Fetch one or more registers from the inferior.  REGNO == -1 to get
   them all.  We actually fetch more than requested, when convenient,
   marking them as valid so we won't fetch them again.  */

void
fetch_inferior_registers (regno)
     int regno;
{
  int reglo, reghi;
  int i;
  unsigned long ecp;

  if (regno == -1)
    {
      reglo = 0;
      reghi = NUM_REGS - 1;
    }
  else
    reglo = reghi = regno;

  ecp = registers_addr (inferior_pid);

  for (regno = reglo; regno <= reghi && regmap[regno] != -1; regno++)
    {
      char buf[MAX_REGISTER_RAW_SIZE];
      int ptrace_fun = PTRACE_PEEKTHREAD;

#ifdef PTRACE_PEEKUSP
      ptrace_fun = regno == SP_REGNUM ? PTRACE_PEEKUSP : PTRACE_PEEKTHREAD;
#endif

      for (i = 0; i < REGISTER_RAW_SIZE (regno); i += sizeof (int))
	{
	  unsigned int reg;

	  errno = 0;
	  reg = ptrace (ptrace_fun, inferior_pid,
			(PTRACE_ARG3_TYPE) (ecp + regmap[regno] + i), 0);
	  if (errno)
	    perror_with_name ("fetch_inferior_registers(ptrace)");
  
	  *(int *)&buf[i] = reg;
	}
      supply_register (regno, buf);
    }
}

/* Store our register values back into the inferior.
   If REGNO is -1, do this for all registers.
   Otherwise, REGNO specifies which register (so we can save time).  */

void
store_inferior_registers (regno)
     int regno;
{
  int reglo, reghi;
  int i;
  unsigned long ecp;

  if (regno == -1)
    {
      reglo = 0;
      reghi = NUM_REGS - 1;
    }
  else
    reglo = reghi = regno;

  ecp = registers_addr (inferior_pid);

  for (regno = reglo; regno <= reghi && regmap[regno] != -1; regno++)
    {
      int ptrace_fun = PTRACE_POKEUSER;

#ifdef PTRACE_POKEUSP
      ptrace_fun = regno == SP_REGNUM ? PTRACE_POKEUSP : PTRACE_POKEUSER;
#endif

      for (i = 0; i < REGISTER_RAW_SIZE (regno); i += sizeof (int))
	{
	  unsigned int reg;

	  reg = *(unsigned int *)&registers[REGISTER_BYTE (regno) + i];

	  errno = 0;
	  ptrace (ptrace_fun, inferior_pid,
		  (PTRACE_ARG3_TYPE) (ecp + regmap[regno] + i), reg);
	  if (errno)
	    perror_with_name ("PTRACE_POKEUSER");
	}
    }
}
#endif /* ifndef SPARC */

/* Wait for child to do something.  Return pid of child, or -1 in case
   of error; store status through argument pointer OURSTATUS.  */

int
child_wait (pid, ourstatus)
     int pid;
     struct target_waitstatus *ourstatus;
{
  int save_errno;
  int thread;
  int status;

  while (1)
    {
      int sig;

      if (attach_flag)
	set_sigint_trap();	/* Causes SIGINT to be passed on to the
				   attached process. */
      pid = wait (&status);
#ifdef SPARC
/* Swap halves of status so that the rest of GDB can understand it */
      status = (status << 16) | ((unsigned)status >> 16);
#endif

      save_errno = errno;

      if (attach_flag)
	clear_sigint_trap();

      if (pid == -1)
	{
	  if (save_errno == EINTR)
	    continue;
	  fprintf_unfiltered (gdb_stderr, "Child process unexpectedly missing: %s.\n",
		   safe_strerror (save_errno));
	  /* Claim it exited with unknown signal.  */
	  ourstatus->kind = TARGET_WAITKIND_SIGNALLED;
	  ourstatus->value.sig = TARGET_SIGNAL_UNKNOWN;
	  return -1;
	}

      if (pid != PIDGET (inferior_pid))	/* Some other process?!? */
	continue;

/*      thread = WIFTID (status);*/
      thread = status >> 16;

      /* Initial thread value can only be acquired via wait, so we have to
	 resort to this hack.  */

      if (TIDGET (inferior_pid) == 0)
	{
	  inferior_pid = BUILDPID (inferior_pid, thread);
	  add_thread (inferior_pid);
	}

      pid = BUILDPID (pid, thread);

      store_waitstatus (ourstatus, status);

      return pid;
    }
}

/* Convert a Lynx process ID to a string.  Returns the string in a static
   buffer.  */

char *
lynx_pid_to_str (pid)
     int pid;
{
  static char buf[40];

  sprintf (buf, "process %d thread %d", PIDGET (pid), TIDGET (pid));

  return buf;
}

/* Extract the register values out of the core file and store
   them where `read_register' will find them.

   CORE_REG_SECT points to the register values themselves, read into memory.
   CORE_REG_SIZE is the size of that area.
   WHICH says which set of registers we are handling (0 = int, 2 = float
         on machines where they are discontiguous).
   REG_ADDR is the offset from u.u_ar0 to the register values relative to
            core_reg_sect.  This is used with old-fashioned core files to
	    locate the registers in a large upage-plus-stack ".reg" section.
	    Original upage address X is at location core_reg_sect+x+reg_addr.
 */

void
fetch_core_registers (core_reg_sect, core_reg_size, which, reg_addr)
     char *core_reg_sect;
     unsigned core_reg_size;
     int which;
     unsigned reg_addr;
{
  struct st_entry s;
  unsigned int regno;

  for (regno = 0; regno < NUM_REGS; regno++)
    supply_register (regno, core_reg_sect + offsetof (st_t, ec)
		     + regmap[regno]);
}
