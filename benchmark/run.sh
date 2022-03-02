sudo insmod ../kernel_module/blockmma.ko
sudo chmod 777 /dev/blockmma

# Single accelerator, single process
# Launch an accelerator
./accelerators 4 &
./benchmark 1024 &
./benchmark 1024 &
./accelerators_control "0.0.0.0" "q" 27072
sleep 2

sudo rmmod blockmma
