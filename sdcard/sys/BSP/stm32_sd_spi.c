/******************************************************************************
 * @file    stm32_sd_spi.c
 * @author  Alexei Troussov
 * @version V1.0
 * @date    21-June-2012
 * @brief   High-level communication layer for read/write SD Card mounted on SPI bus.
 *
+--------------------------------------------------------------+
|                     Pin assignment                           |
+-------------------------+---------------+-------------+------+
|  STM32 SPI Pins         |     SD        |    Pin      | uCos |
+-------------------------+---------------+-------------+------+
| SD_SPI_CS_PIN           |   ChipSelect  |    1        | PB11 |
| SD_SPI_MOSI_PIN / MOSI  |   DataIn      |    2        | PB15 |
|                         |   GND         |    3 (0 V)  |      |
|                         |   VDD         |    4 (3.3 V)|      |
| SD_SPI_SCK_PIN / SCLK   |   Clock       |    5        | PB13 |
|                         |   GND         |    6 (0 V)  |      |
| SD_SPI_MISO_PIN / MISO  |   DataOut     |    7        | PB14 |
+-------------------------+---------------+-------------+------+
===============================================================================================================
    | SPI                           | 1-bit SD bus                    | 4-bit SD bus
Pin	| Name    I/O   Desc            | Name    I/O     Desc            | Name    I/O      Desc
===============================================================================================================
 1  | nCS     Ipp   Card Select     | NC      -       NC              | D3      Ipp/Opp  Data 3
 2  | DI      Ipp   Data In (MOSI)  | CMD     Ipp/Ood Cmd/Reply       | CMD     Ipp/Ood  Cmd/Reply
 3  | GND     S     Ground          | GND     S       Ground          | GND     S        Ground
 4  | VDD     S     Power           | VDD     S       Power           | VDD     S        Power
 5  | CLK     Ipp   Clock (SCLK)    | CLK     Ipp     Clock           | CLK     Ipp      Clock
 6  | GND     S     Ground          | GND     S       Ground          | GND     S        Ground
 7  | DO      Opp   Data Out (MISO) | D0      Ipp/Opp Data 0          | D0      Ipp/Opp  Data 0
 8  | NC/nIRQ -/Ood NC(SD)/IRQ(SDIO)| NC/nIRQ -/Ood   NC(SD)/IRQ(SDIO)| D1/nIRQ IOpp/Ood Data 1(+IRQ for SDIO)
 9  | NC      -     -               | NC      -       -               | D2      Ipp/Opp  Data 2
===============================================================================================================
	pp - Push-pull logic
	od - Open-drain logic
===============================================================================================================
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/

#include "main.h"

#include "stm32_sd_spi.h"

#include "stm32_spi.h"
#include <stdio.h>

/** @addtogroup Utilities
 * @{
 */


/** @defgroup STM32_Private_Types
 * @{
 */

/**
 * @brief  Type of SD card
 */
typedef enum _SDCardType
{
	SD_Card_MMC,	/*!< Multimedia card (no CMD8, no ACMD41, but CMD1, uses byte-addressing) */
	SD_Card_SDSC_v1,/*!< Standard Capacity card v1 (no CMD8, but ACMD41, uses byte-addressing) */
	SD_Card_SDSC_v2,/*!< Standard Capacity card v2 (has CMD8+ACMD41, uses byte-addressing) */
	SD_Card_SDHC	/*!< High Capacity card (has CMD8+ACMD41, uses sector-addressing) */
} SDCardType;

/**
 * @brief  Data response sent for CMD24
 */
typedef enum _SD_DataResponse
{
	SD_RESPONSE_MASK          = 0x0E,	/*!< any response value bits have to be masked by this */
	SD_RESPONSE_ACCEPTED      = 0x04,	/*!< data accepted */
	SD_RESPONSE_REJECTED_CRC  = 0x0A,	/*!< data rejected due to CRC error */
	SD_RESPONSE_REJECTED_ERR  = 0x0C	/*!< data rejected due to write error */
} SD_DataResponse;

/**
 * @brief  Data response error
 */
typedef enum _SD_DataError
{
	SD_DATA_TOKEN_OK          = 0x00,
	SD_DATA_TOKEN_ERROR       = 0x01,
	SD_DATA_TOKEN_CC_ERROR    = 0x02,
	SD_DATA_TOKEN_ECC_FAILURE = 0x04,
	SD_DATA_TOKEN_OUT_OF_RANGE= 0x08,
	SD_DATA_TOKEN_CARD_LOCKED = 0x10,
} SD_DataError;

/**
 * @brief  Commands: CMDxx = CMD-number | 0x40
 */
/*
	class 0 (basic):
		CMD0		CMD2		CMD3		CMD4
		CMD7		CMD8		CMD9		CMD10
		CMD11		CMD12		CMD13		CMD15
	class 2 (block read):
		CMD16		CMD17		CMD18		CMD19		CMD20		CMD23
	class 4 (block write):
		CMD16		CMD20		CMD23		CMD24		CMD25		CMD27
	class 5 (erase):
		CMD32		CMD33		CMD38
	class 6 (write protection):
		CMD28		CMD29		CMD30
	class 7 (lock card):
		CMD16		CMD40		CMD42
	class 8 (application-specific):
		CMD55		CMD56
		ACMD6		ACMD13	ACMD22	ACMD23	ACMD41	ACMD42	ACMD51
	class 9 (I/O mode):
		CMD5		CMD52		CMD53
	class 10 (switch):
		CMD6		CMD34		CMD35		CMD36		CMD37		CMD50		CMD57
	all other classes are reserved
 */
typedef enum _SD_CMD
{
	SD_CMD_GO_IDLE_STATE		=  0, /*!< CMD0  = 0x40, ARG=0x00000000, CRC=0x95 */
	SD_CMD_SEND_OP_COND			=  1, /*!< CMD1  = 0x41 */
	SD_CMD_SEND_IF_COND			=  8, /*!< CMD8  = 0x48, ARG=0x000001AA, CRC=0x87 */
	SD_CMD_SEND_APP				= 55, /*!< CMD55 = 0x77, ARG=0x00000000, CRC=0x65 */
	SD_CMD_ACTIVATE_INIT		= 41, /*!< ACMD41= 0x69, ARG=0x40000000, CRC=0x77 */
	SD_CMD_READ_OCR				= 58, /*!< CMD58 = 0x7A, ARG=0x00000000, CRC=0xFF */
	SD_CMD_SEND_CSD				=  9, /*!< CMD9  = 0x49 */
	SD_CMD_SEND_CID				= 10, /*!< CMD10 = 0x4A */
	SD_CMD_SEND_SCR				= 51, /*!< ACMD51= 0x73 */
	SD_CMD_STATUS				= 13, /*!< ACMD13= 0x4D */
	SD_CMD_STOP_TRANSMISSION	= 12, /*!< CMD12 = 0x4C */
	SD_CMD_SET_BLOCKLEN			= 16, /*!< CMD16 = 0x50 */
	SD_CMD_READ_SINGLE_BLOCK	= 17, /*!< CMD17 = 0x51 */
	SD_CMD_READ_MULT_BLOCK		= 18, /*!< CMD18 = 0x52 */
	SD_CMD_SET_BLOCK_COUNT		= 23, /*!< CMD23 = 0x57 */
	SD_CMD_WRITE_SINGLE_BLOCK	= 24, /*!< CMD24 = 0x58 */
	SD_CMD_WRITE_MULT_BLOCK		= 25, /*!< CMD25 = 0x59 */
	SD_CMD_ERASE_BLOCK_START	= 32, /*!< CMD32 = 0x60 */
	SD_CMD_ERASE_BLOCK_END		= 33, /*!< CMD33 = 0x61 */
	SD_CMD_ERASE				= 38  /*!< CMD38 = 0x66 */
} SD_CMD;

/**
 * @}
 *//* STM32_Private_Types */


/** @defgroup STM32_Private_Defines
 * @{
 */

/**
 * @brief  Dummy byte
 */
#define SD_DUMMY_BYTE		0xFF

/**
 * @brief  Number of 8-bit cycles for RUMP UP phase
 */
#define SD_NUM_TRIES_RUMPUP	((uint32_t)2500)

/**
 * @brief  Maximum number of tries to send a command
 */
#define SD_NUM_TRIES		((uint16_t)300)

/**
 * @brief  Maximum number of tries until ACMD41/CMD1 initializes SD card
 * It means a time until "In Idle State" flag is set during initialization
 * For information:
 * ~ 11000 for Kingston 4Gb
 * ~ 10000 for SanDisk  1Gb
 * ~  6000 for Samsung  8Gb
 */
#define SD_NUM_TRIES_INIT	((uint16_t)20000)

