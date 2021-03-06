/******************************************************************************
*
* Copyright (C) 2012 - 2014 Xilinx, Inc.  All rights reserved.
* 
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal 
* in the Software without restriction, including without limitation the rights 
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell  
* copies of the Software, and to permit persons to whom the Software is 
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in 
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications: 
* (a) running on a Xilinx device, or 
* (b) that interact with a Xilinx device through a bus or interconnect.  
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
* XILINX CONSORTIUM BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF 
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in 
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/
/*****************************************************************************/
/**
*
* @file qspi.c
*
* Contains code for the QSPI FLASH functionality.
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver	Who	Date		Changes
* ----- ---- -------- -------------------------------------------------------
* 1.00a ecm	01/10/10 Initial release
* 3.00a mb  25/06/12 InitQspi, data is read first and required config bits
*                    are set
* 4.00a sg	02/28/13 Cleanup
* 					 Removed LPBK_DLY_ADJ register setting code as we use
* 					 divisor 8
* 5.00a sgd	05/17/13 Added Flash Size > 128Mbit support
* 					 Dual Stack support
*					 Fix for CR:721674 - FSBL- Failed to boot from Dual
*					                     stacked QSPI
* 6.00a kc  08/30/13 Fix for CR#722979 - Provide customer-friendly
*                                        changelogs in FSBL
*                    Fix for CR#739711 - FSBL not able to read Large QSPI
*                    					 (512M) in IO Mode
* 7.00a kc  10/25/13 Fix for CR#739968 - FSBL should do the QSPI config
*                    					 settings for Dual parallel
*                    					 configuration in IO mode
*
* </pre>
*
* @note
*
******************************************************************************/

/***************************** Include Files *********************************/

#include "qspi.h"
#include "image_mover.h"
#include "sleep.h"

#ifdef XPAR_PS7_QSPI_LINEAR_0_S_AXI_BASEADDR
#include "xqspips_hw.h"
#include "xqspips.h"

#include "qspi_ctrl.h"
#include "qspi_flash_spansion.h"

#include "dbg_print.h"

/************************** Constant Definitions *****************************/

/*
 * The following constants map to the XPAR parameters created in the
 * xparameters.h file. They are defined here such that a user can easily
 * change all the needed parameters in one place.
 */
#define QSPI_DEVICE_ID		XPAR_XQSPIPS_0_DEVICE_ID


#define QSPI_TEST_NUM		1000000

/*
 * The following constants define the commands which may be sent to the FLASH
 * device.
 */
#define SINGLE_READ_CMD		0x03
#define FAST_READ_CMD		0x0B
#define DUAL_READ_CMD		0x3B
#define QUAD_READ_CMD		0x6B
#define READ_ID_CMD			0x9F

#define WRITE_ENABLE_CMD	0x06
#define WRITE_DISABLE_CMD	0x04
#define BANK_REG_RD			0x16
#define BANK_REG_WR			0x17
/* Bank register is called Extended Address Reg in Micron */
#define EXTADD_REG_RD		0xC8
#define EXTADD_REG_WR		0xC5

#define COMMAND_OFFSET		0 /* FLASH instruction */
#define ADDRESS_1_OFFSET	1 /* MSB byte of address to read or write */
#define ADDRESS_2_OFFSET	2 /* Middle byte of address to read or write */
#define ADDRESS_3_OFFSET	3 /* LSB byte of address to read or write */
#define DATA_OFFSET			4 /* Start of Data for Read/Write */
#define DUMMY_OFFSET		4 /* Dummy byte offset for fast, dual and quad
				     reads */
#define DUMMY_SIZE			1 /* Number of dummy bytes for fast, dual and
						 quad reads */
#define DUMMY_MAX_SIZE			8 /* max Number of dummy bytes for fast, dual and
						 quad reads */
#define RD_ID_SIZE			4 /* Read ID command + 3 bytes ID response */
#define BANK_SEL_SIZE		2 /* BRWR or EARWR command + 1 byte bank value */
#define WRITE_ENABLE_CMD_SIZE	1 /* WE command */
/*
 * The following constants specify the extra bytes which are sent to the
 * FLASH on the QSPI interface, that are not data, but control information
 * which includes the command and address
 */
#define OVERHEAD_SIZE		4

/*
 * The following constants specify the max amount of data and the size of the
 * the buffer required to hold the data and overhead to transfer the data to
 * and from the FLASH.
 */
