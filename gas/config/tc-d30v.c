/* tc-d30v.c -- Assembler code for the Mitsubishi D30V

   Copyright (C) 1997 Free Software Foundation.

   This file is part of GAS, the GNU Assembler.

   GAS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GAS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GAS; see the file COPYING.  If not, write to
   the Free Software Foundation, 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include <stdio.h>
#include <ctype.h>
#include "as.h"
#include "subsegs.h"     
#include "opcode/d30v.h"

const char comment_chars[] = ";";
const char line_comment_chars[] = "#";
const char line_separator_chars[] = "";
const char *md_shortopts = "O";
const char EXP_CHARS[] = "eE";
const char FLT_CHARS[] = "dD";

int Optimizing = 0;

/* fixups */
#define MAX_INSN_FIXUPS (5)
struct d30v_fixup
{
  expressionS exp;
  int operand;
  int pcrel;
  int size;
  bfd_reloc_code_real_type reloc;
};

typedef struct _fixups
{
  int fc;
  struct d30v_fixup fix[MAX_INSN_FIXUPS];
  struct _fixups *next;
} Fixups;

static Fixups FixUps[2];
static Fixups *fixups;

/* local functions */
static int reg_name_search PARAMS ((char *name));
static int register_name PARAMS ((expressionS *expressionP));
static int check_range PARAMS ((unsigned long num, int bits, int flags));
static int postfix PARAMS ((char *p));
static bfd_reloc_code_real_type get_reloc PARAMS ((struct d30v_operand *op, int rel_flag));
static int get_operands PARAMS ((expressionS exp[], int cmp_hack));
static struct d30v_format *find_format PARAMS ((struct d30v_opcode *opcode, expressionS ops[], 
						int cmp_hack));
static long long build_insn PARAMS ((struct d30v_insn *opcode, expressionS *opers));
static void write_long PARAMS ((struct d30v_insn *opcode, long long insn, Fixups *fx));
static void write_1_short PARAMS ((struct d30v_insn *opcode, long long insn, Fixups *fx));
static int write_2_short PARAMS ((struct d30v_insn *opcode1, long long insn1, 
				  struct d30v_insn *opcode2, long long insn2, int exec_type, Fixups *fx));
static long long do_assemble PARAMS ((char *str, struct d30v_insn *opcode));
static unsigned long d30v_insert_operand PARAMS (( unsigned long insn, int op_type,
						   offsetT value, int left, fixS *fix));
static int parallel_ok PARAMS ((struct d30v_insn *opcode1, unsigned long insn1, 
				struct d30v_insn *opcode2, unsigned long insn2,
				int exec_type));
static void d30v_number_to_chars PARAMS ((char *buf, long long value, int nbytes));

struct option md_longopts[] = {
  {NULL, no_argument, NULL, 0}
};
size_t md_longopts_size = sizeof(md_longopts);       


/* The target specific pseudo-ops which we support.  */
const pseudo_typeS md_pseudo_table[] =
{
  { NULL,       NULL,           0 }
};

/* Opcode hash table.  */
static struct hash_control *d30v_hash;

/* reg_name_search does a binary search of the pre_defined_registers
   array to see if "name" is a valid regiter name.  Returns the register
   number from the array on success, or -1 on failure. */

static int
reg_name_search (name)
     char *name;
{
  int middle, low, high;
  int cmp;

  low = 0;
  high = reg_name_cnt() - 1;

  do
    {
      middle = (low + high) / 2;
      cmp = strcasecmp (name, pre_defined_registers[middle].name);
      if (cmp < 0)
	high = middle - 1;
      else if (cmp > 0)
	low = middle + 1;
      else 
	  return pre_defined_registers[middle].value;
    }
  while (low <= high);
  return -1;
}

/* register_name() checks the string at input_line_pointer
   to see if it is a valid register name */

static int
register_name (expressionP)
     expressionS *expressionP;
{
  int reg_number;
  char c, *p = input_line_pointer;
  
  while (*p && *p!='\n' && *p!='\r' && *p !=',' && *p!=' ' && *p!=')')
    p++;

  c = *p;
  if (c)
    *p++ = 0;

  /* look to see if it's in the register table */
  reg_number = reg_name_search (input_line_pointer);
  if (reg_number >= 0) 
    {
      expressionP->X_op = O_register;
      /* temporarily store a pointer to the string here */
      expressionP->X_op_symbol = (struct symbol *)input_line_pointer;
      expressionP->X_add_number = reg_number;
      input_line_pointer = p;
      return 1;
    }
  if (c)
    *(p-1) = c;
  return 0;
}


