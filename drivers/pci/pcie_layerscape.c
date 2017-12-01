/*
 * Copyright 2014-2015 Freescale Semiconductor, Inc.
 * Layerscape PCIe driver
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <asm/arch/fsl_serdes.h>
#include <pci.h>
#include <asm/io.h>
#include <errno.h>
#include <malloc.h>
#include <dm.h>
#include "pcie_layerscape.h"

DECLARE_GLOBAL_DATA_PTR;

LIST_HEAD(ls_pcie_list);

static unsigned int dbi_readl(struct ls_pcie *pcie, unsigned int offset)
{
	return in_le32(pcie->dbi + offset);
}

static void dbi_writel(struct ls_pcie *pcie, unsigned int value,
		       unsigned int offset)
{
	out_le32(pcie->dbi + offset, value);
}

static unsigned int ctrl_readl(struct ls_pcie *pcie, unsigned int offset)
{
	if (pcie->big_endian)
		return in_be32(pcie->ctrl + offset);
	else
		return in_le32(pcie->ctrl + offset);
}

static void ctrl_writel(struct ls_pcie *pcie, unsigned int value,
			unsigned int offset)
{
	if (pcie->big_endian)
		out_be32(pcie->ctrl + offset, value);
	else
		out_le32(pcie->ctrl + offset, value);
}

static int ls_pcie_ltssm(struct ls_pcie *pcie)
{
	u32 state;
	uint svr;

	svr = get_svr();
	if (((svr >> SVR_VAR_PER_SHIFT) & SVR_LS102XA_MASK) == SVR_LS102XA) {
		state = ctrl_readl(pcie, LS1021_PEXMSCPORTSR(pcie->idx));
		state = (state >> LS1021_LTSSM_STATE_SHIFT) & LTSSM_STATE_MASK;
	} else {
		state = ctrl_readl(pcie, PCIE_PF_DBG) & LTSSM_STATE_MASK;
	}

	return state;
}

static int ls_pcie_link_up(struct ls_pcie *pcie)
{
	int ltssm;

	ltssm = ls_pcie_ltssm(pcie);
	if (ltssm < LTSSM_PCIE_L0)
		return 0;

	return 1;
}

static void ls_pcie_cfg0_set_busdev(struct ls_pcie *pcie, u32 busdev)
{
	dbi_writel(pcie, PCIE_ATU_REGION_OUTBOUND | PCIE_ATU_REGION_INDEX0,
		   PCIE_ATU_VIEWPORT);
	dbi_writel(pcie, busdev, PCIE_ATU_LOWER_TARGET);
}

static void ls_pcie_cfg1_set_busdev(struct ls_pcie *pcie, u32 busdev)
{
	dbi_writel(pcie, PCIE_ATU_REGION_OUTBOUND | PCIE_ATU_REGION_INDEX1,
		   PCIE_ATU_VIEWPORT);
	dbi_writel(pcie, busdev, PCIE_ATU_LOWER_TARGET);
}

static void ls_pcie_atu_outbound_set(struct ls_pcie *pcie, int idx, int type,
				      u64 phys, u64 bus_addr, pci_size_t size)
{
	dbi_writel(pcie, PCIE_ATU_REGION_OUTBOUND | idx, PCIE_ATU_VIEWPORT);
	dbi_writel(pcie, (u32)phys, PCIE_ATU_LOWER_BASE);
	dbi_writel(pcie, phys >> 32, PCIE_ATU_UPPER_BASE);
	dbi_writel(pcie, (u32)phys + size - 1, PCIE_ATU_LIMIT);
	dbi_writel(pcie, (u32)bus_addr, PCIE_ATU_LOWER_TARGET);
	dbi_writel(pcie, bus_addr >> 32, PCIE_ATU_UPPER_TARGET);
	dbi_writel(pcie, type, PCIE_ATU_CR1);
	dbi_writel(pcie, PCIE_ATU_ENABLE, PCIE_ATU_CR2);
}

/* Use bar match mode and MEM type as default */
static void ls_pcie_atu_inbound_set(struct ls_pcie *pcie, int idx,
				     int bar, u64 phys)
{
	dbi_writel(pcie, PCIE_ATU_REGION_INBOUND | idx, PCIE_ATU_VIEWPORT);
	dbi_writel(pcie, (u32)phys, PCIE_ATU_LOWER_TARGET);
	dbi_writel(pcie, phys >> 32, PCIE_ATU_UPPER_TARGET);
	dbi_writel(pcie, PCIE_ATU_TYPE_MEM, PCIE_ATU_CR1);
	dbi_writel(pcie, PCIE_ATU_ENABLE | PCIE_ATU_BAR_MODE_ENABLE |
		   PCIE_ATU_BAR_NUM(bar), PCIE_ATU_CR2);
}