#define DATA_SIZE		4096

/*
 * The following defines are for dual flash interface.
 */
#define LQSPI_CR_FAST_QUAD_READ		0x0000006B /* Fast Quad Read output */
#define LQSPI_CR_1_DUMMY_BYTE		0x00000100 /* 1 Dummy Byte between
						     address and return data */

#define SINGLE_QSPI_CONFIG_QUAD_READ	(XQSPIPS_LQSPI_CR_LINEAR_MASK | \
					 LQSPI_CR_1_DUMMY_BYTE | \
					 gu8_qspi_read_cmd)

#define DUAL_QSPI_CONFIG_QUAD_READ	(XQSPIPS_LQSPI_CR_LINEAR_MASK | \
					 XQSPIPS_LQSPI_CR_TWO_MEM_MASK | \
					 XQSPIPS_LQSPI_CR_SEP_BUS_MASK | \
					 LQSPI_CR_1_DUMMY_BYTE | \
					 gu8_qspi_read_cmd)

#define DUAL_STACK_CONFIG_READ	(XQSPIPS_LQSPI_CR_TWO_MEM_MASK | \
					 LQSPI_CR_1_DUMMY_BYTE | \
					 gu8_qspi_read_cmd)

#define SINGLE_QSPI_IO_CONFIG_QUAD_READ	(LQSPI_CR_1_DUMMY_BYTE | \
					 gu8_qspi_read_cmd)

#define DUAL_QSPI_IO_CONFIG_QUAD_READ	(XQSPIPS_LQSPI_CR_TWO_MEM_MASK | \
					 XQSPIPS_LQSPI_CR_SEP_BUS_MASK | \
					 LQSPI_CR_1_DUMMY_BYTE | \
					 gu8_qspi_read_cmd)


/**************************** Type Definitions *******************************/

typedef struct {
	u8 u8_cmd;	/**< Operational code of the instruction */
	u8 u8_dummy;	/**< Size of dummy bytes */
} XQspiCmdTest;


/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

/************************** Variable Definitions *****************************/

XQspiPs QspiInstance;
XQspiPs *QspiInstancePtr;
u32 QspiFlashSize;
u32 QspiFlashMake;
extern u32 FlashReadBaseAddress;
extern u8 LinearBootDeviceFlag;

/*
 * The following variables are used to read and write to the eeprom and they
 * are global to avoid having large buffers on the stack
 */
u8 ReadBuffer[DATA_SIZE + DATA_OFFSET + DUMMY_MAX_SIZE];
u8 WriteBuffer[DATA_OFFSET + DUMMY_MAX_SIZE];

u8 gu8_qspi_read_cmd=QUAD_READ_CMD;
u8 gu8_qspi_dummy_byte=DUMMY_SIZE;
u8 gu8_qspi_dump_raw_data_flag=1;
//u8 gu8_qspi_read_cmd=SINGLE_READ_CMD;


u32 gu32_qspi_config_ok_num[20]={};

#if 0
XQspiCmdTest  QspiCmdTestArray[] = { {0x6b, 1}, {0x6b, 0}, 
									{0x3b, 1}, {0x3b, 0}, 
									{0x03, 1}, {0x03, 0}, 
									{0xff, 0xff} };
#else
XQspiCmdTest  QspiCmdTestArray[] = { {0x6b, 1},  
									{0x3b, 1},
									{0x03, 0}, 
									{0xff, 0xff} };

#endif


#if 1

/******************************************************************************
*
* This functions selects the current bank
*
* @param	BankSel is the bank to be selected in the flash device(s).
*
* @return	XST_SUCCESS if bank selected
*			XST_FAILURE if selection failed
* @note		None.
*
*****************************************************************************/