static int
check_range (num, bits, flags)
     unsigned long num;
     int bits;
     int flags;
{
  long min, max, bit1;
  int retval=0;

  /* don't bother checking 32-bit values */
  if (bits == 32)
    return 0;

  if (flags & OPERAND_SIGNED)
    {
      max = (1 << (bits - 1))-1; 
      min = - (1 << (bits - 1));  
      if (((long)num > max) || ((long)num < min))
	retval = 1;
    }
  else
    {
      max = (1 << bits) - 1;
      min = 0;
      if ((num > max) || (num < min))
	retval = 1;
    }
  return retval;
}


void
md_show_usage (stream)
  FILE *stream;
{
  fprintf(stream, "D30V options:\n\
-O                      optimize.  Will do some operations in parallel.\n");
} 

int
md_parse_option (c, arg)
     int c;
     char *arg;
{
  switch (c)
    {
    case 'O':
      /* Optimize. Will attempt to parallelize operations */
      Optimizing = 1;
      break;
    default:
      return 0;
    }
  return 1;
}

symbolS *
md_undefined_symbol (name)
  char *name;
{
  return 0;
}

/* Turn a string in input_line_pointer into a floating point constant of type
   type, and store the appropriate bytes in *litP.  The number of LITTLENUMS
   emitted is stored in *sizeP .  An error message is returned, or NULL on OK.
 */
char *
md_atof (type, litP, sizeP)
     int type;
     char *litP;
     int *sizeP;
{
  int prec;
  LITTLENUM_TYPE words[4];
  char *t;
  int i;
  
  switch (type)
    {
    case 'f':
      prec = 2;
      break;
    case 'd':
      prec = 4;
      break;
    default:
      *sizeP = 0;
      return "bad call to md_atof";
    }

  t = atof_ieee (input_line_pointer, type, words);
  if (t)
    input_line_pointer = t;
  
  *sizeP = prec * 2;
  
  for (i = 0; i < prec; i++)
    {
      md_number_to_chars (litP, (valueT) words[i], 2);
	  litP += 2;
    }
  return NULL;
}

void
md_convert_frag (abfd, sec, fragP)
  bfd *abfd;
  asection *sec;
  fragS *fragP;
{
  abort ();
}

valueT
md_section_align (seg, addr)
     asection *seg;
     valueT addr;
{
  int align = bfd_get_section_alignment (stdoutput, seg);
  return ((addr + (1 << align) - 1) & (-1 << align));
}


void
md_begin ()
{
  struct d30v_opcode *opcode;
  d30v_hash = hash_new();

  /* Insert opcode names into a hash table. */
  for (opcode = (struct d30v_opcode *)d30v_opcode_table; opcode->name; opcode++)
      hash_insert (d30v_hash, opcode->name, (char *) opcode);

  fixups = &FixUps[0];
  FixUps[0].next = &FixUps[1];
  FixUps[1].next = &FixUps[0];
}


/* this function removes the postincrement or postdecrement
   operator ( '+' or '-' ) from an expression */

static int postfix (p) 
     char *p;
{
  while (*p != '-' && *p != '+') 
    {
      if (*p==0 || *p=='\n' || *p=='\r') 
	break;
      p++;
    }

  if (*p == '-') 
    {
      *p = ' ';
      return (-1);
    }
  if (*p == '+') 
    {
      *p = ' ';
      return (1);
    }

  return (0);
}


static bfd_reloc_code_real_type 
get_reloc (op, rel_flag) 
     struct d30v_operand *op;
     int rel_flag;
{
  switch (op->bits)
    {
    case 6:
      return BFD_RELOC_D30V_6;
    case 12:
      if (!(op->flags & OPERAND_SHIFT))
	as_warn("unexpected 12-bit reloc type");
      if (rel_flag == RELOC_PCREL)
	return BFD_RELOC_D30V_15_PCREL;
      else
	return BFD_RELOC_D30V_15;
    case 18:
      if (!(op->flags & OPERAND_SHIFT))
	as_warn("unexpected 18-bit reloc type");
      if (rel_flag == RELOC_PCREL)
	return BFD_RELOC_D30V_21_PCREL;
      else
	return BFD_RELOC_D30V_21;
    case 32:
      if (rel_flag == RELOC_PCREL)
	return BFD_RELOC_D30V_32_PCREL;
      else
	return BFD_RELOC_D30V_32;
    default:
      return 0;
    }
}

/* get_operands parses a string of operands and returns
   an array of expressions */

