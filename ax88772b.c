// SPDX-License-Identifier: Apache-2.0
//
// Minimal AX88772B-style Raw Gadget example adapted from pl2303.c.
// Purpose:
//   - Enumerate as ASIX AX88772B (0b95:772b)
//   - Implement Linux-derived vendor control requests
//   - Log unknown host/DUT requests
//   - Log bulk OUT traffic from host
//
// First-pass emulator only:
//   - bulk IN is currently idle
//   - interrupt IN is currently idle
//   - descriptors are plausible, but may need to be matched more closely to a real device
//
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdatomic.h>

#include <linux/types.h>
#include <linux/usb/ch9.h>

/*----------------------------------------------------------------------*/

#define UDC_NAME_LENGTH_MAX 128

struct usb_raw_init {
	__u8 driver_name[UDC_NAME_LENGTH_MAX];
	__u8 device_name[UDC_NAME_LENGTH_MAX];
	__u8 speed;
};

enum usb_raw_event_type {
	USB_RAW_EVENT_INVALID = 0,
	USB_RAW_EVENT_CONNECT = 1,
	USB_RAW_EVENT_CONTROL = 2,
	USB_RAW_EVENT_SUSPEND = 3,
	USB_RAW_EVENT_RESUME = 4,
	USB_RAW_EVENT_RESET = 5,
	USB_RAW_EVENT_DISCONNECT = 6,
};

struct usb_raw_event {
	__u32 type;
	__u32 length;
	__u8 data[];
};

struct usb_raw_ep_io {
	__u16 ep;
	__u16 flags;
	__u32 length;
	__u8 data[];
};

#define USB_RAW_EPS_NUM_MAX 30
#define USB_RAW_EP_NAME_MAX 16
#define USB_RAW_EP_ADDR_ANY 0xff

struct usb_raw_ep_caps {
	__u32 type_control : 1;
	__u32 type_iso     : 1;
	__u32 type_bulk    : 1;
	__u32 type_int     : 1;
	__u32 dir_in       : 1;
	__u32 dir_out      : 1;
};

struct usb_raw_ep_limits {
	__u16 maxpacket_limit;
	__u16 max_streams;
	__u32 reserved;
};

struct usb_raw_ep_info {
	__u8 name[USB_RAW_EP_NAME_MAX];
	__u32 addr;
	struct usb_raw_ep_caps caps;
	struct usb_raw_ep_limits limits;
};

struct usb_raw_eps_info {
	struct usb_raw_ep_info eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT           _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN            _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH    _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE      _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ       _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE      _IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE     _IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE       _IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ        _IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE      _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW      _IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EPS_INFO       _IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL      _IO('U', 12)
#define USB_RAW_IOCTL_EP_SET_HALT    _IOW('U', 13, __u32)
#define USB_RAW_IOCTL_EP_CLEAR_HALT  _IOW('U', 14, __u32)
#define USB_RAW_IOCTL_EP_SET_WEDGE   _IOW('U', 15, __u32)

/*----------------------------------------------------------------------*/

int usb_raw_open(void) {
	int fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		perror("open()");
		exit(EXIT_FAILURE);
	}
	return fd;
}

void usb_raw_init(int fd, enum usb_device_speed speed,
		  const char *driver, const char *device) {
	struct usb_raw_init arg;
	memset(&arg, 0, sizeof(arg));
	strcpy((char *)&arg.driver_name[0], driver);
	strcpy((char *)&arg.device_name[0], device);
	arg.speed = speed;
	int rv = ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_INIT)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_run(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_RUN, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_RUN)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_event_fetch(int fd, struct usb_raw_event *event) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
		exit(EXIT_FAILURE);
	}
}

int usb_raw_ep0_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep0_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor *desc) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_ENABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_disable(int fd, int ep) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, ep);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_DISABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_write_may_fail(int fd, struct usb_raw_ep_io *io) {
	return ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
}

int usb_raw_ep_read_may_fail(int fd, struct usb_raw_ep_io *io) {
	return ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
}

void usb_raw_configure(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_CONFIGURE)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_vbus_draw(int fd, uint32_t power) {
	int rv = ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_VBUS_DRAW)");
		exit(EXIT_FAILURE);
	}
}

int usb_raw_eps_info(int fd, struct usb_raw_eps_info *info) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EPS_INFO, info);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EPS_INFO)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

void usb_raw_ep0_stall(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_STALL)");
		exit(EXIT_FAILURE);
	}
}

/*----------------------------------------------------------------------*/

static void hex_dump(const char *prefix, const unsigned char *data, int len) {
	printf("%s", prefix);
	for (int i = 0; i < len; i++)
		printf(" %02x", data[i]);
	printf("\n");
}

