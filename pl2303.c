// SPDX-License-Identifier: Apache-2.0
//
// Minimal PL2303-style Raw Gadget example with a cleaned PTY backend
// and cleaner shutdown/restart handling.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

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

static int g_raw_fd = -1;
static atomic_bool exit_requested = false;

static int usb_raw_open(void) {
	int fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		perror("open(/dev/raw-gadget)");
		exit(EXIT_FAILURE);
	}
	return fd;
}

static void usb_raw_init(int fd, enum usb_device_speed speed,
		  const char *driver, const char *device) {
	struct usb_raw_init arg;
	memset(&arg, 0, sizeof(arg));
	strcpy((char *)&arg.driver_name[0], driver);
	strcpy((char *)&arg.device_name[0], device);
	arg.speed = speed;
	if (ioctl(fd, USB_RAW_IOCTL_INIT, &arg) < 0) {
		perror("ioctl(USB_RAW_IOCTL_INIT)");
		exit(EXIT_FAILURE);
	}
}

static void usb_raw_run(int fd) {
	if (ioctl(fd, USB_RAW_IOCTL_RUN, 0) < 0) {
		perror("ioctl(USB_RAW_IOCTL_RUN)");
		exit(EXIT_FAILURE);
	}
}

static int usb_raw_event_fetch_may_fail(int fd, struct usb_raw_event *event) {
	return ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
}

static int usb_raw_ep0_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

static int usb_raw_ep0_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

static int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor *desc) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_ENABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

static int usb_raw_ep_disable_may_fail(int fd, int ep) {
	return ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, ep);
}

static int usb_raw_ep_write_may_fail(int fd, struct usb_raw_ep_io *io) {
	return ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
}

static int usb_raw_ep_read_may_fail(int fd, struct usb_raw_ep_io *io) {
	return ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
}

static void usb_raw_configure(int fd) {
	if (ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0) < 0) {
		perror("ioctl(USB_RAW_IOCTL_CONFIGURE)");
		exit(EXIT_FAILURE);
	}
}

static void usb_raw_vbus_draw(int fd, uint32_t power) {
	if (ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power) < 0) {
		perror("ioctl(USB_RAW_IOCTL_VBUS_DRAW)");
		exit(EXIT_FAILURE);
	}
}

static int usb_raw_eps_info(int fd, struct usb_raw_eps_info *info) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EPS_INFO, info);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EPS_INFO)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

static void usb_raw_ep0_stall(int fd) {
	if (ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0) < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_STALL)");
		exit(EXIT_FAILURE);
	}
}

/*----------------------------------------------------------------------*/

static void log_control_request(struct usb_ctrlrequest *ctrl) {
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

static void log_event(struct usb_raw_event *event) {
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

#define BCD_USB 0x0200
#define USB_VENDOR  0x067b
#define USB_PRODUCT 0x2303

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

static struct usb_device_descriptor usb_device = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = __constant_cpu_to_le16(BCD_USB),
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = EP_MAX_PACKET_CONTROL,
	.idVendor = __constant_cpu_to_le16(USB_VENDOR),
	.idProduct = __constant_cpu_to_le16(USB_PRODUCT),
	.bcdDevice = __constant_cpu_to_le16(0x0300),
	.iManufacturer = STRING_ID_MANUFACTURER,
	.iProduct = STRING_ID_PRODUCT,
	.iSerialNumber = STRING_ID_SERIAL,
	.bNumConfigurations = 1,
};

static struct usb_qualifier_descriptor usb_qualifier = {
	.bLength = sizeof(struct usb_qualifier_descriptor),
	.bDescriptorType = USB_DT_DEVICE_QUALIFIER,
	.bcdUSB = __constant_cpu_to_le16(BCD_USB),
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = EP_MAX_PACKET_CONTROL,
	.bNumConfigurations = 1,
	.bRESERVED = 0,
};

static struct usb_config_descriptor usb_config = {
	.bLength = USB_DT_CONFIG_SIZE,
	.bDescriptorType = USB_DT_CONFIG,
	.wTotalLength = 0,
	.bNumInterfaces = 1,
	.bConfigurationValue = 1,
	.iConfiguration = STRING_ID_CONFIG,
	.bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower = 0x32,
};

static struct usb_interface_descriptor usb_interface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 3,
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = STRING_ID_INTERFACE,
};