static int
get_operands (exp, cmp_hack) 
     expressionS exp[];
     int cmp_hack;
{
  char *p = input_line_pointer;
  int numops = 0;
  int post = 0;

  if (cmp_hack)
    {
      exp[numops].X_op = O_absent;
      exp[numops++].X_add_number = cmp_hack - 1;
    }

  while (*p)  
    {
      while (*p == ' ' || *p == '\t' || *p == ',') 
	p++;
      if (*p==0 || *p=='\n' || *p=='\r') 
	break;
      
      if (*p == '@') 
	{
	  p++;
	  exp[numops].X_op = O_absent;
	  if (*p == '(') 
	    {
	      p++;
	      exp[numops].X_add_number = OPERAND_ATPAR;
	      post = postfix (p);
	    }
	  else if (*p == '-') 
	    {
	      p++;
	      exp[numops].X_add_number = OPERAND_ATMINUS;
	    }
	  else
	    {
	      exp[numops].X_add_number = OPERAND_ATSIGN;
	      post = postfix (p);
	    }
	  numops++;
	  continue;
	}

      if (*p == ')') 
	{
	  /* just skip the trailing paren */
	  p++;
	  continue;
	}

      input_line_pointer = p;

      /* check to see if it might be a register name */
      if (!register_name (&exp[numops]))
	{
	  /* parse as an expression */
	  expression (&exp[numops]);
	}

      if (exp[numops].X_op == O_illegal) 
	as_bad ("illegal operand");
      else if (exp[numops].X_op == O_absent) 
	as_bad ("missing operand");

      numops++;
      p = input_line_pointer;

      switch (post) 
	{
	case -1:	/* postdecrement mode */
	  exp[numops].X_op = O_absent;
	  exp[numops++].X_add_number = OPERAND_MINUS;
	  break;
	case 1:	/* postincrement mode */
	  exp[numops].X_op = O_absent;
	  exp[numops++].X_add_number = OPERAND_PLUS;
	  break;
	}
      post = 0;
    }

  exp[numops].X_op = 0;
  return (numops);
}

/* build_insn generates the instruction.  It does everything */
/* but write the FM bits. */

static long long
build_insn (opcode, opers) 
     struct d30v_insn *opcode;
     expressionS *opers;
{
  int i, length, bits, shift, flags, format;
  unsigned int number, id=0;
  long long insn;
  struct d30v_opcode *op = opcode->op;
  struct d30v_format *form = opcode->form;

  /*  printf("ecc=%x op1=%x op2=%x mod=%x\n",opcode->ecc,op->op1,op->op2,form->modifier); */
  insn = opcode->ecc << 28 | op->op1 << 25 | op->op2 << 20 | form->modifier << 18;
  /*  printf("insn=%llx\n",insn); */
  for (i=0; form->operands[i]; i++) 
    { 
      flags = d30v_operand_table[form->operands[i]].flags;


      /* must be a register or number */
      if (!(flags & OPERAND_REG) && !(flags & OPERAND_NUM) && 
	  !(flags & OPERAND_NAME) && !(flags & OPERAND_SPECIAL))
	continue;

      bits = d30v_operand_table[form->operands[i]].bits;
      length = d30v_operand_table[form->operands[i]].length;
      shift = 12 - d30v_operand_table[form->operands[i]].position;
      number = opers[i].X_add_number;
      if (flags & OPERAND_REG)
	{
	  /* now check for mvfsys or mvtsys control registers */
	  if (flags & OPERAND_CONTROL && (number & 0x3f) > MAX_CONTROL_REG)
	    {
	      /* PSWL or PSWH */
	      id = (number & 0x3f) - MAX_CONTROL_REG;
	      number = 1;
	    }
	  else if (number & OPERAND_FLAG)
	    {
	      id = 3;  /* number is a flag register */
	    }
	  number &= 0x3F;
	}
      else if (flags & OPERAND_SPECIAL)
	{
	  number = id;
	}
      

      if (opers[i].X_op != O_register && opers[i].X_op != O_constant && !(flags & OPERAND_NAME))
	{
	  /* now create a fixup */

	  if (fixups->fc >= MAX_INSN_FIXUPS)
	    as_fatal ("too many fixups");

	  fixups->fix[fixups->fc].reloc = 
	    get_reloc((struct d30v_operand *)&d30v_operand_table[form->operands[i]], op->reloc_flag);
	  fixups->fix[fixups->fc].size = 4;
	  fixups->fix[fixups->fc].exp = opers[i];
	  fixups->fix[fixups->fc].operand = form->operands[i];
	  fixups->fix[fixups->fc].pcrel = op->reloc_flag;
	  (fixups->fc)++;
	}

      /* truncate to the proper number of bits */
      /*
	if ((opers[i].X_op == O_constant) && check_range (number, bits, flags))
	as_bad("operand out of range: %d",number);
	number &= 0x7FFFFFFF >> (31 - bits);
	*/
      
      if (bits == 32)
	{
	  /* it's a LONG instruction */
	  insn |= (number >> 26);	/* top 6 bits */
	  insn <<= 32;			/* shift the first word over */
	  insn |= ((number & 0x03FC0000) << 2);  /* next 8 bits */ 
	  insn |= number & 0x0003FFFF;		/* bottom 18 bits */
	}
      else
	insn |= number << shift;
    }
  return insn;
}


