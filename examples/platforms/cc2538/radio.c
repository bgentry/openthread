/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the OpenThread platform abstraction for radio communication.
 *
 */

#include <openthread-config.h>
#include <openthread/openthread.h>
#include <openthread/platform/diag.h>
#include <openthread/platform/platform.h>
#include <openthread/platform/radio.h>

#include "platform-cc2538.h"
#include "common/logging.hpp"
#include "utils/code_utils.h"

enum
{
    IEEE802154_MIN_LENGTH = 5,
    IEEE802154_MAX_LENGTH = 127,
    IEEE802154_ACK_LENGTH = 5,
    IEEE802154_FRAME_TYPE_MASK = 0x7,
    IEEE802154_FRAME_TYPE_ACK = 0x2,
    IEEE802154_FRAME_PENDING = 1 << 4,
    IEEE802154_ACK_REQUEST = 1 << 5,
    IEEE802154_DSN_OFFSET = 2,
};

enum
{
    CC2538_RSSI_OFFSET = 73,
    CC2538_CRC_BIT_MASK = 0x80,
    CC2538_LQI_BIT_MASK = 0x7f,
};

enum
{
    CC2538_RECEIVE_SENSITIVITY = -100, // dBm
};

typedef struct TxPowerTable
{
    int8_t  mTxPowerVal;
    uint8_t mTxPowerReg;
} TxPowerTable;

// The transmit power table, the values are from SmartRF Studio 2.4.0
static const TxPowerTable sTxPowerTable[] =
{
    {  7, 0xFF },
    {  5, 0xED },
    {  3, 0xD5 },
    {  1, 0xC5 },
    {  0, 0xB6 },
    { -1, 0xB0 },
    { -3, 0xA1 },
    { -5, 0x91 },
    { -7, 0x88 },
    { -9, 0x72 },
    { -11, 0x62 },
    { -13, 0x58 },
    { -15, 0x42 },
    { -24, 0x00 },
};

static RadioPacket sTransmitFrame;
static RadioPacket sReceiveFrame;
static ThreadError sTransmitError;
static ThreadError sReceiveError;

static uint8_t sTransmitPsdu[IEEE802154_MAX_LENGTH];
static uint8_t sReceivePsdu[IEEE802154_MAX_LENGTH];
static uint8_t sChannel = 0;
static int8_t sTxPower = 0;

static PhyState sState = kStateDisabled;
static bool sIsReceiverEnabled = false;

void enableReceiver(void)
{
    if (!sIsReceiverEnabled)
    {
        otLogInfoPlat(sInstance, "Enabling receiver", NULL);

        // flush rxfifo
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;

        // enable receiver
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_RXON;
        sIsReceiverEnabled = true;
    }
}

void disableReceiver(void)
{
    if (sIsReceiverEnabled)
    {
        otLogInfoPlat(sInstance, "Disabling receiver", NULL);

        while (HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_TX_ACTIVE);

        // flush rxfifo
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;

        if (HWREG(RFCORE_XREG_RXENABLE) != 0)
        {
            // disable receiver
            HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_RFOFF;
        }

        sIsReceiverEnabled = false;
    }
}

void setChannel(uint8_t aChannel)
{
    if (sChannel != aChannel)
    {
        bool enabled = false;

        if (sIsReceiverEnabled)
        {
            disableReceiver();
            enabled = true;
        }

        otLogInfoPlat(sInstance, "Channel=%d", aChannel);

        HWREG(RFCORE_XREG_FREQCTRL) = 11 + (aChannel - 11) * 5;
        sChannel = aChannel;

        if (enabled)
        {
            enableReceiver();
        }
    }
}

void setTxPower(int8_t aTxPower)
{
    uint8_t i = 0;

    if (sTxPower != aTxPower)
    {
        otLogInfoPlat(sInstance, "TxPower=%d", aTxPower);

        for (i = sizeof(sTxPowerTable) / sizeof(TxPowerTable) - 1; i > 0; i--)
        {
            if (aTxPower < sTxPowerTable[i].mTxPowerVal)
            {
                break;
            }
        }

        HWREG(RFCORE_XREG_TXPOWER) = sTxPowerTable[i].mTxPowerReg;
        sTxPower = aTxPower;
    }
}

