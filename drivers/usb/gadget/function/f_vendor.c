// SPDX-License-Identifier: GPL-2.0+


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/usb/composite.h>
#include <linux/configfs.h>

/*-------------------------------------------------------------------------*/

/* Interface descriptor */
static struct usb_interface_descriptor vendor_intf_desc = {
	.bLength =		sizeof(vendor_intf_desc),
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,  /* Dynamic */
	.bAlternateSetting =	0,
	.bNumEndpoints =	0,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,  /* 0xFF */
	.bInterfaceSubClass =	0xFF,
	.bInterfaceProtocol =	0xFF,
	.iInterface =		0,
};

static struct usb_descriptor_header *vendor_fs_function[] = {
	(struct usb_descriptor_header *) &vendor_intf_desc,
	NULL,
};

static struct usb_descriptor_header *vendor_hs_function[] = {
	(struct usb_descriptor_header *) &vendor_intf_desc,
	NULL,
};

static struct usb_descriptor_header *vendor_ss_function[] = {
	(struct usb_descriptor_header *) &vendor_intf_desc,
	NULL,
};

/*-------------------------------------------------------------------------*/

struct f_vendor {
	struct usb_function	function;
};

static inline struct f_vendor *func_to_vendor(struct usb_function *f)
{
	return container_of(f, struct f_vendor, function);
}

/*-------------------------------------------------------------------------*/

static int vendor_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	/* XingCore: No endpoints, nothing to configure */
	return 0;
}

static void vendor_disable(struct usb_function *f)
{
	/* XingCore: No endpoints, nothing to disable */
}

static int vendor_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct f_vendor *vendor = func_to_vendor(f);
	int status;

	/* Allocate interface ID */
	status = usb_interface_id(c, f);
	if (status < 0)
		return status;
	vendor_intf_desc.bInterfaceNumber = status;

	/* XingCore: No endpoints to allocate */

	/* Support all speeds */
	usb_assign_descriptors(f, vendor_fs_function, vendor_hs_function,
			       vendor_ss_function, NULL);

	dev_info(&cdev->gadget->dev, "XingCore vendor interface ready\n");
	return 0;
}

static void vendor_free_func(struct usb_function *f)
{
	struct f_vendor *vendor = func_to_vendor(f);
	kfree(vendor);
}

static void vendor_unbind(struct usb_configuration *c, struct usb_function *f)
{
	usb_free_all_descriptors(f);
}

/*-------------------------------------------------------------------------*/

static struct usb_function *vendor_alloc(struct usb_function_instance *fi)
{
	struct f_vendor *vendor;

	vendor = kzalloc(sizeof(*vendor), GFP_KERNEL);
	if (!vendor)
		return ERR_PTR(-ENOMEM);

	vendor->function.name = "vendor";
	vendor->function.bind = vendor_bind;
	vendor->function.unbind = vendor_unbind;
	vendor->function.set_alt = vendor_set_alt;
	vendor->function.disable = vendor_disable;
	vendor->function.free_func = vendor_free_func;

	return &vendor->function;
}

static void vendor_free_instance(struct usb_function_instance *fi)
{
	kfree(fi);
}

/* ConfigFS support */
static const struct config_item_type vendor_func_type = {
	.ct_owner	= THIS_MODULE,
};

static struct usb_function_instance *vendor_alloc_inst(void)
{
	struct usb_function_instance *fi;

	fi = kzalloc(sizeof(*fi), GFP_KERNEL);
	if (!fi)
		return ERR_PTR(-ENOMEM);

	fi->free_func_inst = vendor_free_instance;

	/* ConfigFS registration - critical for function availability */
	config_group_init_type_name(&fi->group, "", &vendor_func_type);

	return fi;
}

DECLARE_USB_FUNCTION_INIT(vendor, vendor_alloc_inst, vendor_alloc);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("XingCore Emulation");
MODULE_DESCRIPTION("Vendor-specific USB function for Configuration 2");