static struct usb_endpoint_descriptor usb_ep_bulk_out = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = EP_NUM_BULK_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(EP_MAX_PACKET_BULK),
	.bInterval = 0,
};

static struct usb_endpoint_descriptor usb_ep_bulk_in = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN | EP_NUM_BULK_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(EP_MAX_PACKET_BULK),
	.bInterval = 0,
};

static struct usb_endpoint_descriptor usb_ep_int_in = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN | EP_NUM_INT_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = __constant_cpu_to_le16(EP_MAX_PACKET_INT),
	.bInterval = 10,
};

static const char *str_manufacturer = "Prolific";
static const char *str_product = "PL2303 Serial";
static const char *str_serial = "000123";
static const char *str_config = "Default";
static const char *str_interface = "UART";
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

static int build_config(char *data, int length, bool other_speed) {
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

static bool assign_ep_address(struct usb_raw_ep_info *info, struct usb_endpoint_descriptor *ep) {
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

static void process_eps_info(int fd) {
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
	if (use_int_ep) {
		printf("ep_int_in  : addr = 0x%02x (num=%u, IN)\n",
		       usb_ep_int_in.bEndpointAddress,
		       usb_endpoint_num(&usb_ep_int_in));
	} else {
		printf("ep_int_in  : not available on this UDC, continuing without it\n");
	}
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
	char data[EP_MAX_PACKET_BULK];
};

static int ep_bulk_out = -1;
static int ep_bulk_in = -1;
static int ep_int_in = -1;

static int pty_master_fd = -1;
static int pty_slave_fd = -1;
static char pty_slave_name[128];
static const char *pty_symlink_path = "/dev/r2usbc";

static pthread_t ep_bulk_out_thread;
static pthread_t ep_bulk_in_thread;
static pthread_t ep_int_in_thread;

static bool ep_bulk_out_thread_spawned = false;
static bool ep_bulk_in_thread_spawned = false;
static bool ep_int_in_thread_spawned = false;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool gadget_online = false;

/*----------------------------------------------------------------------*/

static void request_exit(void) {
	atomic_store(&exit_requested, true);
	atomic_store(&gadget_online, false);
}

static void handle_exit_signal(int sig) {
	(void)sig;
	request_exit();
}

static int configure_fd_raw(int fd) {
	struct termios tio;

	if (tcgetattr(fd, &tio) < 0) {
		perror("tcgetattr");
		return -1;
	}

	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_cflag &= ~CRTSCTS;
	tio.c_cflag &= ~PARENB;
	tio.c_cflag &= ~CSTOPB;
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		perror("tcsetattr");
		return -1;
	}

	if (tcflush(fd, TCIOFLUSH) < 0) {
		perror("tcflush");
		return -1;
	}

	return 0;
}

static int setup_pty_backend(void) {
	if (openpty(&pty_master_fd, &pty_slave_fd, pty_slave_name, NULL, NULL) < 0) {
		perror("openpty");
		return -1;
	}

	if (configure_fd_raw(pty_slave_fd) < 0)
		return -1;
	if (configure_fd_raw(pty_master_fd) < 0)
		return -1;

	if (chmod(pty_slave_name, 0666) < 0) {
		perror("chmod(pty_slave_name)");
		return -1;
	}

	if (unlink(pty_symlink_path) < 0 && errno != ENOENT) {
		perror("unlink(/dev/r2usbc)");
		return -1;
	}

	if (symlink(pty_slave_name, pty_symlink_path) < 0) {
		perror("symlink(/dev/r2usbc)");
		return -1;
	}

	printf("PTY backend ready: %s\n", pty_slave_name);
	printf("PTY symlink ready: %s -> %s\n", pty_symlink_path, pty_slave_name);
	fflush(stdout);

	return 0;
}

static void cleanup_runtime_artifacts(void) {
	if (unlink(pty_symlink_path) < 0 && errno != ENOENT)
		perror("unlink(/dev/r2usbc)");

	if (pty_slave_fd >= 0) {
		close(pty_slave_fd);
		pty_slave_fd = -1;
	}
	if (pty_master_fd >= 0) {
		close(pty_master_fd);
		pty_master_fd = -1;
	}
}

static void cancel_and_join_thread(pthread_t thread, bool *spawned, const char *name) {
	if (!*spawned)
		return;

	printf("shutdown: stopping %s thread\n", name);
	fflush(stdout);
	pthread_cancel(thread);
	pthread_join(thread, NULL);
	*spawned = false;
}

static void stop_threads_and_eps(int fd) {
	atomic_store(&gadget_online, false);

	cancel_and_join_thread(ep_bulk_out_thread, &ep_bulk_out_thread_spawned, "ep_bulk_out");
	cancel_and_join_thread(ep_bulk_in_thread, &ep_bulk_in_thread_spawned, "ep_bulk_in");
	cancel_and_join_thread(ep_int_in_thread, &ep_int_in_thread_spawned, "ep_int_in");

	pthread_mutex_lock(&state_lock);
	if (fd >= 0) {
		if (ep_bulk_out >= 0) {
			usb_raw_ep_disable_may_fail(fd, ep_bulk_out);
			ep_bulk_out = -1;
		}
		if (ep_bulk_in >= 0) {
			usb_raw_ep_disable_may_fail(fd, ep_bulk_in);
			ep_bulk_in = -1;
		}
		if (ep_int_in >= 0) {
			usb_raw_ep_disable_may_fail(fd, ep_int_in);
			ep_int_in = -1;
		}
	}
	pthread_mutex_unlock(&state_lock);
}

/*----------------------------------------------------------------------*/

static void *ep_bulk_out_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;
	memset(&io, 0, sizeof(io));

	while (!atomic_load(&exit_requested)) {
		int local_ep;
		int local_pty_fd;

		pthread_mutex_lock(&state_lock);
		local_ep = ep_bulk_out;
		local_pty_fd = pty_master_fd;
		pthread_mutex_unlock(&state_lock);

		if (!atomic_load(&gadget_online) || local_ep < 0) {
			usleep(10000);
			continue;
		}

		io.inner.ep = local_ep;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		int rv = usb_raw_ep_read_may_fail(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			if (errno == ESHUTDOWN || errno == EINTR)
				break;
			perror("usb_raw_ep_read_may_fail");
			request_exit();
			break;
		}

		printf("ep_bulk_out: received %d bytes:", rv);
		for (int i = 0; i < rv; i++)
			printf(" %02x", (unsigned char)io.inner.data[i]);
		printf("\n");
		fflush(stdout);

		if (local_pty_fd >= 0 && rv > 0) {
			ssize_t wr = write(local_pty_fd, io.inner.data, (size_t)rv);
			if (wr < 0) {
				if (errno == EINTR || errno == EIO)
					continue;
				perror("write(pty_master_fd)");
				request_exit();
				break;
			}
			printf("pty write: requested=%d written=%zd\n", rv, wr);
			fflush(stdout);
		}
	}

	return NULL;
}

