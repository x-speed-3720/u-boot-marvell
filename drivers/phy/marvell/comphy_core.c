/*
 * Copyright (C) 2015-2016 Marvell International Ltd.
 *
 * Copyright (C) 2016 Stefan Roese <sr@denx.de>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <fdtdec.h>
#include <linux/errno.h>
#include <asm/io.h>

#include "comphy_priv.h"

#define COMPHY_MAX_CHIP 4

DECLARE_GLOBAL_DATA_PTR;

static char *get_speed_string(u32 speed)
{
	char *speed_strings[] = {"1.25 Gbps", "2.5 Gbps", "3.125 Gbps",
				 "5 Gbps", "5.125 Gpbs", "6 Gbps",
				 "10.3125 Gbps" };

	if (speed < 0 || speed > COMPHY_SPEED_MAX)
		return "invalid";

	return speed_strings[speed];
}

static char *get_type_string(u32 type)
{
	char *type_strings[] = {"UNCONNECTED", "PEX0", "PEX1", "PEX2", "PEX3",
				"SATA0", "SATA1", "SGMII0", "SGMII1", "SGMII2",
				"USB3", "USB3_HOST0", "USB3_HOST1",
				"USB3_DEVICE", "RXAUI0", "RXAUI1", "SFI0",
				"SFI1", "AP", "IGNORE"};

	if (type < 0 || type > COMPHY_TYPE_MAX)
		return "invalid";

	return type_strings[type];
}

void reg_set(void __iomem *addr, u32 data, u32 mask)
{
	debug("Write to address = %#010lx, data = %#010x (mask = %#010x) - ",
	      (unsigned long)addr, data, mask);
	debug("old value = %#010x ==> ", readl(addr));
	reg_set_silent(addr, data, mask);
	debug("new value %#010x\n", readl(addr));
}

void reg_set_silent(void __iomem *addr, u32 data, u32 mask)
{
	u32 reg_data;

	reg_data = readl(addr);
	reg_data &= ~mask;
	reg_data |= data;
	writel(reg_data, addr);
}

void reg_set16(void __iomem *addr, u16 data, u16 mask)
{
	debug("Write to address = %#010lx, data = %#06x (mask = %#06x) - ",
	      (unsigned long)addr, data, mask);
	debug("old value = %#06x ==> ", readw(addr));
	reg_set_silent16(addr, data, mask);
	debug("new value %#06x\n", readw(addr));
}

void reg_set_silent16(void __iomem *addr, u16 data, u16 mask)
{
	u16 reg_data;

	reg_data = readw(addr);
	reg_data &= ~mask;
	reg_data |= data;
	writew(reg_data, addr);
}

void comphy_print(struct chip_serdes_phy_config *chip_cfg,
		  struct comphy_map *comphy_map_data)
{
	u32 lane;

	for (lane = 0; lane < chip_cfg->comphy_lanes_count;
	     lane++, comphy_map_data++) {
		if (comphy_map_data->speed == COMPHY_SPEED_INVALID) {
			printf("Comphy-%d: %-13s\n", lane,
			       get_type_string(comphy_map_data->type));
		} else {
			printf("Comphy-%d: %-13s %-10s\n", lane,
			       get_type_string(comphy_map_data->type),
			       get_speed_string(comphy_map_data->speed));
		}
	}
}

int comphy_rx_training(struct udevice *dev, u32 lane)
{
	struct chip_serdes_phy_config *chip_cfg = dev_get_priv(dev);

	if (chip_cfg->rx_training)
		return chip_cfg->rx_training(chip_cfg, lane);

	return 0;
}

static int comphy_probe(struct udevice *dev)
{
	const void *blob = gd->fdt_blob;
	int node = dev_of_offset(dev);
	struct chip_serdes_phy_config *chip_cfg = dev_get_priv(dev);
	int subnode;
	int lane;
	int last_idx = 0;
	static int current_idx;

	/* Save base addresses for later use */
	chip_cfg->comphy_base_addr = (void *)dev_get_addr_index(dev, 0);
	if (IS_ERR(chip_cfg->comphy_base_addr))
		return PTR_ERR(chip_cfg->comphy_base_addr);

	chip_cfg->hpipe3_base_addr = (void *)dev_get_addr_index(dev, 1);
	if (IS_ERR(chip_cfg->hpipe3_base_addr))
		return PTR_ERR(chip_cfg->hpipe3_base_addr);

	chip_cfg->comphy_lanes_count = fdtdec_get_int(blob, node,
						      "max-lanes", 0);
	if (chip_cfg->comphy_lanes_count <= 0) {
		dev_err(&dev->dev, "comphy max lanes is wrong\n");
		return -EINVAL;
	}

	chip_cfg->comphy_mux_bitcount = fdtdec_get_int(blob, node,
						       "mux-bitcount", 0);
	if (chip_cfg->comphy_mux_bitcount <= 0) {
		dev_err(&dev->dev, "comphy mux bit count is wrong\n");
		return -EINVAL;
	}

	if (of_device_is_compatible(dev, "marvell,comphy-armada-3700")) {
		chip_cfg->ptr_comphy_chip_init = comphy_a3700_init;
		chip_cfg->rx_training = NULL;
	}

	if (of_device_is_compatible(dev, "marvell,comphy-cp110")) {
		chip_cfg->ptr_comphy_chip_init = comphy_cp110_init;
		chip_cfg->rx_training = comphy_cp110_sfi_rx_training;
	}

	/*
	 * Bail out if no chip_init function is defined, e.g. no
	 * compatible node is found
	 */
	if (!chip_cfg->ptr_comphy_chip_init) {
		dev_err(&dev->dev, "comphy: No compatible DT node found\n");
		return -ENODEV;
	}

	lane = 0;
	fdt_for_each_subnode(subnode, blob, node) {
		/* Skip disabled ports */
		if (!fdtdec_get_is_enabled(blob, subnode))
			continue;

		chip_cfg->comphy_map_data[lane].type =
			fdtdec_get_int(blob, subnode, "phy-type",
				       COMPHY_TYPE_INVALID);

		if (chip_cfg->comphy_map_data[lane].type ==
		    COMPHY_TYPE_INVALID) {
			printf("no phy type for lane %d, setting lane as unconnected\n",
			       lane + 1);
			continue;
		}

		chip_cfg->comphy_map_data[lane].speed =
			fdtdec_get_int(blob, subnode, "phy-speed",
				       COMPHY_SPEED_INVALID);

		chip_cfg->comphy_map_data[lane].invert =
			fdtdec_get_int(blob, subnode, "phy-invert",
				       COMPHY_POLARITY_NO_INVERT);

		chip_cfg->comphy_map_data[lane].clk_src =
			fdtdec_get_bool(blob, subnode, "clk-src");

		chip_cfg->comphy_map_data[lane].end_point =
			fdtdec_get_bool(blob, subnode, "end_point");

		lane++;
	}

	/* Save CP index for MultiCP devices (A8K) */
	chip_cfg->cp_index = current_idx++;
	/* PHY power UP sequence */
	chip_cfg->ptr_comphy_chip_init(chip_cfg, chip_cfg->comphy_map_data);
	/* PHY print SerDes status */
	printf("Comphy chip #%d:\n", chip_cfg->cp_index);
	comphy_print(chip_cfg, chip_cfg->comphy_map_data);

	/*
	 * Only run the dedicated PHY init code once, in the last PHY init call
	 */
	if (of_machine_is_compatible("marvell,armada8040"))
		last_idx = 1;

	if (chip_cfg->cp_index == last_idx) {
		/* Initialize dedicated PHYs (not muxed SerDes lanes) */
		comphy_dedicated_phys_init();
	}

	return 0;
}

static const struct udevice_id comphy_ids[] = {
	{ .compatible = "marvell,mvebu-comphy" },
	{ }
};

U_BOOT_DRIVER(mvebu_comphy) = {
	.name	= "mvebu_comphy",
	.id	= UCLASS_MISC,
	.of_match = comphy_ids,
	.probe	= comphy_probe,
	.priv_auto_alloc_size = sizeof(struct chip_serdes_phy_config),
};
