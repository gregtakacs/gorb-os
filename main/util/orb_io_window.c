/* The heavy-I/O window — contract in orb_io_window.h. */
#include "orb_io_window.h"

static volatile int32_t  s_depth;
static volatile uint32_t s_gen;

void orb_io_heavy_begin(void)
{
    __atomic_add_fetch(&s_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&s_depth, 1, __ATOMIC_SEQ_CST);
}

void orb_io_heavy_end(void)
{
    if (__atomic_sub_fetch(&s_depth, 1, __ATOMIC_SEQ_CST) < 0)
        __atomic_store_n(&s_depth, 0, __ATOMIC_SEQ_CST);   /* an unpaired end */
}

bool     orb_io_heavy_now(void) { return __atomic_load_n(&s_depth, __ATOMIC_SEQ_CST) > 0; }
uint32_t orb_io_heavy_gen(void) { return __atomic_load_n(&s_gen, __ATOMIC_SEQ_CST); }
