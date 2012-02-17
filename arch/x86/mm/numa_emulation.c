/*
 * NUMA emulation
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/topology.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>
#include <asm/dma.h>

#include "numa_internal.h"

static int emu_nid_to_phys[MAX_NUMNODES] __cpuinitdata;
static char *emu_cmdline __initdata;

void __init numa_emu_cmdline(char *str)
{
	emu_cmdline = str;
}

static int __init emu_find_memblk_by_nid(int nid, const struct numa_meminfo *mi)
{
	int i;

	for (i = 0; i < mi->nr_blks; i++)
		if (mi->blk[i].nid == nid)
			return i;
	return -ENOENT;
}

/*
 * Sets up nid to range from @start to @end.  The return value is -errno if
 * something went wrong, 0 otherwise.
 */
static int __init emu_setup_memblk(struct numa_meminfo *ei,
				   struct numa_meminfo *pi,
				   int nid, int phys_blk, u64 size)
{
	struct numa_memblk *eb = &ei->blk[ei->nr_blks];
	struct numa_memblk *pb = &pi->blk[phys_blk];

	if (ei->nr_blks >= NR_NODE_MEMBLKS) {
		pr_err("NUMA: Too many emulated memblks, failing emulation\n");
		return -EINVAL;
	}

	ei->nr_blks++;
	eb->start = pb->start;
	eb->end = pb->start + size;
	eb->nid = nid;

	if (emu_nid_to_phys[nid] == NUMA_NO_NODE)
		emu_nid_to_phys[nid] = pb->nid;

	pb->start += size;
	if (pb->start >= pb->end) {
		WARN_ON_ONCE(pb->start > pb->end);
		numa_remove_memblk_from(phys_blk, pi);
	}

	printk(KERN_INFO "Faking node %d at %016Lx-%016Lx (%LuMB)\n", nid,
	       eb->start, eb->end, (eb->end - eb->start) >> 20);
	return 0;
}

/*
 * Sets up nr_nodes fake nodes interleaved over physical nodes ranging from addr
 * to max_addr.  The return value is the number of nodes allocated.
 */
