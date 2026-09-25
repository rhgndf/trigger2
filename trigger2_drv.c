// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "trigger2.h"

static int trigger2_usb_suspend(struct usb_interface *interface,
				pm_message_t message)
{
	struct trigger2_device *trigger2 = usb_get_intfdata(interface);
	int ret;

	ret = drm_mode_config_helper_suspend(&trigger2->drm);
	if (ret)
		return ret;

	trigger2_stop_io(trigger2);

	return 0;
}

static int trigger2_usb_resume(struct usb_interface *interface)
{
	struct trigger2_device *trigger2 = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(&trigger2->drm);
}

DEFINE_DRM_GEM_FOPS(trigger2_driver_fops);

static const struct drm_driver trigger2_drm_driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,

	/* GEM hooks */
	.fops = &trigger2_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

static int trigger2_usb_probe(struct usb_interface *interface,
			      const struct usb_device_id *id)
{
	static const u8 endpoints[] = { 0x02, 0x03, 0x04, 0x81, 0 };
	struct trigger2_device *trigger2;
	struct drm_device *dev;
	struct device *dma_dev;
	struct usb_device *udev = interface_to_usbdev(interface);
	int ret;

	if (!usb_check_bulk_endpoints(interface, endpoints))
		return -ENODEV;

	trigger2 = devm_drm_dev_alloc(&interface->dev, &trigger2_drm_driver,
				      struct trigger2_device, drm);
	if (IS_ERR(trigger2))
		return PTR_ERR(trigger2);

	trigger2->intf = interface;
	mutex_init(&trigger2->cmd_lock);

	trigger2->bulk_pipe = usb_sndbulkpipe(udev, 2);

	trigger2->cmd_buf = devm_kmalloc(&interface->dev,
					 TRIGGER2_CMD_BUF_LEN, GFP_KERNEL);
	trigger2->reply_buf = devm_kmalloc(&interface->dev,
					   TRIGGER2_REPLY_BUF_LEN, GFP_KERNEL);
	if (!trigger2->cmd_buf || !trigger2->reply_buf)
		return -ENOMEM;

	mutex_lock(&trigger2->cmd_lock);
	ret = trigger2_boot_locked(trigger2);
	mutex_unlock(&trigger2->cmd_lock);
	if (ret)
		return ret;

	dev = &trigger2->drm;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */
	}

	trigger2->transfer_wq =
		devm_alloc_ordered_workqueue(&interface->dev, DRIVER_NAME, 0);
	if (!trigger2->transfer_wq)
		return -ENOMEM;

	ret = trigger2_transfer_init(trigger2);
	if (ret)
		return ret;

	ret = trigger2_modeset_init(trigger2);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	usb_set_intfdata(interface, trigger2);
	drm_kms_helper_poll_init(dev);
	drm_client_setup(dev, NULL);

	return 0;
}

static void trigger2_usb_disconnect(struct usb_interface *interface)
{
	struct trigger2_device *trigger2 = usb_get_intfdata(interface);
	struct drm_device *dev = &trigger2->drm;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5200, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5201, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5202, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5203, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5204, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5205, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5206, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5207, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5208, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5209, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520a, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520b, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520c, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520d, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520e, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520f, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5300, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5301, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5302, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5303, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5304, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5305, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5306, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5307, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5308, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5309, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x530a, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x530b, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x530c, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x530d, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x530e, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x530f, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5400, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5401, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5402, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5403, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5404, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5405, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5406, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5407, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5408, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5409, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x540a, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x540b, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x540c, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x540d, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x540e, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x540f, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5500, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5501, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5502, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5503, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5504, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5505, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5506, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5507, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5508, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5509, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x550a, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x550b, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x550c, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x550d, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x550e, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x550f, 0) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver trigger2_driver = {
	.name = DRIVER_NAME,
	.probe = trigger2_usb_probe,
	.disconnect = trigger2_usb_disconnect,
	.suspend = trigger2_usb_suspend,
	.resume = trigger2_usb_resume,
	.reset_resume = trigger2_usb_resume,
	.id_table = id_table,
};
module_usb_driver(trigger2_driver);
MODULE_AUTHOR("Ho Jie Feng <hjf3108@gmail.com>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