static void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;
	memset(&io, 0, sizeof(io));
	io.inner.flags = 0;

	while (!atomic_load(&exit_requested)) {
		int local_ep;
		int local_pty_fd;

		pthread_mutex_lock(&state_lock);
		local_ep = ep_bulk_in;
		local_pty_fd = pty_master_fd;
		pthread_mutex_unlock(&state_lock);

		if (!atomic_load(&gadget_online) || local_ep < 0 || local_pty_fd < 0) {
			usleep(10000);
			continue;
		}

		io.inner.ep = local_ep;
		ssize_t rv = read(local_pty_fd, io.inner.data, sizeof(io.data));
		if (rv < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EIO) {
				usleep(10000);
				continue;
			}
			perror("read(pty_master_fd)");
			request_exit();
			break;
		}
		if (rv == 0)
			continue;

		printf("pty read: %zd bytes\n", rv);
		fflush(stdout);

		io.inner.length = (uint32_t)rv;
		int wr = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)&io);
		if (wr < 0) {
			if (errno == ESHUTDOWN || errno == EINTR)
				break;
			perror("usb_raw_ep_write_may_fail(ep_bulk_in)");
			request_exit();
			break;
		}

		printf("ep_bulk_in: sent %d bytes from PTY\n", wr);
		fflush(stdout);
	}

	return NULL;
}

