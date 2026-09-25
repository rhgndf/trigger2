make clean
make all -j
sudo rmmod trigger2 2>/dev/null
sudo insmod trigger2.ko