void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    uint8_t *eui64 = (uint8_t *)IEEE_EUI64;
    (void)aInstance;

    for (uint8_t i = 0; i < OT_EXT_ADDRESS_SIZE; i++)
    {
        aIeeeEui64[i] = eui64[7 - i];
    }
}

void otPlatRadioSetPanId(otInstance *aInstance, uint16_t aPanid)
{
    (void)aInstance;

    otLogInfoPlat(sInstance, "PANID=%X", aPanid);

    HWREG(RFCORE_FFSM_PAN_ID0) = aPanid & 0xFF;
    HWREG(RFCORE_FFSM_PAN_ID1) = aPanid >> 8;
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance, uint8_t *aAddress)
{
    (void)aInstance;

    otLogInfoPlat(sInstance, "ExtAddr=%X%X%X%X%X%X%X%X",
                  aAddress[7], aAddress[6], aAddress[5], aAddress[4], aAddress[3],
                  aAddress[2], aAddress[1], aAddress[0]);

    for (int i = 0; i < 8; i++)
    {
        ((volatile uint32_t *)RFCORE_FFSM_EXT_ADDR0)[i] = aAddress[i];
    }
}

void otPlatRadioSetShortAddress(otInstance *aInstance, uint16_t aAddress)
{
    (void)aInstance;

    otLogInfoPlat(sInstance, "ShortAddr=%X", aAddress);

    HWREG(RFCORE_FFSM_SHORT_ADDR0) = aAddress & 0xFF;
    HWREG(RFCORE_FFSM_SHORT_ADDR1) = aAddress >> 8;
}

void cc2538RadioInit(void)
{
    sTransmitFrame.mLength = 0;
    sTransmitFrame.mPsdu = sTransmitPsdu;
    sReceiveFrame.mLength = 0;
    sReceiveFrame.mPsdu = sReceivePsdu;

    // enable clock
    HWREG(SYS_CTRL_RCGCRFC) = SYS_CTRL_RCGCRFC_RFC0;
    HWREG(SYS_CTRL_SCGCRFC) = SYS_CTRL_SCGCRFC_RFC0;
    HWREG(SYS_CTRL_DCGCRFC) = SYS_CTRL_DCGCRFC_RFC0;

    // Table 23-7.
    HWREG(RFCORE_XREG_AGCCTRL1) = 0x15;
    HWREG(RFCORE_XREG_TXFILTCFG) = 0x09;
    HWREG(ANA_REGS_BASE + ANA_REGS_O_IVCTRL) = 0x0b;

    HWREG(RFCORE_XREG_CCACTRL0) = 0xf8;
    HWREG(RFCORE_XREG_FIFOPCTRL) = IEEE802154_MAX_LENGTH;

    HWREG(RFCORE_XREG_FRMCTRL0) = RFCORE_XREG_FRMCTRL0_AUTOCRC | RFCORE_XREG_FRMCTRL0_AUTOACK;

    // default: SRCMATCH.SRC_MATCH_EN(1), SRCMATCH.AUTOPEND(1),
    // SRCMATCH.PEND_DATAREQ_ONLY(1), RFCORE_XREG_FRMCTRL1_PENDING_OR(0)

    HWREG(RFCORE_XREG_TXPOWER) = sTxPowerTable[0].mTxPowerReg;
    sTxPower = sTxPowerTable[0].mTxPowerVal;

    otLogInfoPlat(sInstance, "Initialized", NULL);
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    (void)aInstance;
    return (sState != kStateDisabled) ? true : false;
}

ThreadError otPlatRadioEnable(otInstance *aInstance)
{
    if (!otPlatRadioIsEnabled(aInstance))
    {
        otLogDebgPlat(sInstance, "State=kStateSleep", NULL);
        sState = kStateSleep;
    }

    return kThreadError_None;
}

ThreadError otPlatRadioDisable(otInstance *aInstance)
{
    if (otPlatRadioIsEnabled(aInstance))
    {
        otLogDebgPlat(sInstance, "State=kStateDisabled", NULL);
        sState = kStateDisabled;
    }

    return kThreadError_None;
}

ThreadError otPlatRadioSleep(otInstance *aInstance)
{
    ThreadError error = kThreadError_InvalidState;
    (void)aInstance;

    if (sState == kStateSleep || sState == kStateReceive)
    {
        otLogDebgPlat(sInstance, "State=kStateSleep", NULL);
        error = kThreadError_None;
        sState = kStateSleep;
        disableReceiver();
    }

    return error;
}

