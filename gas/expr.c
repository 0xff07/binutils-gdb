/* expr.c -operands, expressions-
   Copyright (C) 1987, 1990, 1991, 1992 Free Software Foundation, Inc.

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
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. */

/*
 * This is really a branch office of as-read.c. I split it out to clearly
 * distinguish the world of expressions from the world of statements.
 * (It also gives smaller files to re-compile.)
 * Here, "operand"s are of expressions, not instructions.
 */

#include <ctype.h>
#include <string.h>

#include "as.h"

#include "obstack.h"

#if __STDC__ == 1
static void clean_up_expression (expressionS * expressionP);
#else /* __STDC__ */
static void clean_up_expression ();	/* Internal. */
#endif /* not __STDC__ */
extern const char EXP_CHARS[];	/* JF hide MD floating pt stuff all the same place */
extern const char FLT_CHARS[];

/*
 * Build any floating-point literal here.
 * Also build any bignum literal here.
 */

/* LITTLENUM_TYPE	generic_buffer [6]; *//* JF this is a hack */
/* Seems atof_machine can backscan through generic_bignum and hit whatever
   happens to be loaded before it in memory.  And its way too complicated
   for me to fix right.  Thus a hack.  JF:  Just make generic_bignum bigger,
   and never write into the early words, thus they'll always be zero.
   I hate Dean's floating-point code.  Bleh.
   */
LITTLENUM_TYPE generic_bignum[SIZE_OF_LARGE_NUMBER + 6];
FLONUM_TYPE generic_floating_point_number =
{
  &generic_bignum[6],		/* low (JF: Was 0) */
  &generic_bignum[SIZE_OF_LARGE_NUMBER + 6 - 1],	/* high JF: (added +6) */
  0,				/* leader */
  0,				/* exponent */
  0				/* sign */
};
/* If nonzero, we've been asked to assemble nan, +inf or -inf */
int generic_floating_point_magic;

floating_constant (expressionP)
     expressionS *expressionP;
{
  /* input_line_pointer->*/
  /* floating-point constant. */
  int error_code;

  error_code = atof_generic
    (&input_line_pointer, ".", EXP_CHARS,
     &generic_floating_point_number);

  if (error_code)
    {
      if (error_code == ERROR_EXPONENT_OVERFLOW)
	{
	  as_bad ("bad floating-point constant: exponent overflow, probably assembling junk");
	}
      else
	{
	  as_bad ("bad floating-point constant: unknown error code=%d.", error_code);
	}
    }
  expressionP->X_seg = SEG_BIG;
  /* input_line_pointer->just after constant, */
  /* which may point to whitespace. */
  expressionP->X_add_number = -1;
}



