/* Simulator option handling.
   Copyright (C) 1996, 1997 Free Software Foundation, Inc.
   Contributed by Cygnus Support.

This file is part of GDB, the GNU debugger.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "sim-main.h"
#ifdef HAVE_STRING_H
#include <string.h>
#else
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <ctype.h>
#include "libiberty.h"
#include "../libiberty/alloca-conf.h"
#include "sim-options.h"
#include "sim-io.h"
#include "sim-assert.h"

#include "bfd.h"

/* Add a set of options to the simulator.
   TABLE is an array of OPTIONS terminated by a NULL `opt.name' entry.
   This is intended to be called by modules in their `install' handler.  */

SIM_RC
sim_add_option_table (sd, table)
     SIM_DESC sd;
     const OPTION *table;
{
  struct option_list *ol = ((struct option_list *)
			    xmalloc (sizeof (struct option_list)));

  /* Note: The list is constructed in the reverse order we're called so
     later calls will override earlier ones (in case that ever happens).
     This is the intended behaviour.  */
  ol->next = STATE_OPTIONS (sd);
  ol->options = table;
  STATE_OPTIONS (sd) = ol;

  return SIM_RC_OK;
}

/* Standard option table.
   Modules may specify additional ones.
   The caller of sim_parse_args may also specify additional options
   by calling sim_add_option_table first.  */

static DECLARE_OPTION_HANDLER (standard_option_handler);

/* FIXME: We shouldn't print in --help output options that aren't usable.
   Some fine tuning will be necessary.  One can either move less general
   options to another table or use a HAVE_FOO macro to ifdef out unavailable
   options.  */

/* ??? One might want to conditionally compile out the entries that
   aren't enabled.  There's a distinction, however, between options a
   simulator can't support and options that haven't been configured in.
   Certainly options a simulator can't support shouldn't appear in the
   output of --help.  Whether the same thing applies to options that haven't
   been configured in or not isn't something I can get worked up over.
   [Note that conditionally compiling them out might simply involve moving
   the option to another table.]
   If you decide to conditionally compile them out as well, delete this
   comment and add a comment saying that that is the rule.  */

#define OPTION_DEBUG_INSN	(OPTION_START + 0)
#define OPTION_DEBUG_FILE	(OPTION_START + 1)
#define OPTION_DO_COMMAND	(OPTION_START + 2)
#define OPTION_ARCHITECTURE     (OPTION_START + 3)
#define OPTION_TARGET           (OPTION_START + 4)

static const OPTION standard_options[] =
{
  { {"verbose", no_argument, NULL, 'v'},
      'v', NULL, "Verbose output",
      standard_option_handler },

#if defined (SIM_HAVE_BIENDIAN) /* ??? && WITH_TARGET_BYTE_ORDER == 0 */
  { {"endian", required_argument, NULL, 'E'},
      'E', "big|little", "Set endianness",
      standard_option_handler },
#endif

  { {"debug", no_argument, NULL, 'D'},
      'D', NULL, "Print debugging messages",
      standard_option_handler },
  { {"debug-insn", no_argument, NULL, OPTION_DEBUG_INSN},
      '\0', NULL, "Print instruction debugging messages",
      standard_option_handler },
  { {"debug-file", required_argument, NULL, OPTION_DEBUG_FILE},
      '\0', "FILE NAME", "Specify debugging output file",
      standard_option_handler },

#ifdef SIM_H8300 /* FIXME: Should be movable to h8300 dir.  */
  { {"h8300h", no_argument, NULL, 'h'},
      'h', NULL, "Indicate the CPU is h8/300h or h8/300s",
      standard_option_handler },
#endif

#ifdef SIM_HAVE_FLATMEM
  { {"mem-size", required_argument, NULL, 'm'},
      'm', "MEMORY SIZE", "Specify memory size",
      standard_option_handler },
#endif

  { {"do-command", required_argument, NULL, OPTION_DO_COMMAND},
      '\0', "COMMAND", ""/*undocumented*/,
      standard_option_handler },

  { {"help", no_argument, NULL, 'H'},
      'H', NULL, "Print help information",
      standard_option_handler },

  { {"architecture", required_argument, NULL, OPTION_ARCHITECTURE},
      '\0', "MACHINE", "Specify the architecture to use",
      standard_option_handler },

  { {"target", required_argument, NULL, OPTION_TARGET},
      '\0', "BFDNAME", "Specify the object-code format for the object files",
      standard_option_handler },

  { {NULL, no_argument, NULL, 0}, '\0', NULL, NULL, NULL }
};

