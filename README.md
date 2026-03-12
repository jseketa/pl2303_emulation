# First steps - cloning the repo
Run update - optional.
```bash
sudo apt update
```
Install RPi kernel headers, git and build tools.
```bash
sudo apt install -y git build-essential bc raspberrypi-kernel-headers
```
Create a temporary directory in home directory.
```bash
cd ~
mkdir temp
cd temp
```
Clone the raw-gadget repository, cd into it and run make.
```bash
git clone https://github.com/xairy/raw-gadget.git
cd raw-gadget/raw_gadget
make
```
Create a folder in modules (with kernel name in the path).
Copy compiled raw-gadget (raw-gadget.ko) over.
```bash
sudo mkdir -p /lib/modules/$(uname -r)/extra
sudo cp raw_gadget.ko /lib/modules/$(uname -r)/extra/
```
Regenerate module dependencies.
```bash
sudo depmod -a
```
Load raw-gadget module with modprobe.
```bash
sudo modprobe raw_gadget
```
Check if it is loaded.
```bash
lsmod | grep raw_gadget
```
Show details with modinfo.
```bash
modinfo raw_gadget
```
Make the module load automatically after reboot.
```bash
echo raw_gadget | sudo tee /etc/modules-load.d/raw_gadget.conf
```