integer_constant (radix, expressionP)
     int radix;
     expressionS *expressionP;
{
  register char *digit_2;	/*->2nd digit of number. */
  char c;

  register valueT number;	/* offset or (absolute) value */
  register short int digit;	/* value of next digit in current radix */
  register short int maxdig = 0;/* highest permitted digit value. */
  register int too_many_digits = 0;	/* if we see >= this number of */
  register char *name;		/* points to name of symbol */
  register symbolS *symbolP;	/* points to symbol */

  int small;			/* true if fits in 32 bits. */
  extern char hex_value[];	/* in hex_value.c */

  /* may be bignum, or may fit in 32 bits. */
  /*
   * most numbers fit into 32 bits, and we want this case to be fast.
   * so we pretend it will fit into 32 bits. if, after making up a 32
   * bit number, we realise that we have scanned more digits than
   * comfortably fit into 32 bits, we re-scan the digits coding
   * them into a bignum. for decimal and octal numbers we are conservative: some
   * numbers may be assumed bignums when in fact they do fit into 32 bits.
   * numbers of any radix can have excess leading zeros: we strive
   * to recognise this and cast them back into 32 bits.
   * we must check that the bignum really is more than 32
   * bits, and change it back to a 32-bit number if it fits.
   * the number we are looking for is expected to be positive, but
   * if it fits into 32 bits as an unsigned number, we let it be a 32-bit
   * number. the cavalier approach is for speed in ordinary cases.
   */

  switch (radix)
    {

    case 2:
      maxdig = 2;
      too_many_digits = 33;
      break;
    case 8:
      maxdig = radix = 8;
      too_many_digits = 11;
      break;
    case 16:


      maxdig = radix = 16;
      too_many_digits = 9;
      break;
    case 10:
      maxdig = radix = 10;
      too_many_digits = 11;
    }
  c = *input_line_pointer;
  input_line_pointer++;
  digit_2 = input_line_pointer;
  for (number = 0; (digit = hex_value[c]) < maxdig; c = *input_line_pointer++)
    {
      number = number * radix + digit;
    }
  /* c contains character after number. */
  /* input_line_pointer->char after c. */
  small = input_line_pointer - digit_2 < too_many_digits;
  if (!small)
    {
      /*
       * we saw a lot of digits. manufacture a bignum the hard way.
       */
      LITTLENUM_TYPE *leader;	/*->high order littlenum of the bignum. */
      LITTLENUM_TYPE *pointer;	/*->littlenum we are frobbing now. */
      long carry;

      leader = generic_bignum;
      generic_bignum[0] = 0;
      generic_bignum[1] = 0;
      /* we could just use digit_2, but lets be mnemonic. */
      input_line_pointer = --digit_2;	/*->1st digit. */
      c = *input_line_pointer++;
      for (; (carry = hex_value[c]) < maxdig; c = *input_line_pointer++)
	{
	  for (pointer = generic_bignum;
	       pointer <= leader;
	       pointer++)
	    {
	      long work;

	      work = carry + radix * *pointer;
	      *pointer = work & LITTLENUM_MASK;
	      carry = work >> LITTLENUM_NUMBER_OF_BITS;
	    }
	  if (carry)
	    {
	      if (leader < generic_bignum + SIZE_OF_LARGE_NUMBER - 1)
		{		/* room to grow a longer bignum. */
		  *++leader = carry;
		}
	    }
	}
      /* again, c is char after number, */
      /* input_line_pointer->after c. */
      know (sizeof (int) * 8 == 32);
      know (LITTLENUM_NUMBER_OF_BITS == 16);
      /* hence the constant "2" in the next line. */
      if (leader < generic_bignum + 2)
	{			/* will fit into 32 bits. */
	  number =
	    ((generic_bignum[1] & LITTLENUM_MASK) << LITTLENUM_NUMBER_OF_BITS)
	    | (generic_bignum[0] & LITTLENUM_MASK);
	  small = 1;
	}
      else
	{
	  number = leader - generic_bignum + 1;	/* number of littlenums in the bignum. */
	}
    }
  if (small)
    {
      /*
       * here with number, in correct radix. c is the next char.
       * note that unlike un*x, we allow "011f" "0x9f" to
       * both mean the same as the (conventional) "9f". this is simply easier
       * than checking for strict canonical form. syntax sux!
       */

      switch (c)
	{

#ifdef LOCAL_LABELS_FB
	case 'b':
	  {
	    /*
	     * backward ref to local label.
	     * because it is backward, expect it to be defined.
	     */
	    /* Construct a local label.  */
	    name = fb_label_name ((int) number, 0);

	    /* seen before, or symbol is defined: ok */
	    symbolP = symbol_find (name);
	    if ((symbolP != NULL) && (S_IS_DEFINED (symbolP)))
	      {

		/* local labels are never absolute. don't waste time
		   checking absoluteness. */
		know (SEG_NORMAL (S_GET_SEGMENT (symbolP)));

		expressionP->X_add_symbol = symbolP;
		expressionP->X_seg = S_GET_SEGMENT (symbolP);

	      }
	    else
	      {			/* either not seen or not defined. */
		as_bad ("backw. ref to unknown label \"%d:\", 0 assumed.", number);
		expressionP->X_seg = SEG_ABSOLUTE;
	      }

	    expressionP->X_add_number = 0;
	    break;
	  }			/* case 'b' */

	case 'f':
	  {
	    /*
	     * forward reference. expect symbol to be undefined or
	     * unknown. undefined: seen it before. unknown: never seen
	     * it before.
	     * construct a local label name, then an undefined symbol.
	     * don't create a xseg frag for it: caller may do that.
	     * just return it as never seen before.
	     */
	    name = fb_label_name ((int) number, 1);
	    symbolP = symbol_find_or_make (name);
	    /* we have no need to check symbol properties. */
#ifndef many_segments
	    /* since "know" puts its arg into a "string", we
	       can't have newlines in the argument.  */
	    know (S_GET_SEGMENT (symbolP) == SEG_UNKNOWN || S_GET_SEGMENT (symbolP) == SEG_TEXT || S_GET_SEGMENT (symbolP) == SEG_DATA);
#endif
	    expressionP->X_add_symbol = symbolP;
	    expressionP->X_seg = SEG_UNKNOWN;
	    expressionP->X_subtract_symbol = NULL;
	    expressionP->X_add_number = 0;

	    break;
	  }			/* case 'f' */

#endif /* LOCAL_LABELS_FB */

#ifdef LOCAL_LABELS_DOLLAR

	case '$':
	  {

	    /* If the dollar label is *currently* defined, then this is just
	       another reference to it.  If it is not *currently* defined,
	       then this is a fresh instantiation of that number, so create
	       it.  */

	    if (dollar_label_defined (number))
	      {
		name = dollar_label_name (number, 0);
		symbolP = symbol_find (name);
		know (symbolP != NULL);
	      }
	    else
	      {
		name = dollar_label_name (number, 1);
		symbolP = symbol_find_or_make (name);
	      }

	    expressionP->X_add_symbol = symbolP;
	    expressionP->X_add_number = 0;
	    expressionP->X_seg = S_GET_SEGMENT (symbolP);

	    break;
	  }			/* case '$' */

#endif /* LOCAL_LABELS_DOLLAR */

	default:
	  {
	    expressionP->X_add_number = number;
	    expressionP->X_seg = SEG_ABSOLUTE;
	    input_line_pointer--;	/* restore following character. */
	    break;
	  }			/* really just a number */

	}			/* switch on char following the number */


    }
  else
    {				/* not a small number */
      expressionP->X_add_number = number;
      expressionP->X_seg = SEG_BIG;
      input_line_pointer--;	/*->char following number. */
    }				/* if (small) */
}				/* integer_constant() */


