/******************************************************************************
* File Name          : CanCommTask.c
* Date First Issued  : 11/06/2021
* Description        : Can communications
*******************************************************************************/

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "malloc.h"

#include "main.h"
#include "morse.h"
#include "CanCommTask.h"
#include "MailboxTask.h"
#include "CanTask.h"
#include "can_iface.h"
#include "canfilter_setup.h"
#include "BQTask.h"
#include "cancomm_items.h"
#include "../../../../GliderWinchCommons/embed/svn_common/trunk/db/gen_db.h"

extern struct CAN_CTLBLOCK* pctl0; // Pointer to CAN1 control block
extern CAN_HandleTypeDef hcan1;

static void canfilt(uint16_t mm, struct MAILBOXCAN* p);

#define CANCOMMBIT00 (1 << 0) // Send cell voltage  command
#define CANCOMMBIT01 (1 << 1) // Send misc reading command
#define CANCOMMBIT02 (1 << 2) // Multi-purpose incoming command

struct CANRCVBUF  can_hb; // Dummy heart-beat request CAN msg
uint8_t hbseq; // heartbeat CAN msg sequence number
uint8_t rdyflag_cancomm = 0; // Initialization complete and ready = 1

TaskHandle_t CanCommHandle = NULL;
/* *************************************************************************
 * void StartCanComm(void const * argument);
 *	@brief	: Task startup
 * *************************************************************************/
void StartCanComm(void* argument)
{
//while(1==1) osDelay(100);

	/* We use the parameters from BQ for this task. */
	struct BQFUNCTION* p = &bqfunction;
	struct CANRCVBUF* pcan;

	/* A notification copies the internal notification word to this. */
	uint32_t noteval = 0;    // Receives notification word upon an API notify

	uint8_t i;
	uint8_t code;


	/* Add CAN Mailboxes                               CAN     CAN ID             TaskHandle,Notify bit,Skip, Paytype */
    p->pmbx_cid_cmd_bms_cellvq = MailboxTask_add(pctl0,p->lc.cid_cmd_bms_cellvq, NULL, CANCOMMBIT00,0,U8); // Command: send cell voltages
    if (p->pmbx_cid_cmd_bms_cellvq == NULL) morse_trap(620);
    p->pmbx_cid_cmd_bms_miscq  = MailboxTask_add(pctl0,p->lc.cid_cmd_bms_miscq,  NULL, CANCOMMBIT01,0,U8); // Command: many options	
    if (p->pmbx_cid_cmd_bms_miscq == NULL) morse_trap(621);
    p->pmbx_cid_uni_bms_i      = MailboxTask_add(pctl0,p->lc.cid_uni_bms_i,      NULL, CANCOMMBIT02,0,U8); // muli-purpose BQ76952  #01
    if (p->pmbx_cid_uni_bms_i == NULL) morse_trap(622);

    /* Add CAN msgs to incoming CAN filter. */
 //   canfilt(601, p->pmbx_cid_cmd_bms_cellvq);
 //   canfilt(602, p->pmbx_cid_cmd_bms_miscq);
 //   canfilt(603, p->pmbx_cid_uni_bms_i);

    	/* Pre-load fixed data in all possible CAN msgs */
	for (i = 0; i < NUMCANMSGS; i++)
	{
		p->canmsg[i].pctl = pctl0;   // Control block for CAN module (CAN 1)
		p->canmsg[i].maxretryct = 4; //
		p->canmsg[i].bits = 0;       //
		p->canmsg[i].can.dlc = 8;    // Default payload size (might be modified when loaded and sent)
	}

	/* Preload fixed data for sending cell reading msgs. */
	for (i = 0; i < MAXNUMCELLMSGS; i++)
	{	
		/* Preload CAN IDs */
		p->canmsg[CID_MSG_CELLV01 + i].can.id = p->lc.cid_msg_bms_cellvsmr;

		/* Msg identification = cell readings. */
		p->canmsg[CID_MSG_CELLV01 + i].can.cd.ui[0]  = CMD_CMD_TYPE1;//

		/* Preload cell_number-1 for first payload in cell reading msg. */
		// 0,3,6,9,12,15 in upper nibble; lower order nibble (seq number) = 0;
		p->canmsg[CID_MSG_CELLV01 + i].can.cd.ui[1] = ((i*3) << 4); 
	}

	/* Preload fixed data for sending "misc" readings. */
	p->canmsg[CID_CMD_MISC].can.id = p->lc.cid_msg_bms_cellvsmr;
	p->canmsg[CID_CMD_MISC].can.cd.uc[2] = p->ident_onlyus;

	/* Pre-load dummy CAN msg request for requesting heartbeat. */
	can_hb.id = p->lc.cid_uni_bms_i;
	can_hb.cd.ull = 0; // Clear entire payload
	can_hb.cd.uc[0] = CMD_CMD_TYPE1;  // request code
	can_hb.cd.uc[1] = 0;  // cell msg sequence number 
	can_hb.cd.uc[4] = p->lc.cid_msg_bms_cellvsmr >>  0; // Our CAN ID
	can_hb.cd.uc[5] = p->lc.cid_msg_bms_cellvsmr >>  8;
	can_hb.cd.uc[6] = p->lc.cid_msg_bms_cellvsmr >> 16;
	can_hb.cd.uc[7] = p->lc.cid_msg_bms_cellvsmr >> 24;

	while (CANTaskreadyflag == 0) osDelay(1);

extern CAN_HandleTypeDef hcan1;
	HAL_CAN_Start(&hcan1); // CAN1

	rdyflag_cancomm = 1; // Initialization complete and ready

	for (;;)
	{
		/* Wait for notifications */
		xTaskNotifyWait(0,0xffffffff, &noteval, p->hbct_k);

		/* CAN msg request for sending CELL VOLTAGES. */
			// Code for which modules should respond bits [7:6]
	   		// 11 = All modules respond
       		// 10 = All modules on identified string respond
       		// 01 = Only identified string and module responds
       		// 00 = spare; no response expected
		if ((noteval & CANCOMMBIT00) != 0)
		{
			pcan = &p->pmbx_cid_cmd_bms_cellvq->ncan.can;
			code = pcan->cd.uc[0] & 0xC0; // Extract identification code
			if (((code == (3 << 6))) ||
				((code == (2 << 6)) && ((pcan->cd.uc[0] & 0x30) == p->ident_string)) ||
				((code == (1 << 6)) && ((pcan->cd.uc[0] & 0x3F) == p->ident_onlyus)) )
			{ // Here, respond to request, otherwise, ignore.
				cancomm_items_sendcell(pcan);
			}
		}
		/* CAN msg request for sending MISC READINGS. */
			// Code for which modules should respond bits [7:6]
	   		// 11 = All modules respond
       		// 10 = All modules on identified string respond
       		// 01 = Only identified string and module responds
       		// 00 = spare; no response expected
		if ((noteval & CANCOMMBIT01) != 0)
		{
			pcan = &p->pmbx_cid_cmd_bms_miscq->ncan.can;
			code = pcan->cd.uc[0] & 0xC0; // Extract identification code
			if (((code == (3 << 6))) ||
				((code == (2 << 6)) && ((pcan->cd.uc[0] & 0x30) == p->ident_string)) ||
				((code == (1 << 6)) && ((pcan->cd.uc[0] & 0x3F) == p->ident_onlyus)) )
			{ // Here, respond to request
				cancomm_items_sendcmdr(pcan);
			}
		}
		/* Multi-purpose command */
		if ((noteval & CANCOMMBIT02) != 0)
		{
			cancomm_items_uni_bms(&p->pmbx_cid_uni_bms_i->ncan.can);
		}

		/* Timeout notification. */
		if (noteval == 0)
		{ // Send heartbeat
			/* Use dummy CAN msg, then it looks the same as a request CAN msg. */
			can_hb.cd.uc[0] = CMD_CMD_TYPE2;  // Misc subcommands code
			can_hb.cd.uc[1] = MISCQ_STATUS;   // status code
			cancomm_items_uni_bms(&can_hb);

			/* Use dummy CAN msg, then it looks the same as a request CAN msg. */
			can_hb.cd.uc[0] = CMD_CMD_TYPE1;  // cell readings code
			can_hb.cd.uc[1] = (hbseq & 0x0f); // Group sequence number
			cancomm_items_uni_bms(&can_hb);

			/* Increment 4 bit CAN msg group sequence counter .*/
			hbseq += 1;
	

   	//		xQueueSendToBack(CanTxQHandle,&p->canmsg[CID_CMD_MISC],4);   
		}	
	} 	
}

