/*
 * Copyright 1988, 1989 Hans-J. Boehm, Alan J. Demers
 * Copyright (c) 1991-1996 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996-1999 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1999-2004 Hewlett-Packard Development Company, L.P.
 * Copyright (c) 2009-2022 Ivan Maidanski
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

#include "private/gc_priv.h"

#ifdef ENABLE_DISCLAIM
# include "gc/gc_disclaim.h"
#endif

GC_INNER signed_word GC_bytes_found = 0;
                        /* Number of bytes of memory reclaimed     */
                        /* minus the number of bytes originally    */
                        /* on free lists which we had to drop.     */

#if defined(PARALLEL_MARK)
  GC_INNER signed_word GC_fl_builder_count = 0;
        /* Number of threads currently building free lists without      */
        /* holding the allocator lock.  It is not safe to collect if    */
        /* this is nonzero.  Also, together with the mark lock, it is   */
        /* used as a semaphore during marker threads startup.           */
#endif /* PARALLEL_MARK */

/* We defer printing of leaked objects until we're done with the GC     */
/* cycle, since the routine for printing objects needs to run outside   */
/* the collector, e.g. without the allocator lock.                      */
#ifndef MAX_LEAKED
# define MAX_LEAKED 40
#endif
STATIC ptr_t GC_leaked[MAX_LEAKED] = { NULL };
STATIC unsigned GC_n_leaked = 0;

#ifdef AO_HAVE_store
  GC_INNER volatile AO_t GC_have_errors = 0;
#else
  GC_INNER GC_bool GC_have_errors = FALSE;
#endif

#if !defined(EAGER_SWEEP) && defined(ENABLE_DISCLAIM)
  STATIC void GC_reclaim_unconditionally_marked(void);
#endif

GC_INLINE void GC_add_leaked(ptr_t leaked)
{
    GC_ASSERT(I_HOLD_LOCK());
#   ifndef SHORT_DBG_HDRS
      if (GC_findleak_delay_free && !GC_check_leaked(leaked))
        return;
#   endif

    GC_SET_HAVE_ERRORS();
    if (GC_n_leaked < MAX_LEAKED) {
      GC_leaked[GC_n_leaked++] = leaked;
      /* Make sure it's not reclaimed this cycle */
      GC_set_mark_bit(leaked);
    }
}

/* Print all objects on the list after printing any smashed objects.    */
/* Clear both lists.  Called without the allocator lock held.           */
GC_INNER void GC_print_all_errors(void)
{
    static GC_bool printing_errors = FALSE;
    GC_bool have_errors;
    unsigned i, n_leaked;
    ptr_t leaked[MAX_LEAKED];

    LOCK();
    if (printing_errors) {
        UNLOCK();
        return;
    }
    have_errors = get_have_errors();
    printing_errors = TRUE;
    n_leaked = GC_n_leaked;
    if (n_leaked > 0) {
      GC_ASSERT(n_leaked <= MAX_LEAKED);
      BCOPY(GC_leaked, leaked, n_leaked * sizeof(ptr_t));
      GC_n_leaked = 0;
      BZERO(GC_leaked, n_leaked * sizeof(ptr_t));
    }
    UNLOCK();

    if (GC_debugging_started) {
      GC_print_all_smashed();
    } else {
      have_errors = FALSE;
    }

    if (n_leaked > 0) {
        GC_err_printf("Found %u leaked objects:\n", n_leaked);
        have_errors = TRUE;
    }
    for (i = 0; i < n_leaked; i++) {
        ptr_t p = leaked[i];
#       ifndef SKIP_LEAKED_OBJECTS_PRINTING
          GC_print_heap_obj(p);
#       endif
        GC_free(p);
    }

    if (have_errors
#       ifndef GC_ABORT_ON_LEAK
          && GETENV("GC_ABORT_ON_LEAK") != NULL
#       endif
        ) {
      ABORT("Leaked or smashed objects encountered");
    }

    LOCK();
    printing_errors = FALSE;
    UNLOCK();
}


/*
 * reclaim phase
 *
 */