void log_control_request(struct usb_ctrlrequest *ctrl) {
	printf("  bRequestType: 0x%x (%s), bRequest: 0x%x, wValue: 0x%x, wIndex: 0x%x, wLength: %d\n",
	       ctrl->bRequestType,
	       (ctrl->bRequestType & USB_DIR_IN) ? "IN" : "OUT",
	       ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		printf("  type = USB_TYPE_STANDARD\n");
		break;
	case USB_TYPE_CLASS:
		printf("  type = USB_TYPE_CLASS\n");
		break;
	case USB_TYPE_VENDOR:
		printf("  type = USB_TYPE_VENDOR\n");
		break;
	default:
		printf("  type = unknown\n");
		break;
	}
}

void log_event(struct usb_raw_event *event) {
	switch (event->type) {
	case USB_RAW_EVENT_CONNECT:
		printf("event: connect, length: %u\n", event->length);
		break;
	case USB_RAW_EVENT_CONTROL:
		printf("event: control, length: %u\n", event->length);
		log_control_request((struct usb_ctrlrequest *)&event->data[0]);
		break;
	case USB_RAW_EVENT_SUSPEND:
		printf("event: suspend\n");
		break;
	case USB_RAW_EVENT_RESUME:
		printf("event: resume\n");
		break;
	case USB_RAW_EVENT_RESET:
		printf("event: reset\n");
		break;
	case USB_RAW_EVENT_DISCONNECT:
		printf("event: disconnect\n");
		break;
	default:
		printf("event: %d (unknown), length: %u\n", event->type, event->length);
	}
}

/*----------------------------------------------------------------------*/
/* AX88772B derived command set */

#define BCD_USB 0x0200
#define USB_VENDOR  0x0b95
#define USB_PRODUCT 0x772b

#define STRING_ID_MANUFACTURER 1
#define STRING_ID_PRODUCT      2
#define STRING_ID_SERIAL       3
#define STRING_ID_CONFIG       4
#define STRING_ID_INTERFACE    5

#define EP_MAX_PACKET_CONTROL 64
#define EP_MAX_PACKET_BULK    64
#define EP_MAX_PACKET_INT     8

#define EP_NUM_BULK_OUT 0x0
#define EP_NUM_BULK_IN  0x0
#define EP_NUM_INT_IN   0x0

#define AX_CMD_SET_SW_MII         0x06
#define AX_CMD_READ_MII_REG       0x07
#define AX_CMD_WRITE_MII_REG      0x08
#define AX_CMD_STATMNGSTS_REG     0x09
#define AX_CMD_SET_HW_MII         0x0a
#define AX_CMD_READ_EEPROM        0x0b
#define AX_CMD_READ_RX_CTL        0x0f
#define AX_CMD_WRITE_RX_CTL       0x10
#define AX_CMD_READ_NODE_ID       0x13
#define AX_CMD_WRITE_NODE_ID      0x14
#define AX_CMD_READ_PHY_ID        0x19
#define AX_CMD_READ_MEDIUM_STATUS 0x1a
#define AX_CMD_WRITE_MEDIUM_MODE  0x1b
#define AX_CMD_READ_GPIOS         0x1e
#define AX_CMD_WRITE_GPIOS        0x1f
#define AX_CMD_SW_RESET           0x20
#define AX_CMD_SW_PHY_SELECT      0x22
#define AX_QCTCTRL                0x2a

#define AX_AX88772B_CHIPCODE      0x20
#define AX_EMBD_PHY_ADDR          0x10

#define AX_RX_CTL_SO              0x0080
#define AX_RX_CTL_AB              0x0008
#define AX_DEFAULT_RX_CTL         (AX_RX_CTL_SO | AX_RX_CTL_AB)

#define AX_MEDIUM_FD              0x0002
#define AX_MEDIUM_AC              0x0004
#define AX_MEDIUM_RE              0x0100
#define AX_MEDIUM_PS              0x0200
#define AX88772_MEDIUM_DEFAULT    (AX_MEDIUM_FD | AX_MEDIUM_PS | AX_MEDIUM_AC | AX_MEDIUM_RE)

struct ax88772b_state {
	__u8 mac[6];
	__u16 rx_ctl;
	__u16 medium_mode;
	__u8 gpio;
	__u8 sw_reset;
	__u8 phy_select;
	__u8 chipcode;
	__u8 phy_id[2];
	bool sw_mii_mode;
	__u16 mii_regs[32];
	__u16 eeprom[256];
};

static struct ax88772b_state ax_state;