/**
 * @brief  Maximum number of tries to receive data transmission token
 * It means a time before data transmission starts
 * For information:
 * ~  300 for SanDisk  1Gb
 * ~  600 for Kingston 4Gb
 * ~  900 for SP       4Gb
 * ~  500 for Samsung  8Gb
 * ~  300 for Lexar    4Gb
 */
#define SD_NUM_TRIES_READ	((uint16_t)2000)

/**
 * @brief  Maximum number of tries until SD card writes data
 * It means a time while BUSY flag is set, i.e. card writes the data received
 * For information:
 * ~  6100 for Kingston 4Gb
 * ~  4600 for Lexar    4Gb
 * ~ 80000 for SP       4Gb (9000)
 * ~ 10000 for SanDisk  1Gb
 * ~119000 for Samsung  8Gb
 */
#define SD_NUM_TRIES_WRITE	((uint32_t)1000000)

/**
 * @brief  Maximum number of tries until SD card erases data
 * It means a time while BUSY flag is set, i.e. card erases sectors
 * For information:
 * ~  6100 for Kingston 4Gb
 * ~  5200 for Lexar    4Gb
 * ~ 10300 for SP       4Gb
 * ~   N/A for SanDisk  1Gb
 * ~120000 for Samsung  8Gb
 */
#define SD_NUM_TRIES_ERASE	((uint32_t)1000000)

/**
 * @brief  Start Data tokens:
 *         Tokens (necessary because at nop/idle (and CS active) only 0xff is
 *         on the data/command line)
 */
#define SD_DATA_BLOCK_READ_START           0xFE  /*!< Data token start byte, Start Single/Multiple Block Read */
#define SD_DATA_SINGLE_BLOCK_WRITE_START   0xFE  /*!< Data token start byte, Start Single Block Write */
#define SD_DATA_MULTIPLE_BLOCK_WRITE_START 0xFC  /*!< Data token start byte, Start Multiple Block Write */
#define SD_DATA_MULTIPLE_BLOCK_WRITE_STOP  0xFD  /*!< Data token stop byte, Stop Multiple Block Write */

/**
 * @}
 *//* STM32_Private_Defines */

/** @defgroup STM32_Private_Macros
 * @{
 */

/**
 * @brief  Write a byte on the SD.
 * @param  Data: byte to send.
 * @retval None
 */
#define SD_WriteByte( b )	STM_EVAL_SPI_Send_Recieve_Data( (b) )

/**
 * @brief  Read a byte from the SD.
 * @param  None
 * @retval The received byte.
 */
#define SD_ReadByte()		STM_EVAL_SPI_Send_Recieve_Data( SD_DUMMY_BYTE )

/**
 * @}
 *//* STM32_Private_Macros */


/** @defgroup STM32_Private_Variables
 * @{
 */

SDCardType cardType;

/**
 * @}
 *//* STM32_Private_Variables */


/** @defgroup STM32_Private_Function_Prototypes
 * @{
 */
/**
 * @}
 *//* STM32_Private_Function_Prototypes */


/** @defgroup STM32_Private_Functions
 * @{
 */

/**
 * @brief  Send a command to SD card and receive R1 response
 * @param  Cmd: Command to send to SD card
 * @param  Arg: Command argument
 * @param  Crc: CRC
 * @retval R1 response byte
 */
static SD_Error SD_SendCmd( uint8_t cmd, uint32_t arg, uint8_t crc )
{
	uint8_t res;
	uint16_t i = SD_NUM_TRIES;
	/* send a command */
	SD_WriteByte( (cmd & 0x3F) | 0x40 );	/*!< byte 1 */
	SD_WriteByte( (uint8_t)(arg >> 24) );	/*!< byte 2 */
	SD_WriteByte( (uint8_t)(arg >> 16) );	/*!< byte 3 */
	SD_WriteByte( (uint8_t)(arg >> 8) );	/*!< byte 4 */
	SD_WriteByte( (uint8_t)arg );			/*!< byte 5 */
	SD_WriteByte( crc | 0x01 );				/*!< byte 6: CRC */
	/* a byte received immediately after CMD12 should be discarded... */
	if ( cmd == SD_CMD_STOP_TRANSMISSION )
		SD_ReadByte();
	/* SD Card responds within Ncr (response time),
	   which is 0-8 bytes for SDSC cards, 1-8 bytes for MMC cards */
	do {
		res = SD_ReadByte();
		/* R1 response always starts with 7th bit set to 0 */
	} while ( ( res & SD_CHECK_BIT ) != 0x00 && i-- > 0);
	return (SD_Error)res;
}

/**
 * @brief  Get 4 bytes of R3 or R7 response
 * @param  pres: Pointer to uint32_t variable for result
 * @retval None
 */
static void SD_GetResponse4b( uint8_t* pres )
{
	pres[ 3 ] = SD_ReadByte();
	pres[ 2 ] = SD_ReadByte();
	pres[ 1 ] = SD_ReadByte();
	pres[ 0 ] = SD_ReadByte();
}

/**
 * @brief  Set SD Card sector size to SD_CMD_SET_BLOCKLEN (512 bytes)
 * @param  New sector size
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
static SD_Error SD_FixSectorSize( uint16_t ssize )
{
	return SD_SendCmd( SD_CMD_SET_BLOCKLEN, (uint32_t)ssize, 0xFF );
}

/**
 * @brief  Some commands take longer time and respond with R1b response,
 *         so we have to wait until 0xFF recieved (MISO is set to HIGH)
 * @retval Nonzero if required state wasn't recieved
 */
static SD_Error SD_WaitReady( void )
{
	uint16_t i = SD_NUM_TRIES;
	while ( i-- > 0 )
	{
		if ( SD_ReadByte() == 0xFF )
		{
//printf( " [[ WAIT delay %d ]] ", SD_NUM_TRIES - i );
			return SD_RESPONSE_NO_ERROR;
		}
	}
//printf( " [[ WAIT delay was not enough ]] " );
	return SD_RESPONSE_FAILURE;
}

/**
 * @brief  Wait until data transmission token is received
 * @retval Data transmission token or 0xFF if timeout occured
 */
static uint8_t SD_WaitBytesRead( void )
{
	uint16_t i = SD_NUM_TRIES_READ;
	uint8_t b;
	do {
		b = SD_ReadByte();
	} while ( b == 0xFF && i-- > 0 );

	if ( b != 0xFF )
		printf( " [[ READ delay %d ]] ", SD_NUM_TRIES_READ - i );
	else
		printf( " [[ READ delay was not enough ]] " );

	return b;
}

/**
 * @brief  Writing data into flash takes even longer time and it responds with R1b response,
 *         so we have to wait until 0xFF recieved (MISO is set to HIGH)
 * @retval Nonzero if required state wasn't recieved
 */
static SD_Error SD_WaitBytesWritten( void )
{
	uint32_t i = SD_NUM_TRIES_WRITE;
	while ( i-- > 0 )
	{
		if ( SD_ReadByte() == 0xFF )
		{
			printf( " [[ WRITE delay %lu ]] ", SD_NUM_TRIES_WRITE - i );
			return SD_RESPONSE_NO_ERROR;
		}
	}
	printf( " [[ WRITE delay was not enough ]] " );
	return SD_RESPONSE_FAILURE;
}

/**
 * @brief  Erasing data into flash takes some time and it responds with R1b response,
 *         so we have to wait until 0xFF recieved (MISO is set to HIGH)
 * @retval Nonzero if required state wasn't recieved
 */
static SD_Error SD_WaitBytesErased( void )
{
	uint32_t i = SD_NUM_TRIES_ERASE;
	while ( i-- > 0 )
	{
		if ( SD_ReadByte() == 0xFF )
		{
			printf( " [[ ERASE delay %lu ]] ", SD_NUM_TRIES_ERASE - i );
			return SD_RESPONSE_NO_ERROR;
		}
	}
	printf( " [[ ERASE delay was not enough ]] " );
	return SD_RESPONSE_FAILURE;
}

/**
 * @brief  Hold SPI bus for SD card
 * @param  None
 * @retval None
 */
static void SD_Bus_Hold( void )
{	/* Select SD Card: set SD chip select pin low */
	GPIO_ResetBits( SD_CS_GPIO_PORT, SD_CS_PIN );
}

/**
 * @brief  Release SPI bus used by SD card
 * @param  None
 * @retval None
 */
static void SD_Bus_Release( void )
{	/* Deselect SD Card: set SD chip select pin high */
	GPIO_SetBits( SD_CS_GPIO_PORT, SD_CS_PIN );
	SD_ReadByte();	/* send dummy byte: 8 Clock pulses of delay */
}