u32 QspiCheckRead( u32 u32_correct_exit )
{
	//u32 Status;
	u32 u32_reg;
	u32 u32_loop=0;
	u32 u32_cfg_size=0;
	u32 u32_read_buffer[64];

	u32_cfg_size = sizeof(QspiCmdTestArray)/sizeof(XQspiCmdTest);
	for( u32_loop=0; ((u32_loop<1000)&&(u32_loop<u32_cfg_size)); u32_loop++ )
	{
		if( ( 0xff == QspiCmdTestArray[u32_loop].u8_cmd )
			|| ( 0xff == QspiCmdTestArray[u32_loop].u8_dummy ) )
		{
			xil_printf( "QSPI configuraiton check exits at No.%d loop.\n\r", u32_loop);
			break;
		}

		gu8_qspi_read_cmd=QspiCmdTestArray[u32_loop].u8_cmd;
		gu8_qspi_dummy_byte=QspiCmdTestArray[u32_loop].u8_dummy;
		u32_reg = XQspiPs_In32( 0xe000d000 + XQSPIPS_LQSPI_CR_OFFSET );
		u32_reg &= 0xfffff800;
		u32_reg |= gu8_qspi_read_cmd;
		u32_reg |= ((gu8_qspi_dummy_byte&0x3)<<8);
		XQspiPs_Out32( 0xe000d000 + XQSPIPS_LQSPI_CR_OFFSET, u32_reg );  /* XQSPIPS_LQSPI_CR_OFFSET: 0xa0 */
		xil_printf( "\n\r\n\rCheck QSPI data with command: %02x and dummy bytes:%d\n\r", 
					gu8_qspi_read_cmd, gu8_qspi_dummy_byte);

		QspiAccess( 0, (u32)u32_read_buffer, 128 );
		xil_printf( "QSPI data with command: %02x\n\r", gu8_qspi_read_cmd);
		dbg_mem_word_dump( (u32 *)u32_read_buffer, 128 );

		/* Macronix: work in linear mode.
	       The first 4 bytes is all 0xffffffff. 
		 */ 
		if( ( 0xeafffffe != u32_read_buffer[4] ) 
			|| ( 0xeafffffe != u32_read_buffer[5] )
			|| ( 0xeafffffe != u32_read_buffer[6] )
			|| ( 0xeafffffe != u32_read_buffer[7] )
			|| ( 0xaa995566 != u32_read_buffer[8] )
			|| ( 0x584c4e58 != u32_read_buffer[9] )
			)
		{
			xil_printf( "Caution: QSPI Flash data is wrong with command: %02x and dummy bytes:%d!!!!\n\r", 
					gu8_qspi_read_cmd, gu8_qspi_dummy_byte);
		}
		else
		{
			xil_printf( "QSPI Flash data is correct with command: %02x and dummy bytes:%d.\n\r", 
					gu8_qspi_read_cmd, gu8_qspi_dummy_byte);
			if( 0 != u32_correct_exit )
			{
				break;
			}
		}
	}

	//u32 DumpFirstBootSectionHeader( u32 ImageStartAddress );
	//DumpFirstBootSectionHeader( 0 );

	return XST_SUCCESS;
}


#endif