/* write out a long form instruction */
static void
write_long (opcode, insn, fx) 
     struct d30v_insn *opcode;
     long long insn;
     Fixups *fx;
{
  int i, where;
  char *f = frag_more(8);

  insn |= FM11;
  d30v_number_to_chars (f, insn, 8);

  for (i=0; i < fx->fc; i++) 
    {
      if (fx->fix[i].reloc)
	{ 
	  where = f - frag_now->fr_literal; 
	  fix_new_exp (frag_now,
		       where,
		       fx->fix[i].size,
		       &(fx->fix[i].exp),
		       fx->fix[i].pcrel,
		       fx->fix[i].reloc);
	}
    }
  fx->fc = 0;
}


/* write out a short form instruction by itself */
static void
write_1_short (opcode, insn, fx) 
     struct d30v_insn *opcode;
     long long insn;
     Fixups *fx;
{
  char *f = frag_more(8);
  int i, where;

  /* the other container needs to be NOP */
  /* according to 4.3.1: for FM=00, sub-instructions performed only
     by IU cannot be encoded in L-container. */
  if (opcode->op->unit == IU)
    insn |= FM00 | ((long long)NOP << 32);		/* right container */
  else
    insn = FM00 | (insn << 32) | (long long)NOP;	/* left container */

  d30v_number_to_chars (f, insn, 8);

  for (i=0; i < fx->fc; i++) 
    {
      if (fx->fix[i].reloc)
	{ 
	  where = f - frag_now->fr_literal; 
	  fix_new_exp (frag_now,
		       where, 
		       fx->fix[i].size,
		       &(fx->fix[i].exp),
		       fx->fix[i].pcrel,
		       fx->fix[i].reloc);
	}
    }
  fx->fc = 0;
}

/* write out a short form instruction if possible */
/* return number of instructions not written out */
static int
write_2_short (opcode1, insn1, opcode2, insn2, exec_type, fx) 
     struct d30v_insn *opcode1, *opcode2;
     long long insn1, insn2;
     int exec_type;
     Fixups *fx;
{
  long long insn;
  char *f;
  int i,j, where;

  if(exec_type != 1 && (opcode1->op->flags_used == FLAG_JSR))
    {
      /* subroutines must be called from 32-bit boundaries */
      /* so the return address will be correct */
      write_1_short (opcode1, insn1, fx->next);
      return (1);
    }

  switch (exec_type) 
    {
    case 0:	/* order not specified */
      if ( Optimizing && parallel_ok (opcode1, insn1, opcode2, insn2, exec_type))
	{
	  /* parallel */
	  if (opcode1->op->unit == IU)
	    insn = FM00 | (insn2 << 32) | insn1;
	  else if (opcode2->op->unit == MU)
	    insn = FM00 | (insn2 << 32) | insn1;
	  else
	    {
	      insn = FM00 | (insn1 << 32) | insn2;  
	      fx = fx->next;
	    }
	}
      else if (opcode1->op->unit == IU) 
	{
	  /* reverse sequential */
	  insn = FM10 | (insn2 << 32) | insn1;
	}
      else
	{
	  /* sequential */
	  insn = FM01 | (insn1 << 32) | insn2;
	  fx = fx->next;  
	}
      break;
    case 1:	/* parallel */
      if (opcode1->op->unit == IU)
	{
	  if (opcode2->op->unit == IU)
	    as_fatal ("Two IU instructions may not be executed in parallel");
	  as_warn ("Swapping instruction order");
 	  insn = FM00 | (insn2 << 32) | insn1;
	}
      else if (opcode2->op->unit == MU)
	{
	  if (opcode1->op->unit == MU)
	    as_fatal ("Two MU instructions may not be executed in parallel");
	  as_warn ("Swapping instruction order");
	  insn = FM00 | (insn2 << 32) | insn1;
	}
      else
	{
	  insn = FM00 | (insn1 << 32) | insn2;  
	  fx = fx->next;
	}
      break;
    case 2:	/* sequential */
      if (opcode1->op->unit == IU)
	as_fatal ("IU instruction may not be in the left container");
      insn = FM01 | (insn1 << 32) | insn2;  
      fx = fx->next;
      break;
    case 3:	/* reverse sequential */
      if (opcode2->op->unit == MU)
	as_fatal ("MU instruction may not be in the right container");
      insn = FM10 | (insn1 << 32) | insn2;  
      fx = fx->next;
      break;
    default:
      as_fatal("unknown execution type passed to write_2_short()");
    }

  /*  printf("writing out %llx\n",insn); */
  f = frag_more(8);
  d30v_number_to_chars (f, insn, 8);

  for (j=0; j<2; j++) 
    {
      for (i=0; i < fx->fc; i++) 
	{
	  if (fx->fix[i].reloc)
	    {
	      where = (f - frag_now->fr_literal) + 4*j;

	      fix_new_exp (frag_now,
			   where, 
			   fx->fix[i].size,
			   &(fx->fix[i].exp),
			   fx->fix[i].pcrel,
			   fx->fix[i].reloc);
	    }
	}
      fx->fc = 0;
      fx = fx->next;
    }
  return (0);
}


