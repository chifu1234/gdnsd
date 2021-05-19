/* Copyright © 2012 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd.
 *
 * gdnsd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>
#include "ltarena.h"

#include <gdnsd/alloc.h>
#include <gdnsd/compiler.h>
#include <gdnsd/dname.h>

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

//   ltarena: used for label or dname strings only, pooled to reduce the
// per-alloc overhead of malloc aligning and tracking every single one
// needlessly.  Labels max out at 64 bytes of storage, and dnames at 256, but
// commonly in real-world use they're much smaller, very often more
// like single-digit bytes for labels and maybe 20 bytes for domainnames.
//   Pools start at POOL_SIZE, although after the initial INIT_POOLS_ALLOC
// pools have been filled we'll start using POOL_SIZE*4 as the size of
// further pools; this is mostly an optimization to avoid over-allocating on
// initial pool size for per-zone arenas in the case of thousands of tiny
// domains which each only contain a handful of labels.
//   We initially reserve room in the ltarena object to track INIT_POOLS_ALLOC
// pool pointers, which expands by doubling to support far more pools than
// needed by even the largest zones in existence.
#define MAX_OBJ 256U // Maximum that can be requested from lta_malloc
#define POOL_SIZE 1024U // *must* be >= MAX_OBJ
#define INIT_POOLS_ALLOC 4U // *must* be 2^n && > 0

static_assert(INIT_POOLS_ALLOC > 0, "Init pool alloc non-zero");
static_assert((INIT_POOLS_ALLOC & (INIT_POOLS_ALLOC - 1)) == 0, "Init pool alloc is power of two");
static_assert(POOL_SIZE >= MAX_OBJ, "Pool size fits largest possible alloc");

struct ltarena {
    uint8_t** pools; // array of per-pool pointers
    size_t pool;     // index of current pool for new writes
    size_t poffs;    // offset in current pool for new writes
    size_t palloc;   // allocation size of "pools"
};

struct ltarena* lta_new(void)
{
    struct ltarena* rv = xcalloc(sizeof(*rv));
    rv->palloc = INIT_POOLS_ALLOC;
    rv->pools = xmalloc_n(INIT_POOLS_ALLOC, sizeof(*rv->pools));
    rv->pools[0] = xcalloc(POOL_SIZE);
    return rv;
}

void lta_close(struct ltarena* lta)
{
    lta->pools = xrealloc_n(lta->pools, lta->pool + 1, sizeof(*lta->pools));
}

void lta_destroy(struct ltarena* lta)
{
    size_t whichp = lta->pool + 1U;
    while (whichp--)
        free(lta->pools[whichp]);
    free(lta->pools);
    free(lta);
}

void lta_merge(struct ltarena* target, struct ltarena* source)
{
    uint8_t* target_last_pool = target->pools[target->pool];
    const size_t source_pool_count = source->pool + 1U;
    const size_t new_pool_count = source_pool_count + target->pool + 1U;
    if (new_pool_count >= target->palloc) {
        do {
            target->palloc <<= 1U;
        } while (new_pool_count >= target->palloc);
        target->pools = xrealloc_n(target->pools, target->palloc, sizeof(*target->pools));
    }
    memcpy(&target->pools[target->pool], source->pools, source_pool_count * sizeof(*source->pools));
    target->pool += source->pool;
    target->pool++;
    target->pools[target->pool] = target_last_pool;
    free(source->pools);
    free(source);
}

uint8_t* lta_malloc(struct ltarena* lta, const size_t size)
{
    // Currently, all allocations obey this assertion.
    // Only labels + dnames are stored here
    gdnsd_assume(size);
    gdnsd_assume(size <= MAX_OBJ);

    // handle pool switch if we're out of room
    //   + take care to extend the pools array if necc.
    if (unlikely((lta->poffs + size > POOL_SIZE))) {
        if (unlikely(++lta->pool == lta->palloc)) {
            lta->palloc <<= 1U;
            lta->pools = xrealloc_n(lta->pools, lta->palloc, sizeof(*lta->pools));
        }
        lta->pools[lta->pool] = xcalloc(POOL_SIZE);
        lta->poffs = 0;
    }

    // assign the space and move our poffs pointer
    uint8_t* rval = &lta->pools[lta->pool][lta->poffs];
    lta->poffs += size;

    return rval;
}