/**
 * @brief  Put SD in Idle state.
 * @param  None
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
static SD_Error SD_GoIdleState( void )
{
	uint32_t res = 0;
	uint16_t i;
	uint8_t state;

	/* --- put SD card in SPI mode */
	SD_Bus_Hold();

	i = SD_NUM_TRIES;	/* reset try count... */
	do {	/* loop until In Idle State Response (in R1 format) confirmation */
		state = SD_SendCmd( SD_CMD_GO_IDLE_STATE, 0x00000000, 0x95 ); /* valid CRC is mandatory here */
	} while ( state != SD_IN_IDLE_STATE && i-- > 0 );
	/* still no Idle State Response => return response failure */
	if ( state != SD_IN_IDLE_STATE )
		return SD_RESPONSE_FAILURE;

	/* --- SD card is now in idle state and in SPI mode, activate it and get its type */
	cardType = SD_Card_SDSC_v2;

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* --- try to send CMD8 to offer voltage 2.7-3.6V with check pattern 0xAA */
	i = SD_NUM_TRIES;	/* reset try count... */
	do {
		state = SD_SendCmd( SD_CMD_SEND_IF_COND, 0x000001AA, 0x87 ); /* valid CRC is mandatory here */
		if ( ( state & SD_ILLEGAL_COMMAND ) != 0 )
		{	/* SD card doesn't accept CMD8 => it's SDSC or MMC card... */
			cardType = SD_Card_SDSC_v1;
			break;
		}
		else/* SD card accepts CMD8 => it's SDHC or SDXC card... */
		{	/* get R7 response and verify pattern for sanity check... */
			SD_GetResponse4b( (uint8_t*)&res );
			if ( ( res & 0x0000FFFF ) == 0x000001AA )
				break;	/* check pattern is OK, card accepted offered voltage... */
			/* else specification recommends to retry CMD8 again */
		}
	} while ( i-- > 0 );
	if ( i == 0 )
		return SD_RESPONSE_FAILURE;	/* error occurred... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* --- activate card initialization sequence... */
	/* CMD55(0) -> ACMD41(0) -> ... */
	i = SD_NUM_TRIES_INIT;	/* reset try count... */
	do {
		state = SD_SendCmd( SD_CMD_SEND_APP, 0x00000000, 0x65 );
		if ( state != SD_IN_IDLE_STATE )
		{	/* error occurred => last chance is to try it as a legacy MMC card */
			cardType = SD_Card_MMC;
			break;
		}

		state = SD_WaitReady();	/* make sure card is ready before we go further... */

		if ( cardType == SD_Card_SDSC_v1 )		/* HCS bit (0 here) is ignored by SDSC card */
			state = SD_SendCmd( SD_CMD_ACTIVATE_INIT, 0x00000000, 0xFF );
		else
			state = SD_SendCmd( SD_CMD_ACTIVATE_INIT, 0x40000000, 0x77 );
		/* loop while SD_IN_IDLE_STATE bit is set, meaning card is still performing initialization */
	} while ( ( state & SD_IN_IDLE_STATE ) != 0x00 && i-- > 0 );
	/* it might be legacy MMC card... */
	if ( cardType == SD_Card_SDSC_v1 &&
			( state & SD_IN_IDLE_STATE ) != 0x00 )
		cardType = SD_Card_MMC;

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	if ( cardType == SD_Card_MMC )	/* legacy MMC card is initialized with CMD1... */
	{	/* -> CMD1(0) -> ... */
		i = SD_NUM_TRIES_INIT; /* reset try count... */
		do {
			state = SD_SendCmd( SD_CMD_SEND_OP_COND, 0x00000000, 0xFF );
		} while ( ( state & SD_IN_IDLE_STATE ) != 0x00 && i-- > 0 );
		if ( i == 0 )
			return SD_RESPONSE_FAILURE;	/* error occurred... */
	}
	else if ( cardType == SD_Card_SDSC_v2 ) /* recent cards support byte-addressing, check it... */
	{	/* -> CMD58(0)... */
		if ( i == 0 )	/* first check if timeout occured during its initialization... */
			return SD_RESPONSE_FAILURE;	/* error occurred... */
		/* request OCR register (send CMD58)... */
		state = SD_SendCmd( SD_CMD_READ_OCR, 0x00000000, 0xFF );
		if ( state == SD_RESPONSE_NO_ERROR )
		{ /* get OCR register (R3 response) and check its CCS (bit 30) */
			SD_GetResponse4b( (uint8_t*)&res );
			cardType = ( res & 0x40000000 ) ? SD_Card_SDHC : SD_Card_SDSC_v2;
		}
	}
	/* else cardType == SD_Card_SDSC_v1 */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* print out detected SD card type... */
	switch ( cardType )
	{
	case SD_Card_SDSC_v1:	printf( "SDSC v1 (byte address)" ); break;
	case SD_Card_SDSC_v2:	printf( "SDSC v2 (byte address)" ); break;
	case SD_Card_SDHC:		printf( "SDHC (512-bytes sector address)" ); break;
	case SD_Card_MMC:		printf( "MMC (byte address)" ); break;
	default:				printf( "UNKNOWN" ); break;
	}
	printf( " card initialized successfully\n" );

	return SD_RESPONSE_NO_ERROR;
}

/**
 * @brief  Recieve data from SD Card
 * @param  data: Pre-allocated data buffer
 * @param  len: Number of bytes to receive
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
static SD_Error SD_ReceiveData( uint8_t *data, uint16_t len )
{
	uint16_t i = 0;
	uint8_t b;

	/* some cards need time before transmitting the data... */
	b = SD_WaitBytesRead();

	if ( b != 0xFF )
	{	/* most cards send transmission start token, don't fail if it's not the case... */
		data[ i ] = b;
		if ( data[ i ] == SD_DATA_BLOCK_READ_START ) /* 0xFE */
			data[ i ] = SD_ReadByte();	/* just get the next byte... */

		/* receive the rest of data... */
		for ( i = 1; i < len; ++i )
			data[ i ] = SD_ReadByte();

		/* get CRC bytes (not really needed by us, but required by SD) */
		SD_ReadByte();
		SD_ReadByte();

		return SD_RESPONSE_NO_ERROR;
	}
	return SD_RESPONSE_FAILURE;
}