ThreadError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    ThreadError error = kThreadError_InvalidState;
    (void)aInstance;

    if (sState != kStateDisabled)
    {
        otLogDebgPlat(sInstance, "State=kStateReceive", NULL);

        error = kThreadError_None;
        sState = kStateReceive;
        setChannel(aChannel);
        sReceiveFrame.mChannel = aChannel;
        enableReceiver();
    }

    return error;
}

ThreadError otPlatRadioTransmit(otInstance *aInstance, RadioPacket *aPacket)
{
    ThreadError error = kThreadError_InvalidState;
    (void)aInstance;

    if (sState == kStateReceive)
    {
        int i;

        error = kThreadError_None;
        sState = kStateTransmit;
        sTransmitError = kThreadError_None;

        while (HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_TX_ACTIVE);

        // flush txfifo
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHTX;
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHTX;

        // frame length
        HWREG(RFCORE_SFR_RFDATA) = aPacket->mLength;

        // frame data
        for (i = 0; i < aPacket->mLength; i++)
        {
            HWREG(RFCORE_SFR_RFDATA) = aPacket->mPsdu[i];
        }

        setChannel(aPacket->mChannel);
        setTxPower(aPacket->mPower);

        while ((HWREG(RFCORE_XREG_FSMSTAT1) & 1) == 0);

        // wait for valid rssi
        while ((HWREG(RFCORE_XREG_RSSISTAT) & RFCORE_XREG_RSSISTAT_RSSI_VALID) == 0);

        otEXPECT_ACTION(((HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_CCA) &&
                         !((HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_SFD))),
                        sTransmitError = kThreadError_ChannelAccessFailure);

        // begin transmit
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_TXON;

        while (HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_TX_ACTIVE);

        otLogDebgPlat(sInstance, "Transmitted %d bytes", aPacket->mLength);
    }

exit:
    return error;
}

RadioPacket *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    (void)aInstance;
    return &sTransmitFrame;
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    (void)aInstance;
    return 0;
}

otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    (void)aInstance;
    return kRadioCapsNone;
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    (void)aInstance;
    return (HWREG(RFCORE_XREG_FRMFILT0) & RFCORE_XREG_FRMFILT0_FRAME_FILTER_EN) == 0;
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    (void)aInstance;

    otLogInfoPlat(sInstance, "PromiscuousMode=%d", aEnable ? 1 : 0);

    if (aEnable)
    {
        HWREG(RFCORE_XREG_FRMFILT0) &= ~RFCORE_XREG_FRMFILT0_FRAME_FILTER_EN;
    }
    else
    {
        HWREG(RFCORE_XREG_FRMFILT0) |= RFCORE_XREG_FRMFILT0_FRAME_FILTER_EN;
    }
}

void readFrame(void)
{
    uint8_t length;
    uint8_t crcCorr;
    int i;

    otEXPECT(sState == kStateReceive || sState == kStateTransmit);
    otEXPECT((HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_FIFOP) != 0);

    // read length
    length = HWREG(RFCORE_SFR_RFDATA);
    otEXPECT(IEEE802154_MIN_LENGTH <= length && length <= IEEE802154_MAX_LENGTH);

    // read psdu
    for (i = 0; i < length - 2; i++)
    {
        sReceiveFrame.mPsdu[i] = HWREG(RFCORE_SFR_RFDATA);
    }

    sReceiveFrame.mPower = (int8_t)HWREG(RFCORE_SFR_RFDATA) - CC2538_RSSI_OFFSET;
    crcCorr = HWREG(RFCORE_SFR_RFDATA);

    if (crcCorr & CC2538_CRC_BIT_MASK)
    {
        sReceiveFrame.mLength = length;
        sReceiveFrame.mLqi = crcCorr & CC2538_LQI_BIT_MASK;
    }
    else
    {
        // resets rxfifo
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;

        otLogDebgPlat(sInstance, "Dropping %d received bytes (Invalid CRC)", length);
    }

    // check for rxfifo overflow
    if ((HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_FIFOP) != 0 &&
        (HWREG(RFCORE_XREG_FSMSTAT1) & RFCORE_XREG_FSMSTAT1_FIFO) == 0)
    {
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;
        HWREG(RFCORE_SFR_RFST) = RFCORE_SFR_RFST_INSTR_FLUSHRX;
    }

exit:
    return;
}