/* Test whether a block is completely empty, i.e. contains no marked    */
/* objects.  This does not require the block to be in physical memory.  */
GC_INNER GC_bool GC_block_empty(const hdr *hhdr)
{
    return 0 == hhdr -> hb_n_marks;
}

STATIC GC_bool GC_block_nearly_full(const hdr *hhdr, size_t sz)
{
    return hhdr -> hb_n_marks > HBLK_OBJS(sz) * 7 / 8;
}

/* TODO: This should perhaps again be specialized for USE_MARK_BYTES    */
/* and USE_MARK_BITS cases.                                             */

GC_INLINE ptr_t GC_clear_block(ptr_t q, size_t sz, word *pcount)
{
  ptr_t *p = (ptr_t *)q;
  ptr_t plim = q + sz;

  /* Clear object, advance p to next object in the process.     */
# ifdef USE_MARK_BYTES
    GC_ASSERT((sz & 1) == 0);
    GC_ASSERT((ADDR(p) & (2*sizeof(ptr_t)-1)) == 0);
    p[1] = NULL; /* but do not clear link field */
    for (p += 2; ADDR_LT((ptr_t)p, plim); p += 2) {
      CLEAR_DOUBLE(p);
    }
# else
    p++; /* skip link field */
    while (ADDR_LT((ptr_t)p, plim)) {
      *p++ = NULL;
    }
# endif
  *pcount += sz;
  return (ptr_t)p;
}

/*
 * Restore unmarked small objects in h of size sz to the object
 * free list.  Returns the new list.
 * Clears unmarked objects.  Sz is in bytes.
 */
STATIC ptr_t GC_reclaim_clear(struct hblk *hbp, const hdr *hhdr, size_t sz,
                              ptr_t list, word *pcount)
{
    size_t bit_no;
    ptr_t p, plim;

    GC_ASSERT(hhdr == GC_find_header(hbp));
#   ifndef THREADS
      GC_ASSERT(sz == hhdr -> hb_sz);
#   else
      /* Skip the assertion because of a potential race with GC_realloc. */
#   endif
    GC_ASSERT((sz & (sizeof(ptr_t)-1)) == 0);

    /* Go through all objects in the block. */
    p = hbp -> hb_body;
    plim = p + HBLKSIZE - sz;
    for (bit_no = 0; ADDR_GE(plim, p); bit_no += MARK_BIT_OFFSET(sz)) {
        if (mark_bit_from_hdr(hhdr, bit_no)) {
            p += sz;
        } else {
            /* The object is available - put it on list. */
            obj_link(p) = list;
            list = p;
            FREE_PROFILER_HOOK(p);
            p = GC_clear_block(p, sz, pcount);
        }
    }
    return list;
}

/* The same thing, but don't clear objects: */
STATIC ptr_t GC_reclaim_uninit(struct hblk *hbp, const hdr *hhdr, size_t sz,
                               ptr_t list, word *pcount)
{
    size_t bit_no;
    word n_bytes_found = 0;
    ptr_t p, plim;

#   ifndef THREADS
      GC_ASSERT(sz == hhdr -> hb_sz);
#   endif

    /* Go through all objects in the block. */
    p = hbp -> hb_body;
    plim = (ptr_t)hbp + HBLKSIZE - sz;
    for (bit_no = 0; ADDR_GE(plim, p);
         bit_no += MARK_BIT_OFFSET(sz), p += sz) {
        if (!mark_bit_from_hdr(hhdr, bit_no)) {
            n_bytes_found += sz;
            /* The object is available - put it on list. */
            obj_link(p) = list;
            list = p;
            FREE_PROFILER_HOOK(p);
        }
    }
    *pcount += n_bytes_found;
    return list;
}