static void *ep_int_in_loop(void *arg) {
	(void)arg;
	while (!atomic_load(&exit_requested)) {
		if (!atomic_load(&gadget_online)) {
			usleep(10000);
			continue;
		}
		sleep(1);
	}
	return NULL;
}

/*----------------------------------------------------------------------*/

static bool handle_vendor_in_request(struct usb_ctrlrequest *ctrl, struct usb_raw_control_io *io) {
	memset(io->data, 0, sizeof(io->data));
	io->inner.length = ctrl->wLength;
	if (io->inner.length > sizeof(io->data))
		io->inner.length = sizeof(io->data);
	printf("vendor IN stub: req=0x%02x len=%u\n", ctrl->bRequest, ctrl->wLength);
	return true;
}

static bool handle_vendor_out_request(struct usb_ctrlrequest *ctrl, struct usb_raw_control_io *io) {
	io->inner.length = ctrl->wLength;
	if (io->inner.length > sizeof(io->data))
		io->inner.length = sizeof(io->data);
	printf("vendor OUT stub: req=0x%02x len=%u val=0x%04x idx=0x%04x\n",
	       ctrl->bRequest, ctrl->wLength, ctrl->wValue, ctrl->wIndex);
	return true;
}

static bool handle_class_in_request(struct usb_ctrlrequest *ctrl, struct usb_raw_control_io *io) {
	memset(io->data, 0, sizeof(io->data));
	switch (ctrl->bRequest) {
	case 0x21:
		io->data[0] = 0x80;
		io->data[1] = 0x25;
		io->data[2] = 0x00;
		io->data[3] = 0x00;
		io->data[4] = 0x00;
		io->data[5] = 0x00;
		io->data[6] = 0x08;
		io->inner.length = 7;
		printf("class IN: GET_LINE_CODING\n");
		return true;
	default:
		printf("class IN not handled: req=0x%02x len=%u val=0x%04x idx=0x%04x\n",
		       ctrl->bRequest, ctrl->wLength, ctrl->wValue, ctrl->wIndex);
		return false;
	}
}

static bool handle_class_out_request(struct usb_ctrlrequest *ctrl, struct usb_raw_control_io *io) {
	switch (ctrl->bRequest) {
	case 0x20:
		io->inner.length = ctrl->wLength;
		if (io->inner.length > sizeof(io->data))
			io->inner.length = sizeof(io->data);
		printf("class OUT: SET_LINE_CODING len=%u\n", ctrl->wLength);
		return true;
	case 0x22:
		io->inner.length = 0;
		printf("class OUT: SET_CONTROL_LINE_STATE value=0x%04x\n", ctrl->wValue);
		return true;
	case 0x23:
		io->inner.length = 0;
		printf("class OUT: SEND_BREAK value=0x%04x\n", ctrl->wValue);
		return true;
	default:
		printf("class OUT not handled: req=0x%02x len=%u val=0x%04x idx=0x%04x\n",
		       ctrl->bRequest, ctrl->wLength, ctrl->wValue, ctrl->wIndex);
		return false;
	}
}