static void ls_pcie_dump_atu(struct ls_pcie *pcie)
{
	int i;

	for (i = 0; i < PCIE_ATU_REGION_NUM; i++) {
		dbi_writel(pcie, PCIE_ATU_REGION_OUTBOUND | i,
			   PCIE_ATU_VIEWPORT);
		debug("iATU%d:\n", i);
		debug("\tLOWER PHYS 0x%08x\n",
		      dbi_readl(pcie, PCIE_ATU_LOWER_BASE));
		debug("\tUPPER PHYS 0x%08x\n",
		      dbi_readl(pcie, PCIE_ATU_UPPER_BASE));
		debug("\tLOWER BUS  0x%08x\n",
		      dbi_readl(pcie, PCIE_ATU_LOWER_TARGET));
		debug("\tUPPER BUS  0x%08x\n",
		      dbi_readl(pcie, PCIE_ATU_UPPER_TARGET));
		debug("\tLIMIT      0x%08x\n",
		      readl(pcie->dbi + PCIE_ATU_LIMIT));
		debug("\tCR1        0x%08x\n",
		      dbi_readl(pcie, PCIE_ATU_CR1));
		debug("\tCR2        0x%08x\n",
		      dbi_readl(pcie, PCIE_ATU_CR2));
	}
}

static void ls_pcie_setup_atu(struct ls_pcie *pcie)
{
	struct pci_region *io, *mem, *pref;
	unsigned long long offset = 0;
	int idx = 0;
	uint svr;

	svr = get_svr();
	if (((svr >> SVR_VAR_PER_SHIFT) & SVR_LS102XA_MASK) == SVR_LS102XA) {
		offset = LS1021_PCIE_SPACE_OFFSET +
			 LS1021_PCIE_SPACE_SIZE * pcie->idx;
	}

	/* ATU 0 : OUTBOUND : CFG0 */
	ls_pcie_atu_outbound_set(pcie, PCIE_ATU_REGION_INDEX0,
				 PCIE_ATU_TYPE_CFG0,
				 pcie->cfg_res.start + offset,
				 0,
				 fdt_resource_size(&pcie->cfg_res) / 2);
	/* ATU 1 : OUTBOUND : CFG1 */
	ls_pcie_atu_outbound_set(pcie, PCIE_ATU_REGION_INDEX1,
				 PCIE_ATU_TYPE_CFG1,
				 pcie->cfg_res.start + offset +
				 fdt_resource_size(&pcie->cfg_res) / 2,
				 0,
				 fdt_resource_size(&pcie->cfg_res) / 2);

	pci_get_regions(pcie->bus, &io, &mem, &pref);
	idx = PCIE_ATU_REGION_INDEX1 + 1;

	if (io)
		/* ATU : OUTBOUND : IO */
		ls_pcie_atu_outbound_set(pcie, idx++,
					 PCIE_ATU_TYPE_IO,
					 io->phys_start + offset,
					 io->bus_start,
					 io->size);

	if (mem)
		/* ATU : OUTBOUND : MEM */
		ls_pcie_atu_outbound_set(pcie, idx++,
					 PCIE_ATU_TYPE_MEM,
					 mem->phys_start + offset,
					 mem->bus_start,
					 mem->size);

	if (pref)
		/* ATU : OUTBOUND : pref */
		ls_pcie_atu_outbound_set(pcie, idx++,
					 PCIE_ATU_TYPE_MEM,
					 pref->phys_start + offset,
					 pref->bus_start,
					 pref->size);

	ls_pcie_dump_atu(pcie);
}

