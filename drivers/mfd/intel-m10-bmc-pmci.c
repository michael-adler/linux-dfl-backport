// SPDX-License-Identifier: GPL-2.0
/*
 * MAX10 BMC Platform Management Component Interface (PMCI) based
 * interface.
 *
 * Copyright (C) 2020-2023 Intel Corporation.
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/dfl.h>
#include <linux/mfd/core.h>
#include <linux/mfd/intel-m10-bmc.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/version.h>

struct m10bmc_pmci_device {
	void __iomem *base;
	struct intel_m10bmc m10bmc;
	struct mutex flash_mutex;	/* protects flash_busy and serializes flash read/read */
	bool flash_busy;
};

static void pmci_write_fifo(void __iomem *base, const u32 *buf, size_t count)
{
	while (count--)
		writel(*buf++, base);
}

static void pmci_read_fifo(void __iomem *base, u32 *buf, size_t count)
{
	while (count--)
		*buf++ = readl(base);
}

static u32 pmci_get_write_space(struct m10bmc_pmci_device *pmci)
{
	u32 val;
	int ret;

	ret = read_poll_timeout(readl, val,
				FIELD_GET(M10BMC_N6000_FLASH_FIFO_SPACE, val) ==
				M10BMC_N6000_FIFO_MAX_WORDS,
				M10BMC_FLASH_INT_US, M10BMC_FLASH_TIMEOUT_US,
				false, pmci->base + M10BMC_N6000_FLASH_CTRL);
	if (ret == -ETIMEDOUT)
		return 0;

	return FIELD_GET(M10BMC_N6000_FLASH_FIFO_SPACE, val) * M10BMC_N6000_FIFO_WORD_SIZE;
}

static int pmci_flash_bulk_write(struct intel_m10bmc *m10bmc, const u8 *buf, u32 size)
{
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);
	u32 blk_size, offset = 0, write_count;

	while (size) {
		blk_size = min(pmci_get_write_space(pmci), size);
		if (blk_size == 0) {
			dev_err(m10bmc->dev, "get FIFO available size fail\n");
			return -EIO;
		}

		if (size < M10BMC_N6000_FIFO_WORD_SIZE)
			break;

		write_count = blk_size / M10BMC_N6000_FIFO_WORD_SIZE;
		pmci_write_fifo(pmci->base + M10BMC_N6000_FLASH_FIFO,
				(u32 *)(buf + offset), write_count);

		size -= blk_size;
		offset += blk_size;
	}

	/* Handle remainder (less than M10BMC_N6000_FIFO_WORD_SIZE bytes) */
	if (size) {
		u32 tmp = 0;

		memcpy(&tmp, buf + offset, size);
		pmci_write_fifo(pmci->base + M10BMC_N6000_FLASH_FIFO, &tmp, 1);
	}

	return 0;
}

static int pmci_flash_bulk_read(struct intel_m10bmc *m10bmc, u8 *buf, u32 addr, u32 size)
{
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);
	u32 blk_size, offset = 0, val, full_read_count, read_count;
	int ret;

	while (size) {
		blk_size = min_t(u32, size, M10BMC_N6000_READ_BLOCK_SIZE);
		full_read_count = blk_size / M10BMC_N6000_FIFO_WORD_SIZE;

		read_count = full_read_count;
		if (full_read_count * M10BMC_N6000_FIFO_WORD_SIZE < blk_size)
			read_count++;

		writel(addr + offset, pmci->base + M10BMC_N6000_FLASH_ADDR);
		writel(FIELD_PREP(M10BMC_N6000_FLASH_READ_COUNT, read_count) |
		       M10BMC_N6000_FLASH_RD_MODE,
		       pmci->base + M10BMC_N6000_FLASH_CTRL);

		ret = readl_poll_timeout((pmci->base + M10BMC_N6000_FLASH_CTRL), val,
					 !(val & M10BMC_N6000_FLASH_BUSY),
					 M10BMC_FLASH_INT_US, M10BMC_FLASH_TIMEOUT_US);
		if (ret) {
			dev_err(m10bmc->dev, "read timed out on reading flash 0x%xn", val);
			return ret;
		}

		pmci_read_fifo(pmci->base + M10BMC_N6000_FLASH_FIFO,
			       (u32 *)(buf + offset), full_read_count);

		size -= blk_size;
		offset += blk_size;

		if (full_read_count < read_count)
			break;

		writel(0, pmci->base + M10BMC_N6000_FLASH_CTRL);
	}

	/* Handle remainder (less than M10BMC_N6000_FIFO_WORD_SIZE bytes) */
	if (size) {
		u32 tmp;

		pmci_read_fifo(pmci->base + M10BMC_N6000_FLASH_FIFO, &tmp, 1);
		memcpy(buf + offset, &tmp, size);

		writel(0, pmci->base + M10BMC_N6000_FLASH_CTRL);
	}

	return 0;
}