/******************************************************************************/
/**
*
* This function initializes the controller for the QSPI interface.
*
* @param	None
*
* @return	None
*
* @note		None
*
****************************************************************************/
u32 InitQspi(void)
{
	XQspiPs_Config *QspiConfig;
	int Status;
	//u32 u32_loop=0;
	//u32 u32_loop_flag=1;
	//u32 u32_loop_flag2=1;
	
	xil_printf( "Read Buffer address: 0x%08x.\n\r", (u32)ReadBuffer);
	xil_printf( "Write Buffer address:  0x%08x.\n\r", (u32)WriteBuffer);
	xil_printf( "QSPI data with command: %02x and dummy bytes:%d \n\r", 
					QUAD_READ_CMD, DUMMY_SIZE );

	QspiInstancePtr = &QspiInstance;

	/*
	 * Set up the base address for access
	 */
	FlashReadBaseAddress = XPS_QSPI_LINEAR_BASEADDR;

	/*
	 * Initialize the QSPI driver so that it's ready to use
	 */
	QspiConfig = XQspiPs_LookupConfig(QSPI_DEVICE_ID);
	if (NULL == QspiConfig) {
		return XST_FAILURE;
	}

	Status = XQspiPs_CfgInitialize(QspiInstancePtr, QspiConfig,
					QspiConfig->BaseAddress);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	/*
	 * Set Manual Chip select options and drive HOLD_B pin high.
	 */
	XQspiPs_SetOptions(QspiInstancePtr, XQSPIPS_FORCE_SSELECT_OPTION |
			XQSPIPS_HOLD_B_DRIVE_OPTION);
	/* XQSPIPS_MANUAL_START_OPTION cause zynq_qspi_poll_timeout in U-Boot */
	//XQspiPs_SetOptions(QspiInstancePtr, XQSPIPS_MANUAL_START_OPTION|XQSPIPS_FORCE_SSELECT_OPTION |
	//				XQSPIPS_HOLD_B_DRIVE_OPTION);

	/*
	 * Set the prescaler for QSPI clock
	 * XQSPIPS_CLK_PRESCALE_8: ~8MHz
	 */
	XQspiPs_SetClkPrescaler(QspiInstancePtr, XQSPIPS_CLK_PRESCALE_8);

	/*
	 * Assert the FLASH chip select.
	 */
	XQspiPs_SetSlaveSelect(QspiInstancePtr);

	/*
	 * Read Flash ID and extract Manufacture and Size information
	 */
	Status = FlashReadID();
	if (Status != XST_SUCCESS) {
		xil_printf( "Failed to read Flash ID for the first time.\n\r");
		//return XST_FAILURE;
	}


	QspiControllerSet( QspiInstancePtr );
	QspiFifoStatusCheck( QspiInstancePtr );
	QspiFlashSpansionInit( QspiInstancePtr );
	QspiFlashAllStatusShow( ); /* OK here for MicroZed board.*/

	if (XPAR_PS7_QSPI_0_QSPI_MODE == SINGLE_FLASH_CONNECTION) {

		fsbl_printf(DEBUG_INFO,"QSPI is in single flash connection\r\n");
		/*
		 * For Flash size <128Mbit controller configured in linear mode
		 */
		if (QspiFlashSize <= FLASH_SIZE_16MB) {
			LinearBootDeviceFlag = 1;

			/*
			 * Enable linear mode
			 */
			XQspiPs_SetOptions(QspiInstancePtr,  XQSPIPS_LQSPI_MODE_OPTION |
					XQSPIPS_HOLD_B_DRIVE_OPTION);

			/*
			 * Single linear read
			 */
			XQspiPs_SetLqspiConfigReg(QspiInstancePtr, SINGLE_QSPI_CONFIG_QUAD_READ);

			/*
			 * Enable the controller
			 */
			XQspiPs_Enable(QspiInstancePtr);
		} else {
			/*
			 * Single flash IO read
			 */
			XQspiPs_SetLqspiConfigReg(QspiInstancePtr, SINGLE_QSPI_IO_CONFIG_QUAD_READ);

			/*
			 * Enable the controller
			 */
			XQspiPs_Enable(QspiInstancePtr);
		}
	}

	if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_PARALLEL_CONNECTION) {

		fsbl_printf(DEBUG_INFO,"QSPI is in Dual Parallel connection\r\n");
		/*
		 * For Single Flash size <128Mbit controller configured in linear mode
		 */
		if (QspiFlashSize <= FLASH_SIZE_16MB) {
			/*
			 * Setting linear access flag
			 */
			LinearBootDeviceFlag = 1;

			/*
			 * Enable linear mode
			 */
			XQspiPs_SetOptions(QspiInstancePtr,  XQSPIPS_LQSPI_MODE_OPTION |
					XQSPIPS_HOLD_B_DRIVE_OPTION);

			/*
			 * Dual linear read
			 */
			XQspiPs_SetLqspiConfigReg(QspiInstancePtr, DUAL_QSPI_CONFIG_QUAD_READ);

			/*
			 * Enable the controller
			 */
			XQspiPs_Enable(QspiInstancePtr);
		} else {
			/*
			 * Dual flash IO read
			 */
			XQspiPs_SetLqspiConfigReg(QspiInstancePtr, DUAL_QSPI_IO_CONFIG_QUAD_READ);

			/*
			 * Enable the controller
			 */
			XQspiPs_Enable(QspiInstancePtr);

		}

		/*
		 * Total flash size is two time of single flash size
		 */
		QspiFlashSize = 2 * QspiFlashSize;
	}

	/*
	 * It is expected to same flash size for both chip selection
	 */
	if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_STACK_CONNECTION) {

		fsbl_printf(DEBUG_INFO,"QSPI is in Dual Stack connection\r\n");

		QspiFlashSize = 2 * QspiFlashSize;

		/*
		 * Enable two flash memories on separate buses
		 */
		XQspiPs_SetLqspiConfigReg(QspiInstancePtr, DUAL_STACK_CONFIG_READ);
	}


	QspiCheckRead(0);
	QspiCheckRead(1);
	//QspiFlashAllStatusShow( );  /* data abort here for MicroZed board. */

#if 0
#if 0
	for( ; 1==u32_loop_flag2; )
	{
		QspiCheckRead( );
	}
