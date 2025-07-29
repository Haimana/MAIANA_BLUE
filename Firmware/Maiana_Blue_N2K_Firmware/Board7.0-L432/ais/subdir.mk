################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../ais/ais.cpp \
../ais/ais10.cpp \
../ais/ais12.cpp \
../ais/ais14.cpp \
../ais/ais15.cpp \
../ais/ais16.cpp \
../ais/ais17.cpp \
../ais/ais18.cpp \
../ais/ais19.cpp \
../ais/ais1_2_3.cpp \
../ais/ais20.cpp \
../ais/ais21.cpp \
../ais/ais22.cpp \
../ais/ais23.cpp \
../ais/ais24.cpp \
../ais/ais25.cpp \
../ais/ais26.cpp \
../ais/ais27.cpp \
../ais/ais4_11.cpp \
../ais/ais5.cpp \
../ais/ais6.cpp \
../ais/ais7_13.cpp \
../ais/ais8.cpp \
../ais/ais8_1_22.cpp \
../ais/ais8_1_26.cpp \
../ais/ais8_200.cpp \
../ais/ais8_366.cpp \
../ais/ais8_366_22.cpp \
../ais/ais8_367.cpp \
../ais/ais9.cpp \
../ais/ais_bitset.cpp \
../ais/decode_body.cpp \
../ais/vdm.cpp 

OBJS += \
./ais/ais.o \
./ais/ais10.o \
./ais/ais12.o \
./ais/ais14.o \
./ais/ais15.o \
./ais/ais16.o \
./ais/ais17.o \
./ais/ais18.o \
./ais/ais19.o \
./ais/ais1_2_3.o \
./ais/ais20.o \
./ais/ais21.o \
./ais/ais22.o \
./ais/ais23.o \
./ais/ais24.o \
./ais/ais25.o \
./ais/ais26.o \
./ais/ais27.o \
./ais/ais4_11.o \
./ais/ais5.o \
./ais/ais6.o \
./ais/ais7_13.o \
./ais/ais8.o \
./ais/ais8_1_22.o \
./ais/ais8_1_26.o \
./ais/ais8_200.o \
./ais/ais8_366.o \
./ais/ais8_366_22.o \
./ais/ais8_367.o \
./ais/ais9.o \
./ais/ais_bitset.o \
./ais/decode_body.o \
./ais/vdm.o 

CPP_DEPS += \
./ais/ais.d \
./ais/ais10.d \
./ais/ais12.d \
./ais/ais14.d \
./ais/ais15.d \
./ais/ais16.d \
./ais/ais17.d \
./ais/ais18.d \
./ais/ais19.d \
./ais/ais1_2_3.d \
./ais/ais20.d \
./ais/ais21.d \
./ais/ais22.d \
./ais/ais23.d \
./ais/ais24.d \
./ais/ais25.d \
./ais/ais26.d \
./ais/ais27.d \
./ais/ais4_11.d \
./ais/ais5.d \
./ais/ais6.d \
./ais/ais7_13.d \
./ais/ais8.d \
./ais/ais8_1_22.d \
./ais/ais8_1_26.d \
./ais/ais8_200.d \
./ais/ais8_366.d \
./ais/ais8_366_22.d \
./ais/ais8_367.d \
./ais/ais9.d \
./ais/ais_bitset.d \
./ais/decode_body.d \
./ais/vdm.d 


# Each subdirectory must supply rules for building sources it contributes
ais/%.o: ../ais/%.cpp ais/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU Arm Cross C++ Compiler'
	arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -flto -Wall -DSTM32 -DSTM32L432xx -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Core\Inc" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\mcp_can" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\ais" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\n2k_mcp" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\nmea2000" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Device\ST\STM32L4xx\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\STM32L4xx_HAL_Driver\Inc" -std=gnu++11 -fabi-version=0 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