/*
 * Summary of operand().
 *
 * in:	Input_line_pointer points to 1st char of operand, which may
 *	be a space.
 *
 * out:	A expressionS. X_seg determines how to understand the rest of the
 *	expressionS.
 *	The operand may have been empty: in this case X_seg == SEG_ABSENT.
 *	Input_line_pointer->(next non-blank) char after operand.
 *
 */



static segT
operand (expressionP)
     register expressionS *expressionP;
{
  register char c;
  register symbolS *symbolP;	/* points to symbol */
  register char *name;		/* points to name of symbol */
  /* invented for humans only, hope */
  /* optimising compiler flushes it! */
  register short int radix;	/* 2, 8, 10 or 16, 0 when floating */
  /* 0 means we saw start of a floating- */
  /* point constant. */

  /* digits, assume it is a bignum. */




  SKIP_WHITESPACE ();		/* leading whitespace is part of operand. */
  c = *input_line_pointer++;	/* input_line_pointer->past char in c. */

  switch (c)
    {
#ifdef MRI
    case '%':
      integer_constant (2, expressionP);
      break;
    case '@':
      integer_constant (8, expressionP);
      break;
    case '$':
      integer_constant (16, expressionP);
      break;
#endif
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      input_line_pointer--;

      integer_constant (10, expressionP);
      break;

    case '0':
      /* non-decimal radix */


      c = *input_line_pointer;
      switch (c)
	{

	default:
	  if (c && strchr (FLT_CHARS, c))
	    {
	      input_line_pointer++;
	      floating_constant (expressionP);
	    }
	  else
	    {
	      /* The string was only zero */
	      expressionP->X_add_symbol = 0;
	      expressionP->X_add_number = 0;
	      expressionP->X_seg = SEG_ABSOLUTE;
	    }

	  break;

	case 'x':
	case 'X':
	  input_line_pointer++;
	  integer_constant (16, expressionP);
	  break;

	case 'b':
#ifdef LOCAL_LABELS_FB
	  if (!*input_line_pointer
	      || (!strchr ("+-.0123456789", *input_line_pointer)
		  && !strchr (EXP_CHARS, *input_line_pointer)))
	    {
	      input_line_pointer--;
	      integer_constant (10, expressionP);
	      break;
	    }
#endif
	case 'B':
	  input_line_pointer++;
	  integer_constant (2, expressionP);
	  break;

	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	  integer_constant (8, expressionP);
	  break;

	case 'f':
#ifdef LOCAL_LABELS_FB
	  /* if it says '0f' and the line ends or it doesn't look like
	 a floating point #, its a local label ref.  dtrt */
	  /* likewise for the b's.  xoxorich. */
	  if (c == 'f'
	      && (!*input_line_pointer ||
		  (!strchr ("+-.0123456789", *input_line_pointer) &&
		   !strchr (EXP_CHARS, *input_line_pointer))))
	    {
	      input_line_pointer -= 1;
	      integer_constant (10, expressionP);
	      break;
	    }
#endif

	case 'd':
	case 'D':
	case 'F':
	case 'r':
	case 'e':
	case 'E':
	case 'g':
	case 'G':

	  input_line_pointer++;
	  floating_constant (expressionP);
	  break;

#ifdef LOCAL_LABELS_DOLLAR
	case '$':
	  integer_constant (10, expressionP);
	  break;
#endif
	}

      break;
    case '(':
      /* didn't begin with digit & not a name */
      {
	(void) expression (expressionP);
	/* Expression() will pass trailing whitespace */
	if (*input_line_pointer++ != ')')
	  {
	    as_bad ("Missing ')' assumed");
	    input_line_pointer--;
	  }
	/* here with input_line_pointer->char after "(...)" */
      }
      return expressionP->X_seg;


    case '\'':
      /*
     * Warning: to conform to other people's assemblers NO ESCAPEMENT is permitted
     * for a single quote. The next character, parity errors and all, is taken
     * as the value of the operand. VERY KINKY.
     */
      expressionP->X_add_number = *input_line_pointer++;
      expressionP->X_seg = SEG_ABSOLUTE;
      break;

    case '~':
    case '-':
    case '+':

      {
	/* unary operator: hope for SEG_ABSOLUTE */
	switch (operand (expressionP))
	  {
	  case SEG_ABSOLUTE:
	    /* input_line_pointer -> char after operand */
	    if (c == '-')
	      {
		expressionP->X_add_number = -expressionP->X_add_number;
		/*
	   * Notice: '-' may  overflow: no warning is given. This is compatible
	   * with other people's assemblers. Sigh.
	   */
	      }
	    else
	      {
		expressionP->X_add_number = ~expressionP->X_add_number;
	      }
	    break;

	  case SEG_TEXT:
	  case SEG_DATA:
	  case SEG_BSS:
	  case SEG_PASS1:
	  case SEG_UNKNOWN:
	    if (c == '-')
	      {			/* JF I hope this hack works */
		expressionP->X_subtract_symbol = expressionP->X_add_symbol;
		expressionP->X_add_symbol = 0;
		expressionP->X_seg = SEG_DIFFERENCE;
		break;
	      }
	  default:		/* unary on non-absolute is unsuported */
	    as_warn ("Unary operator %c ignored because bad operand follows", c);
	    break;
	    /* Expression undisturbed from operand(). */
	  }
      }



      break;

    case '.':
      if (!is_part_of_name (*input_line_pointer))
	{
	  extern struct obstack frags;

	  /*
	JF:  '.' is pseudo symbol with value of current location in current
	segment. . .
	*/
	  symbolP = symbol_new ("L0\001",
				now_seg,
	       (valueT) (obstack_next_free (&frags) - frag_now->fr_literal),
				frag_now);

	  expressionP->X_add_number = 0;
	  expressionP->X_add_symbol = symbolP;
	  expressionP->X_seg = now_seg;
	  break;

	}
      else
	{
	  goto isname;


	}
    case ',':
    case '\n':
      /* can't imagine any other kind of operand */
      expressionP->X_seg = SEG_ABSENT;
      input_line_pointer--;
      md_operand (expressionP);
      break;
      /* Fall through */
    default:
      if (is_name_beginner (c))	/* here if did not begin with a digit */
	{
	  /*
       * Identifier begins here.
       * This is kludged for speed, so code is repeated.
       */
	isname:
	  name = --input_line_pointer;
	  c = get_symbol_end ();
	  symbolP = symbol_find_or_make (name);
	  /*
       * If we have an absolute symbol or a reg, then we know its value now.
       */
	  expressionP->X_seg = S_GET_SEGMENT (symbolP);
	  switch (expressionP->X_seg)
	    {
	    case SEG_ABSOLUTE:
	    case SEG_REGISTER:
	      expressionP->X_add_number = S_GET_VALUE (symbolP);
	      break;

	    default:
	      expressionP->X_add_number = 0;
	      expressionP->X_add_symbol = symbolP;
	    }
	  *input_line_pointer = c;
	  expressionP->X_subtract_symbol = NULL;
	}
      else
	{
	  as_bad ("Bad expression");
	  expressionP->X_add_number = 0;
	  expressionP->X_seg = SEG_ABSOLUTE;

	}

    }







  /*
   * It is more 'efficient' to clean up the expressionS when they are created.
   * Doing it here saves lines of code.
   */
  clean_up_expression (expressionP);
  SKIP_WHITESPACE ();		/*->1st char after operand. */
  know (*input_line_pointer != ' ');
  return (expressionP->X_seg);
}				/* operand() */