static int m10bmc_pmci_set_flash_host_mux(struct intel_m10bmc *m10bmc, bool request)
{
	u32 ctrl;
	int ret;

	ret = regmap_update_bits(m10bmc->regmap, M10BMC_N6000_FLASH_MUX_CTRL,
				 M10BMC_N6000_FLASH_HOST_REQUEST,
				 FIELD_PREP(M10BMC_N6000_FLASH_HOST_REQUEST, request));
	if (ret)
		return ret;

	return regmap_read_poll_timeout(m10bmc->regmap,
					M10BMC_N6000_FLASH_MUX_CTRL, ctrl,
					request ?
					(get_flash_mux(ctrl) == M10BMC_N6000_FLASH_MUX_HOST) :
					(get_flash_mux(ctrl) != M10BMC_N6000_FLASH_MUX_HOST),
					M10BMC_FLASH_INT_US, M10BMC_FLASH_TIMEOUT_US);
}

static int m10bmc_pmci_flash_read(struct intel_m10bmc *m10bmc, u8 *buf, u32 addr, u32 size)
{
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);
	int ret, ret2;

	mutex_lock(&pmci->flash_mutex);
	if (pmci->flash_busy) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = m10bmc_pmci_set_flash_host_mux(m10bmc, true);
	if (ret)
		goto mux_fail;

	ret = pmci_flash_bulk_read(m10bmc, buf, addr, size);

mux_fail:
	ret2 = m10bmc_pmci_set_flash_host_mux(m10bmc, false);

unlock:
	mutex_unlock(&pmci->flash_mutex);
	if (ret)
		return ret;
	return ret2;
}

static int m10bmc_pmci_flash_write(struct intel_m10bmc *m10bmc, const u8 *buf, u32 offset, u32 size)
{
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);
	int ret;

	mutex_lock(&pmci->flash_mutex);
	WARN_ON_ONCE(!pmci->flash_busy);
	/* On write, firmware manages flash MUX */
	ret = pmci_flash_bulk_write(m10bmc, buf + offset, size);
	mutex_unlock(&pmci->flash_mutex);

	return ret;
}

static int m10bmc_pmci_flash_lock(struct intel_m10bmc *m10bmc)
{
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);
	int ret = 0;

	mutex_lock(&pmci->flash_mutex);
	if (pmci->flash_busy) {
		ret = -EBUSY;
		goto unlock;
	}

	pmci->flash_busy = true;

unlock:
	mutex_unlock(&pmci->flash_mutex);
	return ret;
}

static void m10bmc_pmci_flash_unlock(struct intel_m10bmc *m10bmc)
{
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);

	mutex_lock(&pmci->flash_mutex);
	WARN_ON_ONCE(!pmci->flash_busy);
	pmci->flash_busy = false;
	mutex_unlock(&pmci->flash_mutex);
}

static const struct intel_m10bmc_flash_bulk_ops m10bmc_pmci_flash_bulk_ops = {
	.read = m10bmc_pmci_flash_read,
	.write = m10bmc_pmci_flash_write,
	.lock_write = m10bmc_pmci_flash_lock,
	.unlock_write = m10bmc_pmci_flash_unlock,
};

static const struct regmap_range m10bmc_pmci_regmap_range[] = {
	regmap_reg_range(M10BMC_N6000_SYS_BASE, M10BMC_N6000_SYS_END),
};

static const struct regmap_access_table m10bmc_pmci_access_table = {
	.yes_ranges	= m10bmc_pmci_regmap_range,
	.n_yes_ranges	= ARRAY_SIZE(m10bmc_pmci_regmap_range),
};

static struct regmap_config m10bmc_pmci_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.wr_table = &m10bmc_pmci_access_table,
	.rd_table = &m10bmc_pmci_access_table,
	.max_register = M10BMC_N6000_SYS_END,
};

