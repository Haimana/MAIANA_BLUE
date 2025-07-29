################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../n2k_mcp/NMEA2000_mcp.cpp 

OBJS += \
./n2k_mcp/NMEA2000_mcp.o 

CPP_DEPS += \
./n2k_mcp/NMEA2000_mcp.d 


# Each subdirectory must supply rules for building sources it contributes
n2k_mcp/%.o: ../n2k_mcp/%.cpp n2k_mcp/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU Arm Cross C++ Compiler'
	arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -flto -Wall -DSTM32 -DSTM32L432xx -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Core\Inc" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\mcp_can" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\ais" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\n2k_mcp" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\nmea2000" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\CMSIS\Device\ST\STM32L4xx\Include" -I"D:\_Pentru_github_\Maiana_Blue_N2K_Firmware\Drivers\STM32L4xx_HAL_Driver\Inc" -std=gnu++11 -fabi-version=0 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