/**
 * @brief  Read the CSD card register.
 *         Reading the contents of the CSD register in SPI mode is a simple
 *         read-block transaction.
 * @param  SD_csd: pointer on an SCD register structure
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
static SD_Error SD_GetCSDRegister( SD_CSD* SD_csd )
{
	SD_Error state;
	uint8_t CSD_Tab[ 16 ];

	state = SD_WaitReady();	/* make sure card is ready before we go further... */
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;

	/* request CSD register (send CMD9)... */
	state = SD_SendCmd( SD_CMD_SEND_CSD, 0x00000000, 0xFF );
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;
	state = SD_ReceiveData( CSD_Tab, 16 );	/* receive CSD register data */

	SD_csd->CSDStruct = ( CSD_Tab[ 0 ] & 0xC0 ) >> 6;				/* Byte 0 */
	SD_csd->SysSpecVersion = ( CSD_Tab[ 0 ] & 0x3C ) >> 2;
	SD_csd->Reserved1 = CSD_Tab[ 0 ] & 0x03;
	SD_csd->TAAC = CSD_Tab[ 1 ];									/* Byte 1 */
	SD_csd->NSAC = CSD_Tab[ 2 ];									/* Byte 2 */
	SD_csd->MaxBusClkFrec = CSD_Tab[ 3 ];							/* Byte 3 */
	SD_csd->CardComdClasses = CSD_Tab[ 4 ] << 4;					/* Byte 4 */
	SD_csd->CardComdClasses |= ( CSD_Tab[ 5 ] & 0xF0 ) >> 4;		/* Byte 5 */
	SD_csd->RdBlockLen = CSD_Tab[ 5 ] & 0x0F;
	SD_csd->PartBlockRead = ( CSD_Tab[ 6 ] & 0x80 ) >> 7;			/* Byte 6 */
	SD_csd->WrBlockMisalign = ( CSD_Tab[ 6 ] & 0x40 ) >> 6;
	SD_csd->RdBlockMisalign = ( CSD_Tab[ 6 ] & 0x20 ) >> 5;
	SD_csd->DSRImpl = ( CSD_Tab[ 6 ] & 0x10 ) >> 4;
	SD_csd->Reserved2 = ( CSD_Tab[ 6 ] & 0x0C ) >> 2;
	if ( SD_csd->CSDStruct == 0 )
	{	/* v1 */
		SD_csd->DeviceSize = ( CSD_Tab[ 6 ] & 0x03 ) << 10;	/* DeviceSize has 12 bits here */
		SD_csd->DeviceSize |= CSD_Tab[ 7 ] << 2;					/* Byte 7 */
		SD_csd->DeviceSize |= ( CSD_Tab[ 8 ] & 0xC0 ) >> 6;			/* Byte 8 */
		SD_csd->MaxRdCurrentVDDMin = ( CSD_Tab[ 8 ] & 0x38 ) >> 3;
		SD_csd->MaxRdCurrentVDDMax = CSD_Tab[ 8 ] & 0x07;
		SD_csd->MaxWrCurrentVDDMin = ( CSD_Tab[ 9 ] & 0xE0 ) >> 5;	/* Byte 9 */
		SD_csd->MaxWrCurrentVDDMax = ( CSD_Tab[ 9 ] & 0x1C ) >> 2;
		SD_csd->DeviceSizeMul = ( CSD_Tab[ 9 ] & 0x03 ) << 1;
		SD_csd->DeviceSizeMul |= ( CSD_Tab[ 10 ] & 0x80 ) >> 7;		/* Byte 10 */
	}
	else
	{	/* v2 */
		SD_csd->Reserved5 = ( CSD_Tab[ 6 ] & 0x03 ) << 2;
		SD_csd->Reserved5 |= ( CSD_Tab[ 7 ] & 0xC0 ) >> 6;			/* Byte 7 */
		SD_csd->DeviceSize = ( CSD_Tab[ 7 ] & 0x3F ) << 16;	/* DeviceSize has 22 bits here */
		SD_csd->DeviceSize |= CSD_Tab[ 8 ] << 8;					/* Byte 8 */
		SD_csd->DeviceSize |= CSD_Tab[ 9 ];							/* Byte 9 */
		SD_csd->Reserved6 = ( CSD_Tab[ 10 ] & 0x80 ) >> 7;			/* Byte 10 */
	}
	SD_csd->EraseBlockEnable = ( CSD_Tab[ 10 ] & 0x40 ) >> 6;
	SD_csd->EraseSectorSize = ( CSD_Tab[ 10 ] & 0x3F ) << 1;
	SD_csd->EraseSectorSize |= ( CSD_Tab[ 11 ] & 0x80 ) >> 7;		/* Byte 11 */
	SD_csd->WrProtectGrSize = CSD_Tab[ 11 ] & 0x7F;
	SD_csd->WrProtectGrEnable = ( CSD_Tab[ 12 ] & 0x80 ) >> 7;		/* Byte 12 */
	SD_csd->ManDeflECC = ( CSD_Tab[ 12 ] & 0x60 ) >> 5;
	SD_csd->WrSpeedFact = ( CSD_Tab[ 12 ] & 0x1C ) >> 2;
	SD_csd->MaxWrBlockLen = ( CSD_Tab[ 12 ] & 0x03 ) << 2;
	SD_csd->MaxWrBlockLen |= ( CSD_Tab[ 13 ] & 0xC0 ) >> 6;			/* Byte 13 */
	SD_csd->WriteBlockPaPartial = ( CSD_Tab[ 13 ] & 0x20 ) >> 5;
	SD_csd->Reserved3 = CSD_Tab[ 13 ] & 0x1E;
	SD_csd->ContentProtectAppli = CSD_Tab[ 13 ] & 0x01;
	SD_csd->FileFormatGroup = ( CSD_Tab[ 14 ] & 0x80 ) >> 7;		/* Byte 14 */
	SD_csd->CopyFlag = ( CSD_Tab[ 14 ] & 0x40 ) >> 6;
	SD_csd->PermWrProtect = ( CSD_Tab[ 14 ] & 0x20 ) >> 5;
	SD_csd->TempWrProtect = ( CSD_Tab[ 14 ] & 0x10 ) >> 4;
	SD_csd->FileFormat = ( CSD_Tab[ 14 ] & 0x0C ) >> 2;
	SD_csd->ECC = CSD_Tab[ 14 ] & 0x03;
	SD_csd->CSD_CRC = ( CSD_Tab[ 15 ] & 0xFE ) >> 1;				/* Byte 15 */
	SD_csd->Reserved4 = CSD_Tab[ 15 ] & 0x01;

	return state;
}

/**
 * @brief  Read the CID card register.
 *         Reading the contents of the CID register in SPI mode is a simple
 *         read-block transaction.
 * @param  SD_cid: pointer on an CID register structure
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
static SD_Error SD_GetCIDRegister( SD_CID* SD_cid )
{
	SD_Error state;
	uint8_t CID_Tab[ 16 ];

	state = SD_WaitReady();	/* make sure card is ready before we go further... */
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;

	/* request CID register (send CMD10)... */
	state = SD_SendCmd( SD_CMD_SEND_CID, 0x00000000, 0xFF );
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;
	state = SD_ReceiveData( CID_Tab, 16 );	/* receive CID register data */

	SD_cid->ManufacturerID = CID_Tab[ 0 ];				/* Byte 0 */
	SD_cid->OEM_AppliID = CID_Tab[ 1 ] << 8;			/* Byte 1 */
	SD_cid->OEM_AppliID |= CID_Tab[ 2 ];				/* Byte 2 */
	SD_cid->ProdName1 = CID_Tab[ 3 ] << 24;				/* Byte 3 */
	SD_cid->ProdName1 |= CID_Tab[ 4 ] << 16;			/* Byte 4 */
	SD_cid->ProdName1 |= CID_Tab[ 5 ] << 8;				/* Byte 5 */
	SD_cid->ProdName1 |= CID_Tab[ 6 ];					/* Byte 6 */
	SD_cid->ProdName2 = CID_Tab[ 7 ];					/* Byte 7 */
	SD_cid->ProdRev = CID_Tab[ 8 ];						/* Byte 8 */
	SD_cid->ProdSN = CID_Tab[ 9 ] << 24;				/* Byte 9 */
	SD_cid->ProdSN |= CID_Tab[ 10 ] << 16;				/* Byte 10 */
	SD_cid->ProdSN |= CID_Tab[ 11 ] << 8;				/* Byte 11 */
	SD_cid->ProdSN |= CID_Tab[ 12 ];					/* Byte 12 */
	SD_cid->Reserved1 |= ( CID_Tab[ 13 ] & 0xF0 ) >> 4;	/* Byte 13 */
	SD_cid->ManufactDate = ( CID_Tab[ 13 ] & 0x0F ) << 8;
	SD_cid->ManufactDate |= CID_Tab[ 14 ];				/* Byte 14 */
	SD_cid->CID_CRC = ( CID_Tab[ 15 ] & 0xFE ) >> 1;	/* Byte 15 */
	SD_cid->Reserved2 = 1;

	return state;
}

/**
 * @brief  Read the SCR card register.
 *         Reading the contents of the SCR register in SPI mode is a simple
 *         read-block transaction.
 * @param  SD_scr: pointer on an SCR register structure
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
static SD_Error SD_GetSCRRegister( SD_SCR* SD_scr )
{
	SD_Error state;
	uint8_t SCR_Tab[ 8 ];

	if ( cardType == SD_Card_MMC )
	{
		printf( "SCR Register is not available for MMC cards\n" );
		return SD_ILLEGAL_COMMAND;
	}

	state = SD_WaitReady();	/* make sure card is ready before we go further... */
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;

	/* request SCR register (send ACMD51)... */
	state = SD_SendCmd( SD_CMD_SEND_APP, 0x00000000, 0x65 );
	if ( state == SD_RESPONSE_NO_ERROR )
		state = SD_SendCmd( SD_CMD_SEND_SCR, 0x00000000, 0xFF );
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;
	state = SD_ReceiveData( SCR_Tab, 8 );	/* receive SCR register data */

	SD_scr->SCR_Version = ( SCR_Tab[ 0 ] & 0xF0 ) >> 4;		/* Byte 0 */
	SD_scr->SpecVersion = SCR_Tab[ 0 ] & 0x0F;
	SD_scr->StateAfterErase = ( SCR_Tab[ 1 ] & 0x80 ) >> 7;	/* Byte 1 */
	SD_scr->Security = ( SCR_Tab[ 1 ] & 0x70 ) >> 4;
	SD_scr->BusWidth = SCR_Tab[ 1 ] & 0x0F;
	SD_scr->SpecVersion3 = ( SCR_Tab[ 2 ] & 0x80 ) >> 7;	/* Byte 2 */
	SD_scr->ExSecurity = ( SCR_Tab[ 2 ] & 0x78 ) >> 3;
	SD_scr->Reserved1 = ( SCR_Tab[ 2 ] & 0x07 ) << 6;
	SD_scr->Reserved1 |= ( SCR_Tab[ 3 ] & 0xFC ) >> 2;		/* Byte 3 */
	SD_scr->CmdSupport1 = ( SCR_Tab[ 3 ] & 0x02 ) >> 1;
	SD_scr->CmdSupport2 = SCR_Tab[ 3 ] & 0x01;
	SD_scr->Reserved2 = SCR_Tab[ 4 ] << 24;					/* Byte 4 */
	SD_scr->Reserved2 |= SCR_Tab[ 5 ] << 16;				/* Byte 5 */
	SD_scr->Reserved2 |= SCR_Tab[ 6 ] << 8;					/* Byte 6 */
	SD_scr->Reserved2 |= SCR_Tab[ 7 ];						/* Byte 7 */

	return state;
}

