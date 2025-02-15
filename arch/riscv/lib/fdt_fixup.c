// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2020 Western Digital Corporation or its affiliates
 *
 */

#define LOG_CATEGORY LOGC_ARCH

#include <common.h>
#include <fdt_support.h>
#include <log.h>
#include <mapmem.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

/**
 * riscv_fdt_copy_resv_mem_node() - Copy reserve memory node entry
 * @src: Pointer to the source device tree from which reserved memory node
 *	 needs to be copied.
 * @dst: Pointer to the destination device tree to which reserved memory node
 *	 needs to be copied.
 *
 * Return: 0 on success or if source doesn't have reserved memory node.
 *	   Error if copy process failed.
 */
int riscv_fdt_copy_resv_mem_node(const void *src, void *dst)
{
	u32 phandle;
	struct fdt_memory pmp_mem;
	fdt_addr_t addr;
	fdt_size_t size;
	int offset, node, err, rmem_offset;
	char basename[32] = {0};
	int bname_len;
	int max_len = sizeof(basename);
	const char *name;
	char *temp;

	offset = fdt_path_offset(src, "/reserved-memory");
	if (offset < 0) {
		log_debug("No reserved memory region found in source FDT\n");
		return 0;
	}

	/*
	 * Extend the FDT by the following estimated size:
	 *
	 * Each PMP memory region entry occupies 64 bytes.
	 * With 16 PMP memory regions we need 64 * 16 = 1024 bytes.
	 */
	err = fdt_open_into(dst, dst, fdt_totalsize(dst) + 1024);
	if (err < 0) {
		pr_info("Device Tree can't be expanded to accommodate new node");
		return err;
	}

	fdt_for_each_subnode(node, src, offset) {
		name = fdt_get_name(src, node, NULL);

		addr = fdtdec_get_addr_size_auto_parent(src, offset, node,
							"reg", 0, &size,
							false);
		if (addr == FDT_ADDR_T_NONE) {
			log_debug("failed to read address/size for %s\n", name);
			continue;
		}
		strncpy(basename, name, max_len);
		temp = strchr(basename, '@');
		if (temp) {
			bname_len = strnlen(basename, max_len) - strnlen(temp,
								       max_len);
			*(basename + bname_len) = '\0';
		}
		pmp_mem.start = addr;
		pmp_mem.end = addr + size - 1;
		err = fdtdec_add_reserved_memory(dst, basename, &pmp_mem,
						 NULL, 0, &phandle, 0);
		if (err < 0 && err != -FDT_ERR_EXISTS) {
			log_err("failed to add reserved memory: %d\n", err);
			return err;
		}
		if (fdt_getprop(src, node, "no-map", NULL)) {
			rmem_offset = fdt_node_offset_by_phandle(dst, phandle);
			fdt_setprop_empty(dst, rmem_offset, "no-map");
		}
	}

	return 0;
}

/**
 * riscv_board_reserved_mem_fixup() - Fix up reserved memory node for a board
 * @fdt: Pointer to the device tree in which reserved memory node needs to be
 *	 added.
 *
 * In RISC-V, any board needs to copy the reserved memory node from the device
 * tree provided by the firmware to the device tree used by U-Boot. This is a
 * common function that individual board fixup functions can invoke.
 *
 * Return: 0 on success or error otherwise.
 */
int riscv_board_reserved_mem_fixup(void *fdt)
{
	int err;
	void *src_fdt_addr;

	src_fdt_addr = map_sysmem(gd->arch.firmware_fdt_addr, 0);

	/* avoid the copy if we are using the same device tree */
	if (src_fdt_addr == fdt)
		return 0;

	err = riscv_fdt_copy_resv_mem_node(src_fdt_addr, fdt);
	if (err < 0)
		return err;

	return 0;
}

#ifdef CONFIG_OF_BOARD_FIXUP
int board_fix_fdt(void *fdt)
{
	int err;

	err = riscv_board_reserved_mem_fixup(fdt);
	if (err < 0) {
		log_err("failed to fixup DT for reserved memory: %d\n", err);
		return err;
	}

	return 0;
}
#endif