void cc2538RadioProcess(otInstance *aInstance)
{
    readFrame();

    if ((sState == kStateReceive && sReceiveFrame.mLength > 0) ||
        (sState == kStateTransmit && sReceiveFrame.mLength > IEEE802154_ACK_LENGTH))
    {
#if OPENTHREAD_ENABLE_DIAG

        if (otPlatDiagModeGet())
        {
            otPlatDiagRadioReceiveDone(aInstance, &sReceiveFrame, sReceiveError);
        }
        else
#endif
        {
            // signal MAC layer for each received frame if promiscous is enabled
            // otherwise only signal MAC layer for non-ACK frame
            if (((HWREG(RFCORE_XREG_FRMFILT0) & RFCORE_XREG_FRMFILT0_FRAME_FILTER_EN) == 0) ||
                (sReceiveFrame.mLength > IEEE802154_ACK_LENGTH))
            {
                otLogDebgPlat(sInstance, "Received %d bytes", sReceiveFrame.mLength);
                otPlatRadioReceiveDone(aInstance, &sReceiveFrame, sReceiveError);
            }
        }
    }

    if (sState == kStateTransmit)
    {
        if (sTransmitError != kThreadError_None || (sTransmitFrame.mPsdu[0] & IEEE802154_ACK_REQUEST) == 0)
        {
            if (sTransmitError != kThreadError_None)
            {
                otLogDebgPlat(sInstance, "Transmit failed ErrorCode=%d", sTransmitError);
            }

            sState = kStateReceive;

#if OPENTHREAD_ENABLE_DIAG

            if (otPlatDiagModeGet())
            {
                otPlatDiagRadioTransmitDone(aInstance, &sTransmitFrame, false, sTransmitError);
            }
            else
#endif
            {
                otPlatRadioTransmitDone(aInstance, &sTransmitFrame, false, sTransmitError);
            }
        }
        else if (sReceiveFrame.mLength == IEEE802154_ACK_LENGTH &&
                 (sReceiveFrame.mPsdu[0] & IEEE802154_FRAME_TYPE_MASK) == IEEE802154_FRAME_TYPE_ACK &&
                 (sReceiveFrame.mPsdu[IEEE802154_DSN_OFFSET] == sTransmitFrame.mPsdu[IEEE802154_DSN_OFFSET]))
        {
            sState = kStateReceive;

#if OPENTHREAD_ENABLE_DIAG

            if (otPlatDiagModeGet())
            {
                otPlatDiagRadioTransmitDone(aInstance, &sTransmitFrame, (sReceiveFrame.mPsdu[0] & IEEE802154_FRAME_PENDING) != 0,
                                            sTransmitError);
            }
            else
#endif
            {
                otPlatRadioTransmitDone(aInstance, &sTransmitFrame, (sReceiveFrame.mPsdu[0] & IEEE802154_FRAME_PENDING) != 0,
                                        sTransmitError);
            }
        }
    }

    sReceiveFrame.mLength = 0;
}

void RFCoreRxTxIntHandler(void)
{
    HWREG(RFCORE_SFR_RFIRQF0) = 0;
}

void RFCoreErrIntHandler(void)
{
    HWREG(RFCORE_SFR_RFERRF) = 0;
}

uint32_t getSrcMatchEntriesEnableStatus(bool aShort)
{
    uint32_t status = 0;
    uint32_t *addr = aShort ? (uint32_t *) RFCORE_XREG_SRCSHORTEN0 : (uint32_t *) RFCORE_XREG_SRCEXTEN0;

    for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_ENABLE_STATUS_SIZE; i++)
    {
        status |= HWREG(addr++) << (i * 8);
    }

    return status;
}

int8_t findSrcMatchShortEntry(const uint16_t aShortAddress)
{
    int8_t entry = -1;
    uint16_t shortAddr;
    uint32_t bitMask;
    uint32_t *addr = NULL;
    uint32_t status = getSrcMatchEntriesEnableStatus(true);

    for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_SHORT_ENTRIES; i++)
    {
        bitMask = 0x00000001 << i;

        if ((status & bitMask) == 0)
        {
            continue;
        }

        addr = (uint32_t *)RFCORE_FFSM_SRCADDRESS_TABLE + (i * RFCORE_XREG_SRCMATCH_SHORT_ENTRY_OFFSET);

        shortAddr = HWREG(addr + 2);
        shortAddr |= HWREG(addr + 3) << 8;

        if ((shortAddr == aShortAddress))
        {
            entry = i;
            break;
        }
    }

    return entry;
}