/**
 * @}
 *//* STM32_Private_Functions */


/** @defgroup STM32_Public_Functions
 * @{
 */

/**
 * @brief  Detect if SD card is correctly plugged in the memory slot.
 * @param  None
 * @retval Return if SD is detected or not
 */
uint8_t SD_Detect( void )
{	/* check GPIO to detect SD */
	if ( GPIO_ReadInputData( SD_DETECT_GPIO_PORT ) & SD_DETECT_PIN )
		return SD_NOT_PRESENT;
	return SD_PRESENT;
}

/**
 * @brief  DeInitializes the SD Card
 * @param  None
 * @retval None
 */
void SD_DeInit( void )
{	/* just shutdown SPI bus - disable SD CS Clock */
	SD_CS_GPIO_CLK_INIT( SD_CS_GPIO_CLK, DISABLE );
}

/**
 * @brief  Initializes the SD Card
 * @param  None
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_Init( void )
{
	GPIO_InitTypeDef GPIO_InitStructure;
	SD_Error state;
	uint32_t i = 0;

	/* step 0:
	 * Check if SD card is present... */
	if ( SD_Detect() == SD_NOT_PRESENT )
		return SD_RESPONSE_FAILURE;

	/* step 1:
	 * Initialize SD card-related pins on SPI bus */
	SD_CS_GPIO_CLK_INIT( SD_CS_GPIO_CLK, ENABLE );	/* enable SD CS clock... */
	GPIO_InitStructure.GPIO_Pin = SD_CS_PIN;		/* configure SD CS pin... */
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init( SD_CS_GPIO_PORT, &GPIO_InitStructure );

	/* step 2:
	 * Card is now powered up (i.e. 1ms at least elapsed at 0.5V),
	 * Supply rump up time (set MOSI HIGH) to let voltage reach stable 2.2V at least.
	 * According to the specs it must be 74 SPI clock cycles minimum at 100-400Khz
	 * At 25Mhz it'll be 250 times more cycles => send 2500 times 0xFF byte.
	 * Chip Select pin should be set HIGH too. */

	/* set SD chip select pin high */
	GPIO_SetBits( SD_CS_GPIO_PORT, SD_CS_PIN );
	/* send dummy byte 0xFF (rise MOSI high for 2500*8 SPI bus clock cycles) */
	while ( i++ < SD_NUM_TRIES_RUMPUP )
		SD_WriteByte( SD_DUMMY_BYTE );

	/* step 3:
	 * Put SD in SPI mode & perform soft reset */
	state = SD_GoIdleState();

	/* step 4:
	 * Force sector size to SD_BLOCK_SIZE (i.e. 512 bytes) */
	if ( state == SD_RESPONSE_NO_ERROR && cardType != SD_Card_SDHC )
		state = SD_FixSectorSize( (uint16_t)SD_BLOCK_SIZE );

	/* step 5:
	 * Release SPI bus for other devices */
	SD_Bus_Release();

	return state;
}

/**
 * @brief  Reads a sector of SD_BLOCK_SIZE bytes from the SD card
 * @param  pBuffer: pointer to the buffer that receives the data read from SD.
 * @param  readAddr: SD's internal address to read from (sector number)
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_SectorRead( uint32_t readAddr, uint8_t* pBuffer )
{
	SD_Error state;

	printf( "--> reading sector %lu ...", readAddr );

	/* non High Capacity cards use byte-oriented addresses */
	if ( cardType != SD_Card_SDHC )
		readAddr <<= 9;

	SD_Bus_Hold();		/* hold SPI bus... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* send CMD17 (SD_CMD_READ_SINGLE_BLOCK) to read one block */
	state = SD_SendCmd( SD_CMD_READ_SINGLE_BLOCK, readAddr, 0xFF );
	/* receive data if command acknowledged... */
	if ( state == SD_RESPONSE_NO_ERROR )
		state = SD_ReceiveData( pBuffer, SD_BLOCK_SIZE );

	SD_Bus_Release();	/* release SPI bus... */

	if ( state == SD_RESPONSE_NO_ERROR )
		printf( "OK\n" );
	else
		printf( "KO(%d)\n", state );

	return state;
}

/**
 * @brief  Reads multiple sectors of SD_BLOCK_SIZE bytes from the SD card
 * @param  pBuffer: pointer to the buffer that receives the data read from SD.
 * @param  readAddr: SD's internal address to read from.
 * @param  nbSectors: number of blocks to be read.
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_SectorsRead( uint32_t readAddr, uint8_t* pBuffer, uint32_t nbSectors )
{
	SD_Error state;

	printf( "--> reading %lu sectors from %lu ...", nbSectors, readAddr );

	/* non High Capacity cards use byte-oriented addresses */
	if ( cardType != SD_Card_SDHC )
		readAddr <<= 9;

	SD_Bus_Hold();		/* hold SPI bus... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* send CMD18 (SD_CMD_READ_MULT_BLOCK) to read multiple blocks */
	state = SD_SendCmd( SD_CMD_READ_MULT_BLOCK, readAddr, 0xFF );
	if ( state == SD_RESPONSE_NO_ERROR )
	{	/* receive data... */
		while ( nbSectors-- > 0 )
		{
			state = SD_ReceiveData( pBuffer, SD_BLOCK_SIZE );
			if ( state != SD_RESPONSE_NO_ERROR )
				break;
			pBuffer += SD_BLOCK_SIZE;
		}
		/* transmission is open-ended (no block count was set) =>
		 * send CMD12 (SD_CMD_STOP_TRANSMISSION) to stop it... */
		state = SD_SendCmd( SD_CMD_STOP_TRANSMISSION, 0x00000000, 0xFF );
	}

	SD_Bus_Release();	/* release SPI bus... */

	if ( state == SD_RESPONSE_NO_ERROR )
		printf( "OK\n" );
	else
		printf( "KO(%d)\n", state );

	return state;
}

/**
 * @brief  Writes a sector of SD_BLOCK_SIZE bytes on the SD card
 * @param  pBuffer: pointer to the buffer with the data to be written on SD.
 * @param  writeAddr: address to write on.
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_SectorWrite( uint32_t writeAddr, const uint8_t* pBuffer )
{
	SD_Error state;
	SD_DataResponse res;
	uint16_t BlockSize = SD_BLOCK_SIZE;

	printf( "--> writing sector %lu ...", writeAddr );

	/* non High Capacity cards use byte-oriented addresses */
	if ( cardType != SD_Card_SDHC )
		writeAddr <<= 9;

	SD_Bus_Hold();		/* hold SPI bus... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* send CMD24 (SD_CMD_WRITE_SINGLE_BLOCK) to write single block */
	state = SD_SendCmd( SD_CMD_WRITE_SINGLE_BLOCK, writeAddr, 0xFF );
	if ( state == SD_RESPONSE_NO_ERROR )
	{	/* wait at least 8 clock cycles (send >=1 0xFF bytes) before transmission starts */
		SD_ReadByte();
		SD_ReadByte();
		SD_ReadByte();
		/* send data token to signify the start of data transmission... */
		SD_WriteByte( SD_DATA_SINGLE_BLOCK_WRITE_START ); /* 0xFE */
		/* send data... */
		while ( BlockSize-- > 0 )
			SD_WriteByte( *pBuffer++ );
		/* put 2 CRC bytes (not really needed by us, but required by SD) */
		SD_ReadByte();
		SD_ReadByte();
		/* check data response... */
		res = (SD_DataResponse)( SD_ReadByte() & SD_RESPONSE_MASK );	/* mask unused bits */
		if ( ( res & SD_RESPONSE_ACCEPTED ) != 0 )
		{	/* card is now processing data and goes to BUSY mode, wait until it finishes... */
			state = SD_WaitBytesWritten();	/* make sure card is ready before we go further... */
		}
		else
			state = SD_RESPONSE_FAILURE;
	}

	SD_Bus_Release();	/* release SPI bus... */

	if ( state == SD_RESPONSE_NO_ERROR )
		printf( "OK\n" );
	else
		printf( "KO(%d)\n", state );

	return state;
}