/* Check 2 instructions and determine if they can be safely */
/* executed in parallel.  Returns 1 if they can be.         */
static int
parallel_ok (op1, insn1, op2, insn2, exec_type)
     struct d30v_insn *op1, *op2;
     unsigned long insn1, insn2;
     int exec_type;
{
  int i, j, flags, mask, shift, regno, bits;
  unsigned long ins, mod_reg[2][3], used_reg[2][3];
  struct d30v_format *f;
  struct d30v_opcode *op;

  /* section 4.3: both instructions must not be IU or MU only */
  if ((op1->op->unit == IU && op2->op->unit == IU)
      || (op1->op->unit == MU && op2->op->unit == MU))
    return 0;

  /*
    [0] r0-r31
    [1] r32-r63
    [2] a0, a1
    */

  for (j = 0; j < 2; j++)
    {
      if (j == 0)
	{
	  f = op1->form;
	  op = op1->op;
	  ins = insn1;
	}
      else
	{
	  f = op2->form;
	  op = op2->op;
	  ins = insn2;
	}
      mod_reg[j][0] = mod_reg[j][1] = 0;
      mod_reg[j][2] = op->flags_set;
      used_reg[j][0] = used_reg[j][1] = 0;
      used_reg[j][2] = op->flags_used;
      for (i = 0; f->operands[i]; i++)
	{
	  flags = d30v_operand_table[f->operands[i]].flags;
	  shift = 12 - d30v_operand_table[f->operands[i]].position;
	  bits = d30v_operand_table[f->operands[i]].bits;
	  if (bits == 32)
	    mask = 0xffffffff;
	  else
	    mask = 0x7FFFFFFF >> (31 - bits);
	  if (flags & OPERAND_REG)
	    {
	      regno = (ins >> shift) & mask;
	      if (flags & OPERAND_DEST)
		{
		  if (flags & OPERAND_ACC)
		    mod_reg[j][2] = 1 << (regno+16);
		  else if (flags & OPERAND_FLAG)
		    mod_reg[j][2] = 1 << regno;
		  else if (!(flags & OPERAND_CONTROL))
		    {
		      if (regno >= 32)
			mod_reg[j][1] = 1 << (regno - 32);
		      else
			mod_reg[j][0] = 1 << regno;
		    }
		}
	      else
		{
		  if (flags & OPERAND_ACC)
		    used_reg[j][2] = 1 << (regno+16);
		  else if (flags & OPERAND_FLAG)
		    used_reg[j][2] = 1 << regno;
		  else if (!(flags & OPERAND_CONTROL))
		    {
		      if (regno >= 32)
			used_reg[j][1] = 1 << (regno - 32);
		      else
			used_reg[j][0] = 1 << regno;
		    }
		}
	    }
	}
    }

  for(j = 0; j < 3; j++)
    if ((mod_reg[0][j] & mod_reg[1][j])
	|| (mod_reg[0][j] & used_reg[1][j])
	|| (mod_reg[1][j] & used_reg[0][j]))
      return 0;
  
  return 1;
}



/* This is the main entry point for the machine-dependent assembler.  str points to a
   machine-dependent instruction.  This function is supposed to emit the frags/bytes 
   it assembles to.  For the D30V, it mostly handles the special VLIW parsing and packing
   and leaves the difficult stuff to do_assemble().
 */

static long long prev_insn = -1;
static struct d30v_insn prev_opcode;
static subsegT prev_subseg;
static segT prev_seg = 0;