static struct mfd_cell m10bmc_pmci_n6000_bmc_subdevs[] = {
	{ .name = "n6000bmc-hwmon" },
	{ .name = "n6000bmc-sec-update" },
	{ .name = "n6000bmc-log" },
};

static struct mfd_cell m10bmc_pmci_c6100_bmc_subdevs[] = {
	{ .name = "c6100bmc-hwmon" },
	{ .name = "n6000bmc-sec-update" },
	{ .name = "c6100bmc-log" },
};

static struct mfd_cell m10bmc_pmci_cmc_bmc_subdevs[] = {
	{ .name = "cmcbmc-hwmon" },
	{ .name = "n6000bmc-sec-update" },
	{ .name = "cmcbmc-log" },
};

static const struct m10bmc_csr_map m10bmc_n6000_csr_map = {
	.base = M10BMC_N6000_SYS_BASE,
	.build_version = M10BMC_N6000_BUILD_VER,
	.fw_version = NIOS2_N6000_FW_VERSION,
	.mac_low = M10BMC_N6000_MAC_LOW,
	.mac_high = M10BMC_N6000_MAC_HIGH,
	.doorbell = M10BMC_N6000_DOORBELL,
	.auth_result = M10BMC_N6000_AUTH_RESULT,
	.bmc_prog_addr = M10BMC_N6000_BMC_PROG_ADDR,
	.bmc_reh_addr = M10BMC_N6000_BMC_REH_ADDR,
	.bmc_magic = M10BMC_N6000_BMC_PROG_MAGIC,
	.sr_prog_addr = M10BMC_N6000_SR_PROG_ADDR,
	.sr_reh_addr = M10BMC_N6000_SR_REH_ADDR,
	.sr_magic = M10BMC_N6000_SR_PROG_MAGIC,
	.pr_prog_addr = M10BMC_N6000_PR_PROG_ADDR,
	.pr_reh_addr = M10BMC_N6000_PR_REH_ADDR,
	.pr_magic = M10BMC_N6000_PR_PROG_MAGIC,
	.rsu_update_counter = M10BMC_N6000_STAGING_FLASH_COUNT,
	.pr_sdm_reh_reg = M10BMC_PMCI_PR_RH0,
	.pr_sdm_csk_reg = M10BMC_PMCI_PR_CSK,
	.sr_sdm_reh_reg = M10BMC_PMCI_SR_RH0,
	.sr_sdm_csk_reg = M10BMC_PMCI_SR_CSK,
	.staging_size = M10BMC_STAGING_SIZE,
};

static const struct m10bmc_csr_map m10bmc_c6100_csr_map = {
	.base = M10BMC_N6000_SYS_BASE,
	.build_version = M10BMC_N6000_BUILD_VER,
	.fw_version = NIOS2_N6000_FW_VERSION,
	.mac_low = M10BMC_N6000_MAC_LOW,
	.mac_high = M10BMC_N6000_MAC_HIGH,
	.doorbell = M10BMC_N6000_DOORBELL,
	.auth_result = M10BMC_N6000_AUTH_RESULT,
	.bmc_prog_addr = M10BMC_C6100_BMC_PROG_ADDR,
	.bmc_reh_addr = M10BMC_C6100_BMC_REH_ADDR,
	.bmc_magic = M10BMC_N6000_BMC_PROG_MAGIC,
	.sr_prog_addr = M10BMC_C6100_SR_PROG_ADDR,
	.sr_reh_addr = M10BMC_C6100_SR_REH_ADDR,
	.sr_magic = M10BMC_N6000_SR_PROG_MAGIC,
	.pr_prog_addr = M10BMC_C6100_PR_PROG_ADDR,
	.pr_reh_addr = M10BMC_C6100_PR_REH_ADDR,
	.pr_magic = M10BMC_N6000_PR_PROG_MAGIC,
	.rsu_update_counter = M10BMC_C6100_STAGING_FLASH_COUNT,
	.pr_sdm_reh_reg = M10BMC_PMCI_PR_RH0,
	.pr_sdm_csk_reg = M10BMC_PMCI_PR_CSK,
	.sr_sdm_reh_reg = M10BMC_PMCI_SR_RH0,
	.sr_sdm_csk_reg = M10BMC_PMCI_SR_CSK,
	.staging_size = M10BMC_STAGING_SIZE,
};

