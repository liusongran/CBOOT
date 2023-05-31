#!/bin/bash

cd /home/neu/Downloads/Cboot
export TEGRA_TOP=$PWD
export TOP=$PWD
export CROSS_COMPILE=/home/neu/Downloads/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-
make -C ./bootloader/partner/t18x/cboot PROJECT=t194 TOOLCHAIN_PREFIX="${CROSS_COMPILE}" DEBUG=2 BUILDROOT="${PWD}"/out NV_TARGET_BOARD=t194ref NV_BUILD_SYSTEM_TYPE=l4t NOECHO=@
## cd out/build-t194
## mv lk.bin  cboot_t194.bin
