################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../Core/Src/DebugMsgHandler.cpp \
../Core/Src/EventQueue.cpp \
../Core/Src/Events.cpp \
../Core/Src/NMEA0183Consumer.cpp \
../Core/Src/NMEASentence.cpp \
../Core/Src/Utils.cpp \
../Core/Src/main.cpp 

C_SRCS += \
../Core/Src/stm32l4xx_it.c \
../Core/Src/system_stm32l4xx.c 

C_DEPS += \
./Core/Src/stm32l4xx_it.d \
./Core/Src/system_stm32l4xx.d 

OBJS += \
./Core/Src/DebugMsgHandler.o \
./Core/Src/EventQueue.o \
./Core/Src/Events.o \
./Core/Src/NMEA0183Consumer.o \
./Core/Src/NMEASentence.o \
./Core/Src/Utils.o \
./Core/Src/main.o \
./Core/Src/stm32l4xx_it.o \
./Core/Src/system_stm32l4xx.o 

CPP_DEPS += \
./Core/Src/DebugMsgHandler.d \
./Core/Src/EventQueue.d \
./Core/Src/Events.d \
./Core/Src/NMEA0183Consumer.d \
./Core/Src/NMEASentence.d \
./Core/Src/Utils.d \
./Core/Src/main.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o: ../Core/Src/%.cpp Core/Src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU Arm Cross C++ Compiler'
	arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -flto -Wall -DSTM32 -DSTM32L432xx -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Core\Inc" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\mcp_can" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\ais" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\n2k_mcp" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\nmea2000" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Device\ST\STM32L4xx\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\STM32L4xx_HAL_Driver\Inc" -std=gnu++11 -fabi-version=0 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

Core/Src/%.o: ../Core/Src/%.c Core/Src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -flto -Wall -DSTM32 -DSTM32L432xx -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Core\Inc" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Device\ST\STM32L4xx\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\STM32L4xx_HAL_Driver\Inc" -std=gnu11 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


