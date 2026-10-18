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
#define CONFIG_FLAG_TX_INHIBIT      0x01u
#define CONFIG_FLAG_HRB_INHIBIT     0x02u

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
