#ifndef USBTMC_H_
#define USBTMC_H_

	/* Includes: */
		#include <stdint.h>
		#include <stdbool.h>
		#include <string.h>
		#include "compat.h"

	/* Macros: */
		#define TMC_IO_EPSIZE            64
		#define TMC_NOTIFICATION_EPSIZE  2

		#define TMC_NOTIFICATION_EPADDR  0x81
		#define TMC_IN_EPADDR            0x82
		#define TMC_OUT_EPADDR           0x03

		/** \name TMC Class specific request constants */
		#define Req_InitiateAbortBulkOut         0x01
		#define Req_CheckAbortBulkOutStatus      0x02
		#define Req_InitiateAbortBulkIn          0x03
		#define Req_CheckAbortBulkInStatus       0x04
		#define Req_InitiateClear                0x05
		#define Req_CheckClearStatus             0x06
		#define Req_GetCapabilities              0x07
		#define Req_IndicatorPulse               0x40
		#define Req_ReadStatusByte               0x80
		#define Req_RenControl                   0xA0
		#define Req_GoToLocal                    0xA1
		#define Req_LocalLockout                 0xA2

		/** \name TMC status codes */
		#define TMC_STATUS_SUCCESS                0x01
		#define TMC_STATUS_PENDING                0x02
		#define TMC_STATUS_FAILED                 0x80
		#define TMC_STATUS_TRANSFER_NOT_IN_PROGRESS 0x81
		#define TMC_STATUS_SPLIT_NOT_IN_PROGRESS  0x82
		#define TMC_STATUS_SPLIT_IN_PROGRESS      0x83

		/** \name TMC bulk message IDs */
		#define TMC_MESSAGEID_DEV_DEP_MSG_OUT     0x01
		#define TMC_MESSAGEID_DEV_DEP_MSG_IN      0x02
		#define TMC_MESSAGEID_DEV_VENDOR_OUT      0x7E
		#define TMC_MESSAGEID_DEV_VENDOR_IN       0x7F
		#define TMC_MESSAGEID_TRIGGER             0x80

	/* Type Defines: */
		typedef struct
		{
			uint8_t LastMessageTransaction;
			uint8_t TermChar;
			uint8_t Reserved[2];
		} TMC_DevOUTMessageHeader_t;

		typedef struct
		{
			uint8_t LastMessageTransaction;
			uint8_t TermChar;
			uint8_t Reserved[2];
		} TMC_DevINMessageHeader_t;

		typedef struct
		{
			uint8_t  MessageID;
			uint8_t  Tag;
			uint8_t  InverseTag;
			uint8_t  Reserved;
			uint32_t TransferSize;

			union
			{
				TMC_DevOUTMessageHeader_t DeviceOUT;
				TMC_DevINMessageHeader_t  DeviceIN;
				uint32_t                  VendorSpecific;
			} MessageIDSpecific;
		} TMC_MessageHeader_t;


	/* Function Prototypes: */
		bool ReadTMCHeader(TMC_MessageHeader_t* const MessageHeader);
		bool WriteTMCHeader(TMC_MessageHeader_t* const MessageHeader);

		void usbtmc_task(void); /* from the main loop */

#endif