/* Internal. Simplify a struct expression for use by expr() */

/*
 * In:	address of a expressionS.
 *	The X_seg field of the expressionS may only take certain values.
 *	Now, we permit SEG_PASS1 to make code smaller & faster.
 *	Elsewise we waste time special-case testing. Sigh. Ditto SEG_ABSENT.
 * Out:	expressionS may have been modified:
 *	'foo-foo' symbol references cancelled to 0,
 *		which changes X_seg from SEG_DIFFERENCE to SEG_ABSOLUTE;
 *	Unused fields zeroed to help expr().
 */

static void
clean_up_expression (expressionP)
     register expressionS *expressionP;
{
  switch (expressionP->X_seg)
    {
    case SEG_ABSENT:
    case SEG_PASS1:
      expressionP->X_add_symbol = NULL;
      expressionP->X_subtract_symbol = NULL;
      expressionP->X_add_number = 0;
      break;

    case SEG_BIG:
    case SEG_ABSOLUTE:
      expressionP->X_subtract_symbol = NULL;
      expressionP->X_add_symbol = NULL;
      break;

    case SEG_UNKNOWN:
      expressionP->X_subtract_symbol = NULL;
      break;

    case SEG_DIFFERENCE:
      /*
	 * It does not hurt to 'cancel' NULL==NULL
	 * when comparing symbols for 'eq'ness.
	 * It is faster to re-cancel them to NULL
	 * than to check for this special case.
	 */
      if (expressionP->X_subtract_symbol == expressionP->X_add_symbol
	  || (expressionP->X_subtract_symbol
	      && expressionP->X_add_symbol
	      && expressionP->X_subtract_symbol->sy_frag == expressionP->X_add_symbol->sy_frag
	      && S_GET_VALUE (expressionP->X_subtract_symbol) == S_GET_VALUE (expressionP->X_add_symbol)))
	{
	  expressionP->X_subtract_symbol = NULL;
	  expressionP->X_add_symbol = NULL;
	  expressionP->X_seg = SEG_ABSOLUTE;
	}
      break;

    case SEG_REGISTER:
      expressionP->X_add_symbol = NULL;
      expressionP->X_subtract_symbol = NULL;
      break;

    default:
      if (SEG_NORMAL (expressionP->X_seg))
	{
	  expressionP->X_subtract_symbol = NULL;
	}
      else
	{
	  BAD_CASE (expressionP->X_seg);
	}
      break;
    }
}				/* clean_up_expression() */