static void ax88772b_init_state(void) {
	memset(&ax_state, 0, sizeof(ax_state));

	ax_state.mac[0] = 0x02;
	ax_state.mac[1] = 0x42;
	ax_state.mac[2] = 0xac;
	ax_state.mac[3] = 0x11;
	ax_state.mac[4] = 0x00;
	ax_state.mac[5] = 0x72;

	ax_state.rx_ctl = AX_DEFAULT_RX_CTL;
	ax_state.medium_mode = AX88772_MEDIUM_DEFAULT;
	ax_state.gpio = 0x00;
	ax_state.sw_reset = 0x00;
	ax_state.phy_select = AX_EMBD_PHY_ADDR;
	ax_state.chipcode = AX_AX88772B_CHIPCODE;
	ax_state.phy_id[0] = 0x00;
	ax_state.phy_id[1] = AX_EMBD_PHY_ADDR;
	ax_state.sw_mii_mode = false;

	ax_state.eeprom[0x04] = (__u16)ax_state.mac[0] | ((__u16)ax_state.mac[1] << 8);
	ax_state.eeprom[0x05] = (__u16)ax_state.mac[2] | ((__u16)ax_state.mac[3] << 8);
	ax_state.eeprom[0x06] = (__u16)ax_state.mac[4] | ((__u16)ax_state.mac[5] << 8);

	ax_state.mii_regs[0x00] = 0x1000; /* BMCR */
	ax_state.mii_regs[0x01] = 0x786d; /* BMSR */
	ax_state.mii_regs[0x02] = 0x003b; /* PHYSID1 placeholder */
	ax_state.mii_regs[0x03] = 0x1861; /* PHYSID2 placeholder */
	ax_state.mii_regs[0x04] = 0x01e1; /* ANAR */
	ax_state.mii_regs[0x05] = 0x0000; /* ANLPAR */

	printf("AX88772B init state: MAC %02x:%02x:%02x:%02x:%02x:%02x, rx_ctl=0x%04x, medium=0x%04x\n",
	       ax_state.mac[0], ax_state.mac[1], ax_state.mac[2],
	       ax_state.mac[3], ax_state.mac[4], ax_state.mac[5],
	       ax_state.rx_ctl, ax_state.medium_mode);
}

/*----------------------------------------------------------------------*/

struct usb_device_descriptor usb_device = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = __constant_cpu_to_le16(BCD_USB),
	.bDeviceClass = USB_CLASS_VENDOR_SPEC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = EP_MAX_PACKET_CONTROL,
	.idVendor = __constant_cpu_to_le16(USB_VENDOR),
	.idProduct = __constant_cpu_to_le16(USB_PRODUCT),
	.bcdDevice = __constant_cpu_to_le16(0x0001),
	.iManufacturer = STRING_ID_MANUFACTURER,
	.iProduct = STRING_ID_PRODUCT,
	.iSerialNumber = STRING_ID_SERIAL,
	.bNumConfigurations = 1,
};

struct usb_qualifier_descriptor usb_qualifier = {
	.bLength = sizeof(struct usb_qualifier_descriptor),
	.bDescriptorType = USB_DT_DEVICE_QUALIFIER,
	.bcdUSB = __constant_cpu_to_le16(BCD_USB),
	.bDeviceClass = USB_CLASS_VENDOR_SPEC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = EP_MAX_PACKET_CONTROL,
	.bNumConfigurations = 1,
	.bRESERVED = 0,
};

struct usb_config_descriptor usb_config = {
	.bLength = USB_DT_CONFIG_SIZE,
	.bDescriptorType = USB_DT_CONFIG,
	.wTotalLength = 0,
	.bNumInterfaces = 1,
	.bConfigurationValue = 1,
	.iConfiguration = STRING_ID_CONFIG,
	.bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower = 0x32,
};

struct usb_interface_descriptor usb_interface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 3,
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass = 0xff,
	.bInterfaceProtocol = 0x00,
	.iInterface = STRING_ID_INTERFACE,
};

struct usb_endpoint_descriptor usb_ep_bulk_out = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = EP_NUM_BULK_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(EP_MAX_PACKET_BULK),
	.bInterval = 0,
};

struct usb_endpoint_descriptor usb_ep_bulk_in = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN | EP_NUM_BULK_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(EP_MAX_PACKET_BULK),
	.bInterval = 0,
};

struct usb_endpoint_descriptor usb_ep_int_in = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN | EP_NUM_INT_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = __constant_cpu_to_le16(EP_MAX_PACKET_INT),
	.bInterval = 10,
};

static const char *str_manufacturer = "ASIX";
static const char *str_product = "AX88772B";
static const char *str_serial = "000772B";
static const char *str_config = "Default";
static const char *str_interface = "Ethernet";
static bool use_int_ep = false;
static bool force_disable_int_ep = false;

/*----------------------------------------------------------------------*/

static int make_string_desc(__u8 *buf, size_t buf_sz, const char *s) {
	size_t len = strlen(s);
	size_t total = 2 + len * 2;
	if (buf_sz < total)
		return -1;
	buf[0] = (__u8)total;
	buf[1] = USB_DT_STRING;
	for (size_t i = 0; i < len; i++) {
		buf[2 + i * 2] = (__u8)s[i];
		buf[3 + i * 2] = 0x00;
	}
	return (int)total;
}