#ifdef CONFIG_FDT_ADD_MEMORY_NODE
#if CONFIG_NR_DRAM_BANKS > 4
#define _MEMORY_BANKS_MAX CONFIG_NR_DRAM_BANKS
#else
#define _MEMORY_BANKS_MAX 4
#endif
/*
 * fdt_pack_reg - pack address and size array into the "reg"-suitable stream
 */
static int fdt_pack_reg(const void *fdt, void *buf, u64 *address, u64 *size,
			int n)
{
	int i;
	int address_cells = fdt_address_cells(fdt, 0);
	int size_cells = fdt_size_cells(fdt, 0);
	char *p = buf;

	for (i = 0; i < n; i++) {
		if (address_cells == 2)
			*(fdt64_t *)p = cpu_to_fdt64(address[i]);
		else
			*(fdt32_t *)p = cpu_to_fdt32(address[i]);
		p += 4 * address_cells;

		if (size_cells == 2)
			*(fdt64_t *)p = cpu_to_fdt64(size[i]);
		else
			*(fdt32_t *)p = cpu_to_fdt32(size[i]);
		p += 4 * size_cells;
	}

	return p - (char *)buf;
}
#endif

int arch_fixup_fdt(void *blob)
{
	int err;
#ifdef CONFIG_EFI_LOADER
	u32 size;
	int chosen_offset;

	size = fdt_totalsize(blob);
	err  = fdt_open_into(blob, blob, size + 32);
	if (err < 0) {
		log_err("Device Tree can't be expanded to accommodate new node");
		return err;
	}
	chosen_offset = fdt_path_offset(blob, "/chosen");
	if (chosen_offset < 0) {
		chosen_offset = fdt_add_subnode(blob, 0, "chosen");
		if (chosen_offset < 0) {
			log_err("chosen node cannot be added\n");
			return chosen_offset;
		}
	}
	/* Overwrite the boot-hartid as U-Boot is the last stage BL */
	err = fdt_setprop_u32(blob, chosen_offset, "boot-hartid",
			      gd->arch.boot_hart);
	if (err < 0)
		return log_msg_ret("could not set boot-hartid", err);
#endif


#ifdef CONFIG_FDT_ADD_MEMORY_NODE
	u8 tmp[_MEMORY_BANKS_MAX * 16];
	u64 ram_base[1];
	u64 ram_size[1];
	char memstart[32];
	int nodeoffset, len;

	/* delete memory node before add new memory node. */
	do {
		nodeoffset = fdt_subnode_offset(blob, 0, "memory");
		if (nodeoffset >= 0) {
			fdt_del_node(blob, nodeoffset);
		}
	} while(nodeoffset >= 0);

	for (int bank_index = CONFIG_NR_DRAM_BANKS - 1; bank_index >= 0; bank_index--){
		if (0 == gd->bd->bi_dram[bank_index].size)
			continue;

		memset(memstart, 0, 32);
		sprintf(memstart, "memory@%llx", gd->bd->bi_dram[bank_index].start);

		ram_base[0] = gd->bd->bi_dram[bank_index].start;
		ram_size[0] = gd->bd->bi_dram[bank_index].size;

		nodeoffset = fdt_add_subnode(blob, 0, memstart);

		err = fdt_setprop(blob, nodeoffset, "device_type", "memory",
				sizeof("memory"));
		if (err < 0) {
			pr_info("WARNING: could not set %s %s.\n", "device_type",
					fdt_strerror(err));
			return err;
		}

		len = fdt_pack_reg(blob, tmp, ram_base, ram_size, 1);

		err = fdt_setprop(blob, nodeoffset, "reg", tmp, len);
		if (err < 0) {
			pr_info("WARNING: could not set %s %s.\n",
					"reg", fdt_strerror(err));
			return err;
		}
	}
#endif

	/* Copy the reserved-memory node to the DT used by OS */
	err = riscv_fdt_copy_resv_mem_node(gd->fdt_blob, blob);
	if (err < 0)
		return err;

	return 0;
}
