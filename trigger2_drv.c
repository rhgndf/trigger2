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
	struct trigger2_device *trigger2;
	struct drm_device *dev;
	struct device *dma_dev;
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_host_interface *alt = interface->cur_altsetting;
	struct usb_endpoint_descriptor *ep;
	int ret, i;

	trigger2 = devm_drm_dev_alloc(&interface->dev, &trigger2_drm_driver,
				      struct trigger2_device, drm);
	if (IS_ERR(trigger2))
		return PTR_ERR(trigger2);

	trigger2->intf = interface;
	mutex_init(&trigger2->cmd_lock);

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_bulk_out(ep)) {
			switch (ep->bEndpointAddress) {
			case 0x02:
				trigger2->bulk_pipe = usb_sndbulkpipe(udev, 2);
				break;
			case 0x03:
				trigger2->cmd_pipe = usb_sndbulkpipe(udev, 3);
				break;
			case 0x04:
				trigger2->aux_pipe = usb_sndbulkpipe(udev, 4);
				break;
			}
		} else if (usb_endpoint_is_bulk_in(ep) &&
			   ep->bEndpointAddress == 0x81) {
			trigger2->reply_pipe = usb_rcvbulkpipe(udev, 1);
		}
	}
	if (!trigger2->bulk_pipe || !trigger2->cmd_pipe ||
	    !trigger2->aux_pipe || !trigger2->reply_pipe)
		return -ENODEV;

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

	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++) {
		trigger2->transfers[i].header =
			devm_kmalloc(&interface->dev, TRIGGER2_FRAME_HEADER_LEN,
				     GFP_KERNEL);
		if (!trigger2->transfers[i].header)
			return -ENOMEM;
	}

	dev = &trigger2->drm;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */
	}

	ret = trigger2_modeset_init(trigger2);
	if (ret)
		return ret;

	ret = trigger2_transfer_init(trigger2);
	if (ret)
		return ret;

	trigger2->transfer_wq = alloc_ordered_workqueue(DRIVER_NAME, 0);
	if (!trigger2->transfer_wq) {
		ret = -ENOMEM;
		goto err_transfer_fini;
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
err_transfer_fini:
	trigger2_transfer_fini(trigger2);
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
	trigger2_transfer_fini(trigger2);
	trigger2_free_bulk_buffer(&trigger2->transfers[0].buf);
	trigger2_free_bulk_buffer(&trigger2->transfers[1].buf);
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5200, 0) },
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