#endif
#endif

	// Disable debugging print information in QspiAccess()
	gu8_qspi_dump_raw_data_flag=0;

	return XST_SUCCESS;
}

/******************************************************************************
*
* This function reads serial FLASH ID connected to the SPI interface.
* It then deduces the make and size of the flash and obtains the
* connection mode to point to corresponding parameters in the flash
* configuration table. The flash driver will function based on this and
* it presently supports Micron and Spansion - 128, 256 and 512Mbit and
* Winbond 128Mbit
*
* @param	none
*
* @return	XST_SUCCESS if read id, otherwise XST_FAILURE.
*
* @note		None.
*
******************************************************************************/
u32 FlashReadID(void)
{
	u32 Status;

	/*
	 * Read ID in Auto mode.
	 */
	WriteBuffer[COMMAND_OFFSET]   = READ_ID_CMD;
	WriteBuffer[ADDRESS_1_OFFSET] = 0x00;		/* 3 dummy bytes */
	WriteBuffer[ADDRESS_2_OFFSET] = 0x00;
	WriteBuffer[ADDRESS_3_OFFSET] = 0x00;

	Status = XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, ReadBuffer,
				RD_ID_SIZE);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	fsbl_printf(DEBUG_INFO,"Single Flash Information\r\n");

	fsbl_printf(DEBUG_INFO,"FlashID=0x%x 0x%x 0x%x\r\n", ReadBuffer[1],
			ReadBuffer[2],
			ReadBuffer[3]);

	/*
	 * Deduce flash make
	 */
	if (ReadBuffer[1] == MICRON_ID) {
		QspiFlashMake = MICRON_ID;
		fsbl_printf(DEBUG_INFO, "MICRON ");
	} else if(ReadBuffer[1] == SPANSION_ID) {
		QspiFlashMake = SPANSION_ID;
		fsbl_printf(DEBUG_INFO, "SPANSION ");
	} else if(ReadBuffer[1] == WINBOND_ID) {
		QspiFlashMake = WINBOND_ID;
		fsbl_printf(DEBUG_INFO, "WINBOND ");
	}
	else
	{
		fsbl_printf(DEBUG_INFO, "Error Flash ID.\n\r");
	}


	/*
	 * Deduce flash Size
	 */
	if (ReadBuffer[3] == FLASH_SIZE_ID_128M) {
		QspiFlashSize = FLASH_SIZE_128M;
		fsbl_printf(DEBUG_INFO, "128M Bits\r\n");
	} else if (ReadBuffer[3] == FLASH_SIZE_ID_256M) {
		QspiFlashSize = FLASH_SIZE_256M;
		fsbl_printf(DEBUG_INFO, "256M Bits\r\n");
	} else if (ReadBuffer[3] == FLASH_SIZE_ID_512M) {
		QspiFlashSize = FLASH_SIZE_512M;
		fsbl_printf(DEBUG_INFO, "512M Bits\r\n");
	} else if (ReadBuffer[3] == FLASH_SIZE_ID_1G) {
		QspiFlashSize = FLASH_SIZE_1G;
		fsbl_printf(DEBUG_INFO, "1G Bits\r\n");
	}

	return XST_SUCCESS;
}



/******************************************************************************
*
* This function reads from the  serial FLASH connected to the
* QSPI interface.
*
* @param	Address contains the address to read data from in the FLASH.
* @param	ByteCount contains the number of bytes to read.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
void FlashRead(u32 Address, u32 ByteCount)
{
	/*
	 * Setup the write command with the specified address and data for the
	 * FLASH
	 */
	//	WriteBuffer[COMMAND_OFFSET]   = QUAD_READ_CMD;
	WriteBuffer[COMMAND_OFFSET]   = gu8_qspi_read_cmd;
	WriteBuffer[ADDRESS_1_OFFSET] = (u8)((Address & 0xFF0000) >> 16);
	WriteBuffer[ADDRESS_2_OFFSET] = (u8)((Address & 0xFF00) >> 8);
	WriteBuffer[ADDRESS_3_OFFSET] = (u8)(Address & 0xFF);

	ByteCount += gu8_qspi_dummy_byte;

	/*
	 * Send the read command to the FLASH to read the specified number
	 * of bytes from the FLASH, send the read command and address and
	 * receive the specified number of bytes of data in the data buffer
	 */
	XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, ReadBuffer,
				ByteCount + OVERHEAD_SIZE);
}