/*
 *			expr_part ()
 *
 * Internal. Made a function because this code is used in 2 places.
 * Generate error or correct X_?????_symbol of expressionS.
 */

/*
 * symbol_1 += symbol_2 ... well ... sort of.
 */

static segT
expr_part (symbol_1_PP, symbol_2_P)
     symbolS **symbol_1_PP;
     symbolS *symbol_2_P;
{
  segT return_value;
#ifndef MANY_SEGMENTS
  know ((*symbol_1_PP) == NULL || (S_GET_SEGMENT (*symbol_1_PP) == SEG_TEXT) || (S_GET_SEGMENT (*symbol_1_PP) == SEG_DATA) || (S_GET_SEGMENT (*symbol_1_PP) == SEG_BSS) || (!S_IS_DEFINED (*symbol_1_PP)));
  know (symbol_2_P == NULL || (S_GET_SEGMENT (symbol_2_P) == SEG_TEXT) || (S_GET_SEGMENT (symbol_2_P) == SEG_DATA) || (S_GET_SEGMENT (symbol_2_P) == SEG_BSS) || (!S_IS_DEFINED (symbol_2_P)));
#endif
  if (*symbol_1_PP)
    {
      if (!S_IS_DEFINED (*symbol_1_PP))
	{
	  if (symbol_2_P)
	    {
	      return_value = SEG_PASS1;
	      *symbol_1_PP = NULL;
	    }
	  else
	    {
	      know (!S_IS_DEFINED (*symbol_1_PP));
	      return_value = SEG_UNKNOWN;
	    }
	}
      else
	{
	  if (symbol_2_P)
	    {
	      if (!S_IS_DEFINED (symbol_2_P))
		{
		  *symbol_1_PP = NULL;
		  return_value = SEG_PASS1;
		}
	      else
		{
		  /* {seg1} - {seg2} */
		  as_bad ("Expression too complex, 2 symbolS forgotten: \"%s\" \"%s\"",
			S_GET_NAME (*symbol_1_PP), S_GET_NAME (symbol_2_P));
		  *symbol_1_PP = NULL;
		  return_value = SEG_ABSOLUTE;
		}
	    }
	  else
	    {
	      return_value = S_GET_SEGMENT (*symbol_1_PP);
	    }
	}
    }
  else
    {				/* (* symbol_1_PP) == NULL */
      if (symbol_2_P)
	{
	  *symbol_1_PP = symbol_2_P;
	  return_value = S_GET_SEGMENT (symbol_2_P);
	}
      else
	{
	  *symbol_1_PP = NULL;
	  return_value = SEG_ABSOLUTE;
	}
    }
#ifndef MANY_SEGMENTS
  know (return_value == SEG_ABSOLUTE || return_value == SEG_TEXT || return_value == SEG_DATA || return_value == SEG_BSS || return_value == SEG_UNKNOWN || return_value == SEG_PASS1);
#endif
  know ((*symbol_1_PP) == NULL || (S_GET_SEGMENT (*symbol_1_PP) == return_value));
  return (return_value);
}				/* expr_part() */