#ifdef ENABLE_DISCLAIM
  /* Call reclaim notifier for block's kind on each unmarked object in  */
  /* block, all within a pair of corresponding enter/leave callbacks.   */
  STATIC ptr_t GC_disclaim_and_reclaim(struct hblk *hbp, hdr *hhdr, size_t sz,
                                       ptr_t list, word *pcount)
  {
    size_t bit_no;
    ptr_t p, plim;
    int (GC_CALLBACK *disclaim)(void *) =
                GC_obj_kinds[hhdr -> hb_obj_kind].ok_disclaim_proc;

    GC_ASSERT(disclaim != 0);
#   ifndef THREADS
      GC_ASSERT(sz == hhdr -> hb_sz);
#   endif
    p = hbp -> hb_body;
    plim = p + HBLKSIZE - sz;

    for (bit_no = 0; ADDR_GE(plim, p); bit_no += MARK_BIT_OFFSET(sz)) {
        if (mark_bit_from_hdr(hhdr, bit_no)) {
            p += sz;
        } else if (disclaim(p)) {
            set_mark_bit_from_hdr(hhdr, bit_no);
            INCR_MARKS(hhdr);
            p += sz;
        } else {
            obj_link(p) = list;
            list = p;
            FREE_PROFILER_HOOK(p);
            p = GC_clear_block(p, sz, pcount);
        }
    }
    return list;
  }
#endif /* ENABLE_DISCLAIM */

/* Don't really reclaim objects, just check for unmarked ones: */
STATIC void GC_reclaim_check(struct hblk *hbp, const hdr *hhdr, size_t sz)
{
    size_t bit_no;
    ptr_t p, plim;

#   ifndef THREADS
      GC_ASSERT(sz == hhdr -> hb_sz);
#   endif
    /* Go through all objects in the block. */
    p = hbp -> hb_body;
    plim = p + HBLKSIZE - sz;
    for (bit_no = 0; ADDR_GE(plim, p);
         bit_no += MARK_BIT_OFFSET(sz), p += sz) {
      if (!mark_bit_from_hdr(hhdr, bit_no))
        GC_add_leaked(p);
    }
}

/* Is a pointer-free block?  Same as IS_PTRFREE() macro but uses    */
/* unordered atomic access to avoid racing with GC_realloc.         */
#ifdef AO_HAVE_load
# define IS_PTRFREE_SAFE(hhdr) (AO_load(&((hhdr) -> hb_descr)) == 0)
#else
  /* No race as GC_realloc holds the allocator lock when updating hb_descr. */
# define IS_PTRFREE_SAFE(hhdr) IS_PTRFREE(hhdr)
#endif

/* Generic procedure to rebuild a free list in hbp.  Also called    */
/* directly from GC_malloc_many.  sz is in bytes.                   */
GC_INNER ptr_t GC_reclaim_generic(struct hblk *hbp, hdr *hhdr, size_t sz,
                                  GC_bool init, ptr_t list, word *pcount)
{
    ptr_t result;

#   ifndef PARALLEL_MARK
      GC_ASSERT(I_HOLD_LOCK());
#   endif
    GC_ASSERT(GC_find_header(hbp) == hhdr);
#   ifndef GC_DISABLE_INCREMENTAL
      GC_remove_protection(hbp, 1, IS_PTRFREE_SAFE(hhdr));
#   endif
#   ifdef ENABLE_DISCLAIM
      if ((hhdr -> hb_flags & HAS_DISCLAIM) != 0) {
        result = GC_disclaim_and_reclaim(hbp, hhdr, sz, list, pcount);
      } else
#   endif
    /* else */ if (init || GC_debugging_started) {
      result = GC_reclaim_clear(hbp, hhdr, sz, list, pcount);
    } else {
#     ifndef AO_HAVE_load
        GC_ASSERT(IS_PTRFREE(hhdr));
#     endif
      result = GC_reclaim_uninit(hbp, hhdr, sz, list, pcount);
    }
    if (IS_UNCOLLECTABLE(hhdr -> hb_obj_kind)) GC_set_hdr_marks(hhdr);
    return result;
}

/*
 * Restore unmarked small objects in the block pointed to by hbp
 * to the appropriate object free list.
 * If entirely empty blocks are to be completely deallocated, then
 * caller should perform that check.
 */
