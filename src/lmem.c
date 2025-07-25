/*
** $Id: lmem.c $
** Interface to Memory Manager
** See Copyright Notice in sol.h
*/

#define lmem_c
#define SOL_CORE

#include "lprefix.h"


#include <stddef.h>

#include "sol.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"



/*
** About the realloc function:
** void *frealloc (void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize' is the old size, 'nsize' is the new size)
**
** - frealloc(ud, p, x, 0) frees the block 'p' and returns NULL.
** Particularly, frealloc(ud, NULL, 0, 0) does nothing,
** which is equivalent to free(NULL) in ISO C.
**
** - frealloc(ud, NULL, x, s) creates a new block of size 's'
** (no matter 'x'). Returns NULL if it cannot create the new block.
**
** - otherwise, frealloc(ud, b, x, y) reallocates the block 'b' from
** size 'x' to size 'y'. Returns NULL if it cannot reallocate the
** block to the new size.
*/


/*
** Macro to call the allocation function.
*/
#define callfrealloc(g,block,os,ns)    ((*g->frealloc)(g->ud, block, os, ns))


/*
** When an allocation fails, it will try again after an emergency
** collection, except when it cannot run a collection.  The GC should
** not be called while the state is not fully built, as the collector
** is not yet fully initialized. Also, it should not be called when
** 'gcstopem' is true, because then the interpreter is in the middle of
** a collection step.
*/
#define cantryagain(g)	(completestate(g) && !g->gcstopem)




#if defined(EMERGENCYGCTESTS)
/*
** First allocation will fail except when freeing a block (frees never
** fail) and when it cannot try again; this fail will trigger 'tryagain'
** and a full GC cycle at every allocation.
*/
static void *firsttry (global_State *g, void *block, size_t os, size_t ns) {
  if (ns > 0 && cantryagain(g))
    return NULL;  /* fail */
  else  /* normal allocation */
    return callfrealloc(g, block, os, ns);
}
#else
#define firsttry(g,block,os,ns)    callfrealloc(g, block, os, ns)
#endif





/*
** {==================================================================
** Functions to allocate/deallocate arrays for the Parser
** ===================================================================
*/

/*
** Minimum size for arrays during parsing, to avoid overhead of
** reallocating to size 1, then 2, and then 4. All these arrays
** will be reallocated to exact sizes or erased when parsing ends.
*/
#define MINSIZEARRAY	4


void *solM_growaux_ (sol_State *L, void *block, int nelems, int *psize,
                     int size_elems, int limit, const char *what) {
  void *newblock;
  int size = *psize;
  if (nelems + 1 <= size)  /* does one extra element still fit? */
    return block;  /* nothing to be done */
  if (size >= limit / 2) {  /* cannot double it? */
    if (l_unlikely(size >= limit))  /* cannot grow even a little? */
      solG_runerror(L, "too many %s (limit is %d)", what, limit);
    size = limit;  /* still have at least one free place */
  }
  else {
    size *= 2;
    if (size < MINSIZEARRAY)
      size = MINSIZEARRAY;  /* minimum size */
  }
  sol_assert(nelems + 1 <= size && size <= limit);
  /* 'limit' ensures that multiplication will not overflow */
  newblock = solM_saferealloc_(L, block, cast_sizet(*psize) * size_elems,
                                         cast_sizet(size) * size_elems);
  *psize = size;  /* update only when everything else is OK */
  return newblock;
}


/*
** In prototypes, the size of the array is also its number of
** elements (to save memory). So, if it cannot shrink an array
** to its number of elements, the only option is to raise an
** error.
*/
void *solM_shrinkvector_ (sol_State *L, void *block, int *size,
                          int final_n, int size_elem) {
  void *newblock;
  size_t oldsize = cast_sizet((*size) * size_elem);
  size_t newsize = cast_sizet(final_n * size_elem);
  sol_assert(newsize <= oldsize);
  newblock = solM_saferealloc_(L, block, oldsize, newsize);
  *size = final_n;
  return newblock;
}

/* }================================================================== */


l_noret solM_toobig (sol_State *L) {
  solG_runerror(L, "memory allocation error: block too big");
}


/*
** Free memory
*/
void solM_free_ (sol_State *L, void *block, size_t osize) {
  global_State *g = G(L);
  sol_assert((osize == 0) == (block == NULL));
  callfrealloc(g, block, osize, 0);
  g->GCdebt -= osize;
}


/*
** In case of allocation fail, this function will do an emergency
** collection to free some memory and then try the allocation again.
*/
static void *tryagain (sol_State *L, void *block,
                       size_t osize, size_t nsize) {
  global_State *g = G(L);
  if (cantryagain(g)) {
    solC_fullgc(L, 1);  /* try to free some memory... */
    return callfrealloc(g, block, osize, nsize);  /* try again */
  }
  else return NULL;  /* cannot run an emergency collection */
}


/*
** Generic allocation routine.
*/
void *solM_realloc_ (sol_State *L, void *block, size_t osize, size_t nsize) {
  void *newblock;
  global_State *g = G(L);
  sol_assert((osize == 0) == (block == NULL));
  newblock = firsttry(g, block, osize, nsize);
  if (l_unlikely(newblock == NULL && nsize > 0)) {
    newblock = tryagain(L, block, osize, nsize);
    if (newblock == NULL)  /* still no memory? */
      return NULL;  /* do not update 'GCdebt' */
  }
  sol_assert((nsize == 0) == (newblock == NULL));
  g->GCdebt = (g->GCdebt + nsize) - osize;
  return newblock;
}


void *solM_saferealloc_ (sol_State *L, void *block, size_t osize,
                                                    size_t nsize) {
  void *newblock = solM_realloc_(L, block, osize, nsize);
  if (l_unlikely(newblock == NULL && nsize > 0))  /* allocation failed? */
    solM_error(L);
  return newblock;
}


void *solM_malloc_ (sol_State *L, size_t size, int tag) {
  if (size == 0)
    return NULL;  /* that's all */
  else {
    global_State *g = G(L);
    void *newblock = firsttry(g, NULL, tag, size);
    if (l_unlikely(newblock == NULL)) {
      newblock = tryagain(L, NULL, tag, size);
      if (newblock == NULL)
        solM_error(L);
    }
    g->GCdebt += size;
    return newblock;
  }
}
