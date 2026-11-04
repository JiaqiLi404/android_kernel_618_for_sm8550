#!/usr/bin/env python3
"""Apply bounded cache adaptations to the pinned ACK tree; see PROVENANCE."""
from pathlib import Path
import sys


def patch(root, relative, marker, edits):
    path = root / relative
    text = path.read_text()
    if marker in text:
        if not all(new in text for old, new in edits):
            raise SystemExit(f'{relative}: incomplete {marker} adaptation')
        return 'already applied'
    for old, new in edits:
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: expected one anchor: {old[:90]!r}')
        text = text.replace(old, new, 1)
    path.write_text(text)
    return 'applied'


ZRAM_EDITS = [
    ('struct zram_pp_slot {\n\tunsigned long\t\tindex;\n\tstruct list_head\tentry;\n};\n',
     'struct zram_pp_slot {\n\tunsigned long\t\tindex;\n\tstruct list_head\tentry;\n};\n\n'
     '/* k618_pp_slab: small post-processing records, with kmalloc fallback. */\n'
     'static struct kmem_cache *zram_pp_cache;\n'),
    ('\tkfree(pps);\n',
     '\tif (zram_pp_cache)\n\t\tkmem_cache_free(zram_pp_cache, pps);\n'
     '\telse\n\t\tkfree(pps);\n'),
    ('\tpps = kmalloc(sizeof(*pps), GFP_NOIO | __GFP_NOWARN);\n',
     '\tpps = zram_pp_cache ?\n'
     '\t\tkmem_cache_alloc(zram_pp_cache, GFP_NOIO | __GFP_NOWARN) :\n'
     '\t\tkmalloc(sizeof(*pps), GFP_NOIO | __GFP_NOWARN);\n'),
    ('\twhile (num_devices != 0) {\n',
     '#if defined(CONFIG_ZRAM_WRITEBACK) || defined(CONFIG_ZRAM_MULTI_COMP)\n'
     '\tzram_pp_cache = KMEM_CACHE(zram_pp_slot, 0);\n'
     '#endif\n\twhile (num_devices != 0) {\n'),
    ('\tunregister_blkdev(zram_major, "zram");\n\tcpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);\n',
     '\tunregister_blkdev(zram_major, "zram");\n\tcpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);\n'
     '#if defined(CONFIG_ZRAM_WRITEBACK) || defined(CONFIG_ZRAM_MULTI_COMP)\n'
     '\tkmem_cache_destroy(zram_pp_cache);\n\tzram_pp_cache = NULL;\n#endif\n'),
]

# Referenced by the host-side boundary tests as well as by the source patch.
BUFIO_POLICY = '''/*
 * k618_bufio_budget: retain metadata during light reclaim only within a budget.
 * OnePlus's low-memory policy is adapted to the current shrink-count interface.
 * Low memory, a large cache, or an explicit disable restores ordinary reclaim.
 */
static bool dm_bufio_retain_when_free = true;
module_param_named(retain_when_free, dm_bufio_retain_when_free, bool, 0644);
MODULE_PARM_DESC(retain_when_free, "Retain metadata within a free-memory budget");

static bool dm_bufio_within_retention_budget(void)
{
	long available;
	unsigned long allocated, budget, total;

	if (!READ_ONCE(dm_bufio_retain_when_free))
		return false;
	available = si_mem_available();
	total = totalram_pages();
	if (available <= 0 || (unsigned long)available <= total / 10)
		return false;
	allocated = READ_ONCE(dm_bufio_current_allocated) >> PAGE_SHIFT;
	budget = min(total / 50, (unsigned long)available / 4);
	return allocated <= budget;
}

'''

BUFIO_EDITS = [
    ('#include <linux/shrinker.h>\n',
     '#include <linux/shrinker.h>\n#include <linux/mm.h>\n'),
    ('static unsigned long dm_bufio_shrink_count(struct shrinker *shrink, struct shrink_control *sc)\n',
     BUFIO_POLICY + 'static unsigned long dm_bufio_shrink_count(struct shrinker *shrink, struct shrink_control *sc)\n'),
    ('\tunsigned long queued_for_cleanup = atomic_long_read(&c->need_shrink);\n\n',
     '\tunsigned long queued_for_cleanup = atomic_long_read(&c->need_shrink);\n\n'
     '\tif (dm_bufio_within_retention_budget())\n\t\treturn 0;\n\n'),
]


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: k618_memory.py <kernel-source>')
    root = Path(sys.argv[1])
    print('k618_memory: zram_pp_slab: ' + patch(root, 'drivers/block/zram/zram_drv.c',
                                              'k618_pp_slab', ZRAM_EDITS))
    print('k618_memory: dm_bufio_budget: ' + patch(root, 'drivers/md/dm-bufio.c',
                                                 'k618_bufio_budget', BUFIO_EDITS))


if __name__ == '__main__':
    main()