STATIC void GC_reclaim_small_nonempty_block(struct hblk *hbp, size_t sz,
                                            GC_bool report_if_found)
{
    hdr *hhdr;

    GC_ASSERT(I_HOLD_LOCK());
    hhdr = HDR(hbp);
    hhdr -> hb_last_reclaimed = (unsigned short)GC_gc_no;
    if (report_if_found) {
        GC_reclaim_check(hbp, hhdr, sz);
    } else {
        struct obj_kind *ok = &GC_obj_kinds[hhdr -> hb_obj_kind];
        void **flh = &(ok -> ok_freelist[BYTES_TO_GRANULES(sz)]);

        *flh = GC_reclaim_generic(hbp, hhdr, sz, ok -> ok_init, (ptr_t)(*flh),
                                  (/* unsigned */ word *)&GC_bytes_found);
    }
}

#ifdef ENABLE_DISCLAIM
  STATIC void GC_disclaim_and_reclaim_or_free_small_block(struct hblk *hbp)
  {
    hdr *hhdr;
    size_t sz;
    struct obj_kind *ok;
    void **flh;
    void *flh_next;

    GC_ASSERT(I_HOLD_LOCK());
    hhdr = HDR(hbp);
    sz = hhdr -> hb_sz;
    ok = &GC_obj_kinds[hhdr -> hb_obj_kind];
    flh = &(ok -> ok_freelist[BYTES_TO_GRANULES(sz)]);

    hhdr -> hb_last_reclaimed = (unsigned short)GC_gc_no;
    flh_next = GC_reclaim_generic(hbp, hhdr, sz, ok -> ok_init, (ptr_t)(*flh),
                                  (/* unsigned */ word *)&GC_bytes_found);
    if (hhdr -> hb_n_marks) {
        *flh = flh_next;
    } else {
        GC_bytes_found += (signed_word)HBLKSIZE;
        GC_freehblk(hbp);
    }
  }
#endif /* ENABLE_DISCLAIM */

/* Restore an unmarked large object or an entirely empty blocks of      */
/* small objects to the heap block free list.  Otherwise enqueue the    */
/* block for later processing by GC_reclaim_small_nonempty_block.       */
/* If report_if_found is TRUE, then process any block immediately, and  */
/* simply report free objects; do not actually reclaim them.            */
STATIC void GC_CALLBACK GC_reclaim_block(struct hblk *hbp,
                                         void *report_if_found)
{
    hdr *hhdr;
    size_t sz;  /* size of objects in current block */
    struct obj_kind *ok;

    GC_ASSERT(I_HOLD_LOCK());
#   if defined(CPPCHECK)
        GC_noop1_ptr(report_if_found);
#   endif
    hhdr = HDR(hbp);
    ok = &GC_obj_kinds[hhdr -> hb_obj_kind];
#   ifdef AO_HAVE_load
        /* Atomic access is used to avoid racing with GC_realloc.       */
        sz = AO_load(&(hhdr -> hb_sz));
#   else
        /* No race as GC_realloc holds the allocator lock while */
        /* updating hb_sz.                                      */
        sz = hhdr -> hb_sz;
#   endif
    if (sz > MAXOBJBYTES) { /* 1 big object */
        if (!mark_bit_from_hdr(hhdr, 0)) {
            if (report_if_found) {
              GC_add_leaked((ptr_t)hbp);
            } else {
#             ifdef ENABLE_DISCLAIM
                if (EXPECT(hhdr -> hb_flags & HAS_DISCLAIM, 0)) {
                  if (ok -> ok_disclaim_proc(hbp)) {
                    /* Not disclaimed => resurrect the object. */
                    set_mark_bit_from_hdr(hhdr, 0);
                    goto in_use;
                  }
                }
#             endif
              if (sz > HBLKSIZE) {
                GC_large_allocd_bytes -= HBLKSIZE * OBJ_SZ_TO_BLOCKS(sz);
              }
              GC_bytes_found += (signed_word)sz;
              GC_freehblk(hbp);
              FREE_PROFILER_HOOK(hbp);
            }
        } else {
#        ifdef ENABLE_DISCLAIM
           in_use:
#        endif
            if (IS_PTRFREE_SAFE(hhdr)) {
              GC_atomic_in_use += sz;
            } else {
              GC_composite_in_use += sz;
            }
        }
    } else {
        GC_bool empty = GC_block_empty(hhdr);

#       ifdef PARALLEL_MARK
          /* Count can be low or one too high because we sometimes      */
          /* have to ignore decrements.  Objects can also potentially   */
          /* be repeatedly marked by each marker.                       */
          /* Here we assume 3 markers at most, but this is extremely    */
          /* unlikely to fail spuriously with more.  And if it does, it */
          /* should be looked at.                                       */
          GC_ASSERT(sz != 0 && (GC_markers_m1 > 1 ? 3 : GC_markers_m1 + 1)
                                * (HBLKSIZE/sz + 1) + 16 >= hhdr->hb_n_marks);
#       else
          GC_ASSERT(sz * hhdr -> hb_n_marks <= HBLKSIZE);
#       endif
#       ifdef VALGRIND_TRACKING
          /* Call GC_free_profiler_hook() on freed objects so that  */
          /* a profiling tool could track the allocations.          */
          {
            ptr_t p = hbp -> hb_body;
            ptr_t plim = p + HBLKSIZE - sz;
            size_t bit_no;

            for (bit_no = 0; ADDR_GE(plim, p);
                 bit_no += MARK_BIT_OFFSET(sz), p += sz) {
              if (!mark_bit_from_hdr(hhdr, bit_no))
                FREE_PROFILER_HOOK(p);
            }
          }
#       endif
        if (report_if_found) {
          GC_reclaim_small_nonempty_block(hbp, sz,
                                          TRUE /* report_if_found */);
        } else if (empty) {
#       ifdef ENABLE_DISCLAIM
          if ((hhdr -> hb_flags & HAS_DISCLAIM) != 0) {
            GC_disclaim_and_reclaim_or_free_small_block(hbp);
          } else
#       endif
          /* else */ {
            GC_bytes_found += (signed_word)HBLKSIZE;
            GC_freehblk(hbp);
            FREE_PROFILER_HOOK(hbp);
          }
        } else if (GC_find_leak || !GC_block_nearly_full(hhdr, sz)) {
          /* group of smaller objects, enqueue the real work */
          struct hblk **rlh = ok -> ok_reclaim_list;

          if (rlh != NULL) {
            rlh += BYTES_TO_GRANULES(sz);
            hhdr -> hb_next = *rlh;
            *rlh = hbp;
          }
        } /* else not worth salvaging. */
        /* We used to do the nearly_full check later, but we    */
        /* already have the right cache context here.  Also     */
        /* doing it here avoids some silly lock contention in   */
        /* GC_malloc_many.                                      */
        if (IS_PTRFREE_SAFE(hhdr)) {
          GC_atomic_in_use += (word)sz * hhdr -> hb_n_marks;
        } else {
          GC_composite_in_use += (word)sz * hhdr -> hb_n_marks;
        }
    }
}