int build_config(char *data, int length, bool other_speed) {
	struct usb_config_descriptor *config = (struct usb_config_descriptor *)data;
	int total_length = 0;
	struct usb_interface_descriptor iface = usb_interface;

	iface.bNumEndpoints = use_int_ep ? 3 : 2;

	assert(length >= (int)sizeof(usb_config));
	memcpy(data, &usb_config, sizeof(usb_config));
	data += sizeof(usb_config);
	length -= sizeof(usb_config);
	total_length += sizeof(usb_config);

	assert(length >= (int)sizeof(iface));
	memcpy(data, &iface, sizeof(iface));
	data += sizeof(iface);
	length -= sizeof(iface);
	total_length += sizeof(iface);

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_ep_bulk_out, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	length -= USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_ep_bulk_in, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	length -= USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	if (use_int_ep) {
		assert(length >= USB_DT_ENDPOINT_SIZE);
		memcpy(data, &usb_ep_int_in, USB_DT_ENDPOINT_SIZE);
		data += USB_DT_ENDPOINT_SIZE;
		length -= USB_DT_ENDPOINT_SIZE;
		total_length += USB_DT_ENDPOINT_SIZE;
	}

	config->wTotalLength = __cpu_to_le16(total_length);
	printf("config->wTotalLength: %d\n", total_length);
	if (other_speed)
		config->bDescriptorType = USB_DT_OTHER_SPEED_CONFIG;
	return total_length;
}

/*----------------------------------------------------------------------*/

static bool endpoint_address_conflicts(__u8 addr, const struct usb_endpoint_descriptor *ep) {
	if (&usb_ep_bulk_out != ep && usb_ep_bulk_out.bEndpointAddress == addr)
		return true;
	if (&usb_ep_bulk_in != ep && usb_ep_bulk_in.bEndpointAddress == addr)
		return true;
	if (&usb_ep_int_in != ep && usb_ep_int_in.bEndpointAddress == addr)
		return true;
	return false;
}

bool assign_ep_address(struct usb_raw_ep_info *info, struct usb_endpoint_descriptor *ep) {
	__u8 candidate_addr;

	if (usb_endpoint_num(ep) != 0)
		return false;
	if (usb_endpoint_dir_in(ep) && !info->caps.dir_in)
		return false;
	if (usb_endpoint_dir_out(ep) && !info->caps.dir_out)
		return false;
	if (usb_endpoint_maxp(ep) > info->limits.maxpacket_limit)
		return false;
	switch (usb_endpoint_type(ep)) {
	case USB_ENDPOINT_XFER_BULK:
		if (!info->caps.type_bulk)
			return false;
		break;
	case USB_ENDPOINT_XFER_INT:
		if (!info->caps.type_int)
			return false;
		break;
	default:
		assert(false);
	}

	if (info->addr == USB_RAW_EP_ADDR_ANY) {
		static int next_in_addr = 1;
		static int next_out_addr = 1;
		if (usb_endpoint_dir_in(ep))
			candidate_addr = (__u8)(USB_DIR_IN | next_in_addr++);
		else
			candidate_addr = (__u8)next_out_addr++;
	} else {
		candidate_addr = (__u8)info->addr;
		if (usb_endpoint_dir_in(ep))
			candidate_addr |= USB_DIR_IN;
	}

	if (endpoint_address_conflicts(candidate_addr, ep))
		return false;

	ep->bEndpointAddress = candidate_addr;
	return true;
}

void process_eps_info(int fd) {
	struct usb_raw_eps_info info;
	memset(&info, 0, sizeof(info));
	int num = usb_raw_eps_info(fd, &info);
	for (int i = 0; i < num; i++) {
		printf("ep #%d:\n", i);
		printf("  name: %s\n", &info.eps[i].name[0]);
		printf("  addr: %u\n", info.eps[i].addr);
		printf("  type: %s %s %s\n",
		       info.eps[i].caps.type_iso ? "iso" : "___",
		       info.eps[i].caps.type_bulk ? "blk" : "___",
		       info.eps[i].caps.type_int ? "int" : "___");
		printf("  dir : %s %s\n",
		       info.eps[i].caps.dir_in ? "in " : "___",
		       info.eps[i].caps.dir_out ? "out" : "___");
		printf("  maxpacket_limit: %u\n", info.eps[i].limits.maxpacket_limit);
		printf("  max_streams: %u\n", info.eps[i].limits.max_streams);
	}

	for (int i = 0; i < num; i++)
		assign_ep_address(&info.eps[i], &usb_ep_bulk_out);

	use_int_ep = false;
	if (!force_disable_int_ep) {
		for (int i = 0; i < num; i++) {
			if (assign_ep_address(&info.eps[i], &usb_ep_int_in)) {
				use_int_ep = true;
				break;
			}
		}
	}

	for (int i = 0; i < num; i++)
		assign_ep_address(&info.eps[i], &usb_ep_bulk_in);

	assert(usb_endpoint_num(&usb_ep_bulk_out) != 0);
	assert(usb_endpoint_num(&usb_ep_bulk_in) != 0);
	printf("ep_bulk_out: addr = 0x%02x (num=%u, OUT)\n",
	       usb_ep_bulk_out.bEndpointAddress,
	       usb_endpoint_num(&usb_ep_bulk_out));
	printf("ep_bulk_in : addr = 0x%02x (num=%u, IN)\n",
	       usb_ep_bulk_in.bEndpointAddress,
	       usb_endpoint_num(&usb_ep_bulk_in));
	if (use_int_ep)
		printf("ep_int_in  : addr = 0x%02x (num=%u, IN)\n",
		       usb_ep_int_in.bEndpointAddress,
		       usb_endpoint_num(&usb_ep_int_in));
	else
		printf("ep_int_in  : not available on this UDC, continuing without it\n");
}