/* Return 0 if the address is valid, -errno if not valid */
static int ls_pcie_addr_valid(struct ls_pcie *pcie, pci_dev_t bdf)
{
	struct udevice *bus = pcie->bus;

	if (!pcie->enabled)
		return -ENXIO;

	if (PCI_BUS(bdf) < bus->seq)
		return -EINVAL;

	if ((PCI_BUS(bdf) > bus->seq) && (!ls_pcie_link_up(pcie)))
		return -EINVAL;

	if (PCI_BUS(bdf) <= (bus->seq + 1) && (PCI_DEV(bdf) > 0))
		return -EINVAL;

	return 0;
}

void *ls_pcie_conf_address(struct ls_pcie *pcie, pci_dev_t bdf,
				   int offset)
{
	struct udevice *bus = pcie->bus;
	u32 busdev;

	if (PCI_BUS(bdf) == bus->seq)
		return pcie->dbi + offset;

	busdev = PCIE_ATU_BUS(PCI_BUS(bdf)) |
		 PCIE_ATU_DEV(PCI_DEV(bdf)) |
		 PCIE_ATU_FUNC(PCI_FUNC(bdf));

	if (PCI_BUS(bdf) == bus->seq + 1) {
		ls_pcie_cfg0_set_busdev(pcie, busdev);
		return pcie->cfg0 + offset;
	} else {
		ls_pcie_cfg1_set_busdev(pcie, busdev);
		return pcie->cfg1 + offset;
	}
}

static int ls_pcie_read_config(struct udevice *bus, pci_dev_t bdf,
			       uint offset, ulong *valuep,
			       enum pci_size_t size)
{
	struct ls_pcie *pcie = dev_get_priv(bus);
	void *address;

	if (ls_pcie_addr_valid(pcie, bdf)) {
		*valuep = pci_get_ff(size);
		return 0;
	}

	address = ls_pcie_conf_address(pcie, bdf, offset);

	switch (size) {
	case PCI_SIZE_8:
		*valuep = readb(address);
		return 0;
	case PCI_SIZE_16:
		*valuep = readw(address);
		return 0;
	case PCI_SIZE_32:
		*valuep = readl(address);
		return 0;
	default:
		return -EINVAL;
	}
}

static int ls_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				uint offset, ulong value,
				enum pci_size_t size)
{
	struct ls_pcie *pcie = dev_get_priv(bus);
	void *address;

	if (ls_pcie_addr_valid(pcie, bdf))
		return 0;

	address = ls_pcie_conf_address(pcie, bdf, offset);

	switch (size) {
	case PCI_SIZE_8:
		writeb(value, address);
		return 0;
	case PCI_SIZE_16:
		writew(value, address);
		return 0;
	case PCI_SIZE_32:
		writel(value, address);
		return 0;
	default:
		return -EINVAL;
	}
}

/* Clear multi-function bit */
static void ls_pcie_clear_multifunction(struct ls_pcie *pcie)
{
	writeb(PCI_HEADER_TYPE_BRIDGE, pcie->dbi + PCI_HEADER_TYPE);
}

/* Fix class value */
static void ls_pcie_fix_class(struct ls_pcie *pcie)
{
	writew(PCI_CLASS_BRIDGE_PCI, pcie->dbi + PCI_CLASS_DEVICE);
}

/* Drop MSG TLP except for Vendor MSG */
static void ls_pcie_drop_msg_tlp(struct ls_pcie *pcie)
{
	u32 val;

	val = dbi_readl(pcie, PCIE_STRFMR1);
	val &= 0xDFFFFFFF;
	dbi_writel(pcie, val, PCIE_STRFMR1);
}

/* Disable all bars in RC mode */
static void ls_pcie_disable_bars(struct ls_pcie *pcie)
{
	u32 sriov;

	sriov = in_le32(pcie->dbi + PCIE_SRIOV);

	/*
	 * TODO: For PCIe controller with SRIOV, the method to disable bars
	 * is different and more complex, so will add later.
	 */
	if (PCI_EXT_CAP_ID(sriov) == PCI_EXT_CAP_ID_SRIOV)
		return;

	dbi_writel(pcie, 0, PCIE_CS2_OFFSET + PCI_BASE_ADDRESS_0);
	dbi_writel(pcie, 0, PCIE_CS2_OFFSET + PCI_BASE_ADDRESS_1);
	dbi_writel(pcie, 0, PCIE_CS2_OFFSET + PCI_ROM_ADDRESS1);
}

