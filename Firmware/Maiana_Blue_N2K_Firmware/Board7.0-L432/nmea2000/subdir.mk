################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../nmea2000/N2kDeviceList.cpp \
../nmea2000/N2kGroupFunction.cpp \
../nmea2000/N2kGroupFunctionDefaultHandlers.cpp \
../nmea2000/N2kMaretron.cpp \
../nmea2000/N2kMessages.cpp \
../nmea2000/N2kMsg.cpp \
../nmea2000/N2kStream.cpp \
../nmea2000/NMEA2000.cpp \
../nmea2000/Seasmart.cpp 

OBJS += \
./nmea2000/N2kDeviceList.o \
./nmea2000/N2kGroupFunction.o \
./nmea2000/N2kGroupFunctionDefaultHandlers.o \
./nmea2000/N2kMaretron.o \
./nmea2000/N2kMessages.o \
./nmea2000/N2kMsg.o \
./nmea2000/N2kStream.o \
./nmea2000/NMEA2000.o \
./nmea2000/Seasmart.o 

CPP_DEPS += \
./nmea2000/N2kDeviceList.d \
./nmea2000/N2kGroupFunction.d \
./nmea2000/N2kGroupFunctionDefaultHandlers.d \
./nmea2000/N2kMaretron.d \
./nmea2000/N2kMessages.d \
./nmea2000/N2kMsg.d \
./nmea2000/N2kStream.d \
./nmea2000/NMEA2000.d \
./nmea2000/Seasmart.d 


# Each subdirectory must supply rules for building sources it contributes
nmea2000/%.o: ../nmea2000/%.cpp nmea2000/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU Arm Cross C++ Compiler'
	arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -flto -Wall -DSTM32 -DSTM32L432xx -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Core\Inc" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\mcp_can" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\ais" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\n2k_mcp" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\nmea2000" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Device\ST\STM32L4xx\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\STM32L4xx_HAL_Driver\Inc" -std=gnu++11 -fabi-version=0 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