/*----------------------------------------------------------------------*/

#define EP0_MAX_DATA 256

struct usb_raw_control_event {
	struct usb_raw_event inner;
	struct usb_ctrlrequest ctrl;
};

struct usb_raw_control_io {
	struct usb_raw_ep_io inner;
	char data[EP0_MAX_DATA];
};

struct usb_raw_bulk_io {
	struct usb_raw_ep_io inner;
	char data[2048];
};

struct usb_raw_int_io {
	struct usb_raw_ep_io inner;
	char data[EP_MAX_PACKET_INT];
};

int ep_bulk_out = -1;
int ep_bulk_in = -1;
int ep_int_in = -1;

pthread_t ep_bulk_out_thread;
pthread_t ep_bulk_in_thread;
pthread_t ep_int_in_thread;

bool ep_bulk_out_thread_spawned = false;
bool ep_bulk_in_thread_spawned = false;
bool ep_int_in_thread_spawned = false;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool gadget_online = false;

/*----------------------------------------------------------------------*/

void *ep_bulk_out_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;
	memset(&io, 0, sizeof(io));

	while (true) {
		int local_ep;

		pthread_mutex_lock(&state_lock);
		local_ep = ep_bulk_out;
		pthread_mutex_unlock(&state_lock);

		if (!atomic_load(&gadget_online) || local_ep < 0) {
			usleep(10000);
			continue;
		}

		io.inner.ep = local_ep;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		int rv = usb_raw_ep_read_may_fail(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0 && errno == ESHUTDOWN) {
			printf("ep_bulk_out: device reset/shutdown, exiting\n");
			break;
		} else if (rv < 0) {
			perror("usb_raw_ep_read_may_fail()");
			exit(EXIT_FAILURE);
		}

		printf("ep_bulk_out: received %d bytes from host\n", rv);
		if (rv > 0)
			hex_dump("ep_bulk_out payload:", (unsigned char *)io.inner.data, rv);
		fflush(stdout);
	}
	return NULL;
}

void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;
	(void)fd;

	while (true) {
		int local_ep;

		pthread_mutex_lock(&state_lock);
		local_ep = ep_bulk_in;
		pthread_mutex_unlock(&state_lock);

		if (!atomic_load(&gadget_online) || local_ep < 0) {
			usleep(10000);
			continue;
		}

		/* Idle for now. Later this can read from TAP/backend and push frames to host. */
		usleep(100000);
	}
	return NULL;
}

void *ep_int_in_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_int_io io;
	memset(&io, 0, sizeof(io));
	io.inner.flags = 0;
	io.inner.length = EP_MAX_PACKET_INT;

	while (true) {
		int local_ep;

		pthread_mutex_lock(&state_lock);
		local_ep = ep_int_in;
		pthread_mutex_unlock(&state_lock);

		if (!atomic_load(&gadget_online) || local_ep < 0) {
			usleep(10000);
			continue;
		}

		memset(io.data, 0, sizeof(io.data));
		io.inner.ep = local_ep;

		int rv = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0 && errno == ESHUTDOWN) {
			printf("ep_int_in: device reset/shutdown, exiting\n");
			break;
		} else if (rv < 0) {
			usleep(250000);
			continue;
		}

		usleep(250000);
	}
	return NULL;
}

/*----------------------------------------------------------------------*/

static void store_mac_to_eeprom_words(void) {
	ax_state.eeprom[0x04] = (__u16)ax_state.mac[0] | ((__u16)ax_state.mac[1] << 8);
	ax_state.eeprom[0x05] = (__u16)ax_state.mac[2] | ((__u16)ax_state.mac[3] << 8);
	ax_state.eeprom[0x06] = (__u16)ax_state.mac[4] | ((__u16)ax_state.mac[5] << 8);
}