/* Expression parser. */

/*
 * We allow an empty expression, and just assume (absolute,0) silently.
 * Unary operators and parenthetical expressions are treated as operands.
 * As usual, Q==quantity==operand, O==operator, X==expression mnemonics.
 *
 * We used to do a aho/ullman shift-reduce parser, but the logic got so
 * warped that I flushed it and wrote a recursive-descent parser instead.
 * Now things are stable, would anybody like to write a fast parser?
 * Most expressions are either register (which does not even reach here)
 * or 1 symbol. Then "symbol+constant" and "symbol-symbol" are common.
 * So I guess it doesn't really matter how inefficient more complex expressions
 * are parsed.
 *
 * After expr(RANK,resultP) input_line_pointer->operator of rank <= RANK.
 * Also, we have consumed any leading or trailing spaces (operand does that)
 * and done all intervening operators.
 */

typedef enum
{
  O_illegal,			/* (0)  what we get for illegal op */

  O_multiply,			/* (1)  * */
  O_divide,			/* (2)  / */
  O_modulus,			/* (3)  % */
  O_left_shift,			/* (4)  < */
  O_right_shift,		/* (5)  > */
  O_bit_inclusive_or,		/* (6)  | */
  O_bit_or_not,			/* (7)  ! */
  O_bit_exclusive_or,		/* (8)  ^ */
  O_bit_and,			/* (9)  & */
  O_add,			/* (10) + */
  O_subtract			/* (11) - */
}

operatorT;

#define __ O_illegal

static const operatorT op_encoding[256] =
{				/* maps ASCII->operators */

  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,

  __, O_bit_or_not, __, __, __, O_modulus, O_bit_and, __,
  __, __, O_multiply, O_add, __, O_subtract, __, O_divide,
  __, __, __, __, __, __, __, __,
  __, __, __, __, O_left_shift, __, O_right_shift, __,
  __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, O_bit_exclusive_or, __,
  __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __,
  __, __, __, __, O_bit_inclusive_or, __, __, __,

  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __,
  __, __, __, __, __, __, __, __, __, __, __, __, __, __, __, __
};


/*
 *	Rank	Examples
 *	0	operand, (expression)
 *	1	+ -
 *	2	& ^ ! |
 *	3	* / % << >>
 */
static const operator_rankT
  op_rank[] =
{0, 3, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1};