void
md_assemble (str)
     char *str;
{
  struct d30v_insn opcode;
  long long insn;
  int extype=0;			/* execution type; parallel, etc */
  static int etype=0;		/* saved extype.  used for multiline instructions */
  char *str2;

  if (etype == 0)
    {
      /* look for the special multiple instruction separators */
      str2 = strstr (str, "||");
      if (str2) 
	extype = 1;
      else
	{
	  str2 = strstr (str, "->");
	  if (str2) 
	    extype = 2;
	  else
	    {
	      str2 = strstr (str, "<-");
	      if (str2) 
		extype = 3;
	    }
	}
      /* str2 points to the separator, if one */
      if (str2) 
	{
	  *str2 = 0;
	  
	  /* if two instructions are present and we already have one saved
	     then first write it out */
	  d30v_cleanup();
	  
	  /* assemble first instruction and save it */
	  prev_insn = do_assemble (str, &prev_opcode);
	  if (prev_insn == -1)
	    as_fatal ("can't find opcode ");
	  fixups = fixups->next;
	  str = str2 + 2;
	}
    }

  insn = do_assemble (str, &opcode);
  if (insn == -1)
    {
      if (extype)
	{
	  etype = extype;
	  return;
	}
      as_fatal ("can't find opcode ");
    }

  if (etype)
    {
      extype = etype;
      etype = 0;
    }

  /* if this is a long instruction, write it and any previous short instruction */
  if (opcode.form->form >= LONG) 
    {
      if (extype) 
	as_fatal("Unable to mix instructions as specified");
      d30v_cleanup();
      write_long (&opcode, insn, fixups);
      prev_insn = -1;
      return;
    }
  
  if ( (prev_insn != -1) && prev_seg && ((prev_seg != now_seg) || (prev_subseg != now_subseg)))
    d30v_cleanup();
  
  if ( (prev_insn != -1) && 
       (write_2_short (&prev_opcode, (long)prev_insn, &opcode, (long)insn, extype, fixups) == 0)) 
    {
      /* no instructions saved */
      prev_insn = -1;
    }
  else
    {
      if (extype) 
	as_fatal("Unable to mix instructions as specified");
      /* save off last instruction so it may be packed on next pass */
      memcpy( &prev_opcode, &opcode, sizeof(prev_opcode));
      prev_insn = insn;
      prev_seg = now_seg;
      prev_subseg = now_subseg;
      fixups = fixups->next;
    }
}


/* do_assemble assembles a single instruction and returns an opcode */
/* it returns -1 (an invalid opcode) on error */

static long long
do_assemble (str, opcode) 
     char *str;
     struct d30v_insn *opcode;
{
  unsigned char *op_start, *save;
  unsigned char *op_end;
  char name[20];
  int cmp_hack, nlen = 0;
  expressionS myops[6];
  long long insn;

  /* Drop leading whitespace */
  while (*str == ' ')
    str++;

  /* find the opcode end */
  for (op_start = op_end = (unsigned char *) (str);
       *op_end
       && nlen < 20
       && *op_end != '/'
       && !is_end_of_line[*op_end] && *op_end != ' ';
       op_end++)
    {
      name[nlen] = tolower(op_start[nlen]);
      nlen++;
    }

  if (nlen == 0)
    return (-1);

  name[nlen] = 0;

  /* if there is an execution condition code, handle it */
  if (*op_end == '/')
    {
      int i = 0;
      while ( (i < ECC_MAX) && strncasecmp(d30v_ecc_names[i],op_end+1,2))
	i++;
      
      if (i == ECC_MAX)
	{
	  char tmp[4];
	  strncpy(tmp,op_end+1,2);
	  tmp[2] = 0;
	  as_fatal ("unknown condition code: %s",tmp);
	  return -1;
	}
      /*      printf("condition code=%d\n",i); */
      opcode->ecc = i;
      op_end += 3;
    }
  else
    opcode->ecc = ECC_AL;
  

  /* CMP and CMPU change their name based on condition codes */
  if (!strncmp(name,"cmp",3))
    {
      int p,i;
      char **str = (char **)d30v_cc_names;
      if (name[3] == 'u')
	p = 4;
      else
	p = 3;

      for(i=1; *str && strncmp(*str,&name[p],2); i++, *str++)
	;

      if (!*str)
	{
	  name[p+2]=0;
	  as_fatal ("unknown condition code: %s",&name[p]);      
	}
      
      cmp_hack = i;
      name[p] = 0;
    }
  else
    cmp_hack = 0;
  
  /*  printf("cmp_hack=%d\n",cmp_hack); */

  /* find the first opcode with the proper name */  
  opcode->op = (struct d30v_opcode *)hash_find (d30v_hash, name);
  if (opcode->op == NULL)
      as_fatal ("unknown opcode: %s",name);

  save = input_line_pointer;
  input_line_pointer = op_end;
  while (!(opcode->form = find_format (opcode->op, myops, cmp_hack)))
    {
      opcode->op++;
      if (strcmp(opcode->op->name,name))
	return -1;
    }
  input_line_pointer = save;

  insn = build_insn (opcode, myops); 
  return (insn);
}


/* find_format() gets a pointer to an entry in the format table.       */
/* It must look at all formats for an opcode and use the operands */
/* to choose the correct one.  Returns NULL on error. */

