/* Convert types from GDB to GCC

   Copyright (C) 2014-2016 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "defs.h"
#include "gdbtypes.h"
#include "compile-internal.h"
#include "gdb_assert.h"
#include "symtab.h"
#include "source.h"
#include "cp-support.h"
#include "demangle.h"
#include "cp-abi.h"
#include "symtab.h"
#include "objfiles.h"
#include "block.h"
#include "gdbcmd.h"
#include "typeprint.h"
#include "c-lang.h"

/* Define to enable debugging for ctor/dtor definitions during
   type conversion.  */

#define DEBUG_XTOR 0

/* A "type" to indicate that convert_cplus_type should not attempt to
   cache its resultant type.  This is used, for example, when defining
   namespaces for the oracle.  */

#define DONT_CACHE_TYPE ((gcc_type) 0)

/* Modifiers for abstract template parameters when used in template function
   declarations, including CV and ref qualifiers and pointer and reference
   type modifiers, e.g., const T*.  */

enum template_parameter_type_modifiers
{
  /* The abstract parameter type is not qualified at all.  */
  PARAMETER_NONE,

  /* The abstract parameter type was declared `const', const T.  */
  PARAMETER_CONST,

  /* The abstract parameter type was declared `volatile', volatile T.  */
  PARAMETER_VOLATILE,

  /* The abstract parameter type was declared `restrict', restrict T.  */
  PARAMETER_RESTRICT,

  /* The abstract parameter type was declared as a pointer, T*.  */
  PARAMETER_POINTER,

  /* The abstract parameter type was declared as a reference, T&.  */
  PARAMETER_LVALUE_REFERENCE,

  /* The abstract parameter type was declared as rvalue reference, T&&.  */
  PARAMETER_RVALUE_REFERENCE
};

/* A vector of template parameter type modifiers.  */

typedef enum template_parameter_type_modifiers template_parameter_modifiers;
DEF_VEC_I (template_parameter_modifiers);

/* A scope of a type.  Type names are broken into "scopes" or
   "components," a series of unqualified names comprising the
   type name, e.g., "namespace1", "namespace2", "myclass", "method".  */

struct compile_cplus_scope
{
  /* The unqualified name of this scope.  */
  struct stoken name;

  /* The block symbol for this type/scope.  */
  struct block_symbol bsymbol;
};
typedef struct compile_cplus_scope compile_cplus_scope_def;
DEF_VEC_O (compile_cplus_scope_def);

/* A processing context.  */

struct compile_cplus_context
{
  VEC (compile_cplus_scope_def) *scopes;
};
#define CONTEXT_SCOPES(PCTX) ((PCTX)->scopes)
typedef struct compile_cplus_context *compile_cplus_context_def;
DEF_VEC_P (compile_cplus_context_def);

/* A list of contexts we are processing.  A processing context
   is simply a non-namespace scope.  */

static VEC (compile_cplus_context_def) *cplus_processing_contexts = NULL;

/* An object that maps a gdb type to a gcc type.  */

struct type_map_instance
{
  /* The gdb type.  */
  const struct type *type;

  /* The corresponding gcc type handle.  */
  gcc_type gcc_type_handle;
};

struct template_defn
{
  /* The string name of the template, excluding any parameters.

     Allocated.  */
  char *name;

  /* !!keiths: Yuck. The string used for hashing this defn.

     Allocated.  */
  char *hash_string;

  /* The template parameters needed by the compiler plug-in.

     Allocated.  */
  struct gcc_cp_template_args *params;

  /* The decl associated with this template definition.  */
  gcc_decl decl;

  /* A list of default values for the parameters of this template.
     Allocated.  */
  struct symbol **default_arguments;

  /* Has this template already been defined?  This is a necessary evil
     since we have to traverse over all hash table entries.  */
  int defined;

};

/* A hashtable instance for tracking template definitions.  */

struct function_template_defn : template_defn
{
  /* The template symbol used to create this template definition.
     NOTE: Any given template_defn could be associated with any number
     of template instances in the program.

     This field is not const since we will be lazily computing template
     parameter indices for the function's argument and return types.  */
  struct template_symbol *tsymbol;

  /* The parent type or NULL if this does not represent a method.  */
  struct type *parent_type;

  /* The fieldlist and method indices for the method or -1 if this template
     definition does not represent a method.  */
  int f_idx;
  int m_idx;

  /* Demangle tree for the template defining this generic.
     Must be freed with cp_demangled_name_parse_free.  */
  struct demangle_parse_info *demangle_info;

  /* Two pointers to storage needed for the demangler tree.  Must be
     freed.  */
  void *demangle_memory;
  char *demangle_name_storage;
};

struct class_template_defn : template_defn
{
  /* The type used to create this template definition.
     NOTE: Any given template_defn could be associated with any number
     of template instances in the program.  */
  struct type *type;
};

/* Flag to enable internal debugging.  */

int debug_compile_cplus_types = 0;

/* Flag to enable internal context switching debugging.  */

static int debug_compile_cplus_contexts = 0;

/* Forward declarations.  */

static gcc_type ccp_convert_void (struct compile_cplus_instance *,
				  struct type *);

static gcc_type ccp_convert_func (struct compile_cplus_instance *instance,
				  struct type *type, int strip_artificial);

static void
  ccp_emit_class_template_decls (struct compile_cplus_instance *instance);
static void
  ccp_emit_function_template_decls (struct compile_cplus_instance *instance);

static void
  ccp_maybe_define_new_function_template
    (struct compile_cplus_instance *instance, const struct symbol *sym,
     struct type *parent_type, int f_idx, int m_idx);

static char *
  ccp_function_template_decl (struct template_symbol *tsymbol,
			      const struct demangle_parse_info *info);

static void
  ccp_maybe_define_new_class_template (struct compile_cplus_instance *instance,
				       struct type *type, const char *name);

static void
  print_template_parameter_list (struct ui_file *stream,
				 const struct template_argument_info *arg_info);

static struct class_template_defn *
  find_class_template_defn (struct compile_cplus_instance *instance, struct type *type);

/* Allocate a new C string from the given TOKEN.  */

static char *
copy_stoken (const struct stoken *token)
{
  char *string = (char *) xmalloc (token->length + 1);

  strncpy (string, token->ptr, token->length);
  string[token->length] = '\0';
  return string;
}

/* !!keiths: Paranoia?  Sadly, we have this information already, but it is
   currently tossed when convert_cplus_type is called from
   convert_one_symbol.  */

static const struct block *
get_current_search_block (void)
{
  const struct block *block;
  enum language save_language;

  /* get_selected_block can change the current language when there
     is no selected frame yet.  */
  /* !!keiths this is probably a bit (over-)defensive, since we can't
     actually compile anything without running the inferior.  */
  save_language = current_language->la_language;
  block = get_selected_block (0);
  set_language (save_language);

  return block;
}

/* Return the declaration name of the natural name NATURAL.
   This returns a name with no function arguments or template parameters.
   The result must be freed by the caller.  */

static char *
ccp_decl_name (const char *natural)
{
  char *name = NULL;

  if (natural != NULL)
    {
      char *stripped;

      /* !!keiths: FIXME: Write a new parser func to do this?  */
      name = cp_func_name (natural);
      if (name == NULL)
	{
	  stripped = cp_strip_template_parameters (natural);
	  if (stripped != NULL)
	    return stripped;

	  name = xstrdup (natural);
	}
      else
	{
	  stripped = cp_strip_template_parameters (name);
	  if (stripped != NULL)
	    {
	      xfree (name);
	      return stripped;
	    }
	}
    }

  return name;
}

/* Returns non-zero if the given TYPE represents a varargs function,
   zero otherwise.  */

static int
ccp_is_varargs_p (const struct type *type)
{
  /* !!keiths: This doesn't always work, unfortunately.  When we have a
     pure virtual method, TYPE_PROTOTYPED == 0.
     But this *may* be needed for several gdb.compile tests.  Or at least
     indicate other unresolved bugs in this file or elsewhere in gdb.  */
  return TYPE_VARARGS (type) /*|| !TYPE_PROTOTYPED (type)*/;
}

/* Get the access flag for the NUM'th field of TYPE.  */

static enum gcc_cp_symbol_kind
get_field_access_flag (const struct type *type, int num)
{
  if (TYPE_FIELD_PROTECTED (type, num))
    return GCC_CP_ACCESS_PROTECTED;
  else if (TYPE_FIELD_PRIVATE (type, num))
    return GCC_CP_ACCESS_PRIVATE;

  /* GDB assumes everything else is public.  */
  return GCC_CP_ACCESS_PUBLIC;
}

/* Get the access flag for the NUM'th method of TYPE's FNI'th
   fieldlist.  */

static enum gcc_cp_symbol_kind
get_method_access_flag (const struct type *type, int fni, int num)
{
  const struct fn_field *methods;

  gdb_assert (TYPE_CODE (type) == TYPE_CODE_STRUCT);

  /* If this type was not declared a class, everything is public.  */
  if (!TYPE_DECLARED_CLASS (type))
    return GCC_CP_ACCESS_PUBLIC;

  /* Otherwise, read accessibility from the fn_field.  */
  methods = TYPE_FN_FIELDLIST1 (type, fni);
  if (TYPE_FN_FIELD_PUBLIC (methods, num))
    return GCC_CP_ACCESS_PUBLIC;
  else if (TYPE_FN_FIELD_PROTECTED (methods, num))
    return GCC_CP_ACCESS_PROTECTED;
  else if (TYPE_FN_FIELD_PRIVATE (methods, num))
    return GCC_CP_ACCESS_PRIVATE;

  gdb_assert_not_reached ("unhandled method access specifier");
}

/* Hash a type_map_instance.  */

static hashval_t
hash_type_map_instance (const void *p)
{
  const struct type_map_instance *inst = (struct type_map_instance *) p;

  return htab_hash_pointer (inst->type);
}

/* Check two type_map_instance objects for equality.  */

static int
eq_type_map_instance (const void *a, const void *b)
{
  const struct type_map_instance *insta = (struct type_map_instance *) a;
  const struct type_map_instance *instb = (struct type_map_instance *) b;

  return insta->type == instb->type;
}



/* Insert an entry into the type map associated with CONTEXT that maps
   from the gdb type TYPE to the gcc type GCC_TYPE.  It is ok for a
   given type to be inserted more than once, provided that the exact
   same association is made each time.  This simplifies how type
   caching works elsewhere in this file -- see how struct type caching
   is handled.  */

static void
insert_type (struct compile_cplus_instance *instance, struct type *type,
	     gcc_type gcc_type)
{
  struct type_map_instance inst, *add;
  void **slot;

  inst.type = type;
  inst.gcc_type_handle = gcc_type;
  slot = htab_find_slot (instance->type_map, &inst, INSERT);

  add = (struct type_map_instance *) *slot;

  /* The type might have already been inserted in order to handle
     recursive types.  */
  gdb_assert (add == NULL || add->gcc_type_handle == gcc_type);

  if (add == NULL)
    {
      add = XNEW (struct type_map_instance);
      *add = inst;
      *slot = add;
    }
}

/* A useful debugging function to output the processing context PCTX
   to stdout.  */

static void __attribute__ ((used))
debug_print_context (const struct compile_cplus_context *pctx)
{
  int i;
  struct compile_cplus_scope *scope;

  if (pctx != NULL)
    {
      for (i = 0;
	   VEC_iterate (compile_cplus_scope_def, CONTEXT_SCOPES (pctx),
			i, scope); ++i)
	{
	  char *name = copy_stoken (&scope->name);
	  const char *symbol;

	  symbol = (scope->bsymbol.symbol != NULL
		    ? SYMBOL_NATURAL_NAME (scope->bsymbol.symbol) : "<none>");
	  printf ("\tname = %s, symbol = %s\n", name, symbol);
	  xfree (name);
	}
    }
}

/* Utility function to convert CODE into a string.  */

static const char *
type_code_to_string (enum type_code code)
{
  const char * const s[] =
    {"BISTRING (deprecated)", "UNDEF (not used)",
      "PTR", "ARRAY", "STRUCT", "UNION", "ENUM",
      "FLAGS", "FUNC", "INT", "FLT", "VOID",
      "SET", "RANGE", "STRING", "ERROR", "METHOD",
      "METHODPTR", "MEMBERPTR", "REF", "CHAR", "BOOL",
      "COMPLEX", "TYPEDEF", "NAMESPACE", "DECFLOAT", "MODULE",
     "INTERNAL_FUNCTION", "XMETHOD"};

  return s[code + 1];
}

/* Get the current processing context.  */

static struct compile_cplus_context *
get_current_processing_context (void)
{
  if (VEC_empty (compile_cplus_context_def, cplus_processing_contexts))
    return NULL;

  return VEC_last (compile_cplus_context_def, cplus_processing_contexts);
}

/* Get the main scope to process in the processing context PCTX.  */

static struct compile_cplus_scope *
get_processing_context_scope (struct compile_cplus_context *pctx)
{
  return VEC_last (compile_cplus_scope_def, CONTEXT_SCOPES (pctx));
}

/* Convert TYPENAME into a vector of namespace and top-most/super
   composite scopes.

   For example, for the input "Namespace::classB::classInner", the
   resultant vector will contain the tokens {"Namespace", 9} and
   {"classB", 6}.

   The resulting VEC must be freed, but the individual components should not.
   This function may return NULL if TYPE represents an anonymous type.  */

static VEC (compile_cplus_scope_def) *
ccp_type_name_to_scopes (const char *type_name)
{
  const char *p;
  char *lookup_name;
  int running_length = 0;
  struct cleanup *back_to;
  VEC (compile_cplus_scope_def) *result = NULL;
  const struct block *block = get_current_search_block ();

  if (type_name == NULL)
    {
      /* An anonymous type.  We cannot really do much here.  We simply cannot
	 look up anonymous types easily/at all.  */
      return NULL;
    }

  back_to = make_cleanup (free_current_contents, &lookup_name);
  p = type_name;
  while (1)
    {
      struct stoken s;
      struct block_symbol bsymbol;

      s.ptr = p;
      s.length = cp_find_first_component (p);
      p += s.length;

      /* Look up the symbol and decide when to stop.  */
      if (running_length == 0)
	{
	  lookup_name = copy_stoken (&s);
	  running_length = s.length + 1;
	}
      else
	{
	  running_length += 2 + s.length;
	  lookup_name = (char *) xrealloc (lookup_name, running_length);
	  strcat (lookup_name, "::");
	  strncat (lookup_name, s.ptr, s.length);
	}
      bsymbol = lookup_symbol (lookup_name, block, VAR_DOMAIN, NULL);
      /* !!keiths: I seem to recall some special silliness with typedefs!  */
      if (bsymbol.symbol != NULL)
	{
	  struct compile_cplus_scope c = { s, bsymbol };

	  VEC_safe_push (compile_cplus_scope_def, result, &c);

	  if (TYPE_CODE (SYMBOL_TYPE (bsymbol.symbol))
	      != TYPE_CODE_NAMESPACE)
	    {
	      /* We're done.  */
	      break;
	    }
	}

      if (*p == ':')
	{
	  ++p;
	  if (*p == ':')
	    ++p;
	  else
	    {
	      /* This shouldn't happen since we are not attempting to
		 loop over user input.  This name is generated by GDB
		 from debug info.  */
	      internal_error (__FILE__, __LINE__,
			      _("malformed TYPE_NAME during parsing"));
	    }
	}
      if (p[0] == '\0')
	break;
    }

  do_cleanups (back_to);
  return result;
}

/* Does PCTX require a new processing context? Returns 1 if a new context
   should be pushed (by the caller), zero otherwise.  */

