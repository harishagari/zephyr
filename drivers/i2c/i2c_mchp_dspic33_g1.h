/*
 * Copyright (c) 2025, Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_I2C_MCHP_DSPIC33_G1_H_
#define ZEPHYR_DRIVERS_I2C_MCHP_DSPIC33_G1_H_

#include <xc.h>

/* I2C Register Offsets from base address */
#define I2C_OFFSET_CON1  ((&I2C1STAT1 - &I2C1CON1) * 0) /* 0x00 */
#define I2C_OFFSET_STAT1 ((&I2C1STAT1 - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_ADD   ((&I2C1ADD - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_MSK   ((&I2C1MSK - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_HBRG  ((&I2C1HBRG - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_TRN   ((&I2C1TRN - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_RCV   ((&I2C1RCV - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_CON2  ((&I2C1CON2 - &I2C1CON1) * sizeof(int))
#define I2C_OFFSET_LBRG  ((&I2C1LBRG - &I2C1CON1) * sizeof(int))

#define I2C_RCV_DATA_MASK 0xFFU

/* I2CxCON1 bit masks - host mode */
#define I2C_CON1_ON       _I2C1CON1_ON_MASK
#define I2C_CON1_SEN      _I2C1CON1_SEN_MASK
#define I2C_CON1_RSEN     _I2C1CON1_RSEN_MASK
#define I2C_CON1_PEN      _I2C1CON1_PEN_MASK
#define I2C_CON1_RCEN     _I2C1CON1_RCEN_MASK
#define I2C_CON1_ACKEN    _I2C1CON1_ACKEN_MASK
#define I2C_CON1_ACKDT    _I2C1CON1_ACKDT_MASK
#define I2C_CON1_DISSLW   _I2C1CON1_DISSLW_MASK

/* I2CxCON1 bit masks - target mode */
#define I2C_CON1_STREN    _I2C1CON1_STREN_MASK
#define I2C_CON1_SCLREL   _I2C1CON1_SCLREL_MASK
#define I2C_CON1_PCIE     _I2C1CON1_PCIE_MASK
#define I2C_CON1_SCIE     _I2C1CON1_SCIE_MASK
#define I2C_CON1_AHEN     _I2C1CON1_AHEN_MASK
#define I2C_CON1_DHEN     _I2C1CON1_DHEN_MASK

/* I2CxSTAT1 bit masks */
#define I2C_STAT1_ACKSTAT _I2C1STAT1_ACKSTAT_MASK
#define I2C_STAT1_TRSTAT  _I2C1STAT1_TRSTAT_MASK
#define I2C_STAT1_BCL     _I2C1STAT1_BCL_MASK
#define I2C_STAT1_IWCOL   _I2C1STAT1_IWCOL_MASK
#define I2C_STAT1_I2COV   _I2C1STAT1_I2COV_MASK

/* I2CxSTAT1 bit masks - target mode */
#define I2C_STAT1_DA      _I2C1STAT1_D_A_MASK
#define I2C_STAT1_P       _I2C1STAT1_P_MASK
#define I2C_STAT1_RW      _I2C1STAT1_R_W_MASK

#endif /* ZEPHYR_DRIVERS_I2C_MCHP_DSPIC33_G1_H_ */
