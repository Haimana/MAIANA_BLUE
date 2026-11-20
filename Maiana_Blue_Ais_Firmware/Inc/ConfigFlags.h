/*
 * ConfigFlags.h
 *
 *  Created on: Oct 11, 2021
 *      Author: peter
 */

#ifndef INC_CONFIGFLAGS_H_
#define INC_CONFIGFLAGS_H_

// flags[0] bit 0: TX inhibit (1 = silent). Absent magic → TX enabled.
// flags[0] bit 1: hourly-reboot inhibit (1 = off). Absent magic → reboot on.
// flags[0] bits 2-3: last GNSS UART rate that delivered valid NMEA
// (GNSS_BAUD_CODE_*). Only a probe-order hint; absent → probe from default.
#define CONFIG_FLAG_TX_INHIBIT      0x01u
#define CONFIG_FLAG_HRB_INHIBIT     0x02u
#define CONFIG_FLAG_GNSS_BAUD_SHIFT 2
#define CONFIG_FLAG_GNSS_BAUD_MASK  0x0Cu

// Persisted GNSS baud codes. Keep existing values stable; 3 is the last
// free code in the 2-bit field.
#define GNSS_BAUD_CODE_NONE         0u
#define GNSS_BAUD_CODE_9600         1u
#define GNSS_BAUD_CODE_115200       2u

typedef struct
{
  uint32_t magic;
  uint32_t reserved;
  uint32_t flags[5];
} ConfigFlags;

typedef union
{
  ConfigFlags config;
  uint64_t dw[4];
} ConfigPage;

typedef struct
{
  uint32_t magic;
  uint8_t value;
  uint8_t fill[3];
} XOTrim;

typedef struct
{
  uint32_t magic;
  uint8_t pa_level;
  uint8_t fill[3];
} TXConfig;


#endif /* INC_CONFIGFLAGS_H_ */
