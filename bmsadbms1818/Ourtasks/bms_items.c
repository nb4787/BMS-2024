/******************************************************************************
* File Name          : bms_items.c
* Date First Issued  : 07/14/2022
* Description        : routines associated BMSTask and bmsspi
*******************************************************************************/

#include "DTW_counter.h"
#include "main.h"
#include "morse.h"
#include "bms_items.h"

/* NOTE: DTEN =>pin<= is tied to +5V/1 */
#define DTMEN  0 // 1 = Enable discharge monitor =>since<= DTEN pin asserted. 0 = normal.
#define DTEN   1 // Discharge timer enable: 0 = disabled, 1 = enabled

#define REFON  1 // Reference remains on until watchdog timeout; 0 = shut after conversion 
#define ADCOPT 0 // Modes 27KHz, 7 KHz, 422 Hz, 25Hz, with MD bits[1:0] ADC conversion cmds
#define DCTO   5 // Discharge timeout code: 0 = disabled, 1 = 0.5 minutes, 5 = 4 minutes
#define FDRF   0 // 0 = normal; 1 = force digital redundancy for ADC conversion to fail
#define PSBITS 0 // Redundancy sequential
#define GPION  0x1FF // Config bits, 9 GPIOs: 0 = pull down on; 1 = off (default)
#define DCC0   0 // GPIO9 pulldown: 0 = OFF; 1 = pull down ON

static void uvov(uint8_t* p, uint8_t s);

/* *************************************************************************
 * void bms_items_cfgset_overunder(void);
 * @brief	: Configuration set: Compute and set over and under voltage comparisons
 * *************************************************************************/
void bms_items_cfgset_overunder(void)
{
	uint32_t vuv; // Under voltage
	uint32_t vov; // Over voltage 
	float fvuv;
	float fvov;
	/* Convert max & min voltage into '1818 regs: CFGARA1,2,3 */
	/* vuv and vov are 12 bit numbers (0-4095) packed into CFGAR1,2,3 .*/
	/* Datasheet formula:
	   Undervoltage comparison voltage = (VUV+1)*16*100 (uV)
	   Overvoltage  comparison voltage = VOV*16*100 (uV) */
	fvuv = ((bqfunction.lc.cellv_min * 1E6) / 1600.0)-1;
	fvov =  (bqfunction.lc.cellv_max * 1E6) / 1600.0;
	vuv = fvuv;
	vov = fvov;

	// CFGAR1 (low ord 8 bits of vuv)
	bmsspiall.configreg[0] &= 0x00ff;
	bmsspiall.configreg[0] |= (vuv << 8);

	// CFGAR2 (hi 24bits of vov | hi ord 4 bits of vuv)
	bmsspiall.configreg[1] = (vov << 4) | (vuv >> 8);
	return;
}
/* *************************************************************************
 * void bms_items_cfgset_dischargebits(uint32_t b);
 * @brief	: Configuration set: Update discharge bits in configreg (in memory)
 * @param   : b = bits for 18 cells (right justified): 0 = OFF, 1 = ON 
 * *************************************************************************/
void bms_items_cfgset_dischargebits(uint32_t b)
{
	b = (b & 0x3FFFF); // JIC
	// Update discharge low ord 12 bits in CFGAR 5,4
	bmsspiall.configreg[2] &= 0xF000; // Retain DCTO[0]-[3]
	bmsspiall.configreg[2] |= (b & 0x0FFF); // DCC1- DCC12

	// Update discharge hi ord 4 bits in CFGBR0
	bmsspiall.configreg[3] &= 0xFC0F; // Clear DCC13-DCC18
	bmsspiall.configreg[3] |= ((b >> 8) & 0x3F0);

	return;
}
/* *************************************************************************
 * void bms_items_cfgset_misc(void);
 * @brief	: Configuration set: misc bits (see #define above)
 * *************************************************************************/
void bms_items_cfgset_misc(void)
{
	bmsspiall.configreg[0] &= 0xFF00; // Retain VUV lo ord settings
	bmsspiall.configreg[0] |= (
		(ADCOPT << 0) |
		(DTEN   << 1) |
		(REFON  << 2) |
		((GPION & 0x1E) << 3) ); /* GPIO5-GPIO1 */

	bmsspiall.configreg[2] &= 0x0FFF; // Retain DCC12-DCC1 settings
	bmsspiall.configreg[2] |= (DCTO << 12); /* 4b code */

	bmsspiall.configreg[3] &= 0x03FF; // Retain DCC18-DCC13, GPIO9-GPIO6
	bmsspiall.configreg[3] |= (
		(DCC0   << 10) |
		(DTMEN  << 11) |
		(PSBITS << 12) |
		(FDRF   << 14) );
	return;
}
/* *************************************************************************
 * void bms_items_cfg_int(void);
 * @brief	: Configuration register initialize
 * *************************************************************************/