/**
 * @brief  Writes multiple sectors of SD_BLOCK_SIZE bytes on the SD card
 * @param  pBuffer: pointer to the buffer with the data to be written on the SD.
 * @param  writeAddr: address to write on.
 * @param  nbSectors: number of blocks to be written.
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_SectorsWrite( uint32_t writeAddr, const uint8_t* pBuffer, uint32_t nbSectors )
{
	SD_Error state;
	SD_DataResponse res;
	uint16_t BlockSize = SD_BLOCK_SIZE;

	printf( "--> writing %lu sectors at %lu ...", nbSectors, writeAddr );

	/* non High Capacity cards use byte-oriented addresses */
	if ( cardType != SD_Card_SDHC )
		writeAddr <<= 9;

	SD_Bus_Hold();		/* hold SPI bus... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* it is recommended to specify in advance the number of blocks being written
	 * to let SD card erase needed number of blocks, write operation should take less time then */
	if ( cardType != SD_Card_MMC )
	{	/* notify card about the total number of blocks to be sent (send CMD23)... */
		state = SD_SendCmd( SD_CMD_SET_BLOCK_COUNT, (uint32_t)nbSectors, 0xFF );
		if ( state != SD_RESPONSE_NO_ERROR )
		{
			SD_Bus_Release();	/* release SPI bus... */
			return state;
		}
	}

	/* request writing data starting from the given address (send CMD25)... */
	state = SD_SendCmd( SD_CMD_WRITE_MULT_BLOCK, writeAddr, 0xFF );
	if ( state == SD_RESPONSE_NO_ERROR )
	{
		/* send some dummy bytes before transmission starts... */
		SD_ReadByte();
		SD_ReadByte();
		SD_ReadByte();
		/* transfer data... */
		while ( nbSectors-- > 0 && state != SD_RESPONSE_FAILURE )
		{	/* --- {{ send data packet */
			SD_WriteByte( SD_DATA_MULTIPLE_BLOCK_WRITE_START ); /* 0xFC */
			/* send data... */
			while ( BlockSize-- > 0 )
				SD_WriteByte( *pBuffer++ );
			/* put CRC bytes (not really needed by us, but required by SD) */
			SD_ReadByte();
			SD_ReadByte();
			/* --- }} */
			/* check data response... */
			res = (SD_DataResponse)( SD_ReadByte() & SD_RESPONSE_MASK );	/* mask unused bits */
			if ( ( res & SD_RESPONSE_ACCEPTED ) != 0 )
			{	/* card is now processing data and goes to BUSY mode, wait until it finishes... */
				state = SD_WaitBytesWritten();	/* make sure card is ready before we go further... */
			}
			else
				state = SD_RESPONSE_FAILURE;
		}
		/* notify SD card that we finished sending data to write on it */
		SD_WriteByte( SD_DATA_MULTIPLE_BLOCK_WRITE_STOP ); /* 0xFD */
		SD_ReadByte(); /* read and discard 1 byte from card */
		/* card is now processing data and goes to BUSY mode, wait until it finishes... */
		state = SD_WaitReady();	/* make sure card is ready before we go further... */
	}

	SD_Bus_Release();	/* release SPI bus... */

	if ( state == SD_RESPONSE_NO_ERROR )
		printf( "OK\n" );
	else
		printf( "KO(%d)\n", state );

	return state;
}

/**
 * @brief  Erase specified range of sectors on SD card
 * @param  eraseAddrFrom: Starting sector number
 * @param  eraseAddrTo: End sector number
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_SectorsErase( uint32_t eraseAddrFrom, uint32_t eraseAddrTo )
{
	SD_Error state;

	if ( cardType == SD_Card_MMC )
	{
		printf( "--> erasing sectors is not supported for MMC cards\n" );
		return SD_ILLEGAL_COMMAND;
	}

	printf( "--> erasing sectors from %lu to %lu ...", eraseAddrFrom, eraseAddrTo );

	/* non High Capacity cards use byte-oriented addresses */
	if ( cardType != SD_Card_SDHC )
	{
		eraseAddrFrom <<= 9;
		eraseAddrTo <<= 9;
	}

	SD_Bus_Hold();		/* hold SPI bus... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */

	/* send starting block address (CMD32)... */
	state = SD_SendCmd( SD_CMD_ERASE_BLOCK_START, (uint32_t)eraseAddrFrom, 0xFF );
	if ( state == SD_RESPONSE_NO_ERROR )
	{	/* send end block address (CMD33)... */
		state = SD_SendCmd( SD_CMD_ERASE_BLOCK_END, (uint32_t)eraseAddrTo, 0xFF );
		if ( state == SD_RESPONSE_NO_ERROR )
		{	/* erase all selected blocks (CMD38)... */
			state = SD_SendCmd( SD_CMD_ERASE, 0x00000000, 0xFF );
			if ( state == SD_RESPONSE_NO_ERROR )
			{	/* wait until sectors get erased... */
				state = SD_WaitBytesErased();	/* make sure card is ready before we go further... */
			}
		}
	}

	SD_Bus_Release();	/* release SPI bus... */

	if ( state == SD_RESPONSE_NO_ERROR )
		printf( "OK\n" );
	else
		printf( "KO(%d)\n", state );

	return state;
}

/**
 * @brief  Retrieve current SD card status structure
 * @param  SD_status: pointer on an SD_Status structure
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_GetStatus( SD_Status* SD_status )
{
	SD_Error state;
	uint8_t status[ 64 ];

	if ( cardType == SD_Card_MMC )
	{
		printf( "SD card status is not available for MMC cards\n" );
		return SD_ILLEGAL_COMMAND;
	}

	SD_Bus_Hold();		/* hold SPI bus... */

	state = SD_WaitReady();	/* make sure card is ready before we go further... */
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;

	/* request SD card status (send ACMD13)... */
	state = SD_SendCmd( SD_CMD_SEND_APP, 0x00000000, 0x65 );
	if ( state == SD_RESPONSE_NO_ERROR )
		state = SD_SendCmd( SD_CMD_STATUS, 0x00000000, 0xFF );
	if ( state != SD_RESPONSE_NO_ERROR )
		return SD_RESPONSE_FAILURE;
	state = SD_ReceiveData( status, 64 );	/* receive SD card status data */

	SD_Bus_Release();	/* release SPI bus... */

	SD_status->BusWidth = ( status[ 0 ] & 0xC0 ) >> 6;		/* Byte 0 */
	SD_status->InSecuredMode = ( status[ 0 ] & 0x20 ) >> 5;
	SD_status->Reserved1 = ( status[ 0 ] & 0x1F ) << 8;
	SD_status->Reserved1 |= status[ 1 ];					/* Byte 1 */
	SD_status->CardType = status[ 2 ] << 8;					/* Byte 2 */
	SD_status->CardType |= status[ 3 ];						/* Byte 3 */
	SD_status->SizeProtectedArea = status[ 4 ] << 24;		/* Byte 4 */
	SD_status->SizeProtectedArea |= status[ 5 ] << 16;		/* Byte 5 */
	SD_status->SizeProtectedArea |= status[ 6 ] << 8;		/* Byte 6 */
	SD_status->SizeProtectedArea |= status[ 7 ];			/* Byte 7 */
	SD_status->SpeedClass = status[ 8 ];					/* Byte 8 */
	SD_status->PerformanceMove = status[ 9 ];				/* Byte 9 */
	SD_status->AU_Size = ( status[ 10 ] & 0xF0 ) >> 4;		/* Byte 10 */
	SD_status->Reserved2 = status[ 10 ] & 0x0F;
	SD_status->EraseSize = status[ 11 ] << 8;				/* Byte 11 */
	SD_status->EraseSize |= status[ 12 ];					/* Byte 12 */
	SD_status->EraseTimeout = ( status[ 13 ] & 0xFC ) >> 2;	/* Byte 13 */
	SD_status->EraseOffset = status[ 13 ] & 0x03;
	SD_status->UHS_SpeedGrade = ( status[ 14 ] & 0xF0 ) >> 4;/* Byte 14 */
	SD_status->UHS_AU_Size = status[ 14 ] & 0x0F;

	return state;
}

/**
 * @brief  Returns information about specific card.
 * @param  cardinfo: pointer to a SD_CardInfo structure that contains all SD
 *         card information.
 * @retval The SD Response:
 *         - SD_RESPONSE_FAILURE: Sequence failed
 *         - SD_RESPONSE_NO_ERROR: Sequence succeed
 */