int
ccp_need_new_context (const struct compile_cplus_context *pctx)
{
  int i, cur_len, new_len;
  struct compile_cplus_scope *cur_scope, *new_scope;
  struct compile_cplus_context *current
    = get_current_processing_context ();

  if (debug_compile_cplus_contexts)
    {
      printf_unfiltered ("scopes for current context, %p:\n", current);
      debug_print_context (current);
      printf_unfiltered ("comparing to scopes of %p:\n", pctx);
      debug_print_context (pctx);
    }

  /* PCTX is "the same" as the current scope iff
     1. current is not NULL
     2. CONTEXT_SCOPES of current and PCX are the same length.
     3. scope names are the same.  */
  if (current == NULL)
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("no current context -- need new context\n");
      return 1;
    }

  cur_len = VEC_length (compile_cplus_scope_def, CONTEXT_SCOPES (current));
  new_len = VEC_length (compile_cplus_scope_def, CONTEXT_SCOPES (pctx));
  if (cur_len != new_len)
    {
      if (debug_compile_cplus_contexts)
	{
	  printf_unfiltered
	    ("length mismatch: current %d, new %d -- need new context\n",
	     cur_len, new_len);
	}
      return 1;
    }

  for (i = 0;
       (VEC_iterate (compile_cplus_scope_def,
		     CONTEXT_SCOPES (current), i, cur_scope)
	&& VEC_iterate (compile_cplus_scope_def,
			CONTEXT_SCOPES (pctx), i, new_scope)); ++i)
    {
      if (cur_scope->name.length != new_scope->name.length)
	{
	  if (debug_compile_cplus_contexts)
	    {
	      printf_unfiltered
		("current scope namelen != newscope namelen -- need new context.\n");
	    }
	  return 1;
	}
      else
	{
	  const char *cur_name = cur_scope->name.ptr;
	  const char *new_name = new_scope->name.ptr;

	  if (strncmp (cur_name, new_name, cur_scope->name.length) != 0)
	    {
	      if (debug_compile_cplus_contexts)
		{
		  char *a = (char *) alloca (cur_scope->name.length + 1);
		  char *b = (char *) alloca (cur_scope->name.length + 1);

		  strncpy (a, cur_scope->name.ptr, cur_scope->name.length);
		  strncpy (b, new_scope->name.ptr, new_scope->name.length);
		  printf_unfiltered ("%s != %s -- need new context\n",
				     cur_name, new_name);
		}
	      return 1;
	    }
	}
    }

  if (debug_compile_cplus_contexts)
    printf_unfiltered ("all scopes identical -- do NOT need new context\n");
  return 0;
}

/* Push the processing context PCTX into the given compiler CONTEXT.

   The main purpose of this function is to push namespaces, including the
   global namespace, into CONTEXT for type conversions.  */

void
ccp_push_processing_context (struct compile_cplus_instance *instance,
			     struct compile_cplus_context *pctx)
{
  /* Push the scope we are processing.  */
  VEC_safe_push (compile_cplus_context_def, cplus_processing_contexts, pctx);

  /* Push the global namespace.  */
  if (debug_compile_cplus_types)
    printf_unfiltered ("push_namespace \"\"\n");
  CPCALL (push_namespace, instance, "");

  if (CONTEXT_SCOPES (pctx) != NULL)
    {
      int ix;
      struct compile_cplus_scope *scope, *last;

      /* Get the scope we are/will be processing.  */
      last = get_processing_context_scope (pctx);

      /* Push all other namespaces.  */
      for (ix = 0;
	   (VEC_iterate (compile_cplus_scope_def, CONTEXT_SCOPES (pctx),
			 ix, scope)
	    && scope != last);
	   ++ix)
	{
	  char *ns;

	  if (scope->name.length == CP_ANONYMOUS_NAMESPACE_LEN
	      && strncmp (scope->name.ptr, CP_ANONYMOUS_NAMESPACE_STR,
			  CP_ANONYMOUS_NAMESPACE_LEN) == 0)
	    ns = NULL;
	  else
	    ns = copy_stoken (&scope->name);
	  if (debug_compile_cplus_types)
	    printf_unfiltered ("push_namespace %s\n", ns);
	  CPCALL (push_namespace, instance, ns);
	  xfree (ns);
	}
    }
}

/* Pop the processing context PCTX from CONTEXT.

   This is largely used to pop any namespaces that were required to
   define a type from CONTEXT.  */

void
ccp_pop_processing_context (struct compile_cplus_instance *instance,
			    struct compile_cplus_context *pctx)
{
  int ix;
  struct compile_cplus_scope *scope;
  struct compile_cplus_context *ctx;

  /* Pop the context.  */
  ctx = VEC_pop (compile_cplus_context_def, cplus_processing_contexts);
  gdb_assert (ctx == pctx);

  if (CONTEXT_SCOPES (pctx) != NULL)
    {
      struct compile_cplus_scope *last
	= VEC_last (compile_cplus_scope_def, CONTEXT_SCOPES (pctx));

      /* Pop namespaces.  */
      for (ix = 0;
	   (VEC_iterate (compile_cplus_scope_def, CONTEXT_SCOPES (pctx),
			 ix, scope)
	    && scope != last);
	   ++ix)
	{
	  char *ns = copy_stoken (&scope->name);

	  if (debug_compile_cplus_types)
	    printf_unfiltered ("pop_namespace %s\n", ns);
	  CPCALL (pop_namespace, instance);
	  xfree (ns);
	}
    }

  /* Pop global namespace.  */
  if (debug_compile_cplus_types)
    printf_unfiltered ("pop_namespace \"\"\n");
  CPCALL (pop_namespace, instance);
}

/* Create a new processing context for TYPE with name TYPE_NAME.
   [TYPE_NAME could be TYPE_NAME or SYMBOL_NATURAL_NAME.]

   If TYPE is a nested or local definition, *NESTED_TYPE is set to the
   result and this function returns NULL.

   Otherwise, *NESTED_TYPE is set to GCC_TYPE_NONE and a new processing
   context is returned. [See description of get_type_scopes for more.]
   The result should be freed with delete_processing_context.  */

struct compile_cplus_context *
new_processing_context (struct compile_cplus_instance *instance,
			const char *type_name, struct type *type,
			gcc_type *nested_type)
{
  char *name;
  struct cleanup *cleanups;
  struct compile_cplus_context *pctx;

  *nested_type = GCC_TYPE_NONE;
  pctx = XCNEW (struct compile_cplus_context);
  cleanups = make_cleanup (xfree, pctx);

  /* Break the type name into components.  If TYPE was defined in some
     superclass, we do not process TYPE but process the enclosing type
     instead.  */
  CONTEXT_SCOPES (pctx) = ccp_type_name_to_scopes (type_name);
  make_cleanup (VEC_cleanup (compile_cplus_scope_def),
		&CONTEXT_SCOPES (pctx));

  if (CONTEXT_SCOPES (pctx) != NULL)
    {
      struct compile_cplus_scope *scope, *cur_scope = NULL;
      struct compile_cplus_context *cur_context;

      cur_context = get_current_processing_context ();
      if (cur_context != NULL)
	cur_scope = get_processing_context_scope (cur_context);

      /* Get the name of the last component, which should be the
	 unqualified name of the type to process.  */
      scope = get_processing_context_scope (pctx);

      if (!types_equal (type, SYMBOL_TYPE (scope->bsymbol.symbol))
	  && (cur_scope == NULL
	      || cur_scope->bsymbol.symbol != scope->bsymbol.symbol))
	{
	  void **slot;
	  struct type_map_instance inst, *found;

	  /* !!keiths: I don't like this.  This seems like it would
	     get us into hot water.  I think it better to return the
	     struct type of the nested type and have the caller do that
	     instead.  I don't think this function should cause scopes
	     to be pushed.  That should be explicitly done by callers.  */
	  /* The type the oracle asked about is defined inside another
	     class(es).  Convert that type instead of defining this type.  */
	  (void) convert_cplus_type (instance,
				     SYMBOL_TYPE (scope->bsymbol.symbol),
				     GCC_CP_ACCESS_NONE);

	  /* If the original type (passed in to us) is defined in a nested
	     class, the previous call will give us that type's gcc_type.
	     Upper layers are expecting to get the original type's
	     gcc_type!  */
	  inst.type = type;
	  slot = htab_find_slot (instance->type_map, &inst, NO_INSERT);
	  gdb_assert (*slot != NULL);
	  found = (struct type_map_instance *) *slot;
	  *nested_type = found->gcc_type_handle;
	  do_cleanups (cleanups);
	  return NULL;
	}
    }
  else
    {
      struct compile_cplus_scope scope;

      if (TYPE_NAME (type) == NULL)
	{
	  struct compile_cplus_context *current;

	  /* Anonymous type  */
	  name = NULL;
	  /* We don't have a qualified name for this to look up, but
	     we need a scope.  We have to assume, then, that it is the same
	     as the current scope, if any.  */
	  /* !!keiths: FIXME: This isn't quite right.  */
	  current = get_current_processing_context ();
	  if (current != NULL)
	    {
	      CONTEXT_SCOPES (pctx) = VEC_copy (compile_cplus_scope_def,
						CONTEXT_SCOPES (current));
	    }
	  else
	    {
	      scope.bsymbol.block = NULL;
	      scope.bsymbol.symbol = NULL;
	      scope.name.ptr = NULL;
	      scope.name.length = 0;
	      VEC_safe_push (compile_cplus_scope_def, CONTEXT_SCOPES (pctx),
			     &scope);
	    }
	}
      else
	{
	  scope.bsymbol = lookup_symbol (TYPE_NAME (type),
					 get_current_search_block (),
					 VAR_DOMAIN, NULL);
	  name = cp_func_name (TYPE_NAME (type));
	  make_cleanup (xfree, name);
	  scope.name.ptr = name;
	  scope.name.length = strlen (name);

	  VEC_safe_push (compile_cplus_scope_def, CONTEXT_SCOPES (pctx),
			 &scope);
	}
    }

  discard_cleanups (cleanups);
  return pctx;
}

/* Delete the processing context PCTX. */

void
delete_processing_context (struct compile_cplus_context *pctx)
{
  if (pctx != NULL)
    {
      VEC_free (compile_cplus_scope_def, CONTEXT_SCOPES (pctx));
      xfree (pctx);
    }
}

/* Convert the access in ACCESS to a string for printing.  */

static const char *
access_to_string (enum gcc_cp_symbol_kind access)
{
  switch (access & GCC_CP_ACCESS_MASK)
    {
    case GCC_CP_ACCESS_NONE:
      return "access_none";
    case GCC_CP_ACCESS_PUBLIC:
      return "access_public";
    case GCC_CP_ACCESS_PROTECTED:
      return "access_protected";
    case GCC_CP_ACCESS_PRIVATE:
      return "access_private";
    default:
      return "access_???";
    }
}

/* !!keiths: not RVALUE REFERENCES!  */
static gcc_type
convert_reference_base (struct compile_cplus_instance *instance, gcc_type base)
{
  if (debug_compile_cplus_types)
    printf_unfiltered ("build_reference_type\n");

  return CPCALL (build_reference_type, instance, base, GCC_CP_REF_QUAL_LVALUE);
}

/* Convert a reference type to its gcc representation.  */

static gcc_type
ccp_convert_reference (struct compile_cplus_instance *instance,
		       struct type *type)
{
  gcc_type target = convert_cplus_type (instance, TYPE_TARGET_TYPE (type),
					GCC_CP_ACCESS_NONE);

  /* !!keiths: GDB does not currently do anything with rvalue references.
     [Except set the type code to TYPE_CODE_ERROR!  */
  return convert_reference_base (instance, target);
}

/* Convert TARGET into a pointer type in the given compiler INSTANCE.  */

static gcc_type
convert_pointer_base (struct compile_cplus_instance *instance,
		      gcc_type target)
{
  if (debug_compile_cplus_types)
    printf_unfiltered ("build_pointer_type\n");
  return CPCALL (build_pointer_type, instance, target);
}

/* Convert a pointer type to its gcc representation.  */

static gcc_type
ccp_convert_pointer (struct compile_cplus_instance *instance,
		     struct type *type)
{
  gcc_type target = convert_cplus_type (instance, TYPE_TARGET_TYPE (type),
					GCC_CP_ACCESS_NONE);

  return convert_pointer_base (instance, target);
}

/* Convert an array type to its gcc representation.  */

static gcc_type
ccp_convert_array (struct compile_cplus_instance *instance, struct type *type)
{
  gcc_type element_type;
  struct type *range = TYPE_INDEX_TYPE (type);

  element_type = convert_cplus_type (instance, TYPE_TARGET_TYPE (type),
				     GCC_CP_ACCESS_NONE);

  if (TYPE_LOW_BOUND_KIND (range) != PROP_CONST)
    {
      const char *s = _("array type with non-constant"
			" lower bound is not supported");

      if (debug_compile_cplus_types)
	printf_unfiltered ("error %s\n", s);
      return CPCALL (error, instance, s);
    }

  if (TYPE_LOW_BOUND (range) != 0)
    {
      const char *s = _("cannot convert array type with "
			"non-zero lower bound to C");

      if (debug_compile_cplus_types)
	printf_unfiltered ("error %s\n", s);
      return CPCALL (error, instance, s);
    }

  if (TYPE_HIGH_BOUND_KIND (range) == PROP_LOCEXPR
      || TYPE_HIGH_BOUND_KIND (range) == PROP_LOCLIST)
    {
      gcc_type result;
      char *upper_bound;

      if (TYPE_VECTOR (type))
	{
	  const char *s = _("variably-sized vector type is not supported");

	  if (debug_compile_cplus_types)
	    printf_unfiltered ("error %s\n", s);
	  return CPCALL (error, instance, s);
	}

      upper_bound = c_get_range_decl_name (&TYPE_RANGE_DATA (range)->high);
      if (debug_compile_cplus_types)
	printf_unfiltered ("build_vla_array_type\n");
      result = CPCALL (build_vla_array_type, instance,
		       element_type, upper_bound);
      xfree (upper_bound);
      return result;
    }
  else
    {
      LONGEST low_bound, high_bound, count;

      if (get_array_bounds (type, &low_bound, &high_bound) == 0)
	count = -1;
      else
	{
	  gdb_assert (low_bound == 0); /* Ensured above.  */
	  count = high_bound + 1;
	}

      if (TYPE_VECTOR (type))
	{
	  if (debug_compile_cplus_types)
	    printf_unfiltered ("build_vector_type\n");
	  return CPCALL (build_vector_type, instance, element_type, count);
	}

      if (debug_compile_cplus_types)
	printf_unfiltered ("build_array_type\n");
      return CPCALL (build_array_type, instance, element_type, count);
    }
}

/* Convert a typedef of TYPE.  If not GCC_CP_ACCESS_NONE, NESTED_ACCESS
   will define the accessibility of the typedef definition in its
   containing class.  */

static gcc_type
ccp_convert_typedef (struct compile_cplus_instance *instance,
		     struct type *type, enum gcc_cp_symbol_kind nested_access)
{
  int need_new_context;
  gcc_type typedef_type, result;
  char *name;
  struct compile_cplus_context *pctx;
  struct cleanup *cleanups;

  cleanups = make_cleanup (null_cleanup, NULL);
  pctx = new_processing_context (instance, TYPE_NAME (type), type, &result);
  if (result != GCC_TYPE_NONE)
    {
      gdb_assert (pctx == NULL);
      do_cleanups (cleanups);
      return result;
    }

  if (TYPE_NAME (type) != NULL)
    {
      name = cp_func_name (TYPE_NAME (type));
      make_cleanup (xfree, name);
    }
  else
    name = NULL;

  need_new_context = ccp_need_new_context (pctx);
  if (need_new_context)
    ccp_push_processing_context (instance, pctx);

  /* FIXME: I moved this call before the context switching so that
     need_new_context would work as a heuristics for namespace-scoped
     types, but it ends up defining anonymous enum types and constants
     in the wrong context, so it can't stay here.  */
  /* !!keiths: And I'm moving it back.  I think checking accessibility
     should now give you what you're after.  It is not GCC_CP_ACCESS_NONE
     if we're looking at a nested type.  */
  typedef_type = convert_cplus_type (instance, check_typedef (type),
				     GCC_CP_ACCESS_NONE);

  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("new_decl typedef %s, gcc_type = %lld\n", name,
			 typedef_type);
    }
  CPCALL (new_decl, instance,
	  name,
	  GCC_CP_SYMBOL_TYPEDEF | nested_access,
	  typedef_type,
	  0,
	  0,
	  /* !!keiths: Wow. More of this!  */
	  NULL,
	  0);

  if (need_new_context)
    ccp_pop_processing_context (instance, pctx);

  delete_processing_context (pctx);
  do_cleanups (cleanups);
  return typedef_type;
}