/* *************************************************************************
 * TaskHandle_t xCanCommCreate(uint32_t taskpriority);
 * @brief	: Create task; task handle created is global for all to enjoy!
 * @param	: taskpriority = Task priority (just as it says!)
 * @return	: CanCommHandle
 * *************************************************************************/
TaskHandle_t xCanCommCreate(uint32_t taskpriority)
{
	BaseType_t ret = xTaskCreate(StartCanComm, "CanComm",\
     (128), NULL, taskpriority,\
     &CanCommHandle);
	if (ret != pdPASS) return NULL;

	return CanCommHandle;
}
/* *************************************************************************
 * static void canfilt(uint16_t mm, struct MAILBOXCAN* p);
 * @brief	: Setup CAN hardware filter with CAN addresses to receive
 * @param	: p    = pointer to ContactorTask
 * @param   : mm = morse_trap numeric number
 * *************************************************************************/
static void canfilt(uint16_t mm, struct MAILBOXCAN* p)
{
/*	HAL_StatusTypeDef canfilter_setup_add32b_id(uint8_t cannum, CAN_HandleTypeDef *phcan, \
    uint32_t id,   \
    uint8_t  fifo );
 @brief	: Add a 32b id, advance bank number & odd/even
 * @param	: cannum = CAN module number 1, 2, or 3
 * @param	: phcan = Pointer to HAL CAN handle (control block)
 * @param	: id    = 32b CAN id
 * @param	: fifo  = fifo: 0 or 1
 * @return	: HAL_ERROR or HAL_OK
*/
	HAL_StatusTypeDef ret;	
	ret = canfilter_setup_add32b_id(1,&hcan1,p->ncan.can.id,0);
	if (ret == HAL_ERROR) morse_trap(mm);	
	return;
}
