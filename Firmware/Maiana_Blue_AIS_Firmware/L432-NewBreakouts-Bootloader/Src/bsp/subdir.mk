################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../Src/bsp/bsp_10_0.cpp \
../Src/bsp/bsp_10_1.cpp \
../Src/bsp/bsp_10_5.cpp \
../Src/bsp/bsp_10_9.cpp \
../Src/bsp/bsp_11_0.cpp \
../Src/bsp/bsp_11_3.cpp \
../Src/bsp/bsp_9_3.cpp 

OBJS += \
./Src/bsp/bsp_10_0.o \
./Src/bsp/bsp_10_1.o \
./Src/bsp/bsp_10_5.o \
./Src/bsp/bsp_10_9.o \
./Src/bsp/bsp_11_0.o \
./Src/bsp/bsp_11_3.o \
./Src/bsp/bsp_9_3.o 

CPP_DEPS += \
./Src/bsp/bsp_10_0.d \
./Src/bsp/bsp_10_1.d \
./Src/bsp/bsp_10_5.d \
./Src/bsp/bsp_10_9.d \
./Src/bsp/bsp_11_0.d \
./Src/bsp/bsp_11_3.d \
./Src/bsp/bsp_9_3.d 


# Each subdirectory must supply rules for building sources it contributes
Src/bsp/%.o: ../Src/bsp/%.cpp Src/bsp/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: Cross ARM GNU C++ Compiler'
	arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -fsingle-precision-constant -flto -Wall -DSTM32L432xx -DVECT_TAB_OFFSET=0x4800 -DLEGACY_BREAKOUTS=0 -I"D:\_Pentru_github_\Maiana_Blue_AIS_Firmware\Inc" -I"D:\_Pentru_github_\Maiana_Blue_AIS_Firmware\Inc\bsp" -I"D:\_Pentru_github_\Maiana_Blue_AIS_Firmware\Drivers\CMSIS\Device\ST\STM32L4xx\Include" -I"D:\_Pentru_github_\Maiana_Blue_AIS_Firmware\Drivers\CMSIS\Include" -I"D:\_Pentru_github_\Maiana_Blue_AIS_Firmware\Drivers\STM32L4xx_HAL_Driver\Inc" -std=gnu++11 -fabi-version=0 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -Wa,-adhlns="$@.lst" -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