/******************************************************************************/
/**
*
* This function provides the QSPI FLASH interface for the Simplified header
* functionality.
*
* @param	SourceAddress is address in FLASH data space
* @param	DestinationAddress is address in DDR data space
* @param	LengthBytes is the length of the data in Bytes
*
* @return
*		- XST_SUCCESS if the write completes correctly
*		- XST_FAILURE if the write fails to completes correctly
*
* @note	none.
*
****************************************************************************/
u32 QspiAccess( u32 SourceAddress, u32 DestinationAddress, u32 LengthBytes)
{
	u8	*BufferPtr;
	u32 Length = 0;
	u32 BankSel = 0;
	u32 LqspiCrReg;
	u32 Status;
	u8 BankSwitchFlag = 1;

	/*
	 * Linear access check
	 */
	if (LinearBootDeviceFlag == 1) {
		/*
		 * Check for non-word tail, add bytes to cover the end
		 */
		if ((LengthBytes%4) != 0){
			LengthBytes += (4 - (LengthBytes & 0x00000003));
		}

		memcpy((void*)DestinationAddress,
		      (const void*)(SourceAddress + FlashReadBaseAddress),
		      (size_t)LengthBytes);
	} else {
		/*
		 * Non Linear access
		 */
		BufferPtr = (u8*)DestinationAddress;

		/*
		 * Dual parallel connection actual flash is half
		 */
		if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_PARALLEL_CONNECTION) {
			SourceAddress = SourceAddress/2;
		}

		while(LengthBytes > 0) {
			/*
			 * Local of DATA_SIZE size used for read/write buffer
			 */
			if(LengthBytes > DATA_SIZE) {
				Length = DATA_SIZE;
			} else {
				Length = LengthBytes;
			}

			/*
			 * Dual stack connection
			 */
			if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_STACK_CONNECTION) {
				/*
				 * Get the current LQSPI configuration value
				 */
				LqspiCrReg = XQspiPs_GetLqspiConfigReg(QspiInstancePtr);

				/*
				 * Select lower or upper Flash based on sector address
				 */
				if (SourceAddress >= (QspiFlashSize/2)) {
					/*
					 * Set selection to U_PAGE
					 */
					XQspiPs_SetLqspiConfigReg(QspiInstancePtr,
							LqspiCrReg | XQSPIPS_LQSPI_CR_U_PAGE_MASK);

					/*
					 * Subtract first flash size when accessing second flash
					 */
					SourceAddress = SourceAddress - (QspiFlashSize/2);

					fsbl_printf(DEBUG_INFO, "stacked - upper CS \n\r");

					/*
					 * Assert the FLASH chip select.
					 */
					XQspiPs_SetSlaveSelect(QspiInstancePtr);
				}
			}

			/*
			 * Select bank
			 */
			if ((SourceAddress >= FLASH_SIZE_16MB) && (BankSwitchFlag == 1)) {
				BankSel = SourceAddress/FLASH_SIZE_16MB;

				fsbl_printf(DEBUG_INFO, "Bank Selection %d\n\r", BankSel);

				Status = SendBankSelect(BankSel);
				if (Status != XST_SUCCESS) {
					fsbl_printf(DEBUG_INFO, "Bank Selection Failed\n\r");
					return XST_FAILURE;
				}

				BankSwitchFlag = 0;
			}

			/*
			 * If data to be read spans beyond the current bank, then
			 * calculate length in current bank else no change in length
			 */
			if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_PARALLEL_CONNECTION) {
				/*
				 * In dual parallel mode, check should be for half
				 * the length.
				 */
				if((SourceAddress & BANKMASK) != ((SourceAddress + (Length/2)) & BANKMASK))
				{
					Length = (SourceAddress & BANKMASK) + FLASH_SIZE_16MB - SourceAddress;
					/*
					 * Above length calculated is for single flash
					 * Length should be doubled since dual parallel
					 */
					Length = Length * 2;
					BankSwitchFlag = 1;
				}
			} else {
				if((SourceAddress & BANKMASK) != ((SourceAddress + Length) & BANKMASK))
				{
					Length = (SourceAddress & BANKMASK) + FLASH_SIZE_16MB - SourceAddress;
					BankSwitchFlag = 1;
				}
			}

			/*
			 * Copying the image to local buffer
			 */
			FlashRead(SourceAddress, Length);