int8_t findSrcMatchExtEntry(const uint8_t *aExtAddress)
{
    int8_t entry = -1;
    uint32_t bitMask;
    uint32_t *addr = NULL;
    uint32_t status = getSrcMatchEntriesEnableStatus(false);

    for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_EXT_ENTRIES; i++)
    {
        uint8_t j = 0;
        bitMask = 0x00000001 << 2 * i;

        if ((status & bitMask) == 0)
        {
            continue;
        }

        addr = (uint32_t *)RFCORE_FFSM_SRCADDRESS_TABLE + (i * RFCORE_XREG_SRCMATCH_EXT_ENTRY_OFFSET);

        for (j = 0; j < sizeof(otExtAddress); j++)
        {
            if (HWREG(addr + j) != aExtAddress[j])
            {
                break;
            }
        }

        if (j == sizeof(otExtAddress))
        {
            entry = i;
            break;
        }
    }

    return entry;
}

void setSrcMatchEntryEnableStatus(bool aShort, uint8_t aEntry, bool aEnable)
{
    uint8_t entry = aShort ? aEntry : (2 * aEntry);
    uint8_t index = entry / 8;
    uint32_t *addrEn = aShort ? (uint32_t *)RFCORE_XREG_SRCSHORTEN0 : (uint32_t *)RFCORE_XREG_SRCEXTEN0;
    uint32_t *addrAutoPendEn = aShort ? (uint32_t *)RFCORE_FFSM_SRCSHORTPENDEN0 : (uint32_t *)RFCORE_FFSM_SRCEXTPENDEN0;
    uint32_t bitMask = 0x00000001;

    if (aEnable)
    {
        HWREG(addrEn + index) |= (bitMask) << (entry % 8);
        HWREG(addrAutoPendEn + index) |= (bitMask) << (entry % 8);
    }
    else
    {
        HWREG(addrEn + index) &= ~((bitMask) << (entry % 8));
        HWREG(addrAutoPendEn + index) &= ~((bitMask) << (entry % 8));
    }
}

int8_t findSrcMatchAvailEntry(bool aShort)
{
    int8_t entry = -1;
    uint32_t bitMask;
    uint32_t shortEnableStatus = getSrcMatchEntriesEnableStatus(true);
    uint32_t extEnableStatus = getSrcMatchEntriesEnableStatus(false);

    otLogDebgPlat(sInstance, "Short enable status: 0x%x", shortEnableStatus);
    otLogDebgPlat(sInstance, "Ext enable status: 0x%x", extEnableStatus);

    if (aShort)
    {
        bitMask = 0x00000001;

        for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_SHORT_ENTRIES; i++)
        {
            if ((extEnableStatus & bitMask) == 0)
            {
                if ((shortEnableStatus & bitMask) == 0)
                {
                    entry = i;
                    break;
                }
            }

            if (i % 2 == 1)
            {
                extEnableStatus = extEnableStatus >> 2;
            }

            shortEnableStatus = shortEnableStatus >> 1;
        }
    }
    else
    {
        bitMask = 0x00000003;

        for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_EXT_ENTRIES; i++)
        {
            if (((extEnableStatus | shortEnableStatus) & bitMask) == 0)
            {
                entry = i;
                break;
            }

            extEnableStatus = extEnableStatus >> 2;
            shortEnableStatus = shortEnableStatus >> 2;
        }
    }

    return entry;
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    (void)aInstance;

    otLogInfoPlat(sInstance, "EnableSrcMatch=%d", aEnable ? 1 : 0);

    if (aEnable)
    {
        // only set FramePending when ack for data poll if there are queued messages
        // for entries in the source match table.
        HWREG(RFCORE_XREG_FRMCTRL1) &= ~RFCORE_XREG_FRMCTRL1_PENDING_OR;
    }
    else
    {
        // set FramePending for all ack.
        HWREG(RFCORE_XREG_FRMCTRL1) |= RFCORE_XREG_FRMCTRL1_PENDING_OR;
    }
}