static struct d30v_format *
find_format (opcode, myops, cmp_hack)
     struct d30v_opcode *opcode;
     expressionS myops[];
     int cmp_hack;
{
  int numops, match, index, i=0, j, k;
  struct d30v_format *fm;
  struct d30v_operand *op;

  /* get all the operands and save them as expressions */
  numops = get_operands (myops, cmp_hack);

  while (index = opcode->format[i++])
    {
      fm = (struct d30v_format *)&d30v_format_table[index];
      k = index;
      while (fm->form == index)
	{
	  match = 1;
	  /* now check the operands for compatibility */
	  for (j = 0; match && fm->operands[j]; j++)
	    {
	      int flags = d30v_operand_table[fm->operands[j]].flags;
	      int X_op = myops[j].X_op;
	      int num = myops[j].X_add_number;
	      
	      if ( flags & OPERAND_SPECIAL )
		break;
	      else if (X_op == 0)
		match = 0;
	      else if (flags & OPERAND_REG)
		{
		  if ((X_op != O_register) ||
		      ((flags & OPERAND_ACC) && !(num & OPERAND_ACC)) ||
		      ((flags & OPERAND_FLAG) && !(num & OPERAND_FLAG)) ||
		      (flags & OPERAND_CONTROL && !(num & OPERAND_CONTROL | num & OPERAND_FLAG)))
		    {
		      match = 0;
		    }
		}
	      else 
		if (((flags & OPERAND_MINUS) && ((X_op != O_absent) || (num != OPERAND_MINUS))) ||
		    ((flags & OPERAND_PLUS) && ((X_op != O_absent) || (num != OPERAND_PLUS))) ||
		    ((flags & OPERAND_ATMINUS) && ((X_op != O_absent) || (num != OPERAND_ATMINUS))) ||
		    ((flags & OPERAND_ATPAR) && ((X_op != O_absent) || (num != OPERAND_ATPAR))) ||
		    ((flags & OPERAND_ATSIGN) && ((X_op != O_absent) || (num != OPERAND_ATSIGN)))) 
		  {
		    match=0;
		  }
		else if (flags & OPERAND_NUM)
		  {
		    /* a number can be a constant or symbol expression */
		    if (fm->form >= LONG)
		      {
			/* If we're testing for a LONG format, either fits */
			if (X_op != O_constant && X_op != O_symbol)
			  match = 0;
		      }
		    /* This is the tricky part.  Will the constant or symbol */
		    /* fit into the space in the current format? */
		    else if (X_op == O_constant)
		      {
			if (check_range (num, d30v_operand_table[fm->operands[j]].bits, flags))
			  match = 0;
		      }
		    else if (X_op == O_symbol && S_IS_DEFINED(myops[j].X_add_symbol) &&
			     (S_GET_SEGMENT(myops[j].X_add_symbol) == now_seg))
		      {
			/* if the symbol is defined, see if the value will fit */
			/* into the form we're considering */
			fragS *f;
			long value;
			/* calculate the current address by running through the previous frags */
			/* and adding our current offset */
			for (value = 0, f = frchain_now->frch_root; f; f = f->fr_next)
			  value += f->fr_fix + f->fr_offset;
			if (opcode->reloc_flag == RELOC_PCREL)
			  value = S_GET_VALUE(myops[j].X_add_symbol) - value -
			    (obstack_next_free(&frchain_now->frch_obstack) - frag_now->fr_literal);
			else
			  value = S_GET_VALUE(myops[j].X_add_symbol);		    
			if (check_range (value, d30v_operand_table[fm->operands[j]].bits, flags)) 
			  match = 0;
		      }
		    else
		      match = 0;
		  }
	    }
	  /* printf("through the loop: match=%d\n",match);  */
	  /* we're only done if the operands matched so far AND there
	     are no more to check */
	  if (match && myops[j].X_op==0) 
	    return fm;
	  match = 0;
	  fm = (struct d30v_format *)&d30v_format_table[++k];
	}
      /* printf("trying another format: i=%d\n",i); */
    }
  return NULL;
}

/* if while processing a fixup, a reloc really needs to be created */
/* then it is done here */
                 
arelent *
tc_gen_reloc (seg, fixp)
     asection *seg;
     fixS *fixp;
{
  arelent *reloc;
  reloc = (arelent *) bfd_alloc_by_size_t (stdoutput, sizeof (arelent));
  reloc->sym_ptr_ptr = &fixp->fx_addsy->bsym;
  reloc->address = fixp->fx_frag->fr_address + fixp->fx_where;
  reloc->howto = bfd_reloc_type_lookup (stdoutput, fixp->fx_r_type);
  if (reloc->howto == (reloc_howto_type *) NULL)
    {
      as_bad_where (fixp->fx_file, fixp->fx_line,
                    "reloc %d not supported by object file format", (int)fixp->fx_r_type);
      return NULL;
    }
  reloc->addend = fixp->fx_addnumber;
  return reloc;
}

int
md_estimate_size_before_relax (fragp, seg)
     fragS *fragp;
     asection *seg;
{
  abort ();
  return 0;
} 