int32_t bshift = 0;
void bms_items_cfg_int(void)
{
	bms_items_cfgset_overunder();

	bms_items_cfgset_dischargebits(0);

	bms_items_cfgset_misc();
	return;
}
/* *************************************************************************
 * void bms_items_extract_statreg(void);
 * @brief	: Extract & calibrate SC, ITMP, VA
  * *************************************************************************/
 /* === for reference ===
struct EXTRACTSTATREG
{
	float    vov;   // Readback of overvoltage setting
	float    vuv;   // Readback of undervoltage setting
	float     sc;   // Sum of Cells (volts)
	float   itmp;   // Internal die temperature (deg C)
	float     va;   // Analog supply voltage (volts)
	float     vd;   // Digital supply (volts)
	uint32_t cov;   // Overvoltage bits 
	uint32_t cuv;   // Undervoltage bits
	uint8_t  rev;   // Revision code
	uint8_t  muxfail; 
	uint8_t  thsd;  // 1 = thermal shutdown 
};
*/
void bms_items_extract_statreg(void)
{
	uint8_t* pp;
	extractstatreg.sc   =  bmsspiall.statreg[0] * 3.0f;
	extractstatreg.itmp = (bmsspiall.statreg[1] / 76.0f) - 276.0f;
	extractstatreg.va   =  bmsspiall.statreg[2] * 0.001f;
	extractstatreg.vd   =  bmsspiall.statreg[3] * 0.001f;

	/* Extract under & overvoltage bits */
	pp = (uint8_t*)&bmsspiall.statreg[4];
	uvov((pp+0), 0);
	uvov((pp+1), 4);
	uvov((pp+2), 8);
	pp = (uint8_t*)&bmsspiall.auxreg[11];
	uvov((pp+0), 0);
	uvov((pp+1), 4);

	extractstatreg.rev  = (bmsspiall.statreg[5] >> 12);
	extractstatreg.muxfail = ((bmsspiall.statreg[5] >> 9) & 1);
	extractstatreg.thsd = ((bmsspiall.statreg[5] >> 8) & 1);
	return;
}
/* *************************************************************************
 * void bms_items_extract_configreg(void);
 * @brief	: Extract current configreg settings
 * *************************************************************************/
/*
struct EXTRACTCONFIGREG
{  // extract settings
	float    vov;   // Readback of overvoltage setting
	float    vuv;   // Readback of undervoltage setting
	uint32_t dcc;   // Discharge cell bits: 1 = ON
	uint16_t gpio;  // 9 GPIO's: 0 = pin at logic 0;
	uint16_t cfbr1; // bits: MUTE,FDRF,PS1,PS0,DTMEN,DCC0,0,0
};
*/
void bms_items_extract_configreg(void)
{
	uint32_t tmp;
	uint8_t* p = &bmsspiall.configreg[0];

	/* Overvoltage comparison setting (volts). */
	tmp  = ((*(p+2) >> 4) | ((uint32_t)*(p+3) << 12) );
	 extractconfigreg.vov = tmp * 16.0f;

	/* Undervoltage comparion setting (volts). */
	tmp= ( (uint32_t)(*(p+1)) | (( (uint32_t)(*(p+2) & 0x0F) << 8)) );
	extractconfigreg.vuv = (tmp +1) * 16.0f;

	/* Discharge cell bits, right justified */
	extractconfigreg.dcc = ( *(p+4) | 
		((uint32_t)(*(p+5) & 0x0F) << 8) |
		((uint32_t)(*p+6) << 8) );

	/* GPIO9-GPIO1 */
	extractconfigreg.gpio = *(p+0) >> 3 | ((uint16_t)(*(p+6) & 0x0F) << 5);
	
	/* Misc bits as datasheet shows: Table 55. CFGBR1 */
	extractconfigreg.cfbr1 = *(p+7); // dfbr1
	return;
}



static void uvov(uint8_t* p, uint8_t s)
{
	if ( (*p & 0x01) != 0)
		extractstatreg.cuv |= (1 << (s+0));
	if ( (*p & 0x04) != 0)
		extractstatreg.cuv |= (1 << (s+1));
	if ( (*p & 0x10) != 0)
		extractstatreg.cuv |= (1 << (s+2));
	if ( (*p & 0x40) != 0)
		extractstatreg.cuv |= (1 << (s+3));

	if ( (*p & 0x02) != 0)
		extractstatreg.cuv |= (1 << (s+0));
	if ( (*p & 0x08) != 0)
		extractstatreg.cuv |= (1 << (s+1));
	if ( (*p & 0x20) != 0)
		extractstatreg.cuv |= (1 << (s+2));
	if ( (*p & 0x80) != 0)
		extractstatreg.cuv |= (1 << (s+3));
	return;
}