bool handle_vendor_in_request(struct usb_ctrlrequest *ctrl, struct usb_raw_control_io *io) {
	memset(io->data, 0, sizeof(io->data));
	io->inner.length = 0;

	switch (ctrl->bRequest) {
	case AX_CMD_READ_MII_REG: {
		__u8 phy = ctrl->wValue & 0x1f;
		__u8 reg = ctrl->wIndex & 0x1f;
		__u16 val = (phy == AX_EMBD_PHY_ADDR) ? ax_state.mii_regs[reg] : 0xffff;
		io->data[0] = val & 0xff;
		io->data[1] = (val >> 8) & 0xff;
		io->inner.length = 2;
		printf("vendor IN: READ_MII_REG phy=0x%02x reg=0x%02x => 0x%04x\n", phy, reg, val);
		return true;
	}

	case AX_CMD_STATMNGSTS_REG:
		io->data[0] = ax_state.chipcode;
		io->inner.length = 1;
		printf("vendor IN: STATMNGSTS_REG => 0x%02x\n", io->data[0]);
		return true;

	case AX_CMD_READ_EEPROM: {
		__u8 word = ctrl->wValue & 0xff;
		__u16 val = ax_state.eeprom[word];
		io->data[0] = val & 0xff;
		io->data[1] = (val >> 8) & 0xff;
		io->inner.length = 2;
		printf("vendor IN: READ_EEPROM word=0x%02x => 0x%04x\n", word, val);
		return true;
	}

	case AX_CMD_READ_RX_CTL:
		io->data[0] = ax_state.rx_ctl & 0xff;
		io->data[1] = (ax_state.rx_ctl >> 8) & 0xff;
		io->inner.length = 2;
		printf("vendor IN: READ_RX_CTL => 0x%04x\n", ax_state.rx_ctl);
		return true;

	case AX_CMD_READ_NODE_ID:
		memcpy(&io->data[0], ax_state.mac, 6);
		io->inner.length = 6;
		printf("vendor IN: READ_NODE_ID\n");
		hex_dump("  mac:", ax_state.mac, 6);
		return true;

	case AX_CMD_READ_PHY_ID:
		io->data[0] = ax_state.phy_id[0];
		io->data[1] = ax_state.phy_id[1];
		io->inner.length = 2;
		printf("vendor IN: READ_PHY_ID => %02x %02x\n", io->data[0], io->data[1]);
		return true;

	case AX_CMD_READ_MEDIUM_STATUS:
		io->data[0] = ax_state.medium_mode & 0xff;
		io->data[1] = (ax_state.medium_mode >> 8) & 0xff;
		io->inner.length = 2;
		printf("vendor IN: READ_MEDIUM_STATUS => 0x%04x\n", ax_state.medium_mode);
		return true;

	case AX_CMD_READ_GPIOS:
		io->data[0] = ax_state.gpio;
		io->inner.length = 1;
		printf("vendor IN: READ_GPIOS => 0x%02x\n", ax_state.gpio);
		return true;

	default:
		printf("vendor IN UNKNOWN: req=0x%02x len=%u val=0x%04x idx=0x%04x\n",
		       ctrl->bRequest, ctrl->wLength, ctrl->wValue, ctrl->wIndex);
		return false;
	}
}

bool handle_vendor_out_request(struct usb_ctrlrequest *ctrl, struct usb_raw_control_io *io) {
	switch (ctrl->bRequest) {
	case AX_CMD_SET_SW_MII:
		ax_state.sw_mii_mode = true;
		io->inner.length = 0;
		printf("vendor OUT: SET_SW_MII\n");
		return true;

	case AX_CMD_SET_HW_MII:
		ax_state.sw_mii_mode = false;
		io->inner.length = 0;
		printf("vendor OUT: SET_HW_MII\n");
		return true;

	case AX_CMD_WRITE_MII_REG:
		io->inner.length = ctrl->wLength;
		if (io->inner.length > sizeof(io->data))
			io->inner.length = sizeof(io->data);
		printf("vendor OUT: WRITE_MII_REG phy=0x%02x reg=0x%02x len=%u\n",
		       ctrl->wValue & 0x1f, ctrl->wIndex & 0x1f, ctrl->wLength);
		return true;

	case AX_CMD_WRITE_RX_CTL:
		ax_state.rx_ctl = ctrl->wValue;
		io->inner.length = 0;
		printf("vendor OUT: WRITE_RX_CTL <= 0x%04x\n", ax_state.rx_ctl);
		return true;

	case AX_CMD_WRITE_NODE_ID:
		io->inner.length = ctrl->wLength;
		if (io->inner.length > sizeof(io->data))
			io->inner.length = sizeof(io->data);
		printf("vendor OUT: WRITE_NODE_ID len=%u\n", ctrl->wLength);
		return true;

	case AX_CMD_WRITE_MEDIUM_MODE:
		ax_state.medium_mode = ctrl->wValue;
		io->inner.length = 0;
		printf("vendor OUT: WRITE_MEDIUM_MODE <= 0x%04x\n", ax_state.medium_mode);
		return true;

	case AX_CMD_WRITE_GPIOS:
		ax_state.gpio = ctrl->wValue & 0xff;
		io->inner.length = 0;
		printf("vendor OUT: WRITE_GPIOS <= 0x%02x (index=0x%04x)\n",
		       ax_state.gpio, ctrl->wIndex);
		return true;

	case AX_CMD_SW_RESET:
		ax_state.sw_reset = ctrl->wValue & 0xff;
		io->inner.length = 0;
		printf("vendor OUT: SW_RESET <= 0x%02x\n", ax_state.sw_reset);
		return true;

	case AX_CMD_SW_PHY_SELECT:
		ax_state.phy_select = ctrl->wValue & 0xff;
		io->inner.length = 0;
		printf("vendor OUT: SW_PHY_SELECT <= 0x%02x\n", ax_state.phy_select);
		return true;

	case AX_QCTCTRL:
		io->inner.length = 0;
		printf("vendor OUT: QCTCTRL value=0x%04x idx=0x%04x\n",
		       ctrl->wValue, ctrl->wIndex);
		return true;

	default:
		io->inner.length = ctrl->wLength;
		if (io->inner.length > sizeof(io->data))
			io->inner.length = sizeof(io->data);
		printf("vendor OUT UNKNOWN: req=0x%02x len=%u val=0x%04x idx=0x%04x\n",
		       ctrl->bRequest, ctrl->wLength, ctrl->wValue, ctrl->wIndex);
		return false;
	}
}