#if !defined(NO_DEBUGGING)
  /* Routines to gather and print heap block info intended for      */
  /* debugging.  Otherwise should be called with the allocator lock */
  /* held.                                                          */

  struct Print_stats
  {
    size_t number_of_blocks;
    size_t total_bytes;
  };

  EXTERN_C_BEGIN /* to avoid "no previous prototype" clang warning */
  unsigned GC_n_set_marks(const hdr *);
  EXTERN_C_END

# ifdef USE_MARK_BYTES
    /* Return the number of set mark bits in the given header.      */
    /* Remains externally visible as used by GNU GCJ currently.     */
    /* There could be a race between GC_clear_hdr_marks and this    */
    /* function but the latter is for a debug purpose.              */
    GC_ATTR_NO_SANITIZE_THREAD
    unsigned GC_n_set_marks(const hdr *hhdr)
    {
      unsigned result = 0;
      size_t i;
      size_t offset = MARK_BIT_OFFSET(hhdr -> hb_sz);
      size_t limit = FINAL_MARK_BIT(hhdr -> hb_sz);

      for (i = 0; i < limit; i += offset) {
        result += (unsigned)(hhdr -> hb_marks[i]);
      }
      GC_ASSERT(hhdr -> hb_marks[limit]); /* the one set past the end */
      return result;
    }

