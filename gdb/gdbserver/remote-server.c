/* Main code for remote server for GDB.
   Copyright (C) 1989, 1993 Free Software Foundation, Inc.

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
#include <setjmp.h>
#include <signal.h>

void read_inferior_memory ();
unsigned char mywait ();
void myresume();
void initialize ();
int create_inferior ();

extern char registers[];
int inferior_pid;
extern char **environ;

/* Descriptor for I/O to remote machine.  */
int remote_desc;
int kiodebug = 0;
int remote_debugging;

void remote_send ();
void putpkt ();
void getpkt ();
void remote_open ();
void write_ok ();
void write_enn ();
void convert_ascii_to_int ();
void convert_int_to_ascii ();
void prepare_resume_reply ();
void decode_m_packet ();
void decode_M_packet ();
jmp_buf toplevel;

main (argc, argv)
     int argc;
     char *argv[];
{
  char ch, status, own_buf[2000], mem_buf[2000];
  int i = 0;
  unsigned char signal;
  unsigned int mem_addr, len;

  if (setjmp(toplevel))
    {
      fprintf(stderr, "Exiting\n");
      exit(1);
    }

  if (argc < 3)
    error("Usage: gdbserver tty prog [args ...]");

  initialize ();
  remote_open (argv[1], 0);

  inferior_pid = create_inferior (argv[2], &argv[2]);
  fprintf (stderr, "Process %s created; pid = %d\n", argv[2], inferior_pid);

  signal = mywait (&status);	/* Wait till we are at 1st instr in prog */

  /* We are now stopped at the first instruction of the target process */

  setjmp(toplevel);
  do
    {
      getpkt (own_buf);
      i = 0;
      ch = own_buf[i++];
      switch (ch)
	{
	case '?':
	  prepare_resume_reply (own_buf, status, signal);
	  break;
	case 'g':
	  convert_int_to_ascii (registers, own_buf, REGISTER_BYTES);
	  break;
	case 'G':
	  convert_ascii_to_int (&own_buf[1], registers, REGISTER_BYTES);
	  store_inferior_registers (-1);
	    write_ok (own_buf);
	  break;
	case 'm':
	  decode_m_packet (&own_buf[1], &mem_addr, &len);
	  read_inferior_memory (mem_addr, mem_buf, len);
	  convert_int_to_ascii (mem_buf, own_buf, len);
	  break;
	case 'M':
	  decode_M_packet (&own_buf[1], &mem_addr, &len, mem_buf);
	  if (write_inferior_memory (mem_addr, mem_buf, len) == 0)
	    write_ok (own_buf);
	  else
	    write_enn (own_buf);
	  break;
	case 'c':
	  myresume (0, 0);
	  signal = mywait (&status);
	  prepare_resume_reply (own_buf, status, signal);
	  break;
	case 's':
	  myresume (1, 0);
	  signal = mywait (&status);
	  prepare_resume_reply (own_buf, status, signal);
	  break;
	case 'k':
	  kill_inferior ();
	  sprintf (own_buf, "q");
	  putpkt (own_buf);
	  fprintf (stderr, "Obtained kill request...terminating\n");
	  close (remote_desc);
	  exit (0);
	default:
	  printf ("\nUnknown option chosen by master\n");
	  write_enn (own_buf);
	  break;
	}

      putpkt (own_buf);
    }
  while (1);

  close (remote_desc);
  /** now get out of here**/
  fprintf (stderr, "Finished reading data from serial link - Bye!\n");
  exit (0);
}