static void process_vendor_out_payload(struct usb_ctrlrequest *ctrl,
				       struct usb_raw_control_io *io,
				       int rv) {
	if (rv <= 0)
		return;

	switch (ctrl->bRequest) {
	case AX_CMD_WRITE_MII_REG: {
		if (rv >= 2) {
			__u8 phy = ctrl->wValue & 0x1f;
			__u8 reg = ctrl->wIndex & 0x1f;
			__u16 val = (__u8)io->inner.data[0] | ((__u8)io->inner.data[1] << 8);
			if (phy == AX_EMBD_PHY_ADDR)
				ax_state.mii_regs[reg] = val;
			printf("vendor OUT payload applied: WRITE_MII_REG phy=0x%02x reg=0x%02x <= 0x%04x\n",
			       phy, reg, val);
		}
		break;
	}

	case AX_CMD_WRITE_NODE_ID:
		if (rv >= 6) {
			memcpy(ax_state.mac, io->inner.data, 6);
			store_mac_to_eeprom_words();
			printf("vendor OUT payload applied: WRITE_NODE_ID\n");
			hex_dump("  new mac:", ax_state.mac, 6);
		}
		break;

	default:
		break;
	}
}

/*----------------------------------------------------------------------*/

bool ep0_request(int fd, struct usb_raw_control_event *event, struct usb_raw_control_io *io) {
	memset(io->data, 0, sizeof(io->data));

	switch (event->ctrl.bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		switch (event->ctrl.bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			switch (event->ctrl.wValue >> 8) {
			case USB_DT_DEVICE:
				memcpy(&io->data[0], &usb_device, sizeof(usb_device));
				io->inner.length = sizeof(usb_device);
				return true;
			case USB_DT_DEVICE_QUALIFIER:
				memcpy(&io->data[0], &usb_qualifier, sizeof(usb_qualifier));
				io->inner.length = sizeof(usb_qualifier);
				return true;
			case USB_DT_CONFIG:
				io->inner.length = build_config(&io->data[0], sizeof(io->data), false);
				return true;
			case USB_DT_OTHER_SPEED_CONFIG:
				io->inner.length = build_config(&io->data[0], sizeof(io->data), true);
				return true;
			case USB_DT_STRING: {
				int idx = event->ctrl.wValue & 0xff;
				if (idx == 0) {
					io->data[0] = 4;
					io->data[1] = USB_DT_STRING;
					io->data[2] = 0x09;
					io->data[3] = 0x04;
					io->inner.length = 4;
					return true;
				}
				int n = -1;
				switch (idx) {
				case STRING_ID_MANUFACTURER: n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_manufacturer); break;
				case STRING_ID_PRODUCT:      n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_product); break;
				case STRING_ID_SERIAL:       n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_serial); break;
				case STRING_ID_CONFIG:       n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_config); break;
				case STRING_ID_INTERFACE:    n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_interface); break;
				default: return false;
				}
				if (n < 0)
					return false;
				io->inner.length = n;
				return true;
			}
			default:
				return false;
			}

		case USB_REQ_SET_CONFIGURATION:
			pthread_mutex_lock(&state_lock);
			ep_bulk_out = usb_raw_ep_enable(fd, &usb_ep_bulk_out);
			printf("ep0: ep_bulk_out enabled: %d\n", ep_bulk_out);
			if (use_int_ep) {
				ep_int_in = usb_raw_ep_enable(fd, &usb_ep_int_in);
				printf("ep0: ep_int_in enabled: %d\n", ep_int_in);
			} else {
				ep_int_in = -1;
				printf("ep0: ep_int_in disabled for this UDC\n");
			}
			ep_bulk_in = usb_raw_ep_enable(fd, &usb_ep_bulk_in);
			printf("ep0: ep_bulk_in enabled: %d\n", ep_bulk_in);
			atomic_store(&gadget_online, true);
			pthread_mutex_unlock(&state_lock);

			if (!ep_bulk_out_thread_spawned) {
				int rv = pthread_create(&ep_bulk_out_thread, 0, ep_bulk_out_loop, (void *)(long)fd);
				if (rv != 0) {
					perror("pthread_create(ep_bulk_out)");
					exit(EXIT_FAILURE);
				}
				ep_bulk_out_thread_spawned = true;
			}
			if (!ep_bulk_in_thread_spawned) {
				int rv = pthread_create(&ep_bulk_in_thread, 0, ep_bulk_in_loop, (void *)(long)fd);
				if (rv != 0) {
					perror("pthread_create(ep_bulk_in)");
					exit(EXIT_FAILURE);
				}
				ep_bulk_in_thread_spawned = true;
			}
			if (use_int_ep && !ep_int_in_thread_spawned) {
				int rv = pthread_create(&ep_int_in_thread, 0, ep_int_in_loop, (void *)(long)fd);
				if (rv != 0) {
					perror("pthread_create(ep_int_in)");
					exit(EXIT_FAILURE);
				}
				ep_int_in_thread_spawned = true;
			}
			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;
			return true;

		case USB_REQ_GET_CONFIGURATION:
			io->data[0] = 1;
			io->inner.length = 1;
			return true;

		case USB_REQ_SET_INTERFACE:
			io->inner.length = 0;
			return true;

		case USB_REQ_GET_INTERFACE:
			io->data[0] = usb_interface.bAlternateSetting;
			io->inner.length = 1;
			return true;

		case USB_REQ_GET_STATUS:
			io->data[0] = 0;
			io->data[1] = 0;
			io->inner.length = 2;
			return true;

		default:
			return false;
		}

	case USB_TYPE_VENDOR:
		if (event->ctrl.bRequestType & USB_DIR_IN)
			return handle_vendor_in_request(&event->ctrl, io);
		return handle_vendor_out_request(&event->ctrl, io);

	case USB_TYPE_CLASS:
		printf("class request not handled for AX88772B: req=0x%02x len=%u val=0x%04x idx=0x%04x\n",
		       event->ctrl.bRequest, event->ctrl.wLength,
		       event->ctrl.wValue, event->ctrl.wIndex);
		return false;

	default:
		return false;
	}
}