# else
    /* Number of set bits in a word.  Not performance critical.     */
    static unsigned count_ones(word v)
    {
      unsigned result = 0;

      for (; v > 0; v >>= 1) {
        if (v & 1) result++;
      }
      return result;
    }

    unsigned GC_n_set_marks(const hdr *hhdr)
    {
      unsigned result = 0;
      size_t sz = hhdr -> hb_sz;
      size_t i;
#     ifdef MARK_BIT_PER_OBJ
        size_t n_objs = HBLK_OBJS(sz);
        size_t n_mark_words
                    = divWORDSZ(n_objs > 0 ? n_objs : 1); /* round down */

        for (i = 0; i <= n_mark_words; i++) {
          result += count_ones(hhdr -> hb_marks[i]);
        }
#     else

        for (i = 0; i < HB_MARKS_SZ; i++) {
          result += count_ones(hhdr -> hb_marks[i]);
        }
#     endif
      GC_ASSERT(result > 0);
      result--; /* exclude the one bit set past the end */
#     ifndef MARK_BIT_PER_OBJ
        if (IS_UNCOLLECTABLE(hhdr -> hb_obj_kind)) {
          size_t lg = BYTES_TO_GRANULES(sz);

          /* As mentioned in GC_set_hdr_marks(), all the bits are set   */
          /* instead of every n-th, thus the result should be adjusted. */
          GC_ASSERT((unsigned)lg != 0 && result % lg == 0);
          result /= (unsigned)lg;
        }
#     endif
      return result;
    }
# endif /* !USE_MARK_BYTES  */

  GC_API unsigned GC_CALL GC_count_set_marks_in_hblk(const void *p) {
    return GC_n_set_marks(HDR(p));
  }

  STATIC void GC_CALLBACK GC_print_block_descr(struct hblk *h, void *raw_ps)
  {
    const hdr *hhdr = HDR(h);
    size_t sz = hhdr -> hb_sz;
    struct Print_stats *ps = (struct Print_stats *)raw_ps;
    size_t n_marks = (size_t)GC_n_set_marks(hhdr);
    size_t n_objs = HBLK_OBJS(sz);

#   ifndef PARALLEL_MARK
      GC_ASSERT(hhdr -> hb_n_marks == n_marks);
#   endif
#   if defined(CPPCHECK)
        GC_noop1_ptr(h);
#   endif
    GC_ASSERT((n_objs > 0 ? n_objs : 1) >= n_marks);
    GC_printf("%u,%u,%u,%u\n", hhdr -> hb_obj_kind, (unsigned)sz,
              (unsigned)n_marks, (unsigned)n_objs);
    ps -> number_of_blocks++;
    ps -> total_bytes += (sz + HBLKSIZE-1) & ~(HBLKSIZE-1); /* round up */
  }

  void GC_print_block_list(void)
  {
    struct Print_stats pstats;

    GC_printf("kind(0=ptrfree/1=normal/2=unc.),"
              "obj_sz,#marks_set,#objs_in_block\n");
    BZERO(&pstats, sizeof(pstats));
    GC_apply_to_all_blocks(GC_print_block_descr, &pstats);
    GC_printf("blocks= %lu, total_bytes= %lu\n",
              (unsigned long)pstats.number_of_blocks,
              (unsigned long)pstats.total_bytes);
  }

  GC_API void GC_CALL GC_print_free_list(int k, size_t lg)
  {
    void *flh_next;
    int n;

    GC_ASSERT(k < MAXOBJKINDS);
    GC_ASSERT(lg <= MAXOBJGRANULES);
    flh_next = GC_obj_kinds[k].ok_freelist[lg];
    for (n = 0; flh_next != NULL; n++) {
      GC_printf("Free object in heap block %p [%d]: %p\n",
                (void *)HBLKPTR(flh_next), n, flh_next);
      flh_next = obj_link(flh_next);
    }
  }
#endif /* !NO_DEBUGGING */

/*
 * Clear all obj_link pointers in the list of free objects *flp.
 * Clear *flp.
 * This must be done before dropping a list of free gcj-style objects,
 * since may otherwise end up with dangling "descriptor" pointers.
 * It may help for other pointer-containing objects.
 */
STATIC void GC_clear_fl_links(void **flp)
{
    void *next;

    for (next = *flp; next != NULL; next = *flp) {
       *flp = NULL;
       flp = &obj_link(next);
    }
}