static void ls_pcie_setup_ctrl(struct ls_pcie *pcie)
{
	ls_pcie_setup_atu(pcie);

	dbi_writel(pcie, 1, PCIE_DBI_RO_WR_EN);
	ls_pcie_fix_class(pcie);
	ls_pcie_clear_multifunction(pcie);
	ls_pcie_drop_msg_tlp(pcie);
	dbi_writel(pcie, 0, PCIE_DBI_RO_WR_EN);

	ls_pcie_disable_bars(pcie);
}

static void ls_pcie_ep_setup_atu(struct ls_pcie *pcie)
{
	u64 phys = CONFIG_SYS_PCI_EP_MEMORY_BASE;

	/* ATU 0 : INBOUND : map BAR0 */
	ls_pcie_atu_inbound_set(pcie, 0, 0, phys);
	/* ATU 1 : INBOUND : map BAR1 */
	phys += PCIE_BAR1_SIZE;
	ls_pcie_atu_inbound_set(pcie, 1, 1, phys);
	/* ATU 2 : INBOUND : map BAR2 */
	phys += PCIE_BAR2_SIZE;
	ls_pcie_atu_inbound_set(pcie, 2, 2, phys);
	/* ATU 3 : INBOUND : map BAR4 */
	phys = CONFIG_SYS_PCI_EP_MEMORY_BASE + PCIE_BAR4_SIZE;
	ls_pcie_atu_inbound_set(pcie, 3, 4, phys);

	/* ATU 0 : OUTBOUND : map MEM */
	ls_pcie_atu_outbound_set(pcie, 0,
				 PCIE_ATU_TYPE_MEM,
				 pcie->cfg_res.start,
				 0,
				 CONFIG_SYS_PCI_MEMORY_SIZE);
}

/* BAR0 and BAR1 are 32bit BAR2 and BAR4 are 64bit */
static void ls_pcie_ep_setup_bar(void *bar_base, int bar, u32 size)
{
	/* The least inbound window is 4KiB */
	if (size < 4 * 1024)
		return;

	switch (bar) {
	case 0:
		writel(size - 1, bar_base + PCI_BASE_ADDRESS_0);
		break;
	case 1:
		writel(size - 1, bar_base + PCI_BASE_ADDRESS_1);
		break;
	case 2:
		writel(size - 1, bar_base + PCI_BASE_ADDRESS_2);
		writel(0, bar_base + PCI_BASE_ADDRESS_3);
		break;
	case 4:
		writel(size - 1, bar_base + PCI_BASE_ADDRESS_4);
		writel(0, bar_base + PCI_BASE_ADDRESS_5);
		break;
	default:
		break;
	}
}

static void ls_pcie_ep_setup_bars(void *bar_base)
{
	/* BAR0 - 32bit - 4K configuration */
	ls_pcie_ep_setup_bar(bar_base, 0, PCIE_BAR0_SIZE);
	/* BAR1 - 32bit - 8K MSIX*/
	ls_pcie_ep_setup_bar(bar_base, 1, PCIE_BAR1_SIZE);
	/* BAR2 - 64bit - 4K MEM desciptor */
	ls_pcie_ep_setup_bar(bar_base, 2, PCIE_BAR2_SIZE);
	/* BAR4 - 64bit - 1M MEM*/
	ls_pcie_ep_setup_bar(bar_base, 4, PCIE_BAR4_SIZE);
}

static void ls_pcie_setup_ep(struct ls_pcie *pcie)
{
	u32 sriov;

	sriov = readl(pcie->dbi + PCIE_SRIOV);
	if (PCI_EXT_CAP_ID(sriov) == PCI_EXT_CAP_ID_SRIOV) {
		int pf, vf;

		for (pf = 0; pf < PCIE_PF_NUM; pf++) {
			for (vf = 0; vf <= PCIE_VF_NUM; vf++) {
				ctrl_writel(pcie, PCIE_LCTRL0_VAL(pf, vf),
					    PCIE_PF_VF_CTRL);

				ls_pcie_ep_setup_bars(pcie->dbi);
				ls_pcie_ep_setup_atu(pcie);
			}
		}
		/* Disable CFG2 */
		ctrl_writel(pcie, 0, PCIE_PF_VF_CTRL);
	} else {
		ls_pcie_ep_setup_bars(pcie->dbi + PCIE_NO_SRIOV_BAR_BASE);
		ls_pcie_ep_setup_atu(pcie);
	}
}