/* Return resultP->X_seg. */
segT 
expr (rank, resultP)
     register operator_rankT rank;	/* Larger # is higher rank. */
     register expressionS *resultP;	/* Deliver result here. */
{
  expressionS right;
  register operatorT op_left;
  register char c_left;		/* 1st operator character. */
  register operatorT op_right;
  register char c_right;

  know (rank >= 0);
  (void) operand (resultP);
  know (*input_line_pointer != ' ');	/* Operand() gobbles spaces. */
  c_left = *input_line_pointer;	/* Potential operator character. */
  op_left = op_encoding[c_left];
  while (op_left != O_illegal && op_rank[(int) op_left] > rank)
    {
      input_line_pointer++;	/*->after 1st character of operator. */
      /* Operators "<<" and ">>" have 2 characters. */
      if (*input_line_pointer == c_left && (c_left == '<' || c_left == '>'))
	{
	  input_line_pointer++;
	}			/*->after operator. */
      if (SEG_ABSENT == expr (op_rank[(int) op_left], &right))
	{
	  as_warn ("Missing operand value assumed absolute 0.");
	  resultP->X_add_number = 0;
	  resultP->X_subtract_symbol = NULL;
	  resultP->X_add_symbol = NULL;
	  resultP->X_seg = SEG_ABSOLUTE;
	}
      know (*input_line_pointer != ' ');
      c_right = *input_line_pointer;
      op_right = op_encoding[c_right];
      if (*input_line_pointer == c_right && (c_right == '<' || c_right == '>'))
	{
	  input_line_pointer++;
	}			/*->after operator. */
      know ((int) op_right == 0 || op_rank[(int) op_right] <= op_rank[(int) op_left]);
      /* input_line_pointer->after right-hand quantity. */
      /* left-hand quantity in resultP */
      /* right-hand quantity in right. */
      /* operator in op_left. */
      if (resultP->X_seg == SEG_PASS1 || right.X_seg == SEG_PASS1)
	{
	  resultP->X_seg = SEG_PASS1;
	}
      else
	{
	  if (resultP->X_seg == SEG_BIG)
	    {
	      as_warn ("Left operand of %c is a %s.  Integer 0 assumed.",
		    c_left, resultP->X_add_number > 0 ? "bignum" : "float");
	      resultP->X_seg = SEG_ABSOLUTE;
	      resultP->X_add_symbol = 0;
	      resultP->X_subtract_symbol = 0;
	      resultP->X_add_number = 0;
	    }
	  if (right.X_seg == SEG_BIG)
	    {
	      as_warn ("Right operand of %c is a %s.  Integer 0 assumed.",
		       c_left, right.X_add_number > 0 ? "bignum" : "float");
	      right.X_seg = SEG_ABSOLUTE;
	      right.X_add_symbol = 0;
	      right.X_subtract_symbol = 0;
	      right.X_add_number = 0;
	    }
	  if (op_left == O_subtract)
	    {
	      /*
	       * Convert - into + by exchanging symbolS and negating number.
	       * I know -infinity can't be negated in 2's complement:
	       * but then it can't be subtracted either. This trick
	       * does not cause any further inaccuracy.
	       */

	      register symbolS *symbolP;

	      right.X_add_number = -right.X_add_number;
	      symbolP = right.X_add_symbol;
	      right.X_add_symbol = right.X_subtract_symbol;
	      right.X_subtract_symbol = symbolP;
	      if (symbolP)
		{
		  right.X_seg = SEG_DIFFERENCE;
		}
	      op_left = O_add;
	    }

	  if (op_left == O_add)
	    {
	      segT seg1;
	      segT seg2;
#ifndef MANY_SEGMENTS

	      know (resultP->X_seg == SEG_DATA || resultP->X_seg == SEG_TEXT || resultP->X_seg == SEG_BSS || resultP->X_seg == SEG_UNKNOWN || resultP->X_seg == SEG_DIFFERENCE || resultP->X_seg == SEG_ABSOLUTE || resultP->X_seg == SEG_PASS1 || resultP->X_seg == SEG_REGISTER);

	      know (right.X_seg == SEG_DATA || right.X_seg == SEG_TEXT || right.X_seg == SEG_BSS || right.X_seg == SEG_UNKNOWN || right.X_seg == SEG_DIFFERENCE || right.X_seg == SEG_ABSOLUTE || right.X_seg == SEG_PASS1);
#endif
	      clean_up_expression (&right);
	      clean_up_expression (resultP);

	      seg1 = expr_part (&resultP->X_add_symbol, right.X_add_symbol);
	      seg2 = expr_part (&resultP->X_subtract_symbol, right.X_subtract_symbol);
	      if (seg1 == SEG_PASS1 || seg2 == SEG_PASS1)
		{
		  need_pass_2 = 1;
		  resultP->X_seg = SEG_PASS1;
		}
	      else if (seg2 == SEG_ABSOLUTE)
		resultP->X_seg = seg1;
	      else if (seg1 != SEG_UNKNOWN
		       && seg1 != SEG_ABSOLUTE
		       && seg2 != SEG_UNKNOWN
		       && seg1 != seg2)
		{
		  know (seg2 != SEG_ABSOLUTE);
		  know (resultP->X_subtract_symbol);
#ifndef MANY_SEGMENTS
		  know (seg1 == SEG_TEXT || seg1 == SEG_DATA || seg1 == SEG_BSS);
		  know (seg2 == SEG_TEXT || seg2 == SEG_DATA || seg2 == SEG_BSS);
#endif
		  know (resultP->X_add_symbol);
		  know (resultP->X_subtract_symbol);
		  as_bad ("Expression too complex: forgetting %s - %s",
			  S_GET_NAME (resultP->X_add_symbol),
			  S_GET_NAME (resultP->X_subtract_symbol));
		  resultP->X_seg = SEG_ABSOLUTE;
		  /* Clean_up_expression() will do the rest. */
		}
	      else
		resultP->X_seg = SEG_DIFFERENCE;

	      resultP->X_add_number += right.X_add_number;
	      clean_up_expression (resultP);
	    }
	  else
	    {			/* Not +. */
	      if (resultP->X_seg == SEG_UNKNOWN || right.X_seg == SEG_UNKNOWN)
		{
		  resultP->X_seg = SEG_PASS1;
		  need_pass_2 = 1;
		}
	      else
		{
		  resultP->X_subtract_symbol = NULL;
		  resultP->X_add_symbol = NULL;
		  /* Will be SEG_ABSOLUTE. */
		  if (resultP->X_seg != SEG_ABSOLUTE || right.X_seg != SEG_ABSOLUTE)
		    {
		      as_bad ("Relocation error. Absolute 0 assumed.");
		      resultP->X_seg = SEG_ABSOLUTE;
		      resultP->X_add_number = 0;
		    }
		  else
		    {
		      switch (op_left)
			{
			case O_bit_inclusive_or:
			  resultP->X_add_number |= right.X_add_number;
			  break;

			case O_modulus:
			  if (right.X_add_number)
			    {
			      resultP->X_add_number %= right.X_add_number;
			    }
			  else
			    {
			      as_warn ("Division by 0. 0 assumed.");
			      resultP->X_add_number = 0;
			    }
			  break;

			case O_bit_and:
			  resultP->X_add_number &= right.X_add_number;
			  break;

			case O_multiply:
			  resultP->X_add_number *= right.X_add_number;
			  break;

			case O_divide:
			  if (right.X_add_number)
			    {
			      resultP->X_add_number /= right.X_add_number;
			    }
			  else
			    {
			      as_warn ("Division by 0. 0 assumed.");
			      resultP->X_add_number = 0;
			    }
			  break;

			case O_left_shift:
			  resultP->X_add_number <<= right.X_add_number;
			  break;

			case O_right_shift:
			  resultP->X_add_number >>= right.X_add_number;
			  break;

			case O_bit_exclusive_or:
			  resultP->X_add_number ^= right.X_add_number;
			  break;

			case O_bit_or_not:
			  resultP->X_add_number |= ~right.X_add_number;
			  break;

			default:
			  BAD_CASE (op_left);
			  break;
			}	/* switch(operator) */
		    }
		}		/* If we have to force need_pass_2. */
	    }			/* If operator was +. */
	}			/* If we didn't set need_pass_2. */
      op_left = op_right;
    }				/* While next operator is >= this rank. */
  return (resultP->X_seg);
}

/*
 *			get_symbol_end()
 *
 * This lives here because it belongs equally in expr.c & read.c.
 * Expr.c is just a branch office read.c anyway, and putting it
 * here lessens the crowd at read.c.
 *
 * Assume input_line_pointer is at start of symbol name.
 * Advance input_line_pointer past symbol name.
 * Turn that character into a '\0', returning its former value.
 * This allows a string compare (RMS wants symbol names to be strings)
 * of the symbol name.
 * There will always be a char following symbol name, because all good
 * lines end in end-of-line.
 */
char
get_symbol_end ()
{
  register char c;

  while (is_part_of_name (c = *input_line_pointer++))
    ;
  *--input_line_pointer = 0;
  return (c);
}


unsigned int 
get_single_number ()
{
  expressionS exp;
  operand (&exp);
  return exp.X_add_number;

}

/*
 * Local Variables:
 * comment-column: 0
 * fill-column: 131
 * End:
 */

/* end of expr.c */