#if 1
			if(1==gu8_qspi_dump_raw_data_flag)
			{
				u32 u32_length;
				u32_length = Length;
				if( u32_length>128 )
				{
					u32_length = 128;
				}
				xil_printf( "Raw Flash data with dummy bytes at source address: 0x%08x.\n\r", SourceAddress);
				dbg_mem_word_dump( (u32 *)ReadBuffer, 128);
			}
#endif

			/*
			 * Moving the data from local buffer to DDR destination address
			 */
			memcpy(BufferPtr, &ReadBuffer[DATA_OFFSET + gu8_qspi_dummy_byte], Length);

			/*
			 * Updated the variables
			 */
			LengthBytes -= Length;

			/*
			 * For Dual parallel connection address increment should be half
			 */
			if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_PARALLEL_CONNECTION) {
				SourceAddress += Length/2;
			} else {
				SourceAddress += Length;
			}

			BufferPtr = (u8*)((u32)BufferPtr + Length);
		}

		/*
		 * Reset Bank selection to zero
		 */
		Status = SendBankSelect(0);
		if (Status != XST_SUCCESS) {
			fsbl_printf(DEBUG_INFO, "Bank Selection Reset Failed\n\r");
			return XST_FAILURE;
		}

		if (XPAR_PS7_QSPI_0_QSPI_MODE == DUAL_STACK_CONNECTION) {

			/*
			 * Reset selection to L_PAGE
			 */
			XQspiPs_SetLqspiConfigReg(QspiInstancePtr,
					LqspiCrReg & (~XQSPIPS_LQSPI_CR_U_PAGE_MASK));

			fsbl_printf(DEBUG_INFO, "stacked - lower CS \n\r");

			/*
			 * Assert the FLASH chip select.
			 */
			XQspiPs_SetSlaveSelect(QspiInstancePtr);
		}
	}

	return XST_SUCCESS;
}



/******************************************************************************
*
* This functions selects the current bank
*
* @param	BankSel is the bank to be selected in the flash device(s).
*
* @return	XST_SUCCESS if bank selected
*			XST_FAILURE if selection failed
* @note		None.
*
******************************************************************************/
u32 SendBankSelect(u8 BankSel)
{
	u32 Status;

	/*
	 * bank select commands for Micron and Spansion are different
	 */
	if (QspiFlashMake == MICRON_ID)	{
		/*
		 * For micron command WREN should be sent first
		 * except for some specific feature set
		 */
		WriteBuffer[COMMAND_OFFSET] = WRITE_ENABLE_CMD;
		Status = XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, NULL,
				WRITE_ENABLE_CMD_SIZE);
		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}

		/*
		 * Send the Extended address register write command
		 * written, no receive buffer required
		 */
		WriteBuffer[COMMAND_OFFSET]   = EXTADD_REG_WR;
		WriteBuffer[ADDRESS_1_OFFSET] = BankSel;
		Status = XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, NULL,
				BANK_SEL_SIZE);
		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}
	}

	if (QspiFlashMake == SPANSION_ID) {
		WriteBuffer[COMMAND_OFFSET]   = BANK_REG_WR;
		WriteBuffer[ADDRESS_1_OFFSET] = BankSel;

		/*
		 * Send the Extended address register write command
		 * written, no receive buffer required
		 */
		Status = XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, NULL,
				BANK_SEL_SIZE);
		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}
	}

	/*
	 * For testing - Read bank to verify
	 */
	if (QspiFlashMake == SPANSION_ID) {
		WriteBuffer[COMMAND_OFFSET]   = BANK_REG_RD;
		WriteBuffer[ADDRESS_1_OFFSET] = 0x00;

		/*
		 * Send the Extended address register write command
		 * written, no receive buffer required
		 */
		Status = XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, ReadBuffer,
				BANK_SEL_SIZE);
		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}
	}

	if (QspiFlashMake == MICRON_ID) {
		WriteBuffer[COMMAND_OFFSET]   = EXTADD_REG_RD;
		WriteBuffer[ADDRESS_1_OFFSET] = 0x00;

		/*
		 * Send the Extended address register write command
		 * written, no receive buffer required
		 */
		Status = XQspiPs_PolledTransfer(QspiInstancePtr, WriteBuffer, ReadBuffer,
				BANK_SEL_SIZE);
		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}
	}

	if (ReadBuffer[1] != BankSel) {
		fsbl_printf(DEBUG_INFO, "BankSel %d != Register Read %d\n\r", BankSel,
				ReadBuffer[1]);
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}
#endif