/*
 * Perform GC_reclaim_block on the entire heap, after first clearing
 * small-object free lists (if we are not just looking for leaks).
 */
GC_INNER void GC_start_reclaim(GC_bool report_if_found)
{
    int k;

    GC_ASSERT(I_HOLD_LOCK());
#   if defined(PARALLEL_MARK)
      GC_ASSERT(0 == GC_fl_builder_count);
#   endif
    /* Reset in use counters.  GC_reclaim_block recomputes them. */
      GC_composite_in_use = 0;
      GC_atomic_in_use = 0;
    /* Clear reclaim- and free-lists.   */
    for (k = 0; k < (int)GC_n_kinds; k++) {
        struct hblk **rlist = GC_obj_kinds[k].ok_reclaim_list;
        GC_bool should_clobber = GC_obj_kinds[k].ok_descriptor != 0;

        if (NULL == rlist) continue; /* means this object kind is not used */

        if (!report_if_found) {
            void **fop;
            void **lim = &GC_obj_kinds[k].ok_freelist[MAXOBJGRANULES + 1];

            for (fop = GC_obj_kinds[k].ok_freelist;
                 ADDR_LT((ptr_t)fop, (ptr_t)lim); fop++) {
              if (*fop != NULL) {
                if (should_clobber) {
                  GC_clear_fl_links(fop);
                } else {
                  *fop = NULL;
                }
              }
            }
        } /* otherwise free-list objects are marked,    */
          /* and it's safe to leave them.               */
        BZERO(rlist, (MAXOBJGRANULES + 1) * sizeof(void *));
    }


    /* Go through all heap blocks (in hblklist) and reclaim unmarked    */
    /* objects or enqueue the block for later processing.               */
    GC_apply_to_all_blocks(GC_reclaim_block, (void *)(word)report_if_found);

# ifdef EAGER_SWEEP
    /* This is a very stupid thing to do.  We make it possible anyway,  */
    /* so that you can convince yourself that it really is very stupid. */
    GC_reclaim_all((GC_stop_func)0, FALSE);
# elif defined(ENABLE_DISCLAIM)
    /* However, make sure to clear reclaimable objects of kinds with    */
    /* unconditional marking enabled before we do any significant       */
    /* marking work.                                                    */
    GC_reclaim_unconditionally_marked();
# endif
# if defined(PARALLEL_MARK)
    GC_ASSERT(0 == GC_fl_builder_count);
# endif
}

GC_INNER void GC_continue_reclaim(size_t lg, int k)
{
    struct hblk *hbp;
    struct obj_kind *ok = &GC_obj_kinds[k];
    struct hblk **rlh = ok -> ok_reclaim_list;
    void **flh;

    GC_ASSERT(I_HOLD_LOCK());
    if (NULL == rlh)
        return; /* No blocks of this kind.      */

    flh = &(ok -> ok_freelist[lg]);
    for (rlh += lg; (hbp = *rlh) != NULL; ) {
        const hdr *hhdr = HDR(hbp);

        *rlh = hhdr -> hb_next;
        GC_reclaim_small_nonempty_block(hbp, hhdr -> hb_sz, FALSE);
        if (*flh != NULL)
            break; /* the appropriate free list is nonempty */
    }
}

/*
 * Reclaim all small blocks waiting to be reclaimed.
 * Abort and return FALSE when/if (*stop_func)() returns TRUE.
 * If this returns TRUE, then it's safe to restart the world
 * with incorrectly cleared mark bits.
 * If ignore_old is TRUE, then reclaim only blocks that have been
 * recently reclaimed, and discard the rest.
 * Stop_func may be 0.
 */
