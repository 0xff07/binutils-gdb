/* ldindr.c
   Handle indirect symbols.

   An indirect symbol is where a global symbol in one file say's that
   all refs like it should be turned into refs of the symbol pointed
   at by the value of the indirect symbol.

   BFD supplies symbols to be indirected with the BFD_INDIRECT bit
   set. Whenever the linker gets one of these, it calls add_indirect
   with the symbol. We look up the symbol which this one dereferneces,
   and stop if they are the same. If they are not the same, copy all
   the information from the current to the dereffed symbol. Set the
   indirect bit in the flag. From now on the ldsym_get stuff will
   perform the indirection for us, at no charge.
*/



#include "bfd.h"
#include "sysdep.h"
#include "ld.h"
#include "ldsym.h"
#include "ldmisc.h"



static asymbol **
DEFUN(move_it,(a_list, b_list),
asymbol **a_list AND
asymbol **b_list)
{
  asymbol **head = a_list;
  asymbol **cursor = head;

  if (a_list == 0) return b_list;
  if (b_list == 0) return a_list;

  while (1) {
    asymbol *ptr = cursor[0];
    asymbol **next = (asymbol **)(ptr->udata);
    if (next == 0) {
      ptr->udata = (PTR) b_list;
      return head;
    }
    cursor = next;
  }
}

#if 0
void 
DEFUN(copy_over,(ldsym, bfdsym),
   ldsym_type *ldsym AND
   asymbol **bfdsym)
{
  while (list && *list)
  {
    refize(Q_enter_global_ref(list, name);
    list = (asymbol **)((*list)->udata);
  }
}
#endif

/* This call allows us to change the symbol table so that all future
   refs to the symbol are patched to know the alias - but we still
   have to fix all the old ones */
void 
DEFUN(add_indirect,(ptr),
asymbol **ptr)
{
  asymbol **p;
  ldsym_type *lgs = ldsym_get((*ptr)->name);
  ldsym_type *new = ldsym_get(((asymbol *)((*ptr)->value))->name);

  /* If the mapping has already been done, stop now */
  if (lgs == new) return;

  lgs->flags |= SYM_INDIRECT;

  if (lgs->sdefs_chain && lgs->sdefs_chain[0]) 
  {
    einfo("indirect symbol already has definition %s",   lgs->sdefs_chain[0]);
  }
  new->scoms_chain = move_it(new->scoms_chain, lgs->scoms_chain);
  lgs->scoms_chain = 0;
  new->srefs_chain = move_it(new->srefs_chain, lgs->srefs_chain);
  lgs->srefs_chain = 0;
  new->sdefs_chain = move_it(new->sdefs_chain, lgs->sdefs_chain);
  lgs->sdefs_chain = 0;

  /* If the result has any commons they should be turned into refs */

  if (new->sdefs_chain && new->scoms_chain) 
  {
    refize(new, new->scoms_chain);
  }    
lgs->sdefs_chain = (asymbol **)new;

}