/* Convert types defined in TYPE.  */

static void
ccp_convert_type_defns (struct compile_cplus_instance *instance,
			struct type *type)
{
  int i;
  enum gcc_cp_symbol_kind accessibility;

  /* Convert typedefs.  */
  for (i = 0; i < TYPE_TYPEDEF_FIELD_COUNT (type); ++i)
    {
      if (TYPE_TYPEDEF_FIELD_PUBLIC (type, i))
	accessibility = GCC_CP_ACCESS_PUBLIC;
      else if (TYPE_TYPEDEF_FIELD_PROTECTED (type, i))
	accessibility = GCC_CP_ACCESS_PROTECTED;
      else if (TYPE_TYPEDEF_FIELD_PRIVATE (type, i))
	accessibility = GCC_CP_ACCESS_PRIVATE;
      else
	gdb_assert_not_reached ("unknown accessibility");
      (void) convert_cplus_type (instance,
				 TYPE_TYPEDEF_FIELD_TYPE (type, i),
				 accessibility);
    }

  /* Convert nested types.  */
  for (i = 0; i < TYPE_NESTED_TYPES_COUNT (type); ++i)
    {
      if (TYPE_NESTED_TYPES_FIELD_PUBLIC (type, i))
	accessibility = GCC_CP_ACCESS_PUBLIC;
      else if (TYPE_NESTED_TYPES_FIELD_PROTECTED (type, i))
	accessibility = GCC_CP_ACCESS_PROTECTED;
      else if (TYPE_NESTED_TYPES_FIELD_PRIVATE (type, i))
	accessibility = GCC_CP_ACCESS_PRIVATE;
      else
	gdb_assert_not_reached ("unknown accessibility");
      (void) convert_cplus_type (instance,
				 TYPE_NESTED_TYPES_FIELD_TYPE (type, i),
				 accessibility);
    }
}

/* Convert data members defined in TYPE, which should be struct/class/union
   with gcc_type COMP_TYPE.  */

static void
ccp_convert_struct_or_union_members (struct compile_cplus_instance *instance,
				     struct type *type, gcc_type comp_type)
{
  int i;

  for (i = TYPE_N_BASECLASSES (type); i < TYPE_NFIELDS (type); ++i)
    {
      CORE_ADDR physaddr;
      gcc_type field_type;
      const char *field_name = TYPE_FIELD_NAME (type, i);

      if (TYPE_FIELD_IGNORE (type, i)
	  || TYPE_FIELD_ARTIFICIAL (type, i))
	continue;

      field_type
	= convert_cplus_type (instance, TYPE_FIELD_TYPE (type, i),
			      GCC_CP_ACCESS_NONE);

      if (field_is_static (&TYPE_FIELD (type, i)))
	{
	  switch (TYPE_FIELD_LOC_KIND (type, i))
	    {
	    case FIELD_LOC_KIND_PHYSADDR:
	      {
		physaddr = TYPE_FIELD_STATIC_PHYSADDR (type, i);

		if (debug_compile_cplus_types)
		  printf_unfiltered ("new_decl static variable %s at %s\n",
		       field_name, core_addr_to_string (physaddr));
		CPCALL (new_decl, instance,
			field_name,
			(GCC_CP_SYMBOL_VARIABLE
			 | get_field_access_flag (type, i)),
			field_type,
			NULL,
			physaddr,
			// FIXME: do we have
			// location info for
			// static data members?
			// -lxo
			NULL,
			0);
	      }
	      break;

	    case FIELD_LOC_KIND_PHYSNAME:
	      {
		const char *physname = TYPE_FIELD_STATIC_PHYSNAME (type, i);
		struct block_symbol sym
		  = lookup_symbol (physname, get_current_search_block (),
				   VAR_DOMAIN, NULL);
		const char *filename;
		unsigned int line;

		if (sym.symbol == NULL)
		  {
		    /* We didn't actually find the symbol.  There's little
		       we can do but ignore this member.  */
		    continue;
		  }
		filename = symbol_symtab (sym.symbol)->filename;
		line = SYMBOL_LINE (sym.symbol);
		physaddr = SYMBOL_VALUE_ADDRESS (sym.symbol);
		if (debug_compile_cplus_types)
		  {
		    printf_unfiltered
		      ("new_decl static variable from physname %s (%s)\n",
		       field_name,
		       core_addr_to_string (physaddr));
		  }
		CPCALL (new_decl, instance,
			field_name,
			(GCC_CP_SYMBOL_VARIABLE
			 | get_field_access_flag (type, i)),
			field_type,
			NULL,
			physaddr,
			filename,
			line);
	      }
	      break;

	    default:
	      gdb_assert_not_reached
		("unexpected static field location kind");
	    }
	}
      else
	{
	  /* !!keiths: I am guessing this is insufficient... */
	  unsigned long bitsize = TYPE_FIELD_BITSIZE (type, i);
	  enum gcc_cp_symbol_kind field_flags = GCC_CP_SYMBOL_FIELD
	    | get_field_access_flag (type, i)
	  /* FIXME:
	    | (field-is-mutable-p (type, i)
	       ? GCC_CP_FLAG_FIELD_MUTABLE
	       : GCC_CP_FLAG_FIELD_NOFLAG)
	     -lxo */
	    ;

	  if (bitsize == 0)
	    bitsize = 8 * TYPE_LENGTH (TYPE_FIELD_TYPE (type, i));

	  if (debug_compile_cplus_types)
	    {
	      printf_unfiltered ("new_field %s, gcc_type = %lld\n",
				 field_name, field_type);
	    }
	  /* FIXME: We have to save the returned decl somewhere, so
	     that we can refer to it in expressions, in context for
	     lambdas, etc.  */
	  CPCALL (new_field, instance,
		  field_name,
		  field_type,
		  field_flags,
		  bitsize,
		  TYPE_FIELD_BITPOS (type, i));
	}
    }
}

/* Convert a method type to its gcc representation.  */

static gcc_type __attribute__ ((used))
ccp_convert_method (struct compile_cplus_instance *instance,
		    struct type *parent_type, struct type *method_type)
{
  gcc_type result, func_type, class_type;
  gcc_cp_qualifiers_flags quals;
  gcc_cp_ref_qualifiers_flags rquals;

  /* Get the actual (proto)type of the method, as a function.  */
  func_type = ccp_convert_func (instance, method_type, 1);

  class_type = convert_cplus_type (instance, parent_type, GCC_CP_ACCESS_NONE);
  quals = (enum gcc_cp_qualifiers) 0; // !!keiths FIXME
  rquals = GCC_CP_REF_QUAL_NONE; // !!keiths FIXME
  if (debug_compile_cplus_types)
    printf_unfiltered ("build_method_type\n");
  result = CPCALL (build_method_type, instance,
		   class_type, func_type, quals, rquals);
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);
  return result;
}

#define OPHASH1(A) ((uint32_t) A << 16)
#define OPHASH2(A,B) OPHASH1(A) | (uint32_t) B << 8
#define OPHASH3(A,B,C) OPHASH2(A,B) | (uint32_t) C

/* Compute a one, two, or three letter hash for the operator given by
   OP.  Returns the computed hash or zero for (some) conversion operators.  */

static uint32_t
operator_hash (const char *op)
{
  const char *p = op;
  uint32_t hash = 0;
  int len = 0;

  while (p[0] != '\0' && (p[0] != '(' || p[1] == ')'))
    {
      ++len;
      ++p;
    }

  switch (len)
    {
    case 1:
      hash = OPHASH1(op[0]);
      break;
    case 2:
     hash = OPHASH2(op[0], op[1]);
     break;
    case 3:
      /* This will also hash "operator int", but the plug-in does not
	 recognize OPHASH3('i', 'n', 't'), so we still end up in the code
	 to do a conversion operator in the caller.  */
      hash = OPHASH3(op[0], op[1], op[2]);
      break;
    default:
      break;
    }

  return hash;
}

/* Returns non-zero iff TYPE represents a binary method.  */

static int
is_binary_method (const struct type *type)
{
  int i, len;

  for (i = 0, len = 0; i < TYPE_NFIELDS (type); ++i)
    {
      if (!TYPE_FIELD_ARTIFICIAL (type, i))
	++len;
    }

 return len > 1;
}

/* See compile-internal.h.  */

const char *
maybe_canonicalize_special_function (const char *field_name,
				     const struct fn_field *method_field,
				     const struct type *method_type,
				     char **outname, int *ignore)
{
  /* We assume that no method is to be ignored.  */
  *ignore = 0;

  /* We only consider ctors and dtors if METHOD_FIELD is non-NULL.  */
  /* !!keiths: Ick.  Maybe we can look it up here instead if it is NULL?  */
  if (method_field != NULL)
    {
      if (method_field->is_constructor)
	{
#if DEBUG_XTOR
	  printf ("*** CONSTRUCTOR %s: ", field_name);
#endif
#if CAUSES_ICE
	  switch (method_field->cdtor_type.ctor_kind)
	    {
	    case complete_object_ctor:
#if DEBUG_XTOR
	      printf ("complete_object_ctor (C1)\n");
#endif
	      return "C1";

	    case base_object_ctor:
#if DEBUG_XTOR
	      printf ("base_object_ctor (C2)\n");
#endif
	      return "C2";

	    case complete_object_allocating_ctor:
	      *ignore = 1;
#if DEBUG_XTOR
	      printf ("complete_object_allocating_ctor -- ignored\n");
#endif
	      return field_name; /* C?  */

	    case unified_ctor:
#if DEBUG_XTOR
	      printf ("unified_ctor (C4) -- ignored\n");
#endif
	      *ignore = 1;
	      return field_name; /* C4  */

	    case object_ctor_group:
#if DEBUG_XTOR
	      printf ("object_ctor_group -- ignored\n");
#endif
	      *ignore = 1;
	      return field_name; /* C?  */

	    case unknown_ctor:
#if DEBUG_XTOR
	      printf ("unknown_ctr -- ignored\n");
#endif
	      *ignore = 1;
	      return field_name; /* unknown  */

	    default:
	      gdb_assert_not_reached ("unknown constructor kind");
	    }
#else
#if DEBUG_XTOR
	  printf ("DISABLED -- ignored\n");
#endif
	  *ignore = 1;
#endif /* CAUSES_ICE  */
	}
      else  if (method_field->is_destructor)
	{
#if DEBUG_XTOR
	  printf ("*** DESTRUCTOR %s: ", field_name);
#endif
	  switch (method_field->cdtor_type.dtor_kind)
	    {
	    case deleting_dtor:
#if DEBUG_XTOR
	      printf ("deleting_dtor (D0)\n");
#endif
	      return "D0";

	    case complete_object_dtor:
#if DEBUG_XTOR
	      printf ("complete_object_dtor (D1)\n");
#endif
	      return "D1";

	    case base_object_dtor:
#if DEBUG_XTOR
	      printf ("base_object_dtor (D2)\n");
#endif
	      return "D2";

	    case unified_dtor:
#if DEBUG_XTOR
	      printf ("unified_dtor (D4) -- ignored\n");
#endif
	      *ignore = 1;
	      return field_name; /* D4  */

	    case object_dtor_group:
#if DEBUG_XTOR
	      printf ("object_dtor_group (D?) -- ignored\n");
#endif
	      *ignore = 1;
	      return field_name; /* D?  */

	    case unknown_dtor:
#if DEBUG_XTOR
	      printf ("unknown_dtor -- ignored\n");
#endif
	      *ignore = 1;
	      return field_name; /* unknown  */

	    default:
	      gdb_assert_not_reached ("unknown destructor kind");
	    }
	}
    }

  if (!is_operator_name (field_name))
    return field_name;

  /* Skip over "operator".  */
  field_name += sizeof ("operator") - 1;

  if (strncmp (field_name, "new", sizeof ("new") - 1) == 0)
    {
      field_name += 3;
      if (*field_name == '\0')
	return "nw";
      else if (*field_name == '[')
	return "na";
    }
  else if (strncmp (field_name, "delete", sizeof ("delete") - 1) == 0)
    {
      if (*field_name == '\0')
	return "dl";
      else if (*field_name == '[')
	return "da";
    }
  else if (field_name[0] == '\"' && field_name[1] == '\"')
    {
      const char *end;
      size_t len;

      /* Skip over \"\" -- the plug-in doesn't want it.  */
      field_name += 2;

      /* Skip any whitespace that may have been introduced during
	 canonicalization.  */
      field_name = skip_spaces_const (field_name);

      /* Find the opening '(', if any.  */
      end = strchr (field_name, '(');
      if (end == NULL)
	end = field_name + strlen (field_name);

      /* Size of buffer: 'li', 'i', sizeof operator name, '\0'  */
      len = 2 + end - field_name + 1;
      *outname = (char *) xmalloc (len);
      strcpy (*outname, "li");
      strncat (*outname, field_name, end - field_name);
      (*outname)[len-1] = '\0';
      return "li";
    }

  switch (operator_hash (field_name))
    {
    case OPHASH1 ('+'):
      if (is_binary_method (method_type))
	return "pl";
      else
	return "ps";
      break;

    case OPHASH1 ('-'):
      if (is_binary_method (method_type))
	return "mi";
      else
	return "ng";
      break;
    case OPHASH1 ('&'):
      if (is_binary_method (method_type))
	return "an";
      else
	return "ad";
      break;

    case OPHASH1 ('*'):
      if (is_binary_method (method_type))
	return "ml";
      else
	return "de";
      break;

    case OPHASH1 ('~'):
      return "co";
    case OPHASH1 ('/'):
      return "dv";
    case OPHASH1 ('%'):
      return "rm";
    case OPHASH1 ('|'):
      return "or";
    case OPHASH1 ('^'):
      return "eo";
    case OPHASH1 ('='):
      return "aS";
    case OPHASH2 ('+', '='):
      return "pL";
    case OPHASH2 ('-', '='):
      return "mI";
    case OPHASH2 ('*', '='):
      return "mL";
    case OPHASH2 ('/', '='):
      return "dV";
    case OPHASH2 ('%', '='):
      return "rM";
    case OPHASH2 ('&', '='):
      return "aN";
    case OPHASH2 ('|', '='):
      return "oR";
    case OPHASH2 ('^', '='):
      return "eO";
    case OPHASH2 ('<', '<'):
      return "ls";
    case OPHASH2 ('>', '>'):
      return "rs";
    case OPHASH3 ('<', '<', '='):
      return "lS";
    case OPHASH3 ('>', '>', '='):
      return "rS";
    case OPHASH2 ('=', '='):
      return "eq";
    case OPHASH2 ('!', '='):
      return "ne";
    case OPHASH1 ('<'):
      return "lt";
    case OPHASH1 ('>'):
      return "gt";
    case OPHASH2 ('<', '='):
      return "le";
    case OPHASH2 ('>', '='):
      return "ge";
    case OPHASH1 ('!'):
      return "nt";
    case OPHASH2 ('&', '&'):
      return "aa";
    case OPHASH2 ('|', '|'):
      return "oo";
    case OPHASH2 ('+', '+'):
      return "pp";
    case OPHASH2 ('-', '-'):
      return "mm";
    case OPHASH1 (','):
      return "cm";
    case OPHASH3 ('-', '>', '*'):
      return "pm";
    case OPHASH2 ('-', '>'):
      return "pt";
    case OPHASH2 ('(', ')'):
      return "cl";
    case OPHASH2 ('[', ']'):
      return "ix";
    case OPHASH1 ('?'):
      return "qu";

    default:
      /* Conversion operators: Full name is not needed.  */
      return "cv";
    }
}

/* Convert all methods defined in TYPE, which should be a class/struct/union
   with gcc_type CLASS_TYPE.  */

static void
ccp_convert_struct_or_union_methods (struct compile_cplus_instance *instance,
				     struct type *type, gcc_type class_type)
{
  int i;

  /* First things first: If this class had any template methods, emit them so
     that the compiler knows about them.  */
  ccp_emit_function_template_decls (instance);


