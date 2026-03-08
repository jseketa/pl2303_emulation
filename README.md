# Draft manual
verified commit
dummy

Raw Gadget is a userspace interface for the Linux USB Gadget subsystem, and the upstream project supports both physical UDC-backed devices such as Raspberry Pi and virtual devices via Dummy HCD/UDC. The kernel docs also note that Raw Gadget depends on CONFIG_USB_RAW_GADGET.

1. Install packages

On a fresh Pi OS / Debian-based system:

sudo apt update
sudo apt install -y git build-essential linux-headers-$(uname -r) kmod

You need headers for the currently running kernel if you want to build an out-of-tree raw_gadget.ko.

2. Enable USB gadget mode on the Pi

On current Raspberry Pi OS, config.txt lives in /boot/firmware/config.txt. Raspberry Pi’s docs note that Bookworm-era systems use /boot/firmware/ for the boot partition.

Edit:

sudo nano /boot/firmware/config.txt

Add:

dtoverlay=dwc2

Then edit:

sudo nano /boot/firmware/cmdline.txt

Add modules-load=dwc2 to the single existing line, for example after rootwait:

... rootwait modules-load=dwc2 ...

Reboot:

sudo reboot
3. Verify the UDC exists

After reboot:

ls /sys/class/udc

You should see at least one controller name. That second argument is what your emulator will use later as the device argument.

4. Clone Raw Gadget
cd ~
git clone https://github.com/xairy/raw-gadget
cd raw-gadget

The upstream repo is here: xairy/raw-gadget.

5. Check whether your kernel already has Raw Gadget

Try:

zgrep CONFIG_USB_RAW_GADGET /proc/config.gz 2>/dev/null || \
grep CONFIG_USB_RAW_GADGET /boot/config-$(uname -r)

If it says CONFIG_USB_RAW_GADGET=y or =m, try loading it:

sudo modprobe raw_gadget
ls -l /dev/raw-gadget

If /dev/raw-gadget appears, you can skip to step 7.

6. If Raw Gadget is missing, build and load the module from the repo

Build the module from the cloned repo:

cd ~/raw-gadget/raw_gadget
make

Load it:

sudo insmod ./raw_gadget.ko

Verify:

lsmod | grep raw_gadget
ls -l /dev/raw-gadget

You want to see /dev/raw-gadget before running the emulator.

7. Put your current pl2303.c into the examples directory

Your current working source is the one in the canvas. Save that content as:

~/raw-gadget/examples/pl2303.c

If you already have it there, keep using that file.

8. Make sure the examples Makefile builds pl2303

Check the examples Makefile:

sed -n '1,200p' ~/raw-gadget/examples/Makefile

Make sure there is a target for pl2303, typically something like:

pl2303: pl2303.c
	$(CC) -o pl2303 pl2303.c -O2 -Wall -g -lpthread

If it is already there, leave it.

9. Build the emulator
cd ~/raw-gadget
make -C examples

This should produce:

~/raw-gadget/examples/pl2303
10. Find the correct UDC name

Run:

ls /sys/class/udc

Example output might be something like:

20980000.usb

That exact string is the device argument to your emulator.

For your program, the first runtime argument is just the driver label you pass into usb_raw_init(), and the second is the concrete UDC device name from /sys/class/udc. In your code, these values are passed as command-line arguments into:

usb_raw_init(fd, USB_SPEED_FULL, driver, device);
11. Start the emulator on the Pi

If your UDC name is 20980000.usb, run:

sudo ~/raw-gadget/examples/pl2303 dwc2 20980000.usb

If your Pi reports a different UDC name, use that instead.

When it starts correctly, you should see lines like:

PTY backend ready: /dev/pts/5
PTY symlink ready: /dev/r2usbc -> /dev/pts/5

and then, after the host connects, USB events.

12. Connect the embedded host to the Pi’s gadget-capable USB port

Use the port that supports device/OTG mode on your Pi model. Once the embedded host enumerates the fake PL2303, your emulator should log connect/reset/control traffic.

13. On the Pi side, use the stable backend path

Your current emulator creates:

/dev/r2usbc

as a symlink to the current PTY, so your local service should open:

/dev/r2usbc

not the changing /dev/pts/N.

14. Quick runtime checks

In another terminal on the Pi:

ls -l /dev/raw-gadget
ls -l /dev/r2usbc
readlink -f /dev/r2usbc

If the embedded host is talking to the emulated PL2303, your emulator window should show control requests and bulk transfers.

15. Optional: test the PTY backend manually

Read what the host sends:

cat /dev/r2usbc

Send data back toward the host:

printf 'hello from pi\r\n' | sudo tee /dev/r2usbc >/dev/null
16. If /dev/raw-gadget is missing after reboot

You need to reload the module:

sudo modprobe raw_gadget

or, if you built it manually and it is not installed system-wide:

sudo insmod ~/raw-gadget/raw_gadget/raw_gadget.ko
Minimal command summary
sudo apt update
sudo apt install -y git build-essential linux-headers-$(uname -r) kmod

sudo nano /boot/firmware/config.txt
# add: dtoverlay=dwc2

sudo nano /boot/firmware/cmdline.txt
# add to existing single line: modules-load=dwc2

sudo reboot

Then:

ls /sys/class/udc

cd ~
git clone https://github.com/xairy/raw-gadget
cd ~/raw-gadget/raw_gadget
make
sudo insmod ./raw_gadget.ko

ls -l /dev/raw-gadget

Then put your current canvas file at ~/raw-gadget/examples/pl2303.c, ensure the Makefile has a pl2303 target, and run:

cd ~/raw-gadget
make -C examples
sudo ~/raw-gadget/examples/pl2303 dwc2 "$(ls /sys/class/udc | head -n1)"