long
md_pcrel_from_section (fixp, sec)
     fixS *fixp;
     segT sec;
{
  if (fixp->fx_addsy != (symbolS *)NULL && !S_IS_DEFINED (fixp->fx_addsy))
    return 0;
  return fixp->fx_frag->fr_address + fixp->fx_where;
}

int
md_apply_fix3 (fixp, valuep, seg)
     fixS *fixp;
     valueT *valuep;
     segT seg;
{
  char *where;
  unsigned long insn, insn2;
  long value;
  int op_type;
  int left=0;

  if (fixp->fx_addsy == (symbolS *) NULL)
    {
      value = *valuep;
      fixp->fx_done = 1;
    }
  else if (!S_IS_DEFINED(fixp->fx_addsy))
    return 0;
  else if (fixp->fx_pcrel)
    {
      value = *valuep;
    } 
  else
    {
      value = fixp->fx_offset;
      if (fixp->fx_subsy != (symbolS *) NULL)
	{
	  if (S_GET_SEGMENT (fixp->fx_subsy) == absolute_section)
	    value -= S_GET_VALUE (fixp->fx_subsy);
	  else
	    {
	      /* We don't actually support subtracting a symbol.  */
	      as_bad_where (fixp->fx_file, fixp->fx_line,
			    "expression too complex");
	    }
	}
    }
  
  /* Fetch the instruction, insert the fully resolved operand
     value, and stuff the instruction back again.  */
  where = fixp->fx_frag->fr_literal + fixp->fx_where;
  insn = bfd_getb32 ((unsigned char *) where);
  
  switch (fixp->fx_r_type)
    {
    case BFD_RELOC_D30V_6:
      insn |= value & 0x3F;
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      break;
    case BFD_RELOC_D30V_15:
      insn |= (value >> 3) & 0xFFF;
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      break;
    case BFD_RELOC_D30V_15_PCREL:
      if ((long)fixp->fx_where & 0x7)
	value += 4;
      insn |= (value >> 3) & 0xFFF;
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      break;
    case BFD_RELOC_D30V_21:
      insn |= (value >> 3) & 0x3FFFF;
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      break;
    case BFD_RELOC_D30V_21_PCREL:
      if ((long)fixp->fx_where & 0x7)
	value += 4;
      insn |= (value >> 3) & 0x3FFFF;
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      break;
    case BFD_RELOC_D30V_32:
      insn2 = bfd_getb32 ((unsigned char *) where + 4);
      insn |= (value >> 26) & 0x3F;	/* top 6 bits */
      insn2 |= ((value & 0x03FC0000) << 2);  /* next 8 bits */ 
      insn2 |= value & 0x0003FFFF;		/* bottom 18 bits */
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      bfd_putb32 ((bfd_vma) insn2, (unsigned char *) where + 4);
      break;
    case BFD_RELOC_D30V_32_PCREL:
      if ((long)fixp->fx_where & 0x7)
	value += 4;
      insn2 = bfd_getb32 ((unsigned char *) where + 4);
      insn |= (value >> 26) & 0x3F;	/* top 6 bits */
      insn2 |= ((value & 0x03FC0000) << 2);  /* next 8 bits */ 
      insn2 |= value & 0x0003FFFF;		/* bottom 18 bits */
      bfd_putb32 ((bfd_vma) insn, (unsigned char *) where);
      bfd_putb32 ((bfd_vma) insn2, (unsigned char *) where + 4);
      break;
    case BFD_RELOC_32:
      bfd_putb32 ((bfd_vma) value, (unsigned char *) where);
      break;
    default:
      as_fatal ("line %d: unknown relocation type: 0x%x",fixp->fx_line,fixp->fx_r_type);
    }
  fixp->fx_done = 1; 
  return 0;
}


/* d30v_cleanup() is called after the assembler has finished parsing the input 
   file or after a label is defined.  Because the D30V assembler sometimes saves short 
   instructions to see if it can package them with the next instruction, there may
   be a short instruction that still needs written.  */
int
d30v_cleanup ()
{
  segT seg;
  subsegT subseg;

  if (prev_insn != -1)
    {
      seg = now_seg;
      subseg = now_subseg;
      subseg_set (prev_seg, prev_subseg);
      write_1_short (&prev_opcode, (long)prev_insn, fixups->next);
      subseg_set (seg, subseg);
      prev_insn = -1;
    }
  return 1;
}


static void                      
d30v_number_to_chars (buf, value, n)
     char *buf;			/* Return 'nbytes' of chars here. */
     long long value;		/* The value of the bits. */
     int n;			/* Number of bytes in the output. */
{
  while (n--)
    {
      buf[n] = value & 0xff;
      value >>= 8;
    }
}