static int ls_pcie_probe(struct udevice *dev)
{
	struct ls_pcie *pcie = dev_get_priv(dev);
	const void *fdt = gd->fdt_blob;
	int node = dev_of_offset(dev);
	u8 header_type;
	u16 link_sta;
	bool ep_mode;
	int ret;

	pcie->bus = dev;

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "dbi", &pcie->dbi_res);
	if (ret) {
		printf("ls-pcie: resource \"dbi\" not found\n");
		return ret;
	}

	pcie->idx = (pcie->dbi_res.start - PCIE_SYS_BASE_ADDR) / PCIE_CCSR_SIZE;

	list_add(&pcie->list, &ls_pcie_list);

	pcie->enabled = is_serdes_configured(PCIE_SRDS_PRTCL(pcie->idx));
	if (!pcie->enabled) {
		printf("PCIe%d: %s disabled\n", pcie->idx, dev->name);
		return 0;
	}

	pcie->dbi = map_physmem(pcie->dbi_res.start,
				fdt_resource_size(&pcie->dbi_res),
				MAP_NOCACHE);

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "lut", &pcie->lut_res);
	if (!ret)
		pcie->lut = map_physmem(pcie->lut_res.start,
					fdt_resource_size(&pcie->lut_res),
					MAP_NOCACHE);

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "ctrl", &pcie->ctrl_res);
	if (!ret)
		pcie->ctrl = map_physmem(pcie->ctrl_res.start,
					 fdt_resource_size(&pcie->ctrl_res),
					 MAP_NOCACHE);
	if (!pcie->ctrl)
		pcie->ctrl = pcie->lut;

	if (!pcie->ctrl) {
		printf("%s: NOT find CTRL\n", dev->name);
		return -1;
	}

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "config", &pcie->cfg_res);
	if (ret) {
		printf("%s: resource \"config\" not found\n", dev->name);
		return ret;
	}

	pcie->cfg0 = map_physmem(pcie->cfg_res.start,
				 fdt_resource_size(&pcie->cfg_res),
				 MAP_NOCACHE);
	pcie->cfg1 = pcie->cfg0 + fdt_resource_size(&pcie->cfg_res) / 2;

	pcie->big_endian = fdtdec_get_bool(fdt, node, "big-endian");

	debug("%s dbi:%lx lut:%lx ctrl:0x%lx cfg0:0x%lx, big-endian:%d\n",
	      dev->name, (unsigned long)pcie->dbi, (unsigned long)pcie->lut,
	      (unsigned long)pcie->ctrl, (unsigned long)pcie->cfg0,
	      pcie->big_endian);

	header_type = readb(pcie->dbi + PCI_HEADER_TYPE);
	ep_mode = (header_type & 0x7f) == PCI_HEADER_TYPE_NORMAL;
	printf("PCIe%u: %s %s", pcie->idx, dev->name,
	       ep_mode ? "Endpoint" : "Root Complex");

	if (ep_mode)
		ls_pcie_setup_ep(pcie);
	else
		ls_pcie_setup_ctrl(pcie);

	if (!ls_pcie_link_up(pcie)) {
		/* Let the user know there's no PCIe link */
		printf(": no link\n");
		return 0;
	}

	/* Print the negotiated PCIe link width */
	link_sta = readw(pcie->dbi + PCIE_LINK_STA);
	printf(": x%d gen%d\n", (link_sta & PCIE_LINK_WIDTH_MASK) >> 4,
	       link_sta & PCIE_LINK_SPEED_MASK);

	return 0;
}

static const struct dm_pci_ops ls_pcie_ops = {
	.read_config	= ls_pcie_read_config,
	.write_config	= ls_pcie_write_config,
};

static const struct udevice_id ls_pcie_ids[] = {
	{ .compatible = "fsl,ls-pcie" },
	{ }
};

U_BOOT_DRIVER(pci_layerscape) = {
	.name = "pci_layerscape",
	.id = UCLASS_PCI,
	.of_match = ls_pcie_ids,
	.ops = &ls_pcie_ops,
	.probe	= ls_pcie_probe,
	.priv_auto_alloc_size = sizeof(struct ls_pcie),
};
