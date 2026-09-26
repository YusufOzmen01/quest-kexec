// SPDX-License-Identifier: GPL-2.0
/* Diagnostic console via vendor EP0 reads on the existing USB gadget.
 * No exit function: the hooked callback must remain resident until reboot.
 */
#include <linux/module.h>
#include <linux/console.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/usb/gadget.h>

#define RING_SIZE 65536
#define TRANSFER_SIZE 1024
/* Private UDC layout from this board's drivers/usb/gadget/udc/core.c. */
struct qlog_udc {
	struct usb_gadget_driver *driver;
	struct usb_gadget *gadget;
	struct device dev;
	struct list_head list;
	bool vbus;
};
static unsigned long (*lookup)(const char *);
static int (*original_setup)(struct usb_gadget *, const struct usb_ctrlrequest *);
static struct usb_request *request;
static unsigned char ring[RING_SIZE];
static unsigned int head, tail;
static DEFINE_SPINLOCK(ring_lock);
static atomic_t dropped = ATOMIC_INIT(0);

static void log_write(struct console *con, const char *s, unsigned int count)
{
	unsigned long flags;
	unsigned int i;
	if (!spin_trylock_irqsave(&ring_lock, flags)) {
		atomic_add(count, &dropped);
		return;
	}
	for (i = 0; i < count; i++) {
		if (head - tail == RING_SIZE) {
			tail++;
			atomic_inc(&dropped);
		}
		ring[head++ & (RING_SIZE - 1)] = s[i];
	}
	spin_unlock_irqrestore(&ring_lock, flags);
}
static struct console log_console = {
	.name = "qkxusb", .write = log_write, .flags = CON_ENABLED, .index = -1,
};
static void log_complete(struct usb_ep *ep, struct usb_request *req) { }

static int log_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
	unsigned long flags;
	unsigned int n = 0, size;
	u8 *out = request->buf;
	/* Magic request namespace: vendor/device/IN, request 0x5a,
	 * value 0x514b, index 0x584c. All other requests pass through unchanged.
	 * Reply is LE32 cumulative dropped bytes followed by console text.
	 */
	if (ctrl->bRequestType != 0xc0 || ctrl->bRequest != 0x5a ||
	    le16_to_cpu(ctrl->wValue) != 0x514b ||
	    le16_to_cpu(ctrl->wIndex) != 0x584c)
		return original_setup(gadget, ctrl);
	size = min_t(unsigned int, le16_to_cpu(ctrl->wLength), TRANSFER_SIZE);
	if (size < 4)
		return -EINVAL;
	spin_lock_irqsave(&ring_lock, flags);
	while (head != tail && n < size - 4)
		out[4 + n++] = ring[tail++ & (RING_SIZE - 1)];
	spin_unlock_irqrestore(&ring_lock, flags);
	out[0] = atomic_read(&dropped);
	out[1] = atomic_read(&dropped) >> 8;
	out[2] = atomic_read(&dropped) >> 16;
	out[3] = atomic_read(&dropped) >> 24;
	request->length = n + 4;
	request->zero = request->length < le16_to_cpu(ctrl->wLength);
	return usb_ep_queue(gadget->ep0, request, GFP_ATOMIC);
}
static int match(struct device *dev, const void *name)
{
	return !strcmp(dev_name(dev), name);
}
static int __init log_init(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	struct class **class;
	struct device *dev;
	struct qlog_udc *udc;
	struct device *(*find)(struct class *, struct device *, const void *,
		int (*)(struct device *, const void *));
	void (*register_con)(struct console *);
	struct mutex *udc_lock;
	int ret = register_kprobe(&kp);
	if (ret)
		return ret;
	lookup = (void *)kp.addr;
	unregister_kprobe(&kp);
	class = (void *)lookup("udc_class");
	udc_lock = (void *)lookup("udc_lock");
	find = (void *)lookup("class_find_device");
	register_con = (void *)lookup("register_console");
	if (!class || !*class || !find || !register_con || !udc_lock)
		return -ENOENT;
	dev = find(*class, NULL, "a600000.dwc3", match);
	if (!dev)
		return -ENODEV;
	udc = container_of(dev, struct qlog_udc, dev);
	mutex_lock(udc_lock);
	if (!udc->driver || !udc->gadget || !udc->driver->setup ||
	    udc->gadget->dev.driver != &udc->driver->driver) {
		ret = -EINVAL;
		goto out;
	}
	request = usb_ep_alloc_request(udc->gadget->ep0, GFP_KERNEL);
	if (!request) {
		ret = -ENOMEM;
		goto out;
	}
	request->buf = kmalloc(TRANSFER_SIZE, GFP_KERNEL);
	if (!request->buf) {
		usb_ep_free_request(udc->gadget->ep0, request);
		ret = -ENOMEM;
		goto out;
	}
	request->complete = log_complete;
	original_setup = udc->driver->setup;
	/* Publish fully initialized state before USB IRQs can enter our code. */
	smp_wmb();
	WRITE_ONCE(udc->driver->setup, log_setup);
	mutex_unlock(udc_lock);
	/* Keep the UDC device reference for the lifetime of this resident module. */
	register_con(&log_console);
	pr_info("qkx_usb_log: resident EP0 console ready; ring=%u\n", RING_SIZE);
	return 0;
out:
	mutex_unlock(udc_lock);
	put_device(dev);
	return ret;
}
module_init(log_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Resident Quest kernel console over USB vendor control reads");