SD_Error SD_GetCardInfo( SD_CardInfo *cardinfo )
{
	SD_Error status;

	SD_Bus_Hold();		/* hold SPI bus... */

	status = SD_GetCSDRegister( &(cardinfo->SD_csd) );
	if ( status == SD_RESPONSE_NO_ERROR )
		status = SD_GetCIDRegister( &(cardinfo->SD_cid) );
	if ( status == SD_RESPONSE_NO_ERROR && cardType != SD_Card_MMC )
		status = SD_GetSCRRegister( &(cardinfo->SD_scr) );

	SD_Bus_Release();	/* release SPI bus... */

	if ( status == SD_RESPONSE_NO_ERROR )
	{	/* to avoid overflow, card capacity is calculated in Kbytes */
		if ( cardinfo->SD_csd.CSDStruct == 0 )
		{	// v1:
			cardinfo->CardCapacity = cardinfo->SD_csd.DeviceSize + 1;
			cardinfo->CardCapacity *= ( 1 << ( cardinfo->SD_csd.DeviceSizeMul + 2 ) );
			cardinfo->CardBlockSize = 1 << cardinfo->SD_csd.RdBlockLen;
			if ( cardinfo->SD_csd.RdBlockLen > 10 )
				cardinfo->CardCapacity *= ( 1 << ( cardinfo->SD_csd.RdBlockLen - 10 ) );
			else
				cardinfo->CardCapacity /= ( 1 << ( 10 - cardinfo->SD_csd.RdBlockLen ) );
		}
		else
		{	// v2:
			cardinfo->CardCapacity = cardinfo->SD_csd.DeviceSize + 1;
			cardinfo->CardBlockSize = 1 << cardinfo->SD_csd.RdBlockLen;
			cardinfo->CardCapacity *= cardinfo->CardBlockSize;
		}
	}

	return status;
}

/**
 * @brief  Prints out human-readable information about SD Card
 * @param  Previously retrieved card info structure
 * @retval None
 */
void SD_DumpCardInfo( const SD_CardInfo *cardinfo )
{
	uint8_t is_OSRv1 = ( cardinfo->SD_csd.CSDStruct == 0 );
	printf( "\nDumping SD Card information:\n\n    GLOBAL INFO\nSD Card type : " );
	/* some cards report wrong CSDStruct field in CSR register => use detected card type instead... */
	if ( is_OSRv1 != 0 )
		printf( "SDSC (v1 or v2)\n" );
	else
		printf( "SDHC or SDXC\n" );
	printf( "Card Capacity : %lu Kbytes\n", cardinfo->CardCapacity );
	printf( "Card Block Size : %lu bytes\n", cardinfo->CardBlockSize );

	printf( "\n    Card identification register (CID)\n" );
	printf( "Manufacturer ID : %d\n", cardinfo->SD_cid.ManufacturerID );
	printf( "OEM / Application ID : %c%c\n",
		((char*)&( cardinfo->SD_cid.OEM_AppliID ))[ 1 ],
		((char*)&( cardinfo->SD_cid.OEM_AppliID ))[ 0 ] );
	printf( "Product Name : %c%c%c%c%c\n",
		((char*)&( cardinfo->SD_cid.ProdName1 ))[ 3 ],
		((char*)&( cardinfo->SD_cid.ProdName1 ))[ 2 ],
		((char*)&( cardinfo->SD_cid.ProdName1 ))[ 1 ],
		((char*)&( cardinfo->SD_cid.ProdName1 ))[ 0 ],
		(char)(    cardinfo->SD_cid.ProdName2 )	);
	printf( "Product Revision : %d.%d\n",
		( cardinfo->SD_cid.ProdRev & 0xF0 ) >> 4,
		( cardinfo->SD_cid.ProdRev & 0x0F ) );
	printf( "Product Serial Number : %lu\n",
		cardinfo->SD_cid.ProdSN );
	printf( "Manufacturing Date (YYYY-MM) : %d-%d\n",
		2000 + ( ( cardinfo->SD_cid.ManufactDate & 0x0FF0 ) >> 4 ),
		cardinfo->SD_cid.ManufactDate & 0x000F );
	printf( "CID CRC : %d\n", cardinfo->SD_cid.CID_CRC & 0x7F );

	printf( "\n    Card-specific data register (CSD)\n" );
	if ( is_OSRv1 != 0 )
	{
		printf( "Data read access-time : " );
		switch ( (cardinfo->SD_csd.TAAC & 0x78) >> 3 )
		{
		case 0x0: printf( "0.0" ); break;
		case 0x1: printf( "1.0" ); break;
		case 0x2: printf( "1.2" ); break;
		case 0x3: printf( "1.3" ); break;
		case 0x4: printf( "1.5" ); break;
		case 0x5: printf( "2.0" ); break;
		case 0x6: printf( "2.5" ); break;
		case 0x7: printf( "3.0" ); break;
		case 0x8: printf( "3.5" ); break;
		case 0x9: printf( "4.0" ); break;
		case 0xA: printf( "4.5" ); break;
		case 0xB: printf( "5.0" ); break;
		case 0xC: printf( "5.5" ); break;
		case 0xD: printf( "6.0" ); break;
		case 0xE: printf( "7.0" ); break;
		case 0xF: printf( "8.0" ); break;
		default: break;
		}
		printf( " x 1" );
		switch ( cardinfo->SD_csd.TAAC & 0x07 )
		{
		case 0: printf( "n" ); break;
		case 1: printf( "0n" ); break;
		case 2: printf( "00n" ); break;
		case 3: printf( "u" ); break;
		case 4: printf( "0u" ); break;
		case 5: printf( "00u" ); break;
		case 6: printf( "m" ); break;
		case 7: printf( "0m" ); break;
		default: break;
		}
		printf( "s\nData read access-time in CLK cycles : %d\n", cardinfo->SD_csd.NSAC );
	}
	printf( "Max. bus clock frequency : %x", cardinfo->SD_csd.MaxBusClkFrec );
	switch ( cardinfo->SD_csd.MaxBusClkFrec )
	{
	case 0x32: printf( " (25Mhz)\n" ); break;
	case 0x5A: printf( " (50Mhz)\n" ); break;
	case 0x0B: printf( " (100Mhz)\n" ); break;
	case 0x2B: printf( " (200Mhz)\n" ); break;
	default:   printf( "\n" ); break;
	}
	printf( "\nCard command classes :" );
	if ( cardinfo->SD_csd.CardComdClasses & ( 0x001 ))	printf( " 0(basic)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 1) )	printf( " 1" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 2) )	printf( " 2(read)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 3) )	printf( " 3" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 4) )	printf( " 4(write)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 5) )	printf( " 5(erase)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 6) )	printf( " 6(protect)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 7) )	printf( " 7(lock)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 8) )	printf( " 8(app)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 << 9) )	printf( " 9(i/o)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 <<10) )	printf( " 10(switch)" );
	if ( cardinfo->SD_csd.CardComdClasses & (1 <<11) )	printf( " 11" );
	printf( "\n" );

	if ( is_OSRv1 != 0 )
	{
		printf( "Max. read data block length : %d ( %d bytes )\n",
			cardinfo->SD_csd.RdBlockLen,
			1 << (cardinfo->SD_csd.RdBlockLen) );
		printf( "Partial blocks for read allowed : %d\n", cardinfo->SD_csd.PartBlockRead );
		printf( "Write block misalignment : %d\n", cardinfo->SD_csd.WrBlockMisalign );
		printf( "Read block misalignment : %d\n", cardinfo->SD_csd.RdBlockMisalign );
	}
	else
	{
		printf( "Max. read data block length : always 512 bytes\n" );
		printf( "Partial blocks for read are not allowed\n" );
		printf( "Read/Write block misalignment is not allowed\n" );
	}
	printf( "DSR implemented : %d\n", cardinfo->SD_csd.DSRImpl );
	printf( "Device Size (4112 <= and <= 65375): %lu\n", cardinfo->SD_csd.DeviceSize );

	if ( is_OSRv1 != 0 )
	{
		printf( "Max. read current at VDD min : " );
		switch ( cardinfo->SD_csd.MaxRdCurrentVDDMin )
		{
		case 0: printf( "0.5" ); break;
		case 1: printf( "1" ); break;
		case 2: printf( "5" ); break;
		case 3: printf( "10" ); break;
		case 4: printf( "25" ); break;
		case 5: printf( "35" ); break;
		case 6: printf( "60" ); break;
		case 7: printf( "100" ); break;
		default: break;
		}
		printf( "mA\nMax. read current at VDD max : " );
		switch ( cardinfo->SD_csd.MaxRdCurrentVDDMax )
		{
		case 0: printf( "0.5" ); break;
		case 1: printf( "1" ); break;
		case 2: printf( "5" ); break;
		case 3: printf( "10" ); break;
		case 4: printf( "25" ); break;
		case 5: printf( "35" ); break;
		case 6: printf( "60" ); break;
		case 7: printf( "100" ); break;
		default: break;
		}
		printf( "mA\nMax. write current at VDD min : " );
		switch ( cardinfo->SD_csd.MaxWrCurrentVDDMin )
		{
		case 0: printf( "1" ); break;
		case 1: printf( "5" ); break;
		case 2: printf( "10" ); break;
		case 3: printf( "25" ); break;
		case 4: printf( "35" ); break;
		case 5: printf( "45" ); break;
		case 6: printf( "80" ); break;
		case 7: printf( "200" ); break;
		default: break;
		}
		printf( "mA\nMax. write current at VDD max : " );
		switch ( cardinfo->SD_csd.MaxWrCurrentVDDMax )
		{
		case 0: printf( "1" ); break;
		case 1: printf( "5" ); break;
		case 2: printf( "10" ); break;
		case 3: printf( "25" ); break;
		case 4: printf( "35" ); break;
		case 5: printf( "45" ); break;
		case 6: printf( "80" ); break;
		case 7: printf( "200" ); break;
		default: break;
		}
		printf( "mA\nDevice size multiplier : %d\n", cardinfo->SD_csd.DeviceSizeMul );
		if ( cardinfo->SD_csd.EraseBlockEnable == 0 )
			printf( "Erase size : 1 or more units of %d bytes each\n", cardinfo->SD_csd.EraseSectorSize );
		else
			printf( "Erase size : 1 or more blocks of 512 bytes each\n" );

		printf( "Write protect group size : %d\n", cardinfo->SD_csd.WrProtectGrSize );
		printf( "Write protect group enable : %d\n", cardinfo->SD_csd.WrProtectGrEnable );
		printf( "Write speed factor (Twrite/Tread) : %d\n", 1 << ( cardinfo->SD_csd.WrSpeedFact & 0x3F ) );
		printf( "Max. write data block length : %d\n", 1 << ( cardinfo->SD_csd.MaxWrBlockLen & 0xF ) );
		printf( "Partial blocks for write allowed : %d\n", cardinfo->SD_csd.WriteBlockPaPartial );
		printf( "File format group : %d\n", cardinfo->SD_csd.FileFormatGroup );
	}
	else
	{
		printf( "Erase size : 1 or more blocks of 512 bytes each\n" );
		printf( "Write protect group disabled\n" );
		printf( "Write timeout : 250ms\n" );
		printf( "Max. write data block length : 512 bytes\n" );
		printf( "Partial blocks for write are not allowed\n" );
	}
	printf( "Copy flag (OTP) : %d\n", cardinfo->SD_csd.CopyFlag );
	printf( "Permanent write protection : %d\n", cardinfo->SD_csd.PermWrProtect );
	printf( "Temporary write protection : %d\n", cardinfo->SD_csd.TempWrProtect );

	if ( is_OSRv1 != 0 )
	{
		printf( "File Format : " );
		switch ( cardinfo->SD_csd.FileFormat )
		{
		case 0: printf( "HDD-like file system with partition table\n" ); break;
		case 1: printf( "DOS FAT (FDD-like) with boot sector only (no partition table)\n" ); break;
		case 2: printf( "Universal File Format\n" ); break;
		case 3: printf( "Others/Unknown\n" ); break;
		default: break;
		}
	}
	printf( "CSD CRC : %d\n", cardinfo->SD_csd.CSD_CRC );

	if ( cardType != SD_Card_MMC )
	{
		printf( "\n    SD Card configuration register (SCR)\n" );
		printf( "SCR structure version : %d\n", cardinfo->SD_scr.SCR_Version );
		printf( "Physical layer specification version number : " );
		switch ( cardinfo->SD_scr.SpecVersion )
		{
		case 0: printf( "Version 1.0 and 1.01" ); break;
		case 1: printf( "Version 1.10" ); break;
		case 2:	printf( "Version %s", ( cardinfo->SD_scr.SpecVersion3 == 0 ) ? "2.00" : "3.0x" ); break;
		default:printf( "reserved" ); break;
		}
		printf( "\nState of bits after sector erase : 0x%s\n", cardinfo->SD_scr.StateAfterErase ? "FF" : "00" );
		printf( "CPRM security version : " );
		switch ( cardinfo->SD_scr.Security )
		{
		case 0: printf( "no security" ); break;
		case 1: printf( "not used" ); break;
		case 2: printf( "SDSC security ver 1.01" ); break;
		case 3: printf( "SDHC security ver 2.00" ); break;
		case 4: printf( "SDXC security ver 3.xx" ); break;
		default:printf( "reserved" ); break;
		}
		printf( "\nSupported data bus width :" );
		if ( cardinfo->SD_scr.BusWidth & 0x01 ) printf( " 1 bit" );
		if ( cardinfo->SD_scr.BusWidth & 0x04 ) printf( " 4 bit" );
		printf( "\nExtended security is%s supported\n", ( cardinfo->SD_scr.ExSecurity == 0 ) ? " not" : "" );
		printf( "Support of CMD23 (set block count) : %c\n", cardinfo->SD_scr.CmdSupport1 ? 'Y' : 'N' );
		printf( "Support of CMD20 (speed class control) : %c\n", cardinfo->SD_scr.CmdSupport2 ? 'Y' : 'N' );
	}
	printf( "\nDONE\n" );
}