GC_INNER GC_bool GC_reclaim_all(GC_stop_func stop_func, GC_bool ignore_old)
{
    size_t lg;
    int k;
    const hdr * hhdr;
    struct hblk * hbp;
    struct hblk ** rlp;
    struct hblk ** rlh;
#   ifndef NO_CLOCK
      CLOCK_TYPE start_time = CLOCK_TYPE_INITIALIZER;

      if (GC_print_stats == VERBOSE)
        GET_TIME(start_time);
#   endif
    GC_ASSERT(I_HOLD_LOCK());

    for (k = 0; k < (int)GC_n_kinds; k++) {
        rlp = GC_obj_kinds[k].ok_reclaim_list;
        if (rlp == 0) continue;
        for (lg = 1; lg <= MAXOBJGRANULES; lg++) {
            for (rlh = rlp + lg; (hbp = *rlh) != NULL; ) {
                if (stop_func != (GC_stop_func)0 && (*stop_func)()) {
                    return FALSE;
                }
                hhdr = HDR(hbp);
                *rlh = hhdr -> hb_next;
                if (!ignore_old
                    || (word)(hhdr -> hb_last_reclaimed) == GC_gc_no - 1) {
                    /* It's likely we'll need it this time, too */
                    /* It's been touched recently, so this      */
                    /* shouldn't trigger paging.                */
                    GC_reclaim_small_nonempty_block(hbp, hhdr -> hb_sz, FALSE);
                }
            }
        }
    }
#   ifndef NO_CLOCK
      if (GC_print_stats == VERBOSE) {
        CLOCK_TYPE done_time;

        GET_TIME(done_time);
        GC_verbose_log_printf(
                        "Disposing of reclaim lists took %lu ms %lu ns\n",
                        MS_TIME_DIFF(done_time, start_time),
                        NS_FRAC_TIME_DIFF(done_time, start_time));
      }
#   endif
    return TRUE;
}

#if !defined(EAGER_SWEEP) && defined(ENABLE_DISCLAIM)
/* We do an eager sweep on heap blocks where unconditional marking has  */
/* been enabled, so that any reclaimable objects have been reclaimed    */
/* before we start marking.  This is a simplified GC_reclaim_all        */
/* restricted to kinds where ok_mark_unconditionally is true.           */
  STATIC void GC_reclaim_unconditionally_marked(void)
  {
    int k;

    GC_ASSERT(I_HOLD_LOCK());
    for (k = 0; k < (int)GC_n_kinds; k++) {
        size_t lg;
        struct obj_kind *ok = &GC_obj_kinds[k];
        struct hblk **rlp = ok -> ok_reclaim_list;

        if (NULL == rlp || !(ok -> ok_mark_unconditionally)) continue;

        for (lg = 1; lg <= MAXOBJGRANULES; lg++) {
            struct hblk **rlh = rlp + lg;
            struct hblk *hbp;

            while ((hbp = *rlh) != NULL) {
                const hdr *hhdr = HDR(hbp);

                *rlh = hhdr -> hb_next;
                GC_reclaim_small_nonempty_block(hbp, hhdr -> hb_sz, FALSE);
            }
        }
    }
  }
#endif /* !EAGER_SWEEP && ENABLE_DISCLAIM */

struct enumerate_reachable_s {
  GC_reachable_object_proc proc;
  void *client_data;
};

STATIC void GC_CALLBACK GC_do_enumerate_reachable_objects(struct hblk *hbp,
                                                          void *ed_ptr)
{
  const hdr *hhdr = HDR(hbp);
  ptr_t p, plim;
  const struct enumerate_reachable_s *ped
                        = (struct enumerate_reachable_s *)ed_ptr;
  size_t sz = hhdr -> hb_sz;
  size_t bit_no;

  if (GC_block_empty(hhdr)) return;

  p = hbp -> hb_body;
  if (sz > MAXOBJBYTES) { /* one big object */
    plim = p;
  } else {
    plim = p + HBLKSIZE - sz;
  }
  /* Go through all objects in the block. */
  for (bit_no = 0; ADDR_GE(plim, p); bit_no += MARK_BIT_OFFSET(sz), p += sz) {
    if (mark_bit_from_hdr(hhdr, bit_no)) {
      ped -> proc(p, sz, ped -> client_data);
    }
  }
}

GC_API void GC_CALL GC_enumerate_reachable_objects_inner(
                                                GC_reachable_object_proc proc,
                                                void *client_data)
{
  struct enumerate_reachable_s ed;

  GC_ASSERT(I_HOLD_READER_LOCK());
  ed.proc = proc;
  ed.client_data = client_data;
  GC_apply_to_all_blocks(GC_do_enumerate_reachable_objects, &ed);
}