  /* Now define the actual methods/template specializations.  */
  for (i = 0; i < TYPE_NFN_FIELDS (type); ++i)
    {
      int j;
      struct fn_field *methods = TYPE_FN_FIELDLIST1 (type, i);
      char *overloaded_name = ccp_decl_name (TYPE_FN_FIELDLIST_NAME (type, i));
      struct cleanup *outer = make_cleanup (xfree, overloaded_name);

      /* Loop through the fieldlist, adding decls to the compiler's
	 representation of the class.  */
      for (j = 0; j < TYPE_FN_FIELDLIST_LENGTH (type, i); ++j)
	{
#if 0
	  int qual_flags = 0;
	  /* pmuldoon: Need to detect LVALUE/RVALUE Qualifiers here. */
	  int ref_qual_flags = GCC_CP_REF_QUAL_NONE;
	  gcc_type prototype;

	  /* Skip artificial methods (for now?) */
	  if (! TYPE_FN_FIELD_ARTIFICIAL (methods, j))
	    {
	      struct type *temp_type = TYPE_FN_FIELD_TYPE (methods, j);

	      /* Convert to a function, first. */
	      prototype = convert_cplus_type (instance, temp_type);

	      if (TYPE_CONST (temp_type))
		qual_flags |= GCC_CP_QUALIFIER_CONST;
	      if (TYPE_VOLATILE (temp_type))
		qual_flags |= GCC_CP_QUALIFIER_VOLATILE;
	      if (TYPE_RESTRICT (temp_type))
		qual_flags |= GCC_CP_QUALIFIER_RESTRICT;

	      if (debug_compile_cplus_types)
		printf_unfiltered ("build_method_type\n");
	      CPCALL (build_method_type, instance,
		      class_type, prototype, qual_flags, ref_qual_flags);
	    }
#else
	  CORE_ADDR address;
	  gcc_type method_type;
	  struct block_symbol sym;
	  const char *filename;
	  unsigned int line;
	  const char *kind; //debug
	  gcc_cp_symbol_kind_flags sym_kind = GCC_CP_SYMBOL_FUNCTION;
	  const char *name;
	  char *special_name;
	  struct cleanup *back_to;
	  int ignore;
	  struct template_symbol *tsym = NULL;

	  /* Skip artificial methods.  */
	  if (TYPE_FN_FIELD_ARTIFICIAL (methods, j))
	    continue;

	  special_name = NULL;
	  name = maybe_canonicalize_special_function (overloaded_name,
						      &methods[j],
						      methods[j].type,
						      &special_name,
						      &ignore);
	  if (ignore)
	    continue;

	  back_to = make_cleanup (null_cleanup, NULL);

	  if (special_name != NULL)
	    {
	      make_cleanup (xfree, special_name);
	      name = special_name;
	    }

	  if (name != overloaded_name)
	    {
	      sym_kind |= GCC_CP_FLAG_SPECIAL_FUNCTION;
	    }
	  sym = lookup_symbol (TYPE_FN_FIELD_PHYSNAME (methods, j),
			       get_current_search_block (), VAR_DOMAIN, NULL);

	  if (sym.symbol == NULL)
	    {
	      if (TYPE_FN_FIELD_VIRTUAL_P (methods, j))
		{
		  /* !!keiths: This is beyond hacky, and is really only a
		     lame workaround for detecting pure virtual methods.  */
		  if (debug_compile_cplus_types)
		    {
		      printf_unfiltered ("new_decl pure virtual method %s\n",
					 name);
		    }
		  kind = "";
		  method_type
		    = ccp_convert_method (instance, type,
					  TYPE_FN_FIELD_TYPE (methods, j));

		  CPCALL (new_decl, instance,
			  name,
			  (sym_kind
			   | get_method_access_flag (type, i, j)
			   | GCC_CP_FLAG_VIRTUAL_FUNCTION
			   | GCC_CP_FLAG_PURE_VIRTUAL_FUNCTION),
			  method_type,
			  NULL,
			  0,
			  NULL,
			  0);
		  do_cleanups (back_to);
		  continue;
		}

	      /* This can happen if we have a DW_AT_declaration DIE
		 for the method, but no "definition"-type DIE (with
		 DW_AT_specification referencing the decl DIE), i.e.,
		 the compiler has probably optimized the method away.

		 In this case, all we can hope to do is issue a warning
		 to the user letting him know.  If the user has not actually
		 requested using this method, things should still work.  */
	      warning (_("Method %s appears to be optimized out.\n"
			 "All references to this method will be undefined."),
			 TYPE_FN_FIELD_PHYSNAME (methods, j));
	      do_cleanups (back_to);
	      continue;
	    }

	  filename = symbol_symtab (sym.symbol)->filename;
	  line = SYMBOL_LINE (sym.symbol);
	  /* !!keiths: Is this sufficient?  */
	  address = BLOCK_START (SYMBOL_BLOCK_VALUE (sym.symbol));

	  /* Short-circuit for method templates.  */
	  if (SYMBOL_IS_CPLUS_TEMPLATE_FUNCTION (sym.symbol))
	    {
	      int x;
	      struct template_symbol *tsym;
	      struct function_template_defn *defn;
	      struct gcc_cp_template_args targs;

	      tsym = (struct template_symbol *) sym.symbol;
	      defn = find_function_template_defn (instance, tsym);

	      /* All templates must have been defined already.  If not, someone
		 goofed.  */
	      gdb_assert (defn != NULL);

	      /* Build and output the specialization for this method
		 template.  */
	      targs.n_elements = tsym->template_arguments->n_arguments;
	      targs.kinds = XNEWVEC (char, targs.n_elements);
	      make_cleanup (xfree, targs.kinds);
	      targs.elements = XNEWVEC (gcc_cp_template_arg,
					targs.n_elements);
	      make_cleanup (xfree, targs.elements);
	      enumerate_template_arguments (instance, &targs, defn,
					    tsym->template_arguments);

	      if (debug_compile_cplus_types)
		{
		  printf_unfiltered ("specialize_function_template %s\n",
				     SYMBOL_NATURAL_NAME (sym.symbol));
		}
	      CPCALL (specialize_function_template, instance,
		      get_template_decl (defn),
		      &targs,
		      address,
		      filename,
		      line);

	      do_cleanups (back_to);
	      continue;
	    }

	  if (TYPE_FN_FIELD_STATIC_P (methods, j))
	    {
	      kind = "static";
	      method_type = ccp_convert_func (instance,
					      TYPE_FN_FIELD_TYPE (methods, j),
					      1);
	    }
	  else
	    {
	      kind = "";
	      method_type
		= ccp_convert_method (instance, type,
				      TYPE_FN_FIELD_TYPE (methods, j));
	    }

	  if (TYPE_FN_FIELD_VIRTUAL_P (methods, j))
	    sym_kind |= GCC_CP_FLAG_VIRTUAL_FUNCTION;

	  /* FIXME: for cdtors, we must call new_decl with a zero
	     address, if we haven't created the base declaration
	     yet, and then define_cdtor_clone with the address of
	     each clone.  When we leave the address out, GCC uses
	     the address oracle.  -lxo  */
	  if ((sym_kind & GCC_CP_FLAG_SPECIAL_FUNCTION)
	      && (name[0] == 'C' || name[0] == 'D'))
	    {
	      address = 0;
	      /* FIXME: We should declare only one cdtor for each
		 clone set with "C" or "D" as the name, with address
		 zero, then define each address with
		 define_cdtor_clone.  Until this is implemented,
		 declare only one of these, and let the address oracle
		 take care of the addresses.  -lxo */
	      if (name[1] != '2' && name[1] != '4')
		{
		  do_cleanups (back_to);
		  continue;
		}
	    }

	  if (debug_compile_cplus_types)
	    {
	      printf_unfiltered ("new_decl%s method %s at %s\n", kind, name,
				 core_addr_to_string (address));
	    }
	  CPCALL (new_decl, instance,
		  name,
		  sym_kind | get_method_access_flag (type, i, j),
		  method_type,
		  NULL,
		  address,
		  filename,
		  line);
	  do_cleanups (back_to);
#endif
	}

      do_cleanups (outer);
    }
}

/* Scan the methods of TYPE looking for any templates, defining
   templates (but not emitting them) and any default argument types.  */

static void
ccp_scan_type_for_templates (struct compile_cplus_instance *instance,
			     struct type *type)
{
  int i;

  for (i = 0; i < TYPE_NFN_FIELDS (type); ++i)
    {
      int j;
      struct fn_field *methods = TYPE_FN_FIELDLIST1 (type, i);

      for (j = 0; j < TYPE_FN_FIELDLIST_LENGTH (type, i); ++j)
	{
	  struct block_symbol sym
	    = lookup_symbol (TYPE_FN_FIELD_PHYSNAME (methods, j),
			     get_current_search_block (), VAR_DOMAIN, NULL);

	  ccp_maybe_define_new_function_template (instance, sym.symbol,
						  type, i, j);
	}
    }
}

/* Convert a struct or union type to its gcc representation.  If this type
   was defined in another type, NESTED_ACCESS should indicate the
   accessibility of this type.  */

static gcc_type
ccp_convert_struct_or_union (struct compile_cplus_instance *instance,
			     struct type *type,
			     enum gcc_cp_symbol_kind nested_access)
{
  int i, need_new_context;
  gcc_type result;
  gcc_decl resuld; /* FIXME: yeah, it's a terrible pun.  Please make
		      it go once we separate declaration from
		      definition (see below).  -lxo */
  struct compile_cplus_context *pctx;
  char *name = NULL;
  const char *filename = NULL;  /* !!keiths: FIXME  */
  unsigned short line = 0;
  struct cleanup *cleanups;
  struct class_template_defn *defn;

  /* Create an empty cleanup chain.  */
  cleanups = make_cleanup (null_cleanup, NULL);

  /* Get the decl name of this type.  */
  if (TYPE_NAME (type) != NULL)
    {
      name = ccp_decl_name (TYPE_NAME (type));
      make_cleanup (xfree, name);
    }
  else
    {
      /* !!keiths: Wow.  */
      if (TYPE_CODE (type) == TYPE_CODE_STRUCT)
	name = "anonymous struct";
      else
	name = "anonymous union";
    }

  /* First things first: If this type has any templates in it, make sure
     that we collect default arguments and get those types defined BEFORE
     this type is defined.  */
  ccp_scan_type_for_templates (instance, type);

  /* !!keiths: If this is a new template class, define the template
     and then do the specialization.  */
  ccp_maybe_define_new_class_template (instance, type, name);

  /* Emit any template class decls.  */
  ccp_emit_class_template_decls (instance);

  /* Create a new processing context for TYPE.  */
  pctx = new_processing_context (instance, TYPE_NAME (type), type, &result);
  if (result != GCC_TYPE_NONE)
    {
      /* The type requested was actually defined inside another type,
	 such as a nested class definition.  Return that type.  */
      gdb_assert (pctx == NULL);
      do_cleanups (cleanups);
      return result;
    }

  /* Push all scopes.  */
  need_new_context = ccp_need_new_context (pctx);
  if (need_new_context)
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("entering new processing scope %p\n", pctx);
      ccp_push_processing_context (instance, pctx);
    }
  else
    {
      if (debug_compile_cplus_contexts)
	{
	  printf_unfiltered
	    ("staying in current scope -- scopes are identical\n");
	}
    }

  /* First we create the resulting type and enter it into our hash
     table.  This lets recursive types work.  */

  defn = find_class_template_defn (instance, type);
  if (defn != NULL)
    {
      struct gcc_cp_template_args args;

      args.n_elements = TYPE_N_TEMPLATE_ARGUMENTS (defn->type);
      args.kinds = XNEWVEC (char, args.n_elements);
      make_cleanup (xfree, args.kinds);
      args.elements = XNEWVEC (gcc_cp_template_arg, args.n_elements);
      make_cleanup (xfree, args.elements);
      enumerate_template_arguments (instance, &args, defn,
				    TYPE_TEMPLATE_ARGUMENT_INFO (defn->type));
      if (debug_compile_cplus_types)
	printf_unfiltered ("specialize_class_template %s for template decl %lld\n",
			   name, defn->decl);
      resuld = CPCALL (specialize_class_template, instance,
		       defn->decl, &args, filename, line);
    }
  else if (TYPE_CODE (type) == TYPE_CODE_STRUCT)
    {
      if (debug_compile_cplus_types)
	printf_unfiltered  ("new_decl for class %s\n", name);
      resuld = CPCALL (new_decl, instance, name,
		       GCC_CP_SYMBOL_CLASS | nested_access
		       | (TYPE_DECLARED_CLASS (type)
			  ? GCC_CP_FLAG_CLASS_NOFLAG
			  : GCC_CP_FLAG_CLASS_IS_STRUCT),
		       0, NULL, 0, filename, line);
    }
  else
    {
      gdb_assert (TYPE_CODE (type) == TYPE_CODE_UNION);
      if (debug_compile_cplus_types)
	printf_unfiltered ("new_decl for union type %s\n", name);
      resuld = CPCALL (new_decl, instance, name,
		       GCC_CP_SYMBOL_UNION | nested_access,
		       0, NULL, 0, filename, line);
    }
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_decl = %lld\n", resuld);

  /* FIXME: we should be able to pop the context at this point, rather
     than at the end, and we ought to delay the rest of this function
     to the point in which we need the class or union to be a complete
     type, otherwise some well-formed C++ types cannot be represented.
     -lxo */

  if (TYPE_CODE (type) == TYPE_CODE_STRUCT)
    {
      struct gcc_vbase_array bases;
      int num_baseclasses = TYPE_N_BASECLASSES (type);

      memset (&bases, 0, sizeof (bases));

      if (num_baseclasses > 0)
	{
	  bases.elements = XNEWVEC (gcc_type, num_baseclasses);
	  bases.flags = XNEWVEC (enum gcc_cp_symbol_kind, num_baseclasses);
	  bases.n_elements = num_baseclasses;
	  for (i = 0; i < num_baseclasses; ++i)
	    {
	      struct type *base_type = TYPE_BASECLASS (type, i);

	      bases.flags[i] = GCC_CP_SYMBOL_BASECLASS
		| get_field_access_flag (type, i)
		| (BASETYPE_VIA_VIRTUAL (type, i)
		   ? GCC_CP_FLAG_BASECLASS_VIRTUAL
		   : GCC_CP_FLAG_BASECLASS_NOFLAG);
	      bases.elements[i] = convert_cplus_type (instance, base_type,
						      GCC_CP_ACCESS_NONE);
	    }
	}

      if (debug_compile_cplus_types)
	printf_unfiltered  ("start_class_definition for class %s\n", name);
      result = CPCALL (start_class_definition, instance, resuld,
		       &bases, filename, line);
      if (debug_compile_cplus_types)
	printf_unfiltered ("\tgcc_type = %lld\n", result);

      xfree (bases.flags);
      xfree (bases.elements);
    }
  else
    {
      gdb_assert (TYPE_CODE (type) == TYPE_CODE_UNION);

      if (debug_compile_cplus_types)
	printf_unfiltered ("start_class_definition %s\n", name);
      result = CPCALL (start_class_definition, instance, resuld,
		       NULL, filename, line);
      if (debug_compile_cplus_types)
	printf_unfiltered ("\tgcc_type = %lld\n", result);
    }

  insert_type (instance, type, result);

  /* Add definitions.  */
  ccp_convert_type_defns (instance, type);

  /* Add methods.  */
  ccp_convert_struct_or_union_methods (instance, type, result);

  /* Add members.  */
  ccp_convert_struct_or_union_members (instance, type, result);

  /* FIXME: add friend declarations.  -lxo  */

  /* All finished.  */
  if (debug_compile_cplus_types)
    printf_unfiltered ("finish_record_or_union %s (%lld)\n", name, result);
  CPCALL (finish_record_or_union, instance, TYPE_LENGTH (type));

  /* Pop all scopes.  */
  if (need_new_context)
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("leaving processing scope %p\n", pctx);
      ccp_pop_processing_context (instance, pctx);
    }
  else
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("identical contexts -- not leaving context\n");
    }
  delete_processing_context (pctx);

  do_cleanups (cleanups);
  return result;
}

/* Convert an enum type to its gcc representation.  If this type
   was defined in another type, NESTED_ACCESS should indicate the
   accessibility of this type.*/