/**
 * @brief  Prints out SD card status in human-readable form
 * @param  Previously retrieved SD card status structure
 * @retval None
 */
void SD_DumpStatus( const SD_Status* SD_status )
{
	printf( "\nDumping SD Card status information:\n\n" );
	if ( cardType != SD_Card_MMC )
	{
		printf( "Bus width : " );
		switch( SD_status->BusWidth )
		{
		case 0x00: printf( "1 bit" ); break;
		case 0x02: printf( "4 bits" ); break;
		default: printf( "reserved" ); break;
		}
		printf( "\nSD card is%s in secured mode\n", SD_status->InSecuredMode ? "" : " not" );
		printf( "Card Type : " );
		switch ( SD_status->CardType )
		{
		case 0x0000: printf( "Regular SD card" ); break;
		case 0x0001: printf( "SD ROM card" ); break;
		case 0x0002: printf( "OTP card" ); break;
		default: printf( "other card" ); break;
		}
		printf( "\nSize of protected area : %lu\n", SD_status->SizeProtectedArea );
		printf( "Speed class : " );
		switch( SD_status->SpeedClass )
		{
		case 0x00: printf( "Class 0"  ); break;
		case 0x01: printf( "Class 2"  ); break;
		case 0x02: printf( "Class 4"  ); break;
		case 0x03: printf( "Class 6"  ); break;
		case 0x04: printf( "Class 10" ); break;
		default:   printf( "Reserved" ); break;
		}
		printf( "\nPerformance move : " );
		switch( SD_status->PerformanceMove )
		{
		case 0x00: printf( "Sequential write" ); break;
		case 0xFF: printf( "Infinity" ); break;
		default: printf( "%d Mb/sec", SD_status->PerformanceMove ); break;
		}
		printf( "\nAllocation Unit size : " );
		switch( SD_status->AU_Size )
		{
		case 0x00: printf( "not defined" ); break;
		case 0x01: printf( "16 Kb" ); break;
		case 0x02: printf( "32 Kb" ); break;
		case 0x03: printf( "64 Kb" ); break;
		case 0x04: printf( "128 Kb" ); break;
		case 0x05: printf( "256 Kb" ); break;
		case 0x06: printf( "512 Kb" ); break;
		case 0x07: printf( "1 Mb" ); break;
		case 0x08: printf( "2 Mb" ); break;
		case 0x09: printf( "4 Mb" ); break;
		case 0x0A: printf( "8 Mb" ); break;
		case 0x0B: printf( "12 Mb" ); break;
		case 0x0C: printf( "16 Mb" ); break;
		case 0x0D: printf( "24 Mb" ); break;
		case 0x0E: printf( "32 Mb" ); break;
		case 0x0F: printf( "64 Mb" ); break;
		default: break;
		}
		printf( "\nErase Size : %d AU blocks\n", SD_status->EraseSize );
		printf( "Erase Timeout : %d seconds\n", SD_status->EraseTimeout );
		printf( "Erase Offset : %d seconds\n", SD_status->EraseOffset );
		printf( "Speed Grade for UHS mode : %s\n",
			( SD_status->UHS_SpeedGrade == 0 ) ? "< 10 Mb/sec" : "> 10 Mb/sec" );
		printf( "Allocation Unit size for UHS mode : " );
		switch( SD_status->UHS_AU_Size )
		{
		case 0x00: printf( "not defined" ); break;
		case 0x07: printf( "1 Mb" ); break;
		case 0x08: printf( "2 Mb" ); break;
		case 0x09: printf( "4 Mb" ); break;
		case 0x0A: printf( "8 Mb" ); break;
		case 0x0B: printf( "12 Mb" ); break;
		case 0x0C: printf( "16 Mb" ); break;
		case 0x0D: printf( "24 Mb" ); break;
		case 0x0E: printf( "32 Mb" ); break;
		case 0x0F: printf( "64 Mb" ); break;
		default:   printf( "not used" ); break;
		}
	}
	printf( "\n\nDONE\n" );
}

/**
 * @}
 *//* STM32_Public_Functions */


/**
 * @}
 *//* Utilities */