static int __init split_nodes_interleave(struct numa_meminfo *ei,
					 struct numa_meminfo *pi,
					 u64 addr, u64 max_addr, int nr_nodes)
{
	nodemask_t physnode_mask = NODE_MASK_NONE;
	u64 size;
	int big;
	int nid = 0;
	int i, ret;

	if (nr_nodes <= 0)
		return -1;
	if (nr_nodes > MAX_NUMNODES) {
		pr_info("numa=fake=%d too large, reducing to %d\n",
			nr_nodes, MAX_NUMNODES);
		nr_nodes = MAX_NUMNODES;
	}

	/*
	 * Calculate target node size.  x86_32 freaks on __udivdi3() so do
	 * the division in ulong number of pages and convert back.
	 */
	size = max_addr - addr - memblock_x86_hole_size(addr, max_addr);
	size = PFN_PHYS((unsigned long)(size >> PAGE_SHIFT) / nr_nodes);

	/*
	 * Calculate the number of big nodes that can be allocated as a result
	 * of consolidating the remainder.
	 */
	big = ((size & ~FAKE_NODE_MIN_HASH_MASK) * nr_nodes) /
		FAKE_NODE_MIN_SIZE;

	size &= FAKE_NODE_MIN_HASH_MASK;
	if (!size) {
		pr_err("Not enough memory for each node.  "
			"NUMA emulation disabled.\n");
		return -1;
	}

	for (i = 0; i < pi->nr_blks; i++)
		node_set(pi->blk[i].nid, physnode_mask);

	/*
	 * Continue to fill physical nodes with fake nodes until there is no
	 * memory left on any of them.
	 */
	while (nodes_weight(physnode_mask)) {
		for_each_node_mask(i, physnode_mask) {
			u64 dma32_end = PFN_PHYS(MAX_DMA32_PFN);
			u64 start, limit, end;
			int phys_blk;

			phys_blk = emu_find_memblk_by_nid(i, pi);
			if (phys_blk < 0) {
				node_clear(i, physnode_mask);
				continue;
			}
			start = pi->blk[phys_blk].start;
			limit = pi->blk[phys_blk].end;
			end = start + size;

			if (nid < big)
				end += FAKE_NODE_MIN_SIZE;

			/*
			 * Continue to add memory to this fake node if its
			 * non-reserved memory is less than the per-node size.
			 */
			while (end - start -
			       memblock_x86_hole_size(start, end) < size) {
				end += FAKE_NODE_MIN_SIZE;
				if (end > limit) {
					end = limit;
					break;
				}
			}

			/*
			 * If there won't be at least FAKE_NODE_MIN_SIZE of
			 * non-reserved memory in ZONE_DMA32 for the next node,
			 * this one must extend to the boundary.
			 */
			if (end < dma32_end && dma32_end - end -
			    memblock_x86_hole_size(end, dma32_end) < FAKE_NODE_MIN_SIZE)
				end = dma32_end;

			/*
			 * If there won't be enough non-reserved memory for the
			 * next node, this one must extend to the end of the
			 * physical node.
			 */
			if (limit - end -
			    memblock_x86_hole_size(end, limit) < size)
				end = limit;

			ret = emu_setup_memblk(ei, pi, nid++ % nr_nodes,
					       phys_blk,
					       min(end, limit) - start);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

/*
 * Returns the end address of a node so that there is at least `size' amount of
 * non-reserved memory or `max_addr' is reached.
 */
static u64 __init find_end_of_node(u64 start, u64 max_addr, u64 size)
{
	u64 end = start + size;

	while (end - start - memblock_x86_hole_size(start, end) < size) {
		end += FAKE_NODE_MIN_SIZE;
		if (end > max_addr) {
			end = max_addr;
			break;
		}
	}
	return end;
}

/*
 * Sets up fake nodes of `size' interleaved over physical nodes ranging from
 * `addr' to `max_addr'.  The return value is the number of nodes allocated.
 */
static int __init split_nodes_size_interleave(struct numa_meminfo *ei,
					      struct numa_meminfo *pi,
					      u64 addr, u64 max_addr, u64 size)
{
	nodemask_t physnode_mask = NODE_MASK_NONE;
	u64 min_size;
	int nid = 0;
	int i, ret;

	if (!size)
		return -1;
	/*
	 * The limit on emulated nodes is MAX_NUMNODES, so the size per node is
	 * increased accordingly if the requested size is too small.  This
	 * creates a uniform distribution of node sizes across the entire
	 * machine (but not necessarily over physical nodes).
	 */
	min_size = (max_addr - addr - memblock_x86_hole_size(addr, max_addr)) /
						MAX_NUMNODES;
	min_size = max(min_size, FAKE_NODE_MIN_SIZE);
	if ((min_size & FAKE_NODE_MIN_HASH_MASK) < min_size)
		min_size = (min_size + FAKE_NODE_MIN_SIZE) &
						FAKE_NODE_MIN_HASH_MASK;
	if (size < min_size) {
		pr_err("Fake node size %LuMB too small, increasing to %LuMB\n",
			size >> 20, min_size >> 20);
		size = min_size;
	}
	size &= FAKE_NODE_MIN_HASH_MASK;

	for (i = 0; i < pi->nr_blks; i++)
		node_set(pi->blk[i].nid, physnode_mask);

	/*
	 * Fill physical nodes with fake nodes of size until there is no memory
	 * left on any of them.
	 */
	while (nodes_weight(physnode_mask)) {
		for_each_node_mask(i, physnode_mask) {
			u64 dma32_end = PFN_PHYS(MAX_DMA32_PFN);
			u64 start, limit, end;
			int phys_blk;

			phys_blk = emu_find_memblk_by_nid(i, pi);
			if (phys_blk < 0) {
				node_clear(i, physnode_mask);
				continue;
			}
			start = pi->blk[phys_blk].start;
			limit = pi->blk[phys_blk].end;

			end = find_end_of_node(start, limit, size);
			/*
			 * If there won't be at least FAKE_NODE_MIN_SIZE of
			 * non-reserved memory in ZONE_DMA32 for the next node,
			 * this one must extend to the boundary.
			 */
			if (end < dma32_end && dma32_end - end -
			    memblock_x86_hole_size(end, dma32_end) < FAKE_NODE_MIN_SIZE)
				end = dma32_end;

			/*
			 * If there won't be enough non-reserved memory for the
			 * next node, this one must extend to the end of the
			 * physical node.
			 */
			if (limit - end -
			    memblock_x86_hole_size(end, limit) < size)
				end = limit;

			ret = emu_setup_memblk(ei, pi, nid++ % MAX_NUMNODES,
					       phys_blk,
					       min(end, limit) - start);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

/**
 * numa_emulation - Emulate NUMA nodes
 * @numa_meminfo: NUMA configuration to massage
 * @numa_dist_cnt: The size of the physical NUMA distance table
 *
 * Emulate NUMA nodes according to the numa=fake kernel parameter.
 * @numa_meminfo contains the physical memory configuration and is modified
 * to reflect the emulated configuration on success.  @numa_dist_cnt is
 * used to determine the size of the physical distance table.
 *
 * On success, the following modifications are made.
 *
 * - @numa_meminfo is updated to reflect the emulated nodes.
 *
 * - __apicid_to_node[] is updated such that APIC IDs are mapped to the
 *   emulated nodes.
 *
 * - NUMA distance table is rebuilt to represent distances between emulated
 *   nodes.  The distances are determined considering how emulated nodes
 *   are mapped to physical nodes and match the actual distances.
 *
 * - emu_nid_to_phys[] reflects how emulated nodes are mapped to physical
 *   nodes.  This is used by numa_add_cpu() and numa_remove_cpu().
 *
 * If emulation is not enabled or fails, emu_nid_to_phys[] is filled with
 * identity mapping and no other modification is made.
 */
void __init numa_emulation(struct numa_meminfo *numa_meminfo, int numa_dist_cnt)
{
	static struct numa_meminfo ei __initdata;
	static struct numa_meminfo pi __initdata;
	const u64 max_addr = PFN_PHYS(max_pfn);
	u8 *phys_dist = NULL;
	size_t phys_size = numa_dist_cnt * numa_dist_cnt * sizeof(phys_dist[0]);
	int max_emu_nid, dfl_phys_nid;
	int i, j, ret;

	if (!emu_cmdline)
		goto no_emu;

	memset(&ei, 0, sizeof(ei));
	pi = *numa_meminfo;

	for (i = 0; i < MAX_NUMNODES; i++)
		emu_nid_to_phys[i] = NUMA_NO_NODE;

	/*
	 * If the numa=fake command-line contains a 'M' or 'G', it represents
	 * the fixed node size.  Otherwise, if it is just a single number N,
	 * split the system RAM into N fake nodes.
	 */
	if (strchr(emu_cmdline, 'M') || strchr(emu_cmdline, 'G')) {
		u64 size;

		size = memparse(emu_cmdline, &emu_cmdline);
		ret = split_nodes_size_interleave(&ei, &pi, 0, max_addr, size);
	} else {
		unsigned long n;

		n = simple_strtoul(emu_cmdline, NULL, 0);
		ret = split_nodes_interleave(&ei, &pi, 0, max_addr, n);
	}

	if (ret < 0)
		goto no_emu;

	if (numa_cleanup_meminfo(&ei) < 0) {
		pr_warning("NUMA: Warning: constructed meminfo invalid, disabling emulation\n");
		goto no_emu;
	}

	/* copy the physical distance table */
	if (numa_dist_cnt) {
		u64 phys;

		phys = memblock_find_in_range(0, PFN_PHYS(max_pfn_mapped),
					      phys_size, PAGE_SIZE);
		if (!phys) {
			pr_warning("NUMA: Warning: can't allocate copy of distance table, disabling emulation\n");
			goto no_emu;
		}
		memblock_reserve(phys, phys_size);
		phys_dist = __va(phys);

		for (i = 0; i < numa_dist_cnt; i++)
			for (j = 0; j < numa_dist_cnt; j++)
				phys_dist[i * numa_dist_cnt + j] =
					node_distance(i, j);
	}

	/*
	 * Determine the max emulated nid and the default phys nid to use
	 * for unmapped nodes.
	 */
	max_emu_nid = 0;
	dfl_phys_nid = NUMA_NO_NODE;
	for (i = 0; i < ARRAY_SIZE(emu_nid_to_phys); i++) {
		if (emu_nid_to_phys[i] != NUMA_NO_NODE) {
			max_emu_nid = i;
			if (dfl_phys_nid == NUMA_NO_NODE)
				dfl_phys_nid = emu_nid_to_phys[i];
		}
	}
	if (dfl_phys_nid == NUMA_NO_NODE) {
		pr_warning("NUMA: Warning: can't determine default physical node, disabling emulation\n");
		goto no_emu;
	}

	/* commit */
	*numa_meminfo = ei;

	/*
	 * Transform __apicid_to_node table to use emulated nids by
	 * reverse-mapping phys_nid.  The maps should always exist but fall
	 * back to zero just in case.
	 */
	for (i = 0; i < ARRAY_SIZE(__apicid_to_node); i++) {
		if (__apicid_to_node[i] == NUMA_NO_NODE)
			continue;
		for (j = 0; j < ARRAY_SIZE(emu_nid_to_phys); j++)
			if (__apicid_to_node[i] == emu_nid_to_phys[j])
				break;
		__apicid_to_node[i] = j < ARRAY_SIZE(emu_nid_to_phys) ? j : 0;
	}

	/* make sure all emulated nodes are mapped to a physical node */
	for (i = 0; i < ARRAY_SIZE(emu_nid_to_phys); i++)
		if (emu_nid_to_phys[i] == NUMA_NO_NODE)
			emu_nid_to_phys[i] = dfl_phys_nid;

	/* transform distance table */
	numa_reset_distance();
	for (i = 0; i < max_emu_nid + 1; i++) {
		for (j = 0; j < max_emu_nid + 1; j++) {
			int physi = emu_nid_to_phys[i];
			int physj = emu_nid_to_phys[j];
			int dist;

			if (physi >= numa_dist_cnt || physj >= numa_dist_cnt)
				dist = physi == physj ?
					LOCAL_DISTANCE : REMOTE_DISTANCE;
			else
				dist = phys_dist[physi * numa_dist_cnt + physj];

			numa_set_distance(i, j, dist);
		}
	}

	/* free the copied physical distance table */
	if (phys_dist)
		memblock_free(__pa(phys_dist), phys_size);
	return;

no_emu:
	/* No emulation.  Build identity emu_nid_to_phys[] for numa_add_cpu() */
	for (i = 0; i < ARRAY_SIZE(emu_nid_to_phys); i++)
		emu_nid_to_phys[i] = i;
}

#ifndef CONFIG_DEBUG_PER_CPU_MAPS
void __cpuinit numa_add_cpu(int cpu)
{
	int physnid, nid;

	nid = early_cpu_to_node(cpu);
	BUG_ON(nid == NUMA_NO_NODE || !node_online(nid));

	physnid = emu_nid_to_phys[nid];

	/*
	 * Map the cpu to each emulated node that is allocated on the physical
	 * node of the cpu's apic id.
	 */
	for_each_online_node(nid)
		if (emu_nid_to_phys[nid] == physnid)
			cpumask_set_cpu(cpu, node_to_cpumask_map[nid]);
}

void __cpuinit numa_remove_cpu(int cpu)
{
	int i;

	for_each_online_node(i)
		cpumask_clear_cpu(cpu, node_to_cpumask_map[i]);
}
#else	/* !CONFIG_DEBUG_PER_CPU_MAPS */
static void __cpuinit numa_set_cpumask(int cpu, bool enable)
{
	int nid, physnid;

	nid = early_cpu_to_node(cpu);
	if (nid == NUMA_NO_NODE) {
		/* early_cpu_to_node() already emits a warning and trace */
		return;
	}

	physnid = emu_nid_to_phys[nid];

	for_each_online_node(nid) {
		if (emu_nid_to_phys[nid] != physnid)
			continue;

		debug_cpumask_set_cpu(cpu, nid, enable);
	}
}

void __cpuinit numa_add_cpu(int cpu)
{
	numa_set_cpumask(cpu, true);
}

void __cpuinit numa_remove_cpu(int cpu)
{
	numa_set_cpumask(cpu, false);
}
#endif	/* !CONFIG_DEBUG_PER_CPU_MAPS */