static gcc_type
ccp_convert_enum (struct compile_cplus_instance *instance, struct type *type,
		  enum gcc_cp_symbol_kind nested_access)
{
  int i, need_new_context;
  gcc_type int_type, result;
  struct compile_cplus_context *pctx;
  char *name = NULL;
  const char *filename = NULL;
  unsigned short line = 0;
  struct cleanup *cleanups;
  /* !!keiths: This does not appear to work. GCC complains about
     being unable to convert enum values from '(MyEnum)0' to 'int'.  */
  int scoped_enum_p = /*TYPE_DECLARED_CLASS (type) ? TRUE :*/ FALSE;

  /* Create an empty cleanup chain.  */
  cleanups = make_cleanup (null_cleanup, NULL);

  /* Create a new processing context for TYPE.  */
  pctx = new_processing_context (instance, TYPE_NAME (type), type, &result);
  if (result != GCC_TYPE_NONE)
    {
      /* The type requested was actually defined inside another type,
	 such as a nested class definition.  Return that type.  */
      gdb_assert (pctx == NULL);
      do_cleanups (cleanups);
      return result;
    }

  if (TYPE_NAME (type) != NULL)
    {
      name = cp_func_name (TYPE_NAME (type));
      make_cleanup (xfree, name);
    }
  else
    {
      /* !!keiths: Wow.  */
      name = "anonymous enum";
    }

  /* Push all scopes.  */
  need_new_context = ccp_need_new_context (pctx);
  if (need_new_context)
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("entering new processing scope %p\n", pctx);
      ccp_push_processing_context (instance, pctx);
    }
  else
    {
      if (debug_compile_cplus_contexts)
	{
	  printf_unfiltered
	    ("staying in current scope -- scopes are identical\n");
	}
    }

  // FIXME: drop any namespaces and enclosing class names, if any. -lxo
  /* !!keiths: Why?  Drop or push, just like convert_struct_or_union?  */
  // Drop them from "name", if they're there at all.

  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("int_type %d %d\n", TYPE_UNSIGNED (type),
			 TYPE_LENGTH (type));
    }
  int_type = CPCALL (int_type, instance,
		     TYPE_UNSIGNED (type), TYPE_LENGTH (type), NULL);
  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("\tgcc_type = %lld\n", int_type);
      printf_unfiltered ("start_new_enum_type %s %s\n", name,
			 access_to_string (nested_access));
    }
  result = CPCALL (start_new_enum_type, instance,
		   name, int_type,
		   GCC_CP_SYMBOL_ENUM | nested_access
		   | (scoped_enum_p
		      ? GCC_CP_FLAG_ENUM_SCOPED
		      : GCC_CP_FLAG_ENUM_NOFLAG),
		   filename, line);
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);

  for (i = 0; i < TYPE_NFIELDS (type); ++i)
    {
      char *fname = cp_func_name (TYPE_FIELD_NAME (type, i));

      if (TYPE_FIELD_LOC_KIND (type, i) != FIELD_LOC_KIND_ENUMVAL
	  || fname == NULL)
	continue;

      if (debug_compile_cplus_types)
	printf_unfiltered ("build_add_enum_constant %s = %ld\n", fname,
	     TYPE_FIELD_ENUMVAL (type, i));
      CPCALL (build_add_enum_constant, instance,
	      result, fname, TYPE_FIELD_ENUMVAL (type, i));
      xfree (fname);
    }

  if (debug_compile_cplus_types)
    printf_unfiltered ("finish_enum_type %s (%lld)\n", name, result);
  CPCALL (finish_enum_type, instance, result);

  /* Pop all scopes.  */
  if (need_new_context)
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("leaving processing scope %p\n", pctx);
      ccp_pop_processing_context (instance, pctx);
    }
  else
    {
      if (debug_compile_cplus_contexts)
	printf_unfiltered ("identical contexts -- not leaving context\n");
    }

  /* Delete the processing context.  */
  delete_processing_context (pctx);

  do_cleanups (cleanups);
  return result;
}

/* Convert a function type to its gcc representation.  This function does
   not deal with function templates.  */

static gcc_type
ccp_convert_func (struct compile_cplus_instance *instance, struct type *type,
		  int strip_artificial)
{
  int i, artificials;
  gcc_type result, return_type;
  struct gcc_type_array array;
  int is_varargs = ccp_is_varargs_p (type);

  /* This approach means we can't make self-referential function
     types.  Those are impossible in C, though.  */
  return_type = convert_cplus_type (instance, TYPE_TARGET_TYPE (type),
				    GCC_CP_ACCESS_NONE);

  array.n_elements = TYPE_NFIELDS (type);
  array.elements = XNEWVEC (gcc_type, TYPE_NFIELDS (type));
  artificials = 0;
  for (i = 0; i < TYPE_NFIELDS (type); ++i)
    {
      if (strip_artificial && TYPE_FIELD_ARTIFICIAL (type, i))
	{
	  --array.n_elements;
	  ++artificials;
	}
      else
	{
	  array.elements[i - artificials]
	    = convert_cplus_type (instance, TYPE_FIELD_TYPE (type, i),
				  GCC_CP_ACCESS_NONE);
	}
    }

  /* FIXME: add default args, exception specs and, once support is
     added, attributes.  -lxo */

  /* We omit setting the argument types to `void' to be a little flexible
     with some minsyms like printf (compile-cplus.exp has examples).  */
  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("build_function_type %lld %d\n",
			 return_type, is_varargs);
    }
  result = CPCALL (build_function_type, instance,
		   return_type, &array, is_varargs);
  xfree (array.elements);

  return result;
}

/* Convert an integer type to its gcc representation.  */

static gcc_type
ccp_convert_int (struct compile_cplus_instance *instance, struct type *type)
{
  gcc_type result;

  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("int_type %d %d %d\n", TYPE_LENGTH (type),
			 TYPE_UNSIGNED (type), TYPE_NOSIGN (type));
    }
  if (TYPE_NOSIGN (type))
    {
      gdb_assert (TYPE_LENGTH (type) == 1);
      result = CPCALL (char_type, instance);
    }
  else
    result = CPCALL (int_type, instance, TYPE_UNSIGNED (type), TYPE_LENGTH (type),
		     TYPE_NAME (type));
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);
  return result;
}

/* Convert a floating-point type to its gcc representation.  */

static gcc_type
ccp_convert_float (struct compile_cplus_instance *instance, struct type *type)
{
  gcc_type result;

  if (debug_compile_cplus_types)
    printf_unfiltered  ("float_type %s\n", TYPE_SAFE_NAME (type));
  result = CPCALL (float_type, instance, TYPE_LENGTH (type), TYPE_NAME (type));
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);
  return result;
}

/* Convert the 'void' type to its gcc representation.  */

static gcc_type
ccp_convert_void (struct compile_cplus_instance *instance, struct type *type)
{
  gcc_type result;

  if (debug_compile_cplus_types)
    printf_unfiltered ("void_type\n");
  result = CPCALL (void_type, instance);
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);
  return result;
}

/* Convert a boolean type to its gcc representation.  */

static gcc_type
ccp_convert_bool (struct compile_cplus_instance *instance, struct type *type)
{
  gcc_type result;

  if (debug_compile_cplus_types)
    printf_unfiltered  ("bool_type %s\n", TYPE_SAFE_NAME (type));
  result = CPCALL (bool_type, instance);
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);
  return result;
}

/* Add the qualifiers given by QUALS to BASE.  */

static gcc_type
convert_qualified_base (struct compile_cplus_instance *instance,
			gcc_type base, gcc_cp_qualifiers_flags quals)
{
  gcc_type result = base;

  if (quals != 0)
    {
      if (debug_compile_cplus_types)
	printf_unfiltered ("build_qualified_type: ");
      result = CPCALL (build_qualified_type, instance, base, quals);
      if (debug_compile_cplus_types)
	printf_unfiltered ("gcc_type = %lld\n", result);
    }

  return result;
}

/* Convert a qualified type to its gcc representation.  */

static gcc_type
ccp_convert_qualified (struct compile_cplus_instance *instance,
		       struct type *type)
{
  struct type *unqual = make_unqualified_type (type);
  gcc_type unqual_converted;
  gcc_cp_qualifiers_flags quals = (enum gcc_cp_qualifiers) 0;
  gcc_type result;

  unqual_converted = convert_cplus_type (instance, unqual, GCC_CP_ACCESS_NONE);

  if (TYPE_CONST (type))
    quals |= GCC_CP_QUALIFIER_CONST;
  if (TYPE_VOLATILE (type))
    quals |= GCC_CP_QUALIFIER_VOLATILE;
  if (TYPE_RESTRICT (type))
    quals |= GCC_CP_QUALIFIER_RESTRICT;

  return convert_qualified_base (instance, unqual_converted, quals);
}

/* Convert a complex type to its gcc representation.  */

static gcc_type
ccp_convert_complex (struct compile_cplus_instance *instance,
		     struct type *type)
{
  gcc_type result;
  gcc_type base = convert_cplus_type (instance, TYPE_TARGET_TYPE (type),
				      GCC_CP_ACCESS_NONE);

  if (debug_compile_cplus_types)
    printf_unfiltered ("build_complex_type %s\n", TYPE_SAFE_NAME (type));
  result = CPCALL (build_complex_type, instance, base);
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_type = %lld\n", result);
  return result;
}

/* Convert a namespace of TYPE.  */

static gcc_type
ccp_convert_namespace (struct compile_cplus_instance *instance,
		       struct type *type)
{
  int need_new_context;
  gcc_type dummy;
  char *name = NULL;
  struct compile_cplus_context *pctx;
  struct cleanup *cleanups;

  /* !!keiths: I don't think we can define namespaces inside other types,
     so the only way to get here is to be defining either a global namespace
     or something defined via namespaces all in the global namespace.  */

  cleanups = make_cleanup (null_cleanup, NULL);
  pctx = new_processing_context (instance, TYPE_NAME (type), type, &dummy);

  if (TYPE_NAME (type) != NULL)
    {
      name = cp_func_name (TYPE_NAME (type));
      make_cleanup (xfree, name);
    }
  else
    name = "";

  need_new_context = ccp_need_new_context (pctx);
  if (need_new_context)
    ccp_push_processing_context (instance, pctx);

  if (debug_compile_cplus_types)
    printf_unfiltered ("push_namespace %s\n", name);
  CPCALL (push_namespace, instance, name);

  if (debug_compile_cplus_types)
    printf_unfiltered ("pop_namespace %s\n", name);
  CPCALL (pop_namespace, instance);

  if (need_new_context)
    ccp_pop_processing_context (instance, pctx);

  delete_processing_context (pctx);
  do_cleanups (cleanups);

  return DONT_CACHE_TYPE;
}

/* A helper function which knows how to convert most types from their
   gdb representation to the corresponding gcc form.  This examines
   the TYPE and dispatches to the appropriate conversion function.  It
   returns the gcc type.

   If the type was defined in another type, NESTED_ACCESS should indicate the
   accessibility of this type.  */

static gcc_type
convert_type_cplus_basic (struct compile_cplus_instance *instance,
			  struct type *type,
			  enum gcc_cp_symbol_kind nested_access)
{
  /* Reference types seem to always have a const qualifier, but we
     don't want that to be propagated to the GCC type, because GCC
     doesn't like the reference types themselves to be qualified.  */
  if (TYPE_CODE (type) == TYPE_CODE_REF)
    return ccp_convert_reference (instance, type);

  /* If we are converting a qualified type, first convert the
     unqualified type and then apply the qualifiers.  */
  if ((TYPE_INSTANCE_FLAGS (type) & (TYPE_INSTANCE_FLAG_CONST
				     | TYPE_INSTANCE_FLAG_VOLATILE
				     | TYPE_INSTANCE_FLAG_RESTRICT)) != 0)
    return ccp_convert_qualified (instance, type);

  switch (TYPE_CODE (type))
    {
#if 0
    case TYPE_CODE_REF:
      return ccp_convert_reference (instance, type);
#endif

    case TYPE_CODE_PTR:
      return ccp_convert_pointer (instance, type);

    case TYPE_CODE_ARRAY:
      return ccp_convert_array (instance, type);

    case TYPE_CODE_STRUCT:
    case TYPE_CODE_UNION:
      return ccp_convert_struct_or_union (instance, type, nested_access);

    case TYPE_CODE_ENUM:
      return ccp_convert_enum (instance, type, nested_access);

    case TYPE_CODE_FUNC:
      return ccp_convert_func (instance, type, 0);

#if 0
    case TYPE_CODE_METHOD:
      return ccp_convert_method (instance, type);
#endif

    case TYPE_CODE_INT:
      return ccp_convert_int (instance, type);

    case TYPE_CODE_FLT:
      return ccp_convert_float (instance, type);

    case TYPE_CODE_VOID:
      return ccp_convert_void (instance, type);

    case TYPE_CODE_BOOL:
      return ccp_convert_bool (instance, type);

    case TYPE_CODE_COMPLEX:
      return ccp_convert_complex (instance, type);

    case TYPE_CODE_NAMESPACE:
      return ccp_convert_namespace (instance, type);

    case TYPE_CODE_TYPEDEF:
      return ccp_convert_typedef (instance, type, nested_access);
    }

  {
    char *s = xstrprintf (_("unhandled TYPE_CODE_%s"),
			  type_code_to_string (TYPE_CODE (type)));

    if (debug_compile_cplus_types)
      printf_unfiltered ("error %s\n", s);
    return CPCALL (error, instance, s);
    xfree (s);
  }
}

/* See compile-internal.h.  */

gcc_type
convert_cplus_type (struct compile_cplus_instance *instance,
		    struct type *type, enum gcc_cp_symbol_kind nested_access)
{
  struct type_map_instance inst, *found;
  gcc_type result;

  inst.type = type;
  found = (struct type_map_instance *) htab_find (instance->type_map, &inst);
  if (found != NULL)
    return found->gcc_type_handle;

  result = convert_type_cplus_basic (instance, type, nested_access);
  if (result != DONT_CACHE_TYPE)
    insert_type (instance, type, result);
  return result;
}



/* Template support functions.  */

/* Allocate a new struct cgcc_cp_template_args.  */

static struct gcc_cp_template_args *
new_gcc_cp_template_args (int num)
{
  struct gcc_cp_template_args *args;

  args = XCNEW (struct gcc_cp_template_args);
  args->n_elements = num;
  args->kinds = XNEWVEC (char, num);
  memset (args->kinds, -1, sizeof (char) * num);
  args->elements = XCNEWVEC (gcc_cp_template_arg, num);

  return args;
}

/* Compute the hash string used by the given function template definition.  */

static void
ccp_compute_function_template_defn_hash_string
  (struct function_template_defn *defn)
{

  gdb_assert (defn->hash_string == NULL);
  gdb_assert (defn->demangle_info != NULL);

  /* Make sure template arguments have been decoded.  */
  cp_decode_template_type_indices (defn->tsymbol, defn->demangle_info);

  /* Output the template generic.  */
  defn->hash_string = ccp_function_template_decl (defn->tsymbol,
						  defn->demangle_info);
#if 0
  printf ("hash string for function \"%s\" is \"%s\"\n",
	  defn->name, defn->hash_string);
#endif
}

/* Compute the hash string used by the given function template definition.  */

static void
ccp_compute_class_template_defn_hash_string (struct class_template_defn *defn)
{
  struct ui_file *stream;
  long length;
  struct cleanup *back_to;

  gdb_assert (defn->hash_string == NULL);

  stream = mem_fileopen ();
  back_to = make_cleanup_ui_file_delete (stream);

  /* class|struct|union NAME<parameters>  */
  if (TYPE_CODE (defn->type) == TYPE_CODE_STRUCT)
    {
      if (TYPE_DECLARED_CLASS (defn->type))
	fputs_unfiltered ("class ", stream);
      else
	fputs_unfiltered ("struct ", stream);
    }
  else
    {
      gdb_assert (TYPE_CODE (defn->type) == TYPE_CODE_UNION);
      fputs_unfiltered ("union ", stream);
    }

  fputs_unfiltered (defn->name, stream);
  fputc_unfiltered ('<', stream);
  print_template_parameter_list (stream, TYPE_TEMPLATE_ARGUMENT_INFO (defn->type));
  fputc_unfiltered ('>', stream);

  defn->hash_string = ui_file_xstrdup (stream, &length);
  do_cleanups (back_to);
#if 0
  printf ("hash string for class \"%s\" is \"%s\"\n",
	  defn->name, defn->hash_string);
#endif
}

