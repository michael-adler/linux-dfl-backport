// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Bridge Driver for FPGA Management Engine (FME)
 *
 * Copyright (C) 2017-2018 Intel Corporation, Inc.
 *
 * Authors:
 *   Wu Hao <hao.wu@intel.com>
 *   Joseph Grecco <joe.grecco@intel.com>
 *   Enno Luebbers <enno.luebbers@intel.com>
 *   Tim Whisonant <tim.whisonant@intel.com>
 *   Ananda Ravuri <ananda.ravuri@intel.com>
 *   Henry Mitchel <henry.mitchel@intel.com>
 */

#include <linux/module.h>
#include <linux/fpga/fpga-bridge.h>

#include "dfl.h"
#include "dfl-fme-pr.h"

struct fme_br_priv {
	struct dfl_fme_br_pdata *pdata;
	struct dfl_fpga_port_ops *port_ops;
	struct dfl_feature_dev_data *port_fdata;
};

static int fme_bridge_enable_set(struct fpga_bridge *bridge, bool enable)
{
	struct fme_br_priv *priv = bridge->priv;
	struct dfl_feature_dev_data *port_fdata;
	struct dfl_fpga_port_ops *ops;

	if (!priv->port_fdata) {
		port_fdata = dfl_fpga_cdev_find_port_data(priv->pdata->cdev,
							  &priv->pdata->port_id,
							  dfl_fpga_check_port_id);
		if (!port_fdata)
			return -ENODEV;

		priv->port_fdata = port_fdata;
	}

	if (priv->port_fdata && !priv->port_ops) {
		ops = dfl_fpga_port_ops_get(priv->port_fdata);
		if (!ops || !ops->enable_set)
			return -ENOENT;

		priv->port_ops = ops;
	}

	return priv->port_ops->enable_set(priv->port_fdata, enable);
}

static const struct fpga_bridge_ops fme_bridge_ops = {
	.enable_set = fme_bridge_enable_set,
};

static int fme_br_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fme_br_priv *priv;
	struct fpga_bridge *br;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdata = dev_get_platdata(dev);

	br = fpga_bridge_register(dev, "DFL FPGA FME Bridge",
				  &fme_bridge_ops, priv);
	if (IS_ERR(br))
		return PTR_ERR(br);

	platform_set_drvdata(pdev, br);

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int fme_br_remove(struct platform_device *pdev)
#else
static void fme_br_remove(struct platform_device *pdev)
#endif
{
	struct fpga_bridge *br = platform_get_drvdata(pdev);
	struct fme_br_priv *priv = br->priv;

	fpga_bridge_unregister(br);

	if (priv->port_ops)
		dfl_fpga_port_ops_put(priv->port_ops);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static struct platform_driver fme_br_driver = {
	.driver	= {
		.name    = DFL_FPGA_FME_BRIDGE,
	},
	.probe   = fme_br_probe,
	.remove  = fme_br_remove,
};

module_platform_driver(fme_br_driver);

MODULE_DESCRIPTION("FPGA Bridge for DFL FPGA Management Engine");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:dfl-fme-bridge");