static SIM_RC
standard_option_handler (sd, opt, arg, is_command)
     SIM_DESC sd;
     int opt;
     char *arg;
     int is_command;
{
  int i,n;

  switch (opt)
    {
    case 'v' :
      STATE_VERBOSE_P (sd) = 1;
      break;

#ifdef SIM_HAVE_BIENDIAN
    case 'E' :
      if (strcmp (arg, "big") == 0)
	{
	  if (WITH_TARGET_BYTE_ORDER == LITTLE_ENDIAN)
	    {
	      sim_io_eprintf (sd, "Simulator compiled for little endian only.\n");
	      return SIM_RC_FAIL;
	    }
	  /* FIXME:wip: Need to set something in STATE_CONFIG.  */
	  current_target_byte_order = BIG_ENDIAN;
	}
      else if (strcmp (arg, "little") == 0)
	{
	  if (WITH_TARGET_BYTE_ORDER == BIG_ENDIAN)
	    {
	      sim_io_eprintf (sd, "Simulator compiled for big endian only.\n");
	      return SIM_RC_FAIL;
	    }
	  /* FIXME:wip: Need to set something in STATE_CONFIG.  */
	  current_target_byte_order = LITTLE_ENDIAN;
	}
      else
	{
	  sim_io_eprintf (sd, "Invalid endian specification `%s'\n", arg);
	  return SIM_RC_FAIL;
	}
      break;
#endif

    case 'D' :
      if (! WITH_DEBUG)
	sim_io_eprintf (sd, "Debugging not compiled in, `-D' ignored\n");
      else
	{
	  for (n = 0; n < MAX_NR_PROCESSORS; ++n)
	    for (i = 0; i < MAX_DEBUG_VALUES; ++i)
	      CPU_DEBUG_FLAGS (STATE_CPU (sd, n))[i] = 1;
	}
      break;

    case OPTION_DEBUG_INSN :
      if (! WITH_DEBUG)
	sim_io_eprintf (sd, "Debugging not compiled in, `--debug-insn' ignored\n");
      else
	{
	  for (n = 0; n < MAX_NR_PROCESSORS; ++n)
	    CPU_DEBUG_FLAGS (STATE_CPU (sd, n))[DEBUG_INSN_IDX] = 1;
	}
      break;

    case OPTION_DEBUG_FILE :
      if (! WITH_DEBUG)
	sim_io_eprintf (sd, "Debugging not compiled in, `--debug-file' ignored\n");
      else
	{
	  FILE *f = fopen (arg, "w");

	  if (f == NULL)
	    {
	      sim_io_eprintf (sd, "Unable to open debug output file `%s'\n", arg);
	      return SIM_RC_FAIL;
	    }
	  for (n = 0; n < MAX_NR_PROCESSORS; ++n)
	    CPU_DEBUG_FILE (STATE_CPU (sd, n)) = f;
	}
      break;

#ifdef SIM_H8300 /* FIXME: Can be moved to h8300 dir.  */
    case 'h' :
      set_h8300h (1);
      break;
#endif

#ifdef SIM_HAVE_FLATMEM
    case 'm':
      {
	unsigned long ul = strtol (arg, NULL, 0);
	/* 16384: some minimal amount */
	if (! isdigit (arg[0]) || ul < 16384)
	  {
	    sim_io_eprintf (sd, "Invalid memory size `%s'", arg);
	    return SIM_RC_FAIL;
	  }
	STATE_MEM_SIZE (sd) = ul;
      }
      break;
#endif

    case OPTION_DO_COMMAND:
      sim_do_command (sd, arg);
      break;

    case OPTION_ARCHITECTURE:
      {
	const struct bfd_arch_info *ap = bfd_scan_arch (arg);
	if (ap == NULL)
	  {
	    sim_io_eprintf (sd, "Architecture `%s' unknown\n", arg);
	    return SIM_RC_FAIL;
	  }
	STATE_ARCHITECTURE (sd) = ap;
	break;
      }

    case OPTION_TARGET:
      {
	STATE_TARGET (sd) = xstrdup (arg);
	break;
      }

    case 'H':
      sim_print_help (sd, is_command);
      if (STATE_OPEN_KIND (sd) == SIM_OPEN_STANDALONE)
	exit (0);
      /* FIXME: 'twould be nice to do something similar if gdb.  */
      break;
    }

  return SIM_RC_OK;
}