/* Allocate a minimal generic function template definition.  This does
   not allocate any memory for the parameters or default values.  */

static struct function_template_defn *
new_function_template_defn (struct template_symbol *tsymbol,
			    struct type *parent_type, int f_idx, int m_idx)
{
  struct function_template_defn *defn;

  defn = XCNEW (struct function_template_defn);
  defn->name = xstrdup (tsymbol->search_name);
  defn->tsymbol = tsymbol;
  defn->parent_type = parent_type;
  defn->f_idx = f_idx;
  defn->m_idx = m_idx;

  defn->demangle_info
    = cp_mangled_name_to_comp (tsymbol->linkage_name, DMGL_ANSI | DMGL_PARAMS,
			       &defn->demangle_memory,
			       &defn->demangle_name_storage);
  ccp_compute_function_template_defn_hash_string (defn);

  return defn;
}

/* Allocate a minimal generic class template definition.  This does not
   allocate any memory for the parameters or default values.  */

static struct class_template_defn *
new_class_template_defn (const char *name, struct type *type)
{
  struct class_template_defn *defn;

  defn = XCNEW (struct class_template_defn);
  defn->name = xstrdup (name);
  defn->type = type;
  ccp_compute_class_template_defn_hash_string (defn);

  return defn;
}

/* Free a template_defn.  */

static void
delete_template_defn (void *e)
{
  struct template_defn *defn = static_cast<struct template_defn *> (e);

  xfree (defn->name);
  xfree (defn->hash_string);
  if (defn->params != NULL)
    {
      xfree (defn->params->kinds);
      xfree (defn->params->elements);
      xfree (defn->params);
    }
  xfree (defn->default_arguments);
  xfree (defn);
}

/* Free a struct function_template_defn.  */

static void
delete_function_template_defn (void *e)
{
  struct function_template_defn *defn = static_cast<struct function_template_defn *> (e);

  cp_demangled_name_parse_free (defn->demangle_info);
  xfree (defn->demangle_memory);
  xfree (defn->demangle_name_storage);
  delete_template_defn (e);
}

/* Free a struct class_template_defn.  */

static void
delete_class_template_defn (void *e)
{
  delete_template_defn (e);
}

/* Hashing function for struct template_defn.  */

static hashval_t
hash_template_defn (const void *p)
{
  const struct template_defn *defn = static_cast<const struct template_defn *> (p);

  /* The definition's hash string should have already been computed.  */
  gdb_assert (defn->hash_string != NULL);
  return htab_hash_string (defn->hash_string);
}

/* Check two template definitions for equality.

   Returns 1 if the two are equal.  */

/* !!keiths: I don't think this is going to be sufficient
   when we have templates of the same name in different
   CUs.  I wonder if we should check the symtab & lineno of the
   symbols in the two template_defn's.  */

static int
eq_template_defn (const void *a, const void *b)
{
  const struct template_defn *d1 = static_cast<const struct template_defn *> (a);
  const struct template_defn *d2 = static_cast<const struct template_defn *> (b);

  return streq (d1->hash_string, d2->hash_string);
}

/* Fill in the `kinds'  member of DEST from ARG_INFO.  */

static void
enumerate_template_parameter_kinds (struct compile_cplus_instance *instance,
				    struct gcc_cp_template_args *dest,
				    const struct template_argument_info *arg_info)
{
  int i;

  for (i = 0; i < arg_info->n_arguments; ++i)
    {
      switch (arg_info->argument_kinds[i])
	{
	case type_parameter:
	  dest->kinds[i] = GCC_CP_TPARG_CLASS;
	  break;
	case value_parameter:
	  dest->kinds[i] = GCC_CP_TPARG_VALUE;
	  break;
	case template_parameter:
	  dest->kinds[i] = GCC_CP_TPARG_TEMPL;
	  break;
	case variadic_parameter:
	  dest->kinds[i] = GCC_CP_TPARG_PACK;
	  break;
	default:
	  gdb_assert_not_reached ("unexpected template parameter kind");
	}
    }
}

/* Helper function to define and return the `value' of TYPE of the template
   parameter ARG in compile INSTANCE.  */

static gcc_expr
get_template_argument_value (struct compile_cplus_instance *instance,
			     gcc_type type, /*const*/ struct symbol *arg)
{
  gcc_expr value = 0;

  switch (SYMBOL_CLASS (arg))
    {
      /* !!keiths: More (incomplete) fun.  */
    case LOC_CONST:
      if (debug_compile_cplus_types)
	{
	  printf_unfiltered ("literal_expr %lld %ld\n",
			     type, SYMBOL_VALUE (arg));
	}
	value = CPCALL (literal_expr, instance, type, SYMBOL_VALUE (arg));
      break;

    case LOC_COMPUTED:
      {
	struct value *val;
	struct frame_info *frame = NULL;

	/* !!keiths: I don't think this can happen, but I've been
	   wrong before.  */
	if (symbol_read_needs_frame (arg))
	  {
	    frame = get_selected_frame (NULL);
	    gdb_assert (frame != NULL);
	  }
	val = read_var_value (arg, instance->base.block, frame);

	/* !!keiths: This is a hack, but I don't want to write
	   yet another linkage name translation function.  At least
	   not just yet.  */
	value = CPCALL (literal_expr, instance, type, value_address (val));
      }
      break;

    default:
      gdb_assert_not_reached
	("unhandled template value argument symbol class");
    }

  return value;
}

/* Enumerate the template parameters of the generic form of the template
   definition DEFN into DEST.  */

static void
ccp_define_template_parameters_generic
  (struct compile_cplus_instance *instance, struct gcc_cp_template_args *dest,
   /*const*/ struct symbol **default_arguments,
   const struct template_argument_info *arg_info, const char *filename, int line)
{
  int i;

  for (i = 0; i < arg_info->n_arguments; ++i)
    {
      const char *id = SYMBOL_NATURAL_NAME (arg_info->arguments[i]);

      switch (arg_info->argument_kinds[i])
	{
	case type_parameter:
	  {
	    /* GDB doesn't support variadic templates yet.  */
	    int is_pack = 0;
	    gcc_type default_type = 0;

	    if (default_arguments[i] != NULL)
	      {
		struct type *type = SYMBOL_TYPE (default_arguments[i]);

		/* This type must previously have been converted,
		   or GCC will error with "definition of TYPE inside
		   template parameter list."  */
		default_type = convert_cplus_type (instance, type,
						   GCC_CP_ACCESS_NONE);
	      }

	    if (debug_compile_cplus_types)
	      {
		printf_unfiltered
		  ("new_template_typename_parm %s %d %lld %s %d\n",
		   id, is_pack, default_type, filename, line);
	      }

	    /* !!keiths: This would be the base generic parameter...
	       Shouldn't this carry any qualifiers with it?  Can we
	       actually figure out those qualifiers, e.g,
	    const T& foo<A> () vs T foo<const A&> ().  */
	    dest->elements[i].type
	      = CPCALL (new_template_typename_parm, instance, id, is_pack,
			default_type, filename, line);
	  }
	  break;

	case value_parameter:
	  {
	    gcc_expr default_value = 0;
	    gcc_type gcctype;
	    struct type *ptype = SYMBOL_TYPE (arg_info->arguments[i]);

	    /* Get the argument's type.  This type must also have been
	     previously defined (or declared) to prevent errors.  */
	    gcctype
	      = convert_cplus_type (instance, ptype, GCC_CP_ACCESS_NONE);
	    dest->elements[i].type = gcctype;

	    if (default_arguments[i] != NULL)
	      {
		default_value
		  = get_template_argument_value (instance, gcctype, default_arguments[i]);
	      }

	    if (debug_compile_cplus_types)
	      {
		printf_unfiltered
		  ("new_template_value_parm %lld %s %lld %s %d\n",
		   gcctype, id, default_value, filename, line);
	      }

	    CPCALL (new_template_value_parm, instance, gcctype, id,
			default_value, filename, line);
	  }
	  break;

	case template_parameter:
	  dest->elements[i].templ = 0;
	  break;

	case variadic_parameter:
	  /* GDB doesn't support variadic templates.  */
	  dest->elements[i].pack = 0;
	  break;

	default:
	  gdb_assert_not_reached ("unexpected template parameter kind");
	}
    }
}

/* See definition in compile-internal.h.  */

void
enumerate_template_arguments (struct compile_cplus_instance *instance,
			      struct gcc_cp_template_args *dest,
			      const struct template_defn *defn,
			      const struct template_argument_info *arg_info)
{
  int i;

  /* Fill in the parameter kinds.  */
  enumerate_template_parameter_kinds (instance, dest, arg_info);

  /* Loop over the arguments, converting parameter types, values, etc into DEST.  */
  for (i = 0; i < arg_info->n_arguments; ++i)
    {
      switch (arg_info->argument_kinds[i])
	{
	case type_parameter:
	  {
	    gcc_type type = convert_cplus_type (instance,
						SYMBOL_TYPE (arg_info->arguments[i]),
						GCC_CP_ACCESS_NONE);

	    dest->elements[i].type = type;
	  }
	  break;

	case value_parameter:
	  {
	    gcc_type type = defn->params->elements[i].type;

	    dest->elements[i].value
	      = get_template_argument_value (instance, type, arg_info->arguments[i]);
	  }
	  break;

	case template_parameter:
	  break;

	case variadic_parameter:
	  break;

	default:
	  gdb_assert_not_reached ("unexpected template parameter kind");
	}
    }
}

/* See definition in compile-internal.h.  */

gcc_decl
get_template_decl (const struct function_template_defn *templ_defn)
{
  return templ_defn->decl;
}

/* Define the type for all default template parameters for the template arguments
   given by ARGUMENTS.  */

static void
define_default_template_parameter_types (struct compile_cplus_instance *instance,
					 const struct template_argument_info *arg_info,
					 struct symbol * const* default_arguments)
{
  int i;

  for (i = 0; i < arg_info->n_arguments; ++i)
    {
      if (default_arguments[i] != NULL)
	{
	  switch (arg_info->argument_kinds[i])
	    {
	    case type_parameter:
	    case value_parameter:
	      convert_cplus_type (instance,
				  SYMBOL_TYPE (default_arguments[i]),
				  GCC_CP_ACCESS_NONE);
	      break;

	    case template_parameter:
	    case variadic_parameter:
	    default:
	      gdb_assert (_("unexpected template parameter kind"));
	    }
	}
    }
}

/* Add the modifiers given by MODIFIERS to TYPE.  */

static gcc_type
add_template_type_modifiers (struct compile_cplus_instance *instance,
			     gcc_type type,
			     VEC (template_parameter_modifiers) *modifiers)
{
  int ix;
  template_parameter_modifiers elt;
  gcc_cp_qualifiers_flags qualf = (enum gcc_cp_qualifiers) 0;

  for (ix = 0;
       VEC_iterate (template_parameter_modifiers, modifiers, ix, elt);
       ++ix)
    {
      switch (elt)
	{
	case PARAMETER_NONE:
	  break;

	case PARAMETER_CONST:
	  qualf |= GCC_CP_QUALIFIER_CONST;
	  break;

	case PARAMETER_VOLATILE:
	  qualf |= GCC_CP_QUALIFIER_VOLATILE;
	  break;

	case PARAMETER_RESTRICT:
	  qualf |= GCC_CP_QUALIFIER_RESTRICT;
	  break;

	case PARAMETER_POINTER:
	  type = convert_qualified_base (instance, type, qualf);
	  type = convert_pointer_base (instance, type);
	  qualf = (enum gcc_cp_qualifiers) 0;
	  break;

	case PARAMETER_LVALUE_REFERENCE:
	  type = convert_qualified_base (instance, type, qualf);
	  type = convert_pointer_base (instance, type);
	  qualf = (enum gcc_cp_qualifiers) 0;
	  break;

	case PARAMETER_RVALUE_REFERENCE:
	  type = convert_qualified_base (instance, type, qualf);
	  type = convert_reference_base (instance, type);
	  qualf = (enum gcc_cp_qualifiers) 0;
	  break;

	default:
	  gdb_assert_not_reached ("unknown template parameter modifier");
	}
    }

  return type;
}

/* Get the abstract template type described by COMP, returning any
   type modifiers in *MODIFIERS.  */

static const struct demangle_component *
ccp_get_template_type (const struct demangle_component *comp,
		       VEC (template_parameter_modifiers) **modifiers)
{
  int done = 0;

  *modifiers = NULL;

  /* This is probably a little too simplistic...  */
  while (!done)
    {
      switch (comp->type)
	{
	case DEMANGLE_COMPONENT_POINTER:
	  VEC_safe_insert (template_parameter_modifiers, *modifiers, 0,
			   PARAMETER_POINTER);
	  comp = d_left (comp);
	  break;

	case DEMANGLE_COMPONENT_REFERENCE:
	  VEC_safe_insert (template_parameter_modifiers, *modifiers, 0,
			   PARAMETER_LVALUE_REFERENCE);
	  comp = d_left (comp);
	  break;

	case DEMANGLE_COMPONENT_CONST:
	  VEC_safe_insert (template_parameter_modifiers, *modifiers, 0,
			   PARAMETER_CONST);
	  comp = d_left (comp);
	  break;

	case DEMANGLE_COMPONENT_RESTRICT:
	  VEC_safe_insert (template_parameter_modifiers, *modifiers, 0,
			   PARAMETER_RESTRICT);
	  comp = d_left (comp);
	  break;

	case DEMANGLE_COMPONENT_VOLATILE:
	  VEC_safe_insert (template_parameter_modifiers, *modifiers, 0,
			   PARAMETER_VOLATILE);
	  comp = d_left (comp);
	  break;

	case DEMANGLE_COMPONENT_TEMPLATE_PARAM:
	default:
	  done = 1;
	  break;
	}
    }

  return comp;
}

/* Add the type modifiers described in COMP to BASE_TYPE.  */

static gcc_type
ccp_add_type_modifiers (struct compile_cplus_instance *instance,
			gcc_type base_type,
			const struct demangle_component *comp)
{
  gcc_type result;
  struct cleanup *back_to;
  VEC (template_parameter_modifiers) *modifiers = NULL;

  (void) ccp_get_template_type (comp, &modifiers);
  back_to
    = make_cleanup (VEC_cleanup (template_parameter_modifiers), &modifiers);
  result = add_template_type_modifiers (instance, base_type, modifiers);
  do_cleanups (back_to);
  return result;
}

/* A hashtable callback to define (to the plug-in) and fill-in the
   function template definition based on the template instance in SLOT.
   CALL_DATA should be the compiler instance to use.  */

/* !!keiths: This is heinously long!  */

