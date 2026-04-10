// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux hotkey driver for Uniwill notebooks.
 *
 * Special thanks go to Pőcze Barnabás, Christoffer Sandberg and Werner Sembach
 * for supporting the development of this driver either through prior work or
 * by answering questions regarding the underlying WMI interface.
 *
 * Copyright (C) 2025 Armin Wolf <W_Armin@gmx.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "uniwill-wmi.h"

#define DRIVER_NAME		"uniwill-wmi"
#define UNIWILL_EVENT_GUID	"ABBC0F72-8EA1-11D1-00A0-C90629100000"

static BLOCKING_NOTIFIER_HEAD(uniwill_wmi_chain_head);

static void devm_uniwill_wmi_unregister_notifier(void *data)
{
	struct notifier_block *nb = data;

	blocking_notifier_chain_unregister(&uniwill_wmi_chain_head, nb);
}

int devm_uniwill_wmi_register_notifier(struct device *dev, struct notifier_block *nb)
{
	int ret;

	ret = blocking_notifier_chain_register(&uniwill_wmi_chain_head, nb);
	if (ret < 0)
		return ret;

	return devm_add_action_or_reset(dev, devm_uniwill_wmi_unregister_notifier, nb);
}

int uniwill_wmi_evaluate(u8 function, u32 arg)
{
	u8 buf[40] = {};
	struct acpi_buffer input = { sizeof(buf), buf };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	memcpy(&buf[0], &arg, sizeof(arg));
	buf[5] = function;

	status = wmi_evaluate_method(UNIWILL_WMI_MGMT_GUID_BC, 0, 0x04,
				     &input, &output);
	kfree(output.pointer);

	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

/*
 * Write a single byte to the EC RAM using WMI method 0x04.
 *
 * Some EC registers (e.g. the direct fan speed registers at 0x1804/0x1809)
 * are not accessible through the ACPI ECRW method but can be reached
 * through this WMI interface.
 */
int uniwill_wmi_ec_write(u16 addr, u8 data)
{
	u8 buf[40] = {};
	struct acpi_buffer input = { sizeof(buf), buf };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	buf[0] = addr & 0xff;
	buf[1] = (addr >> 8) & 0xff;
	buf[2] = data;
	/* buf[3] = 0 (data_high) */
	/* buf[5] = 0 (function: 0 = write) */

	status = wmi_evaluate_method(UNIWILL_WMI_MGMT_GUID_BC, 0, 0x04,
				     &input, &output);
	obj = output.pointer;

	if (obj && obj->type == ACPI_TYPE_BUFFER && obj->buffer.length >= 4) {
		u32 result;

		memcpy(&result, obj->buffer.pointer, sizeof(result));
		if (result == 0xfefefefe) {
			kfree(obj);
			return -EIO;
		}
	}

	kfree(obj);

	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static void uniwill_wmi_notify(struct wmi_device *wdev, union acpi_object *obj)
{
	u32 value;
	int ret;

	if (obj->type != ACPI_TYPE_INTEGER)
		return;

	value = obj->integer.value;

	dev_dbg(&wdev->dev, "Received WMI event %u\n", value);

	ret = blocking_notifier_call_chain(&uniwill_wmi_chain_head, value, NULL);
	if (notifier_to_errno(ret) < 0)
		dev_err(&wdev->dev, "Failed to handle event %u\n", value);
}

/*
 * We cannot fully trust this GUID since Uniwill just copied the WMI GUID
 * from the Windows driver example, and others probably did the same.
 *
 * Because of this we cannot use this WMI GUID for autoloading. Instead the
 * associated driver will be registered manually after matching a DMI table.
 */
static const struct wmi_device_id uniwill_wmi_id_table[] = {
	{ UNIWILL_EVENT_GUID, NULL },
	{ }
};

static struct wmi_driver uniwill_wmi_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = uniwill_wmi_id_table,
	.notify = uniwill_wmi_notify,
	.no_singleton = true,
};

int __init uniwill_wmi_register_driver(void)
{
	return wmi_driver_register(&uniwill_wmi_driver);
}

void __exit uniwill_wmi_unregister_driver(void)
{
	wmi_driver_unregister(&uniwill_wmi_driver);
}