/* Add the standard option list to the simulator.  */

SIM_RC
standard_install (SIM_DESC sd)
{
  SIM_ASSERT (STATE_MAGIC (sd) == SIM_MAGIC_NUMBER);
  if (sim_add_option_table (sd, standard_options) != SIM_RC_OK)
    return SIM_RC_FAIL;
  return SIM_RC_OK;
}

/* Return non-zero if arg is a duplicate argument.
   If ARG is NULL, initialize.  */

#define ARG_HASH_SIZE 97
#define ARG_HASH(a) ((256 * (unsigned char) a[0] + (unsigned char) a[1]) % ARG_HASH_SIZE)

static int
dup_arg_p (arg)
     char *arg;
{
  int hash;
  static char **arg_table = NULL;

  if (arg == NULL)
    {
      if (arg_table == NULL)
	arg_table = (char **) xmalloc (ARG_HASH_SIZE * sizeof (char *));
      memset (arg_table, 0, ARG_HASH_SIZE * sizeof (char *));
      return 0;
    }

  hash = ARG_HASH (arg);
  while (arg_table[hash] != NULL)
    {
      if (strcmp (arg, arg_table[hash]) == 0)
	return 1;
      /* We assume there won't be more than ARG_HASH_SIZE arguments so we
	 don't check if the table is full.  */
      if (++hash == ARG_HASH_SIZE)
	hash = 0;
    }
  arg_table[hash] = arg;
  return 0;
}
     
/* Called by sim_open to parse the arguments.  */

SIM_RC
sim_parse_args (sd, argv)
     SIM_DESC sd;
     char **argv;
{
  int i, argc, num_opts;
  char *p, *short_options;
  /* The `val' option struct entry is dynamically assigned for options that
     only come in the long form.  ORIG_VAL is used to get the original value
     back.  */
  unsigned char *orig_val;
  struct option *lp, *long_options;
  const struct option_list *ol;
  const OPTION *opt;
  OPTION_HANDLER **handlers;

  /* Count the number of arguments.  */
  for (argc = 0; argv[argc] != NULL; ++argc)
    continue;

  /* Count the number of options.  */
  num_opts = 0;
  for (ol = STATE_OPTIONS (sd); ol != NULL; ol = ol->next)
    for (opt = ol->options; opt->opt.name != NULL; ++opt)
      ++num_opts;

  /* Initialize duplicate argument checker.  */
  (void) dup_arg_p (NULL);

  /* Build the option table for getopt.  */
  long_options = (struct option *) alloca ((num_opts + 1) * sizeof (struct option));
  lp = long_options;
  short_options = (char *) alloca (num_opts * 3 + 1);
  p = short_options;
#if 0 /* ??? necessary anymore? */
  /* Set '+' as first char so argument permutation isn't done.  This is done
     to workaround a problem with invoking getopt_long in run.c.: optind gets
     decremented when the program name is reached.  */
  *p++ = '+';
#endif
  handlers = (OPTION_HANDLER **) alloca (256 * sizeof (OPTION_HANDLER *));
  memset (handlers, 0, 256 * sizeof (OPTION_HANDLER *));
  orig_val = (unsigned char *) alloca (256);
  for (i = OPTION_START, ol = STATE_OPTIONS (sd); ol != NULL; ol = ol->next)
    for (opt = ol->options; opt->opt.name != NULL; ++opt)
      {
	if (dup_arg_p (opt->opt.name))
	  continue;
	if (opt->shortopt != 0)
	  {
	    *p++ = opt->shortopt;
	    if (opt->opt.has_arg == required_argument)
	      *p++ = ':';
	    else if (opt->opt.has_arg == optional_argument)
	      { *p++ = ':'; *p++ = ':'; }
	  }
	*lp = opt->opt;
	/* Dynamically assign `val' numbers for long options that don't have
	   a short option equivalent.  */
	if (OPTION_LONG_ONLY_P (opt->opt.val))
	  lp->val = i++;
	handlers[(unsigned char) lp->val] = opt->handler;
	orig_val[(unsigned char) lp->val] = opt->opt.val;
	++lp;
      }
  *p = 0;
  lp->name = NULL;

  /* Ensure getopt is initialized.  */
  optind = 0;
  while (1)
    {
      int longind, optc;

      optc = getopt_long (argc, argv, short_options, long_options, &longind);
      if (optc == -1)
	{
	  if (STATE_OPEN_KIND (sd) == SIM_OPEN_STANDALONE)
	    STATE_PROG_ARGV (sd) = sim_copy_argv (argv + optind);
	  break;
	}
      if (optc == '?')
	return SIM_RC_FAIL;

      if ((*handlers[optc]) (sd, orig_val[optc], optarg, 0/*!is_command*/) == SIM_RC_FAIL)
	return SIM_RC_FAIL;
    }

  return SIM_RC_OK;
}

