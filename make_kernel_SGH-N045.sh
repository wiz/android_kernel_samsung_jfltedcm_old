export CROSS_COMPILE=/home/jmaurice/android/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi-
export VARIANT_DEFCONFIG=jf_dcm_defconfig 
make jf_defconfig
make -j16
make zImage
