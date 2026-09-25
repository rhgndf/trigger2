// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
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
	int ret;
	struct trigger2_device *trigger2;
	struct usb_endpoint_descriptor *bulk_out;
	struct drm_device *dev;
	struct device *dma_dev;
	struct usb_device *udev = interface_to_usbdev(interface);
	/* Heuristic: Presence of audio interfaces indicates HDMI. */
	bool is_hdmi = udev->actconfig->desc.bNumInterfaces > 1;

	trigger2 = devm_drm_dev_alloc(&interface->dev, &trigger2_drm_driver,
				      struct trigger2_device, drm);
	if (IS_ERR(trigger2))
		return PTR_ERR(trigger2);

	trigger2->intf = interface;

	ret = usb_find_bulk_out_endpoint(interface->cur_altsetting, &bulk_out);
	if (ret)
		return ret;
	trigger2->bulk_pipe =
		usb_sndbulkpipe(udev, usb_endpoint_num(bulk_out));

	dev = &trigger2->drm;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */
	}

	ret = trigger2_modeset_init(trigger2, is_hdmi);
	if (ret)
		return ret;

	trigger2_transfer_init(trigger2);

	trigger2->transfer_wq = alloc_ordered_workqueue(DRIVER_NAME, 0);
	if (!trigger2->transfer_wq) {
		ret = -ENOMEM;
		return ret;
	}

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, trigger2);

	drm_kms_helper_poll_init(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_poll_fini;

	drm_client_setup(dev, NULL);

	return 0;

err_poll_fini:
	drm_kms_helper_poll_fini(dev);
	usb_set_intfdata(interface, NULL);
	destroy_workqueue(trigger2->transfer_wq);
	return ret;
}

static void trigger2_usb_disconnect(struct usb_interface *interface)
{
	struct trigger2_device *trigger2 = usb_get_intfdata(interface);
	struct drm_device *dev = &trigger2->drm;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	trigger2_stop_io(trigger2);
	destroy_workqueue(trigger2->transfer_wq);
	trigger2_free_bulk_buffer(&trigger2->transfers[0].buf);
	trigger2_free_bulk_buffer(&trigger2->transfers[1].buf);
}

static const struct usb_device_id id_table[] = {
	/* From Windows driver INF file */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5800, 0) }, /* HDMI */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5801, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5802, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5803, 0) },
	{ USB_DEVICE(0x0711, 0x5804) }, /* VGA */
	{ USB_DEVICE(0x0711, 0x5805) },
	{ USB_DEVICE(0x0711, 0x5806) },
	{ USB_DEVICE(0x0711, 0x5807) },
	{ USB_DEVICE(0x0711, 0x5808) },
	{ USB_DEVICE(0x0711, 0x5809) },
	{ USB_DEVICE(0x0711, 0x580A) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x580B, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x580C, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x580D, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x580E, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x580F, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5810, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5811, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5812, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5813, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5814, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5815, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5816, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5817, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5818, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5819, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x581A, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x581B, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x581C, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x581D, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x581E, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x581F, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5820, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5821, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5822, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5823, 0) },
	{ USB_DEVICE(0x0711, 0x5824) },
	{ USB_DEVICE(0x0711, 0x5825) },
	{ USB_DEVICE(0x0711, 0x5826) },
	{ USB_DEVICE(0x0711, 0x5827) },
	{ USB_DEVICE(0x0711, 0x5828) },
	{ USB_DEVICE(0x0711, 0x5829) },
	{ USB_DEVICE(0x0711, 0x582A) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x582B, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x582C, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x582D, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x582E, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x582F, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5830, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5831, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5832, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5833, 0) },
	{ USB_DEVICE(0x0711, 0x5834) },
	{ USB_DEVICE(0x0711, 0x5835) },
	{ USB_DEVICE(0x0711, 0x5836) },
	{ USB_DEVICE(0x0711, 0x5837) },
	{ USB_DEVICE(0x0711, 0x5838) },
	{ USB_DEVICE(0x0711, 0x5839) },
	{ USB_DEVICE(0x0711, 0x583A) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x583B, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x583C, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x583D, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x583E, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x583F, 0) },
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