void stop_threads_and_eps(int fd) {
	atomic_store(&gadget_online, false);

	if (ep_bulk_out_thread_spawned) {
		printf("ep0: stopping ep_bulk_out thread\n");
		pthread_cancel(ep_bulk_out_thread);
		pthread_join(ep_bulk_out_thread, NULL);
		ep_bulk_out_thread_spawned = false;
	}
	if (ep_bulk_in_thread_spawned) {
		printf("ep0: stopping ep_bulk_in thread\n");
		pthread_cancel(ep_bulk_in_thread);
		pthread_join(ep_bulk_in_thread, NULL);
		ep_bulk_in_thread_spawned = false;
	}
	if (ep_int_in_thread_spawned) {
		printf("ep0: stopping ep_int_in thread\n");
		pthread_cancel(ep_int_in_thread);
		pthread_join(ep_int_in_thread, NULL);
		ep_int_in_thread_spawned = false;
	}

	pthread_mutex_lock(&state_lock);
	if (ep_bulk_out >= 0) {
		usb_raw_ep_disable(fd, ep_bulk_out);
		ep_bulk_out = -1;
	}
	if (ep_bulk_in >= 0) {
		usb_raw_ep_disable(fd, ep_bulk_in);
		ep_bulk_in = -1;
	}
	if (ep_int_in >= 0) {
		usb_raw_ep_disable(fd, ep_int_in);
		ep_int_in = -1;
	}
	pthread_mutex_unlock(&state_lock);
}

void ep0_loop(int fd) {
	while (true) {
		struct usb_raw_control_event event;
		memset(&event, 0, sizeof(event));
		event.inner.length = sizeof(event.ctrl);
		usb_raw_event_fetch(fd, (struct usb_raw_event *)&event);
		log_event((struct usb_raw_event *)&event);

		if (event.inner.type == USB_RAW_EVENT_CONNECT) {
			process_eps_info(fd);
			continue;
		}
		if (event.inner.type == USB_RAW_EVENT_RESET || event.inner.type == USB_RAW_EVENT_DISCONNECT) {
			stop_threads_and_eps(fd);
			continue;
		}
		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_control_io io;
		memset(&io, 0, sizeof(io));
		io.inner.ep = 0;
		io.inner.flags = 0;
		io.inner.length = 0;

		bool reply = ep0_request(fd, &event, &io);
		if (!reply) {
			printf("ep0: stalling\n");
			usb_raw_ep0_stall(fd);
			continue;
		}

		if (event.ctrl.wLength < io.inner.length)
			io.inner.length = event.ctrl.wLength;

		if (event.ctrl.bRequestType & USB_DIR_IN) {
			int rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
			printf("ep0: transferred %d bytes (in)\n", rv);
		} else {
			int rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
			printf("ep0: transferred %d bytes (out)\n", rv);

			if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR && rv > 0) {
				printf("ep0 vendor OUT payload:");
				for (int i = 0; i < rv; i++)
					printf(" %02x", (unsigned char)io.inner.data[i]);
				printf("\n");
				process_vendor_out_payload(&event.ctrl, &io, rv);
			}
		}
	}
}

int main(int argc, char **argv) {
	const char *driver = "dummy_udc";
	const char *device = "dummy_udc.0";
	if (argc >= 2) driver = argv[1];
	if (argc >= 3) device = argv[2];

	ax88772b_init_state();

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_FULL, driver, device);
	usb_raw_run(fd);
	ep0_loop(fd);
	close(fd);
	return 0;
}