static int
define_function_template (void **slot, void *call_data)
{
  int i, need_new_context, ignore;
  struct function_template_defn *defn;
  gcc_type return_type, func_type, result;
  struct gcc_type_array array;
  int artificials, is_varargs;
  struct type *templ_type, *method_type;
  struct compile_cplus_context *pctx;
  struct cleanup *back_to;
  struct compile_cplus_instance *instance;
  const struct template_symbol *tsym;
  char *id, *special_name;
  const char *name;
  gcc_cp_symbol_kind_flags sym_kind = GCC_CP_SYMBOL_FUNCTION;
  struct fn_field *method_field;
  struct demangle_component *comp;

  defn = (struct function_template_defn *) *slot;
  if (defn->defined)
    {
      /* This template has already been defined.  Keep looking for more
	 undefined templates.  */
      return 1;
    }

  /* This should have already been done in
     ccp_maybe_define_new_function_template.  */
  gdb_assert (defn->demangle_info != NULL);

  defn->defined = 1;
  instance = (struct compile_cplus_instance *) call_data;
  tsym = defn->tsymbol;

  /* The defn->name may be a qualified name, containing, e.g., namespaces.
     We don't want those for the decl.  */
  back_to = make_cleanup (null_cleanup, NULL);
  special_name = NULL;
  if (defn->parent_type != NULL
      && defn->f_idx != -1 && defn->m_idx != -1)
    {
      struct fn_field *methods
	= TYPE_FN_FIELDLIST1 (defn->parent_type, defn->f_idx);

      method_field = &methods[defn->m_idx];
      method_type = method_field->type;
    }
  else
    {
      method_field = NULL;
      method_type = SYMBOL_TYPE (&defn->tsymbol->base);
    }
  id = ccp_decl_name (defn->name);
  make_cleanup (xfree, id);
  name = maybe_canonicalize_special_function (id,
					      method_field,
					      method_type,
					      &special_name,
					      &ignore);

  /* Ignore any "ignore" -- we need the template defined even if
     this specific instance shouldn't emit a template.  */

  if (special_name != NULL)
    {
      make_cleanup (xfree, special_name);
      name = special_name;
    }

  if (name != id)
    sym_kind |= GCC_CP_FLAG_SPECIAL_FUNCTION;

  /* Define any default value types.  */
  define_default_template_parameter_types (instance, defn->tsymbol->template_arguments,
					   defn->default_arguments);

  /* Assess the processing context.  */
  pctx = new_processing_context (instance, SYMBOL_NATURAL_NAME (&tsym->base),
				 SYMBOL_TYPE (&tsym->base), &result);
  if (result != GCC_TYPE_NONE)
    {
      gdb_assert (pctx == NULL);
      do_cleanups (back_to);
      /* new_processing_context returned the type of the actual template
	 instance from which we're constructing the template definition.
	 It is already defined.  */
      return 1;
    }

  /* If we need a new context, push it.  */
  need_new_context = ccp_need_new_context (pctx);
  if (need_new_context)
    ccp_push_processing_context (instance, pctx);

  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("start_new_template_decl for function generic %s\n",
			 defn->hash_string);
    }
  CPCALL (start_new_template_decl, instance);

  /* Get the parameters' generic kinds and types.  */
  enumerate_template_parameter_kinds (instance, defn->params, tsym->template_arguments);
  ccp_define_template_parameters_generic (instance, defn->params,
					  defn->default_arguments,
					  tsym->template_arguments,
					  symbol_symtab (&tsym->base)->filename,
					  SYMBOL_LINE (&tsym->base));

  /* Find the function node describing this template function.  */
  gdb_assert (defn->demangle_info->tree->type
	      == DEMANGLE_COMPONENT_TYPED_NAME);
  comp = d_right (defn->demangle_info->tree);
  gdb_assert (comp->type == DEMANGLE_COMPONENT_FUNCTION_TYPE);

  /* The return type is either a concrete type (TYPE_TARGET_TYPE)
     or a template parameter.  */
  if (tsym->template_return_index != -1)
    {
      gcc_type param_type
	= defn->params->elements[tsym->template_return_index].type;

      return_type
	= ccp_add_type_modifiers (instance, param_type, d_left (comp));
    }
  else if (tsym->conversion_operator_index != -1)
    {
      int done = 0;
      gcc_type param_type
	= defn->params->elements[tsym->conversion_operator_index].type;

      /* Conversion operators do not have a return type or arguments,
	 so we need to use the CONVERSION node in the left/name sub-tree
	 of the demangle tree.  */

      comp = d_left (defn->demangle_info->tree);
      while (!done)
	{
	  switch (comp->type)
	    {
	    case DEMANGLE_COMPONENT_TEMPLATE:
	      comp = d_left (comp);
	      break;

	    case DEMANGLE_COMPONENT_QUAL_NAME:
	      comp = d_right (comp);
	      break;

	    case DEMANGLE_COMPONENT_CONVERSION:
	    default:
	      done = 1;
	      break;
	    }
	}

      /* We had better have found a CONVERSION node if
	 tsym->conversion_operator_index was set!  */
      gdb_assert (comp->type == DEMANGLE_COMPONENT_CONVERSION);
      return_type = ccp_add_type_modifiers (instance, param_type,
					    d_left (comp));
    }
  else
    {
      return_type
	= convert_cplus_type (instance,
			      TYPE_TARGET_TYPE (SYMBOL_TYPE (&tsym->base)),
			      GCC_CP_ACCESS_NONE);
    }

  /* Get the parameters' definitions, and put them into ARRAY.  */
  templ_type = SYMBOL_TYPE (&tsym->base);
  is_varargs = ccp_is_varargs_p (templ_type);
  array.n_elements = TYPE_NFIELDS (templ_type);
  array.elements = XNEWVEC (gcc_type, TYPE_NFIELDS (templ_type));
  make_cleanup (xfree, array.elements);
  artificials = 0;

  /* d_right (info->tree) is FUNCTION_TYPE (assert above).  */
  comp = d_right (d_right (defn->demangle_info->tree));
  gdb_assert (comp != NULL && comp->type == DEMANGLE_COMPONENT_ARGLIST);

  for (i = 0; i < TYPE_NFIELDS (templ_type); ++i)
    {
      if (TYPE_FIELD_ARTIFICIAL (templ_type, i))
	{
	  --array.n_elements;
	  ++artificials;
	}
      else
	{
	  int tidx = tsym->template_argument_indices[i - artificials];
	  struct type *arg_type = TYPE_FIELD_TYPE (templ_type, i);

	  if (tidx == -1)
	    {
	      /* The parameter's type is a concrete type.  */
	      array.elements[i - artificials]
		= convert_cplus_type (instance, arg_type, GCC_CP_ACCESS_NONE);
	    }
	  else
	    {
	      /* The parameter's type is a template parameter.  */
	      result = defn->params->elements[tidx].type;

	      array.elements[i - artificials]
		= ccp_add_type_modifiers (instance, result, d_left (comp));
	    }

	  /* Move to the next ARGLIST node.  */
	  comp = d_right (comp);
	}
    }

  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("build_function_type for template %s %lld %d\n",
			 defn->hash_string, return_type, is_varargs);
    }
  func_type = CPCALL (build_function_type, instance, return_type, &array,
		      is_varargs);

  /* If we have a method, create its type and set additional symbol flags
     for the compiler.  */
  if (defn->parent_type != NULL && defn->f_idx != -1 && defn->m_idx != -1)
    {
      gcc_type class_type;
      struct fn_field *methods
	= TYPE_FN_FIELDLIST1 (defn->parent_type, defn->f_idx);

      /* Get the defining class's type.  This should already be in the
	 cache.  */
      class_type = convert_cplus_type (instance, defn->parent_type,
				       GCC_CP_ACCESS_NONE);

      /* Add any virtuality flags.  */
      if (TYPE_FN_FIELD_VIRTUAL_P (methods, defn->m_idx))
	{
	  struct block_symbol sym;

	  sym_kind |= GCC_CP_FLAG_VIRTUAL_FUNCTION;

	  /* Unfortunate to have to do a symbol lookup, but this is the only
	     way to know if we have a pure virtual method.  */
	  sym = lookup_symbol (TYPE_FN_FIELD_PHYSNAME (methods, defn->m_idx),
			       get_current_search_block (), VAR_DOMAIN, NULL);
	  if (sym.symbol == NULL)
	    {
	      /* !!keiths: The pure virtual hack.  See
		 ccp_convert_struct_or_union_methods for more.  */
	      sym_kind |= GCC_CP_FLAG_PURE_VIRTUAL_FUNCTION;
	    }
	}

      /* Add access flags.  */
      sym_kind |= get_method_access_flag (defn->parent_type,
					  defn->f_idx, defn->m_idx);

      /* Create the method type.  */
      if (!TYPE_FN_FIELD_STATIC_P (methods, defn->m_idx))
	{
	  gcc_cp_qualifiers_flags quals;
	  gcc_cp_ref_qualifiers_flags rquals;

	  quals = (enum gcc_cp_qualifiers) 0; // !!keiths FIXME
	  rquals = GCC_CP_REF_QUAL_NONE; // !!keiths FIXME
	  if (debug_compile_cplus_types)
	    {
	      printf_unfiltered ("build_method_type for template %s\n",
				 defn->hash_string);
	    }
	  func_type = CPCALL (build_method_type, instance, class_type,
			      func_type, quals, rquals);
	}
    }

  /* Finally, define the new generic template declaration.  */
  if (debug_compile_cplus_types)
    printf_unfiltered ("new_decl function template defn %s\n", name);
  defn->decl = CPCALL (new_decl, instance,
		       name,
		       sym_kind,
		       func_type,
		       0,
		       0,
		       symbol_symtab (&(tsym->base))->filename,
		       SYMBOL_LINE (&(tsym->base)));

  /* Pop the processing context, if we pushed one, and then delete it.  */
  if (need_new_context)
    ccp_pop_processing_context (instance, pctx);
  delete_processing_context (pctx);

  do_cleanups (back_to);

  /* Do all templates in the table.  */
  return 1;
}

/* A hashtable callback to define (to the plug-in) and fill-in the
   class template definition based on the template instance in SLOT.
   CALL_DATA should be the compiler instance to use.  */

static int
define_class_template (void **slot, void *call_data)
{
  struct class_template_defn *defn;
  struct compile_cplus_instance *instance;
  struct compile_cplus_context *pctx;
  gcc_type result;
  int need_new_context;

  defn = (struct class_template_defn *) *slot;
  if (defn->defined)
    {
      /* This template has already been defined.  Keep looking for more
	 undefined templates.  */
      return 1;
    }

  defn->defined = 1;
  instance = (struct compile_cplus_instance *) call_data;

  /* Define any default value types.  */
  define_default_template_parameter_types (instance,
					   TYPE_TEMPLATE_ARGUMENT_INFO (defn->type),
					   defn->default_arguments);

  /* Asses the processing context.  */
  pctx = new_processing_context (instance, defn->name, defn->type, &result);
  if (result != GCC_TYPE_NONE)
    {
      gdb_assert (pctx == NULL);
      /* new_processing_context returned the type of the actual template
	 instance from which we're constructing the template definition.
	 It is already defined.  */
      return 1;
    }

  /* If we need a new context, push it.  */
  need_new_context = ccp_need_new_context (pctx);
  if (need_new_context)
    ccp_push_processing_context (instance, pctx);

  /* Now start a new template list for this template.  */
  if (debug_compile_cplus_types)
    {
      printf_unfiltered ("start_new_template_decl for class generic %s\n",
			 defn->hash_string);
    }
  CPCALL (start_new_template_decl, instance);

  /* Get the parameters' generic kinds and types.  */
  enumerate_template_parameter_kinds (instance, defn->params,
				      TYPE_TEMPLATE_ARGUMENT_INFO (defn->type));
  ccp_define_template_parameters_generic (instance, defn->params,
					  defn->default_arguments,
					  TYPE_TEMPLATE_ARGUMENT_INFO (defn->type),
					  /* filename */ NULL, /* line */ 0); // !!keiths FIXME

  /* Define the new generic template declaration.  */
  if (TYPE_CODE (defn->type) == TYPE_CODE_STRUCT)
    {
      if (debug_compile_cplus_types)
	printf_unfiltered ("new_decl for class template defn %s\n", defn->name);
      defn->decl = CPCALL (new_decl, instance,
			   defn->name,
			   GCC_CP_SYMBOL_CLASS /* | nested_access? */
			   | (TYPE_DECLARED_CLASS (defn->type)
			      ? GCC_CP_FLAG_CLASS_NOFLAG
			      : GCC_CP_FLAG_CLASS_IS_STRUCT),
			   0, NULL, 0, /*filename*/ NULL, /*line*/ 0);
    }
  else
    {
      gdb_assert (TYPE_CODE (defn->type) == TYPE_CODE_UNION);
      if (debug_compile_cplus_types)
	printf_unfiltered ("new_decl for union template defn %s\n", defn->name);
      defn->decl = CPCALL (new_decl, instance,
			   defn->name,
			   GCC_CP_SYMBOL_UNION /* | nested_access? */,
			   0, NULL, 0, /*fileanme*/NULL, /*line*/0);
    }
  if (debug_compile_cplus_types)
    printf_unfiltered ("\tgcc_decl = %lld\n", defn->decl);

  if (need_new_context)
    ccp_pop_processing_context (instance, pctx);
  delete_processing_context (pctx);

  /* Do all templates in the table.  */
  return 1;
}

/* If SYM is a template symbol whose generic we have not yet declared,
   add it to INSTANCE's list of template definitions and scan for default
   values.  */

static void
ccp_maybe_define_new_function_template (struct compile_cplus_instance *instance,
					const struct symbol *sym,
					struct type *parent_type,
					int f_idx, int m_idx)
{
  if (sym != NULL && SYMBOL_IS_CPLUS_TEMPLATE_FUNCTION (sym))
    {
      int j;
      void **slot;
      struct function_template_defn *defn;
      struct template_symbol *tsym = (struct template_symbol *) sym;

      defn = new_function_template_defn (tsym, parent_type, f_idx, m_idx);
      slot = htab_find_slot (instance->function_template_defns, defn, INSERT);
      if (*slot == NULL)
	{
	  /* New template definition.  Allocate memory for the parameters
	     and default values.  Then copy it into the new slot.  */
	  defn->params = new_gcc_cp_template_args (tsym->template_arguments->n_arguments);
	  defn->default_arguments
	    = XCNEWVEC (struct symbol *, tsym->template_arguments->n_arguments);
	  *slot = defn;
	}
      else
	{
	  /* Existing definition.  Use the existing slot.  */
	  delete_function_template_defn (defn);
	  defn = (struct function_template_defn *) *slot;
	}

      /* Loop over the template arguments, noting any default values.  */
      for (j = 0; j < tsym->template_arguments->n_arguments; ++j)
	{
	  if (defn->default_arguments[j] == NULL
	      && tsym->template_arguments->default_arguments[j] != NULL)
	    {
	      defn->default_arguments[j]
		= tsym->template_arguments->default_arguments[j];

	      /* We don't want to define them here because it could start
		 emitting template definitions before we're even done
		 collecting the default values.  [Easy to demonstrate if the
		 default value is a class.]  */
	    }
	}
    }
}

/* A helper function to emit declarations for any new function templates
   in INSTANCE.  */

static void
ccp_emit_function_template_decls (struct compile_cplus_instance *instance)
{
  htab_traverse (instance->function_template_defns,
		 define_function_template, instance);
}

/* A helper function to emit declarations for any new class templates
   in INSTANCE.  */

static void
ccp_emit_class_template_decls (struct compile_cplus_instance *instance)
{
  htab_traverse (instance->class_template_defns,
		 define_class_template, instance);
}

/* See compile-internal.h.  */

void
compile_cplus_define_templates (struct compile_cplus_instance *instance,
				VEC (block_symbol_d) *symbols)
{
  int i;
  struct block_symbol *elt;

  /* We need to do this in two passes.  On the first pass, we collect
     the list of "unique" template definitions we need (using the template
     hashing function) and we collect the list of default values for the
     template (which can only be done after we have a list of all templates).
     On the second pass, we iterate over the list of templates we need to
     define, enumerating those definitions (with default values) to the
     compiler plug-in.  */

  for (i = 0; VEC_iterate (block_symbol_d, symbols, i, elt); ++i)
    ccp_maybe_define_new_function_template (instance, elt->symbol,
					    NULL, -1, -1);

  /* !!keiths: From here on out, we MUST have all types declared or defined,
     otherwise GCC will give us "definition of TYPE in template parameter
     list."  */
  /* Create any new template definitions we encountered.  */
  ccp_emit_function_template_decls (instance);
  ccp_emit_class_template_decls (instance);
}

/* See definition in compile-internal.h.  */

struct function_template_defn *
find_function_template_defn (struct compile_cplus_instance *instance,
			     struct template_symbol *tsym)
{
  void **slot;
  struct function_template_defn *defn;

  /* Some times, the template has no search name defined, i.e., no linkage
     name from the compiler.  There's not much we can do in this case.  */
  if (tsym->search_name == NULL)
    return NULL;

  defn = new_function_template_defn (tsym, NULL, -1, -1);
  slot = htab_find_slot (instance->function_template_defns, defn, NO_INSERT);
  delete_function_template_defn (defn);
  if (slot != NULL && *slot != NULL)
    {
      /* A template generic for this was already defined.  */
      return (struct function_template_defn *) *slot;
    }