ThreadError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, const uint16_t aShortAddress)
{
    ThreadError error = kThreadError_None;
    int8_t entry = findSrcMatchAvailEntry(true);
    uint32_t *addr = (uint32_t *)RFCORE_FFSM_SRCADDRESS_TABLE;
    (void)aInstance;

    otLogDebgPlat(sInstance, "Add ShortAddr entry: %d", entry);

    otEXPECT_ACTION(entry >= 0, error = kThreadError_NoBufs);

    addr += (entry * RFCORE_XREG_SRCMATCH_SHORT_ENTRY_OFFSET);

    HWREG(addr++) = HWREG(RFCORE_FFSM_PAN_ID0);
    HWREG(addr++) = HWREG(RFCORE_FFSM_PAN_ID1);
    HWREG(addr++) = aShortAddress & 0xFF;
    HWREG(addr++) = aShortAddress >> 8;

    setSrcMatchEntryEnableStatus(true, (uint8_t)(entry), true);

exit:
    return error;
}

ThreadError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const uint8_t *aExtAddress)
{
    ThreadError error = kThreadError_None;
    int8_t entry = findSrcMatchAvailEntry(false);
    uint32_t *addr = (uint32_t *)RFCORE_FFSM_SRCADDRESS_TABLE;
    (void)aInstance;

    otLogDebgPlat(sInstance, "Add ExtAddr entry: %d", entry);

    otEXPECT_ACTION(entry >= 0, error = kThreadError_NoBufs);

    addr += (entry * RFCORE_XREG_SRCMATCH_EXT_ENTRY_OFFSET);

    for (uint8_t i = 0; i < sizeof(otExtAddress); i++)
    {
        HWREG(addr++) = aExtAddress[i];
    }

    setSrcMatchEntryEnableStatus(false, (uint8_t)(entry), true);

exit:
    return error;
}

ThreadError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, const uint16_t aShortAddress)
{
    ThreadError error = kThreadError_None;
    int8_t entry = findSrcMatchShortEntry(aShortAddress);
    (void)aInstance;

    otLogDebgPlat(sInstance, "Clear ShortAddr entry: %d", entry);

    otEXPECT_ACTION(entry >= 0, error = kThreadError_NoAddress);

    setSrcMatchEntryEnableStatus(true, (uint8_t)(entry), false);

exit:
    return error;
}

ThreadError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const uint8_t *aExtAddress)
{
    ThreadError error = kThreadError_None;
    int8_t entry = findSrcMatchExtEntry(aExtAddress);
    (void)aInstance;

    otLogDebgPlat(sInstance, "Clear ExtAddr entry: %d", entry);

    otEXPECT_ACTION(entry >= 0, error = kThreadError_NoAddress);

    setSrcMatchEntryEnableStatus(false, (uint8_t)(entry), false);

exit:
    return error;
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    uint32_t *addrEn = (uint32_t *)RFCORE_XREG_SRCSHORTEN0;
    uint32_t *addrAutoPendEn = (uint32_t *)RFCORE_FFSM_SRCSHORTPENDEN0;
    (void)aInstance;

    otLogDebgPlat(sInstance, "Clear ShortAddr entries", NULL);

    for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_ENABLE_STATUS_SIZE; i++)
    {
        HWREG(addrEn++) = 0;
        HWREG(addrAutoPendEn++) = 0;
    }
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    uint32_t *addrEn = (uint32_t *)RFCORE_XREG_SRCEXTEN0;
    uint32_t *addrAutoPendEn = (uint32_t *)RFCORE_FFSM_SRCEXTPENDEN0;
    (void)aInstance;

    otLogDebgPlat(sInstance, "Clear ExtAddr entries", NULL);

    for (uint8_t i = 0; i < RFCORE_XREG_SRCMATCH_ENABLE_STATUS_SIZE; i++)
    {
        HWREG(addrEn++) = 0;
        HWREG(addrAutoPendEn++) = 0;
    }
}

ThreadError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    (void)aInstance;
    (void)aScanChannel;
    (void)aScanDuration;
    return kThreadError_NotImplemented;
}

void otPlatRadioSetDefaultTxPower(otInstance *aInstance, int8_t aPower)
{
    // TODO: Create a proper implementation for this driver.
    (void)aInstance;
    (void)aPower;
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    (void)aInstance;
    return CC2538_RECEIVE_SENSITIVITY;
}