static bool ep0_request(int fd, struct usb_raw_control_event *event, struct usb_raw_control_io *io) {
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
				case STRING_ID_MANUFACTURER:
					n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_manufacturer);
					break;
				case STRING_ID_PRODUCT:
					n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_product);
					break;
				case STRING_ID_SERIAL:
					n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_serial);
					break;
				case STRING_ID_CONFIG:
					n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_config);
					break;
				case STRING_ID_INTERFACE:
					n = make_string_desc((__u8 *)&io->data[0], sizeof(io->data), str_interface);
					break;
				default:
					return false;
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
				int rv = pthread_create(&ep_bulk_out_thread, NULL, ep_bulk_out_loop, (void *)(long)fd);
				if (rv != 0) {
					errno = rv;
					perror("pthread_create(ep_bulk_out)");
					exit(EXIT_FAILURE);
				}
				ep_bulk_out_thread_spawned = true;
			}
			if (!ep_bulk_in_thread_spawned) {
				int rv = pthread_create(&ep_bulk_in_thread, NULL, ep_bulk_in_loop, (void *)(long)fd);
				if (rv != 0) {
					errno = rv;
					perror("pthread_create(ep_bulk_in)");
					exit(EXIT_FAILURE);
				}
				ep_bulk_in_thread_spawned = true;
			}
			if (use_int_ep && !ep_int_in_thread_spawned) {
				int rv = pthread_create(&ep_int_in_thread, NULL, ep_int_in_loop, (void *)(long)fd);
				if (rv != 0) {
					errno = rv;
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
		if (event->ctrl.bRequestType & USB_DIR_IN)
			return handle_class_in_request(&event->ctrl, io);
		return handle_class_out_request(&event->ctrl, io);

	default:
		return false;
	}
}

static void ep0_loop(int fd) {
	while (!atomic_load(&exit_requested)) {
		struct usb_raw_control_event event;
		memset(&event, 0, sizeof(event));
		event.inner.length = sizeof(event.ctrl);

		int rv = usb_raw_event_fetch_may_fail(fd, (struct usb_raw_event *)&event);
		if (rv < 0) {
			if (errno == EINTR && atomic_load(&exit_requested))
				break;
			if (errno == ESHUTDOWN)
				break;
			perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
			request_exit();
			break;
		}

		log_event((struct usb_raw_event *)&event);

		if (event.inner.type == USB_RAW_EVENT_CONNECT) {
			process_eps_info(fd);
			continue;
		}
		if (event.inner.type == USB_RAW_EVENT_RESET || event.inner.type == USB_RAW_EVENT_DISCONNECT) {
			printf("ep0: session ended, tearing down endpoints and waiting for reconnect
");
			fflush(stdout);
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
			int xfer = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
			printf("ep0: transferred %d bytes (in)\n", xfer);
		} else {
			int xfer = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
			printf("ep0: transferred %d bytes (out)\n", xfer);
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR && xfer > 0) {
				printf("ep0 vendor OUT payload:");
				for (int i = 0; i < xfer; i++)
					printf(" %02x", (unsigned char)io.inner.data[i]);
				printf("\n");
			}
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS && xfer > 0) {
				printf("ep0 class OUT payload:");
				for (int i = 0; i < xfer; i++)
					printf(" %02x", (unsigned char)io.inner.data[i]);
				printf("\n");
			}
		}
	}
}

static void install_signal_handlers(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_exit_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: allow blocking syscalls to wake with EINTR */

	if (sigaction(SIGINT, &sa, NULL) < 0) {
		perror("sigaction(SIGINT)");
		exit(EXIT_FAILURE);
	}
	if (sigaction(SIGTERM, &sa, NULL) < 0) {
		perror("sigaction(SIGTERM)");
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char **argv) {
	const char *driver = "dummy_udc";
	const char *device = "dummy_udc.0";
	if (argc >= 2)
		driver = argv[1];
	if (argc >= 3)
		device = argv[2];

	install_signal_handlers();

	if (setup_pty_backend() < 0)
		return 1;

	g_raw_fd = usb_raw_open();
	usb_raw_init(g_raw_fd, USB_SPEED_FULL, driver, device);
	usb_raw_run(g_raw_fd);
	printf("raw-gadget running\n");
	fflush(stdout);

	ep0_loop(g_raw_fd);

	stop_threads_and_eps(g_raw_fd);
	cleanup_runtime_artifacts();

	if (g_raw_fd >= 0) {
		close(g_raw_fd);
		g_raw_fd = -1;
	}

	printf("shutdown: complete\n");
	fflush(stdout);
	return 0;
}