/* Print help messages for the options.  */

void
sim_print_help (sd, is_command)
     SIM_DESC sd;
     int is_command;
{
  const struct option_list *ol;
  const OPTION *opt;

  if (STATE_OPEN_KIND (sd) == SIM_OPEN_STANDALONE)
    sim_io_printf (sd, "Usage: %s [options] program [program args]\n",
		   STATE_MY_NAME (sd));

  /* Initialize duplicate argument checker.  */
  (void) dup_arg_p (NULL);

  if (STATE_OPEN_KIND (sd) == SIM_OPEN_STANDALONE)
    sim_io_printf (sd, "Options:\n");
  else
    sim_io_printf (sd, "Commands:\n");

  for (ol = STATE_OPTIONS (sd); ol != NULL; ol = ol->next)
    for (opt = ol->options; opt->opt.name != NULL; ++opt)
      {
	int comma, len;
	const OPTION *o;

	if (dup_arg_p (opt->opt.name))
	  continue;

	if (opt->doc == NULL)
	  continue;

	if (opt->doc_name != NULL && opt->doc_name [0] == '\0')
	  continue;

	sim_io_printf (sd, "  ");

	comma = 0;
	len = 2;

	if (!is_command)
	  {
	    o = opt;
	    do
	      {
		if (o->shortopt != '\0')
		  {
		    sim_io_printf (sd, "%s-%c", comma ? ", " : "", o->shortopt);
		    len += (comma ? 2 : 0) + 2;
		    if (o->arg != NULL)
		      {
			if (o->opt.has_arg == optional_argument)
			  {
			    sim_io_printf (sd, "[%s]", o->arg);
			    len += 1 + strlen (o->arg) + 1;
			  }
			else
			  {
			    sim_io_printf (sd, " %s", o->arg);
			    len += 1 + strlen (o->arg);
			  }
		      }
		    comma = 1;
		  }
		++o;
	      }
	    while (o->opt.name != NULL && o->doc == NULL);
	  }
	
	o = opt;
	do
	  {
	    const char *name;
	    if (o->doc_name != NULL)
	      name = o->doc_name;
	    else
	      name = o->opt.name;
	    if (name != NULL)
	      {
		sim_io_printf (sd, "%s%s%s",
			       comma ? ", " : "",
			       is_command ? "" : "--",
			       name);
		len += ((comma ? 2 : 0)
			+ (is_command ? 0 : 2)
			+ strlen (name));
		if (o->arg != NULL)
		  {
		    if (o->opt.has_arg == optional_argument)
		      {
			sim_io_printf (sd, " [%s]", o->arg);
			len += 2 + strlen (o->arg) + 1;
		      }
		    else
		      {
			sim_io_printf (sd, " %s", o->arg);
			len += 1 + strlen (o->arg);
		      }
		  }
		comma = 1;
	      }
	    ++o;
	  }
	while (o->opt.name != NULL && o->doc == NULL);

	if (len >= 30)
	  {
	    sim_io_printf (sd, "\n");
	    len = 0;
	  }

	for (; len < 30; len++)
	  sim_io_printf (sd, " ");

	sim_io_printf (sd, "%s\n", opt->doc);
      }

  sim_io_printf (sd, "\n");
  sim_io_printf (sd, "Note: Depending on the simulator configuration some %ss\n",
		 STATE_OPEN_KIND (sd) == SIM_OPEN_STANDALONE ? "option" : "command");
  sim_io_printf (sd, "      may not be applicable\n");

  if (STATE_OPEN_KIND (sd) == SIM_OPEN_STANDALONE)
    {
      sim_io_printf (sd, "\n");
      sim_io_printf (sd, "program args    Arguments to pass to simulated program.\n");
      sim_io_printf (sd, "                Note: Very few simulators support this.\n");
    }
}




