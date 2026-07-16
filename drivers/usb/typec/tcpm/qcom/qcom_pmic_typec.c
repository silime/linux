// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Linaro Ltd. All rights reserved.
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/role.h>
#include <linux/usb/tcpm.h>
#include <linux/usb/typec_mux.h>

#include <drm/bridge/aux-bridge.h>

#include "qcom_pmic_typec.h"
#include "qcom_pmic_typec_pdphy.h"
#include "qcom_pmic_typec_port.h"

struct pmic_typec_resources {
	const struct pmic_typec_pdphy_resources	*pdphy_res;
	const struct pmic_typec_port_resources	*port_res;
	const char				*charger_psy_name;
};

#define PM8150B_CHARGER_PSY_NAME "pm8150b-charger"

static int qcom_pmic_typec_get_current_limit(struct tcpc_dev *tcpc)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);
	union power_supply_propval val;
	struct power_supply *psy;
	int ret;

	psy = power_supply_get_by_name(tcpm->charger_psy_name);
	if (!psy)
		return 900;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_MAX,
					&val);
	power_supply_put(psy);

	/* APSD may still be running when TCPM asks for the Rp-default limit. */
	return ret || val.intval <= 0 ? 900 : val.intval / 1000;
}

static int qcom_pmic_typec_set_current_limit(struct tcpc_dev *tcpc,
					     u32 max_ma, u32 mv)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);
	union power_supply_propval val = { .intval = max_ma * 1000 };
	struct power_supply *psy;
	int ret;

	/* A detach does not need to overwrite the charger's next APSD limit. */
	if (!max_ma)
		return 0;

	psy = power_supply_get_by_name(tcpm->charger_psy_name);
	if (!psy)
		return -ENODEV;

	ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_CURRENT_MAX,
					&val);
	power_supply_put(psy);

	return ret;
}

static int qcom_pmic_typec_init(struct tcpc_dev *tcpc)
{
	return 0;
}

static int qcom_pmic_typec_probe(struct platform_device *pdev)
{
	struct pmic_typec *tcpm;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct pmic_typec_resources *res;
	struct regmap *regmap;
	struct auxiliary_device *bridge_dev;
	u32 base;
	int ret;

	res = of_device_get_match_data(dev);
	if (!res)
		return -ENODEV;

	tcpm = devm_kzalloc(dev, sizeof(*tcpm), GFP_KERNEL);
	if (!tcpm)
		return -ENOMEM;

	tcpm->dev = dev;
	tcpm->charger_psy_name = res->charger_psy_name;
	tcpm->tcpc.init = qcom_pmic_typec_init;
	if (res->charger_psy_name) {
		tcpm->tcpc.get_current_limit = qcom_pmic_typec_get_current_limit;
		tcpm->tcpc.set_current_limit = qcom_pmic_typec_set_current_limit;
	}

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get regmap\n");
		return -ENODEV;
	}

	ret = of_property_read_u32_index(np, "reg", 0, &base);
	if (ret)
		return ret;

	ret = qcom_pmic_typec_port_probe(pdev, tcpm,
					 res->port_res, regmap, base);
	if (ret)
		return ret;

	if (res->pdphy_res) {
		ret = of_property_read_u32_index(np, "reg", 1, &base);
		if (ret)
			return ret;

		ret = qcom_pmic_typec_pdphy_probe(pdev, tcpm,
						  res->pdphy_res, regmap, base);
		if (ret)
			return ret;
	} else {
		ret = qcom_pmic_typec_pdphy_stub_probe(pdev, tcpm);
		if (ret)
			return ret;
	}

	platform_set_drvdata(pdev, tcpm);

	tcpm->tcpc.fwnode = device_get_named_child_node(tcpm->dev, "connector");
	if (!tcpm->tcpc.fwnode)
		return -EINVAL;

	bridge_dev = devm_drm_dp_hpd_bridge_alloc(tcpm->dev, to_of_node(tcpm->tcpc.fwnode));
	if (IS_ERR(bridge_dev)) {
		ret = PTR_ERR(bridge_dev);
		goto fwnode_remove;
	}

	tcpm->tcpm_port = tcpm_register_port(tcpm->dev, &tcpm->tcpc);
	if (IS_ERR(tcpm->tcpm_port)) {
		ret = PTR_ERR(tcpm->tcpm_port);
		goto fwnode_remove;
	}

	ret = tcpm->port_start(tcpm, tcpm->tcpm_port);
	if (ret)
		goto port_unregister;

	ret = tcpm->pdphy_start(tcpm, tcpm->tcpm_port);
	if (ret)
		goto port_stop;

	ret = devm_drm_dp_hpd_bridge_add(tcpm->dev, bridge_dev);
	if (ret)
		goto pdphy_stop;

	return 0;

pdphy_stop:
	tcpm->pdphy_stop(tcpm);
port_stop:
	tcpm->port_stop(tcpm);
port_unregister:
	tcpm_unregister_port(tcpm->tcpm_port);
fwnode_remove:
	fwnode_handle_put(tcpm->tcpc.fwnode);

	return ret;
}

static void qcom_pmic_typec_remove(struct platform_device *pdev)
{
	struct pmic_typec *tcpm = platform_get_drvdata(pdev);

	tcpm->pdphy_stop(tcpm);
	tcpm->port_stop(tcpm);
	tcpm_unregister_port(tcpm->tcpm_port);
	fwnode_handle_put(tcpm->tcpc.fwnode);
}

static const struct pmic_typec_resources pm8150b_typec_res = {
	.pdphy_res = &pm8150b_pdphy_res,
	.port_res = &pm8150b_port_res,
	.charger_psy_name = PM8150B_CHARGER_PSY_NAME,
};

static const struct pmic_typec_resources pmi632_typec_res = {
	/* PD PHY not present */
	.port_res = &pm8150b_port_res,
};

static const struct of_device_id qcom_pmic_typec_table[] = {
	{ .compatible = "qcom,pm8150b-typec", .data = &pm8150b_typec_res },
	{ .compatible = "qcom,pmi632-typec", .data = &pmi632_typec_res },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pmic_typec_table);

static struct platform_driver qcom_pmic_typec_driver = {
	.driver = {
		.name = "qcom,pmic-typec",
		.of_match_table = qcom_pmic_typec_table,
	},
	.probe = qcom_pmic_typec_probe,
	.remove = qcom_pmic_typec_remove,
};

module_platform_driver(qcom_pmic_typec_driver);

MODULE_DESCRIPTION("QCOM PMIC USB Type-C Port Manager Driver");
MODULE_LICENSE("GPL");