static const struct intel_m10bmc_platform_info m10bmc_pmci_n6000 = {
	.cells = m10bmc_pmci_n6000_bmc_subdevs,
	.n_cells = ARRAY_SIZE(m10bmc_pmci_n6000_bmc_subdevs),
	.csr_map = &m10bmc_n6000_csr_map,
};

static const struct intel_m10bmc_platform_info m10bmc_pmci_c6100 = {
	.cells = m10bmc_pmci_c6100_bmc_subdevs,
	.n_cells = ARRAY_SIZE(m10bmc_pmci_c6100_bmc_subdevs),
	.csr_map = &m10bmc_c6100_csr_map,
};

static const struct intel_m10bmc_platform_info m10bmc_pmci_cmc = {
	.cells = m10bmc_pmci_cmc_bmc_subdevs,
	.n_cells = ARRAY_SIZE(m10bmc_pmci_cmc_bmc_subdevs),
	.csr_map = &m10bmc_c6100_csr_map,
};

static int m10bmc_pmci_probe(struct dfl_device *ddev)
{
	const struct intel_m10bmc_platform_info *pinfo;
	struct device *dev = &ddev->dev;
	struct m10bmc_pmci_device *pmci;
	int ret;

	switch (ddev->revision) {
	case 1:
		pinfo = &m10bmc_pmci_n6000;
		break;
	case 2:
		pinfo = &m10bmc_pmci_c6100;
		break;
	case 3:
		pinfo = &m10bmc_pmci_cmc;
		break;
	default:
		return -ENODEV;
	}

	pmci = devm_kzalloc(dev, sizeof(*pmci), GFP_KERNEL);
	if (!pmci)
		return -ENOMEM;

	pmci->m10bmc.flash_bulk_ops = &m10bmc_pmci_flash_bulk_ops;
	pmci->m10bmc.dev = dev;

	pmci->base = devm_ioremap_resource(dev, &ddev->mmio_res);
	if (IS_ERR(pmci->base))
		return PTR_ERR(pmci->base);

	mutex_init(&pmci->flash_mutex);

	pmci->m10bmc.regmap =
		devm_regmap_init_indirect_register(dev,
						   pmci->base + M10BMC_N6000_INDIRECT_BASE,
						   &m10bmc_pmci_regmap_config);
	if (IS_ERR(pmci->m10bmc.regmap)) {
		ret = PTR_ERR(pmci->m10bmc.regmap);
		goto destroy_mutex;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0) && RHEL_RELEASE_CODE < 0x803
	ret = device_add_groups(dev, m10bmc_dev_groups);
	if (ret)
		return ret;
#endif

	ret = m10bmc_dev_init(&pmci->m10bmc, pinfo);
	if (ret)
		goto destroy_mutex;
	return 0;

destroy_mutex:
	mutex_destroy(&pmci->flash_mutex);
	return ret;
}

static void m10bmc_pmci_remove(struct dfl_device *ddev)
{
	struct intel_m10bmc *m10bmc = dev_get_drvdata(&ddev->dev);
	struct m10bmc_pmci_device *pmci = container_of(m10bmc, struct m10bmc_pmci_device, m10bmc);

	mutex_destroy(&pmci->flash_mutex);
}

#define FME_FEATURE_ID_M10BMC_PMCI	0x12

static const struct dfl_device_id m10bmc_pmci_ids[] = {
	{ FME_ID, FME_FEATURE_ID_M10BMC_PMCI },
	{ }
};
MODULE_DEVICE_TABLE(dfl, m10bmc_pmci_ids);

static struct dfl_driver m10bmc_pmci_driver = {
	.drv	= {
		.name       = "intel-m10-bmc",
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0) || RHEL_RELEASE_CODE >= 0x803
		.dev_groups = m10bmc_dev_groups,
#endif
	},
	.id_table = m10bmc_pmci_ids,
	.probe    = m10bmc_pmci_probe,
	.remove   = m10bmc_pmci_remove,
};

module_dfl_driver(m10bmc_pmci_driver);

MODULE_DESCRIPTION("MAX10 BMC PMCI-based interface");
MODULE_ALIAS("dfl:t0000f0012");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("INTEL_M10_BMC_CORE");