SIM_RC
sim_args_command (sd, cmd)
     SIM_DESC sd;
     char *cmd;
{
  /* something to do? */
  if (cmd == NULL)
    return SIM_RC_OK; /* FIXME - perhaphs help would be better */
  
  if (cmd [0] == '-')
    {
      /* user specified -<opt> ... form? */
      char **argv = buildargv (cmd);
      SIM_RC rc = sim_parse_args (sd, argv);
      freeargv (argv);
      return rc;
    }
  else
    {
      /* user specified <opt> form? */
      const struct option_list *ol;
      const OPTION *opt;
      char **argv = buildargv (cmd);
      /* most recent option match */
      const OPTION *matching_opt = NULL;
      int matching_argi = -1;
      if (argv [0] != NULL)
	for (ol = STATE_OPTIONS (sd); ol != NULL; ol = ol->next)
	  for (opt = ol->options; opt->opt.name != NULL; ++opt)
	    {
	      int argi = 0;
	      const char *name = opt->opt.name;
	      while (strncmp (name, argv [argi], strlen (argv [argi])) == 0)
		{
		  name = &name [strlen (argv[argi])];
		  if (name [0] == '-')
		    {
		      /* leading match ...<a-b-c>-d-e-f - continue search */
		      name ++; /* skip `-' */
		      argi ++;
		      continue;
		    }
		  else if (name [0] == '\0')
		    {
		      /* exact match ...<a-b-c-d-e-f> - better than before? */
		      if (argi > matching_argi)
			{
			  matching_argi = argi;
			  matching_opt = opt;
			}
		      break;
		    }
		  else
		    break;
		}
	    }
      if (matching_opt != NULL)
	{
	  switch (matching_opt->opt.has_arg)
	    {
	    case no_argument:
	      if (argv [matching_argi + 1] == NULL)
		matching_opt->handler (sd, matching_opt->opt.val,
				       NULL, 1/*is_command*/);
	      else
		sim_io_eprintf (sd, "Command `%s' takes no arguments\n",
				matching_opt->opt.name);
	      break;
	    case optional_argument:
	      if (argv [matching_argi + 1] == NULL)
		matching_opt->handler (sd, matching_opt->opt.val,
				       NULL, 1/*is_command*/);
	      else if (argv [matching_argi + 2] == NULL)
		matching_opt->handler (sd, matching_opt->opt.val,
				       argv [matching_argi + 1], 1/*is_command*/);
	      else
		sim_io_eprintf (sd, "Command `%s' requires no more than one argument\n",
				matching_opt->opt.name);
	      break;
	    case required_argument:
	      if (argv [matching_argi + 1] == NULL)
		sim_io_eprintf (sd, "Command `%s' requires an argument\n",
				matching_opt->opt.name);
	      else if (argv [matching_argi + 2] == NULL)
		matching_opt->handler (sd, matching_opt->opt.val,
				       argv [matching_argi + 1], 1/*is_command*/);
	      else
		sim_io_eprintf (sd, "Command `%s' requires only one argument\n",
				matching_opt->opt.name);
	    }
	  return SIM_RC_OK;
	}
    }
      
  /* didn't find anything that remotly matched */
  return SIM_RC_FAIL;
}