  /* No generic for this template was found.  */
  return NULL;
}

/* Return the template definition for TYPE or NULL if none exists.  */

static struct class_template_defn *
find_class_template_defn (struct compile_cplus_instance *instance, struct type *type)
{
  void **slot;
  struct class_template_defn *defn;
  char *name;
  struct cleanup *back_to;

  /* There are no template definitions associated with anonymous types or
     types without template arguments.  */
  if (TYPE_NAME (type) == NULL || TYPE_TEMPLATE_ARGUMENT_INFO (type) == NULL)
    return NULL;

  name = ccp_decl_name (TYPE_NAME (type));
  back_to = make_cleanup (xfree, name);
  defn = new_class_template_defn (name, type);
  slot = htab_find_slot (instance->class_template_defns, defn, NO_INSERT);
  delete_class_template_defn (defn);
  if (slot != NULL && *slot != NULL)
    {
      /* A template generic for this was already defined.  */
      do_cleanups (back_to);
      return (struct class_template_defn *) *slot;
    }

  /* No generic for this template was found.  */
  do_cleanups (back_to);
  return NULL;
}

/* Print the type modifiers MODIFIERS to STREAM.  */

static void
print_template_type_modifiers (VEC (template_parameter_modifiers) *modifiers,
			       struct ui_file *stream)
{
  int ix;
  template_parameter_modifiers elt;

  for (ix = 0;
       VEC_iterate (template_parameter_modifiers, modifiers, ix, elt);
       ++ix)
    {
      switch (elt)
	{
	case PARAMETER_NONE:
	  break;

	case PARAMETER_CONST:
	  fputs_unfiltered (" const", stream);
	  break;

	case PARAMETER_VOLATILE:
	  fputs_unfiltered (" volatile", stream);
	  break;

	case PARAMETER_RESTRICT:
	  fputs_unfiltered (" restrict", stream);
	  break;

	case PARAMETER_POINTER:
	  fputs_unfiltered ("*", stream);
	  break;

	case PARAMETER_LVALUE_REFERENCE:
	  fputc_unfiltered ('&', stream);
	  break;

	case PARAMETER_RVALUE_REFERENCE:
	  fputs_unfiltered ("&&", stream);

	default:
	  gdb_assert_not_reached ("unknown template parameter modifier");
	}
    }
}

/* Print the generic parameter type given by COMP from the template symbol
   TSYMBOL to STREAM.  This function prints the generic template parameter
   type, not the instanced type, e.g., "const T&".  */

static void
print_template_type (const struct demangle_component *comp,
		     const struct template_symbol *tsymbol,
		     struct ui_file *stream)
{
  VEC (template_parameter_modifiers) *modifiers;
  long idx;
  struct symbol *sym;
  struct cleanup *back_to;

  /* Get the template parameter and modifiers.  */
  comp = ccp_get_template_type (comp, &modifiers);
  back_to
    = make_cleanup (VEC_cleanup (template_parameter_modifiers), &modifiers);

  /* This had better be a template parameter!  */
  gdb_assert (comp->type == DEMANGLE_COMPONENT_TEMPLATE_PARAM);

  /* Using the parameter's index, get the parameter's symbol and print it
     with modifiers.  */
  idx = comp->u.s_number.number;
  sym = tsymbol->template_arguments->arguments[idx];
  fputs_unfiltered (SYMBOL_NATURAL_NAME (sym), stream);
  print_template_type_modifiers (modifiers, stream);
  do_cleanups (back_to);
}

/* Print the template parameter list of a type/symbol to STREAM.  */

static void
print_template_parameter_list (struct ui_file *stream,
			       const struct template_argument_info *arg_info)
{
  int i;

  for (i = 0; i < arg_info->n_arguments; ++i)
    {
      if (i != 0)
	fputs_unfiltered (", ", stream);

      switch (arg_info->argument_kinds[i])
	{
	case type_parameter:
	  fprintf_unfiltered (stream, "typename %s",
			      SYMBOL_NATURAL_NAME (arg_info->arguments[i]));
	  break;

	case value_parameter:
	  c_print_type (SYMBOL_TYPE (arg_info->arguments[i]), "", stream, -1, 0,
			&type_print_raw_options);
	  fprintf_unfiltered (stream, " %s",
			      SYMBOL_NATURAL_NAME (arg_info->arguments[i]));
	  break;

	case template_parameter:
	  break;

	case variadic_parameter:
	  break;

	default:
	  gdb_assert_not_reached ("unexpected template parameter kind");
	}
    }
}

/* Print out the generic template function argument list of the template
   symbol TSYMBOL to STREAM.  COMP represents the FUNCTION_TYPE of the
   demangle tree for TSYMBOL.  */

static void
print_function_template_arglist (const struct demangle_component *comp,
				 const struct template_symbol *tsymbol,
				 struct ui_file *stream)
{
  int done = 0;
  int quals = 0;
  int i, artificials;
  struct demangle_component *arg;
  struct type *ttype = SYMBOL_TYPE (&tsymbol->base);

  for (i = 0, artificials = 0; i < TYPE_NFIELDS (ttype); ++i)
    {
      int tidx;

      if (TYPE_FIELD_ARTIFICIAL (ttype, i))
	{
	  ++artificials;
	  continue;
	}

      if ((i - artificials) > 0)
	fputs_unfiltered (", ", stream);

      tidx = tsymbol->template_argument_indices[i - artificials];
      if (tidx == -1)
	{
	  /* A concrete type was used to define this argument.  */
	  c_print_type (TYPE_FIELD_TYPE (ttype, i), "", stream, -1, 0,
			&type_print_raw_options);
	  continue;
	}

      /* The type of this argument was specified by a template parameter,
	 possibly with added CV and ref qualifiers.  */

      /* Get the next ARGLIST node and print it.  */
      comp = d_right (comp);
      gdb_assert (comp != NULL);
      gdb_assert (comp->type == DEMANGLE_COMPONENT_ARGLIST);
      print_template_type (d_left (comp), tsymbol, stream);
    }
}

/* Print the conversion operator in COMP for the template symbol TSYMBOL
   to STREAM.  */

static void
print_conversion_node (const struct demangle_component *comp,
		       const struct template_symbol *tsymbol,
		       struct ui_file *stream)
{
  while (1)
    {
      switch (comp->type)
	{
	case DEMANGLE_COMPONENT_TYPED_NAME:
	case DEMANGLE_COMPONENT_TEMPLATE:
	  comp = d_left (comp);
	  break;

	case DEMANGLE_COMPONENT_QUAL_NAME:
	  {
	    /* Print out the qualified name.  */
	    struct cleanup *back_to;
	    char *ret = cp_comp_to_string (d_left (comp), 10);

	    back_to = make_cleanup (xfree, ret);
	    fprintf_unfiltered (stream, "%s::", ret);
	    do_cleanups (back_to);

	    /* Follow the rest of the name.  */
	    comp = d_right (comp);
	  }
	  break;

	case DEMANGLE_COMPONENT_CONVERSION:
	  fputs_unfiltered ("operator ", stream);
	  print_template_type (d_left (comp), tsymbol, stream);
	  return;

	default:
	  return;
	}
    }
}

/* Return a string representing the template declaration for TSYMBOL.
   All template symbols deriving from the same source declaration should
   yield the same string representation.

   This string representation is of the generic form
   RETURN_TYPE QUALIFIED_NAME <parameter list>(argument list), with
   generic template parameters instead of any instanced type.

   For example, both "void foo<int> (int)" and "void foo<A> (A)" will
   return "T foo<typename T>(T)".  */

static char *
ccp_function_template_decl (struct template_symbol *tsymbol,
			    const struct demangle_parse_info *info)
{
  struct demangle_component *ret_comp;
  struct ui_file *stream;
  long length;
  struct cleanup *back_to;
  struct block_symbol symbol;
  char *str = NULL;

  gdb_assert (info != NULL);

  stream  = mem_fileopen ();
  back_to = make_cleanup_ui_file_delete (stream);

  ret_comp = info->tree;
  if (ret_comp != NULL)
    {
      if (ret_comp->type == DEMANGLE_COMPONENT_TYPED_NAME)
	ret_comp = d_right (ret_comp);

      /* Print out the return type to the stream (if there is one).  */
      if (d_left (ret_comp) != NULL)
	{
	  if (tsymbol->template_return_index == -1)
	    {
	      struct type *return_type
		= TYPE_TARGET_TYPE (SYMBOL_TYPE (&tsymbol->base));

	      c_print_type (return_type, "", stream, -1, 0,
			    &type_print_raw_options);
	    }
	  else
	    print_template_type (d_left (ret_comp), tsymbol, stream);
	  fputc_unfiltered (' ', stream);
	}

      /* Print the name of the template.  */
      if (tsymbol->conversion_operator_index != -1)
	print_conversion_node (info->tree, tsymbol, stream);
      else
	{
	  fputs_unfiltered (tsymbol->search_name, stream);
	  if (tsymbol->search_name[strlen (tsymbol->search_name) - 1]
	      == '<')
	    fputc_unfiltered (' ', stream);
	}

      /* Print out template (generic) arguments.  */
      fputc_unfiltered ('<', stream);
      print_template_parameter_list (stream, tsymbol->template_arguments);
      fputc_unfiltered ('>', stream);

      /* Print out function arguments.  */
      fputc_unfiltered ('(', stream);
      print_function_template_arglist (ret_comp, tsymbol, stream);
      fputc_unfiltered (')', stream);
    }

  str = ui_file_xstrdup (stream, &length);
  do_cleanups (back_to);
  return str;
}

/* A command to test ccp_function_template_decl.  */

static void
print_template_defn_command (char *arg, int from_tty)
{
  struct ui_file *stream;
  long length;
  struct cleanup *back_to;
  struct block_symbol symbol;
  struct template_symbol *tsymbol;
  struct demangle_parse_info *info;
  void *storage = NULL;
  char *demangled_name = NULL;
  char *demangled_name_storage = NULL;
  char *str = NULL;

  demangled_name = gdb_demangle (arg, DMGL_ANSI | DMGL_PARAMS | DMGL_RET_DROP);
  if (demangled_name == NULL)
    {
      fprintf_filtered (gdb_stderr, _("could not demangle \"%s\"\n"), arg);
      return;
    }

  back_to = make_cleanup (xfree, demangled_name);
  make_cleanup (free_current_contents, &storage);
  make_cleanup (free_current_contents, &demangled_name_storage);
  symbol = lookup_symbol (demangled_name, NULL, VAR_DOMAIN, NULL);
  if (symbol.symbol == NULL)
    {
      fprintf_filtered (gdb_stderr, _("could not find symbol for \"%s\"\n"),
			arg);
      return;
    }

  if (!SYMBOL_IS_CPLUS_TEMPLATE_FUNCTION (symbol.symbol))
    {
      fprintf_filtered (gdb_stderr, _("symbol \"%s\" does not represent a"
				      " template function\n"), arg);
      return;
    }

  tsymbol = (struct template_symbol *) symbol.symbol;
  cp_decode_template_type_indices (tsymbol, NULL);
  info = cp_mangled_name_to_comp (arg, DMGL_ANSI | DMGL_PARAMS, &storage,
				  &demangled_name_storage);
  make_cleanup_cp_demangled_name_parse_free (info);
  str = ccp_function_template_decl (tsymbol, info);
  do_cleanups (back_to);
  fprintf_filtered (gdb_stdout, "%s\n", str);
  xfree (str);
}

/* If TYPE is a previously unseen class template, define the template
   generic.  */

static void
ccp_maybe_define_new_class_template (struct compile_cplus_instance *instance,
				     struct type *type, const char *name)
{
  int i;
  void **slot;
  struct class_template_defn *defn;

  if (TYPE_N_TEMPLATE_ARGUMENTS (type) == 0)
    return;

  defn = new_class_template_defn (name, type);
  slot = htab_find_slot (instance->class_template_defns, defn, INSERT);
  if (*slot == NULL)
    {
      /* New template definition.  Allocate memory for the parameters and
	 default values.  Then copy it into the new slot.  */
      defn->params
	= new_gcc_cp_template_args (TYPE_N_TEMPLATE_ARGUMENTS (type));
      defn->default_arguments
	= XCNEWVEC (struct symbol *, TYPE_N_TEMPLATE_ARGUMENTS (type));
      *slot = defn;
    }
  else
    {
      /* Existing definition.  Use the existing slot.  */
      delete_function_template_defn (defn);
      defn = (struct class_template_defn *) *slot;
    }

  /* Loop over the template arguments, noting any default values.  */
  for (i = 0; i < TYPE_N_TEMPLATE_ARGUMENTS (type); ++i)
    {
      if (defn->default_arguments[i] == NULL
	  && TYPE_TEMPLATE_DEFAULT_ARGUMENT (type, i) != NULL)
	{
	  defn->default_arguments[i]
	    = TYPE_TEMPLATE_DEFAULT_ARGUMENT (type, i);

	  /* We don't want to define them here because it could start
	     emitting template definitions before we're even done
	     collecting the default values.  [Easy to demonstrate if the
	     default value is a class.]  */
	}
    }
}



/* Delete the compiler instance C.  */

static void
delete_instance (struct compile_instance *c)
{
  struct compile_cplus_instance *instance = (struct compile_cplus_instance *) c;

  instance->base.fe->ops->destroy (instance->base.fe);
  htab_delete (instance->type_map);
  if (instance->symbol_err_map != NULL)
    htab_delete (instance->symbol_err_map);
  htab_delete (instance->function_template_defns);
  htab_delete (instance->class_template_defns);
  xfree (instance);
}

/* See compile-internal.h.  */

struct compile_instance *
new_cplus_compile_instance (struct gcc_cp_context *fe)
{
  struct compile_cplus_instance *result = XCNEW (struct compile_cplus_instance);

  result->base.fe = &fe->base;
  result->base.destroy = delete_instance;
  result->base.gcc_target_options = ("-std=gnu++11"
				     /* We don't need this any more,
					the user expression function
					is regarded as a friend of
					every class, so that GDB users
					get to access private and
					protected members.

				     " -fno-access-control"

				     */
				     /* Otherwise the .o file may need
					"_Unwind_Resume" and
					"__gcc_personality_v0".

					??? Why would that be a
					problem? -lxo

				     " -fno-exceptions"

				     */
				     );

  result->type_map = htab_create_alloc (10, hash_type_map_instance,
					eq_type_map_instance,
					xfree, xcalloc, xfree);
  result->function_template_defns
    = htab_create_alloc (10, hash_template_defn, eq_template_defn,
			 delete_function_template_defn, xcalloc, xfree);

  result->class_template_defns
    = htab_create_alloc (10, hash_template_defn, eq_template_defn,
			 delete_class_template_defn, xcalloc, xfree);

  fe->cp_ops->set_callbacks (fe, gcc_cplus_convert_symbol,
			     gcc_cplus_symbol_address,
			     gcc_cplus_enter_scope,
			     gcc_cplus_leave_scope,
			     result);

  return &result->base;
}

void _initialize_compile_cplus_types (void);

void
_initialize_compile_cplus_types (void)
{
  add_setshow_boolean_cmd ("compile-cplus-types", no_class,
			     &debug_compile_cplus_types, _("\
Set debugging of C++ compile type conversion."), _("\
Show debugging of C++ compile type conversion."), _("\
When enabled debugging messages are printed during C++ type conversion for\n\
the compile commands."),
			     NULL,
			     NULL,
			     &setdebuglist,
			     &showdebuglist);

  add_setshow_boolean_cmd ("compile-cplus-contexts", no_class,
			     &debug_compile_cplus_contexts, _("\
Set debugging of C++ compile contexts."), _("\
Show debugging of C++ compile contexts."), _("\
When enabled debugging messages are printed about definition contexts during\n\
C++ type conversion for the compile commands."),
			     NULL,
			     NULL,
			     &setdebuglist,
			     &showdebuglist);

  add_cmd ("tdef", class_maintenance, print_template_defn_command,
	   _("Print the template generic for the given linkage name."),
	   &maint_cplus_cmd_list);

}
