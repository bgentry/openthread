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
 *   This file implements IPv6 networking.
 */

#define WPP_NAME "ip6.tmh"

#ifdef OPENTHREAD_CONFIG_FILE
#include OPENTHREAD_CONFIG_FILE
#else
#include <openthread-config.h>
#endif

#include "ip6.hpp"

#include "openthread-instance.h"
#include "common/code_utils.hpp"
#include "common/debug.hpp"
#include "common/logging.hpp"
#include "common/message.hpp"
#include "net/icmp6.hpp"
#include "net/ip6_address.hpp"
#include "net/ip6_routes.hpp"
#include "net/netif.hpp"
#include "net/udp6.hpp"
#include "thread/mle.hpp"

namespace ot {
namespace Ip6 {

Ip6::Ip6(void):
    mRoutes(*this),
    mIcmp(*this),
    mUdp(*this),
    mMpl(*this),
    mMessagePool(GetInstance()),
    mForwardingEnabled(false),
    mSendQueueTask(mTaskletScheduler, HandleSendQueue, this),
    mReceiveIp6DatagramCallback(NULL),
    mReceiveIp6DatagramCallbackContext(NULL),
    mIsReceiveIp6FilterEnabled(false),
    mNetifListHead(NULL)
{
}

Message *Ip6::NewMessage(uint16_t aReserved)
{
    return mMessagePool.New(Message::kTypeIp6, sizeof(Header) + sizeof(HopByHopHeader) + sizeof(OptionMpl) + aReserved);
}

uint16_t Ip6::UpdateChecksum(uint16_t aChecksum, uint16_t aValue)
{
    uint16_t result = aChecksum + aValue;
    return result + (result < aChecksum);
}

uint16_t Ip6::UpdateChecksum(uint16_t aChecksum, const void *aBuf, uint16_t aLength)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(aBuf);

    for (int i = 0; i < aLength; i++)
    {
        aChecksum = Ip6::UpdateChecksum(aChecksum, (i & 1) ? bytes[i] : static_cast<uint16_t>(bytes[i] << 8));
    }

    return aChecksum;
}

uint16_t Ip6::UpdateChecksum(uint16_t aChecksum, const Address &aAddress)
{
    return Ip6::UpdateChecksum(aChecksum, aAddress.mFields.m8, sizeof(aAddress));
}

uint16_t Ip6::ComputePseudoheaderChecksum(const Address &aSource, const Address &aDestination, uint16_t aLength,
                                          IpProto aProto)
{
    uint16_t checksum;

    checksum = Ip6::UpdateChecksum(0, aLength);
    checksum = Ip6::UpdateChecksum(checksum, static_cast<uint16_t>(aProto));
    checksum = UpdateChecksum(checksum, aSource);
    checksum = UpdateChecksum(checksum, aDestination);

    return checksum;
}

void Ip6::SetReceiveDatagramCallback(otIp6ReceiveCallback aCallback, void *aCallbackContext)
{
    mReceiveIp6DatagramCallback = aCallback;
    mReceiveIp6DatagramCallbackContext = aCallbackContext;
}

ThreadError Ip6::AddMplOption(Message &aMessage, Header &aHeader)
{
    ThreadError error = kThreadError_None;
    HopByHopHeader hbhHeader;
    OptionMpl mplOption;
    OptionPadN padOption;

    hbhHeader.SetNextHeader(aHeader.GetNextHeader());
    hbhHeader.SetLength(0);
    mMpl.InitOption(mplOption, aHeader.GetSource());

    // Mpl option may require two bytes padding.
    if ((mplOption.GetTotalLength() + sizeof(hbhHeader)) % 8)
    {
        padOption.Init(2);
        SuccessOrExit(error = aMessage.Prepend(&padOption, padOption.GetTotalLength()));
    }

    SuccessOrExit(error = aMessage.Prepend(&mplOption, mplOption.GetTotalLength()));
    SuccessOrExit(error = aMessage.Prepend(&hbhHeader, sizeof(hbhHeader)));
    aHeader.SetPayloadLength(aHeader.GetPayloadLength() + sizeof(hbhHeader) + sizeof(mplOption));
    aHeader.SetNextHeader(kProtoHopOpts);
exit:
    return error;
}

ThreadError Ip6::AddTunneledMplOption(Message &aMessage, Header &aHeader, MessageInfo &aMessageInfo)
{
    ThreadError error = kThreadError_None;
    Header tunnelHeader;
    const NetifUnicastAddress *source;
    MessageInfo messageInfo(aMessageInfo);

    // Use IP-in-IP encapsulation (RFC2473) and ALL_MPL_FORWARDERS address.
    memset(&messageInfo.GetPeerAddr(), 0, sizeof(Address));
    messageInfo.GetPeerAddr().mFields.m16[0] = HostSwap16(0xff03);
    messageInfo.GetPeerAddr().mFields.m16[7] = HostSwap16(0x00fc);

    tunnelHeader.Init();
    tunnelHeader.SetHopLimit(static_cast<uint8_t>(kDefaultHopLimit));
    tunnelHeader.SetPayloadLength(aHeader.GetPayloadLength() + sizeof(tunnelHeader));
    tunnelHeader.SetDestination(messageInfo.GetPeerAddr());
    tunnelHeader.SetNextHeader(kProtoIp6);

    VerifyOrExit((source = SelectSourceAddress(messageInfo)) != NULL,
                 error = kThreadError_Error);

    tunnelHeader.SetSource(source->GetAddress());

    SuccessOrExit(error = AddMplOption(aMessage, tunnelHeader));
    SuccessOrExit(error = aMessage.Prepend(&tunnelHeader, sizeof(tunnelHeader)));

exit:
    return error;
}

ThreadError Ip6::InsertMplOption(Message &aMessage, Header &aIp6Header, MessageInfo &aMessageInfo)
{
    ThreadError error = kThreadError_None;

    VerifyOrExit(aIp6Header.GetDestination().IsMulticast() &&
                 aIp6Header.GetDestination().GetScope() >= Address::kRealmLocalScope);

    if (aIp6Header.GetDestination().IsRealmLocalMulticast())
    {
        aMessage.RemoveHeader(sizeof(aIp6Header));

        if (aIp6Header.GetNextHeader() == kProtoHopOpts)
        {
            HopByHopHeader hbh;
            uint8_t hbhLength = 0;
            OptionMpl mplOption;

            // read existing hop-by-hop option header
            aMessage.Read(0, sizeof(hbh), &hbh);
            hbhLength = (hbh.GetLength() + 1) * 8;

            // increase existing hop-by-hop option header length by 8 bytes
            hbh.SetLength(hbh.GetLength() + 1);
            aMessage.Write(0, sizeof(hbh), &hbh);

            // make space for MPL Option + padding by shifting hop-by-hop option header
            SuccessOrExit(error = aMessage.Prepend(NULL, 8));
            aMessage.CopyTo(8, 0, hbhLength, aMessage);

            // insert MPL Option
            mMpl.InitOption(mplOption, aIp6Header.GetSource());
            aMessage.Write(hbhLength, mplOption.GetTotalLength(), &mplOption);

            // insert Pad Option (if needed)
            if (mplOption.GetTotalLength() % 8)
            {
                OptionPadN padOption;
                padOption.Init(8 - (mplOption.GetTotalLength() % 8));
                aMessage.Write(hbhLength + mplOption.GetTotalLength(), padOption.GetTotalLength(), &padOption);
            }

            // increase IPv6 Payload Length
            aIp6Header.SetPayloadLength(aIp6Header.GetPayloadLength() + 8);
        }
        else
        {
            SuccessOrExit(error = AddMplOption(aMessage, aIp6Header));
        }

        SuccessOrExit(error = aMessage.Prepend(&aIp6Header, sizeof(aIp6Header)));
    }
    else
    {
        SuccessOrExit(error = AddTunneledMplOption(aMessage, aIp6Header, aMessageInfo));
    }

exit:
    return error;
}

ThreadError Ip6::RemoveMplOption(Message &aMessage)
{
    ThreadError error = kThreadError_None;
    Header ip6Header;
    HopByHopHeader hbh;
    uint16_t offset;
    uint16_t endOffset;
    uint16_t mplOffset = 0;
    uint8_t mplLength = 0;
    bool remove = false;

    offset = 0;
    aMessage.Read(offset, sizeof(ip6Header), &ip6Header);
    offset += sizeof(ip6Header);
    VerifyOrExit(ip6Header.GetNextHeader() == kProtoHopOpts);

    aMessage.Read(offset, sizeof(hbh), &hbh);
    endOffset = offset + (hbh.GetLength() + 1) * 8;
    VerifyOrExit(aMessage.GetLength() >= endOffset, error = kThreadError_Parse);

    offset += sizeof(hbh);

    while (offset < endOffset)
    {
        OptionHeader option;

        aMessage.Read(offset, sizeof(option), &option);

        switch (option.GetType())
        {
        case OptionMpl::kType:
            mplOffset = offset;
            mplLength = option.GetLength();

            if (mplOffset == sizeof(ip6Header) + sizeof(hbh) && hbh.GetLength() == 0)
            {
                // first and only IPv6 Option, remove IPv6 HBH Option header
                remove = true;
            }
            else if (mplOffset + 8 == endOffset)
            {
                // last IPv6 Option, remove last 8 bytes
                remove = true;
            }

            offset += sizeof(option) + option.GetLength();
            break;

        case OptionPad1::kType:
            offset += sizeof(OptionPad1);
            break;

        case OptionPadN::kType:
            offset += sizeof(option) + option.GetLength();
            break;

        default:
            // encountered another option, now just replace MPL Option with PadN
            remove = false;
            offset += sizeof(option) + option.GetLength();
            break;
        }
    }

    // verify that IPv6 Options header is properly formed
    VerifyOrExit(offset == endOffset, error = kThreadError_Parse);

    if (remove)
    {
        // last IPv6 Option, shrink HBH Option header
        uint8_t buf[8];

        offset = endOffset - sizeof(buf);

        while (offset >= sizeof(buf))
        {
            aMessage.Read(offset - sizeof(buf), sizeof(buf), buf);
            aMessage.Write(offset, sizeof(buf), buf);
            offset -= sizeof(buf);
        }

        aMessage.RemoveHeader(sizeof(buf));

        if (mplOffset == sizeof(ip6Header) + sizeof(hbh))
        {
            // remove entire HBH header
            ip6Header.SetNextHeader(hbh.GetNextHeader());
        }
        else
        {
            // update HBH header length
            hbh.SetLength(hbh.GetLength() - 1);
            aMessage.Write(sizeof(ip6Header), sizeof(hbh), &hbh);
        }

        ip6Header.SetPayloadLength(ip6Header.GetPayloadLength() - sizeof(buf));
        aMessage.Write(0, sizeof(ip6Header), &ip6Header);
    }
    else if (mplOffset != 0)
    {
        // replace MPL Option with PadN Option
        OptionPadN padOption;

        padOption.Init(sizeof(OptionHeader) + mplLength);
        aMessage.Write(mplOffset, padOption.GetTotalLength(), &padOption);
    }

exit:
    return error;
}

void Ip6::EnqueueDatagram(Message &aMessage)
{
    mSendQueue.Enqueue(aMessage);
    mSendQueueTask.Post();
}

ThreadError Ip6::SendDatagram(Message &aMessage, MessageInfo &aMessageInfo, IpProto aIpProto)
{
    ThreadError error = kThreadError_None;
    Header header;
    uint16_t payloadLength = aMessage.GetLength();
    uint16_t checksum;
    const NetifUnicastAddress *source;

    header.Init();
    header.SetPayloadLength(payloadLength);
    header.SetNextHeader(aIpProto);
    header.SetHopLimit(aMessageInfo.mHopLimit ? aMessageInfo.mHopLimit : static_cast<uint8_t>(kDefaultHopLimit));

    if (aMessageInfo.GetSockAddr().IsUnspecified() ||
        aMessageInfo.GetSockAddr().IsMulticast())
    {
        VerifyOrExit((source = SelectSourceAddress(aMessageInfo)) != NULL,
                     error = kThreadError_Error);
        header.SetSource(source->GetAddress());
    }
    else
    {
        header.SetSource(aMessageInfo.GetSockAddr());
    }

    header.SetDestination(aMessageInfo.GetPeerAddr());

    if (header.GetDestination().IsLinkLocal() || header.GetDestination().IsLinkLocalMulticast())
    {
        VerifyOrExit(aMessageInfo.GetInterfaceId() != 0, error = kThreadError_Drop);
    }

    if (aMessageInfo.GetPeerAddr().IsRealmLocalMulticast())
    {
        SuccessOrExit(error = AddMplOption(aMessage, header));
    }

    SuccessOrExit(error = aMessage.Prepend(&header, sizeof(header)));

    if (aMessageInfo.GetPeerAddr().IsMulticast() &&
        aMessageInfo.GetPeerAddr().GetScope() > Address::kRealmLocalScope)
    {
        SuccessOrExit(error = AddTunneledMplOption(aMessage, header, aMessageInfo));
    }

    // compute checksum
    checksum = ComputePseudoheaderChecksum(header.GetSource(), header.GetDestination(),
                                           payloadLength, aIpProto);

    switch (aIpProto)
    {
    case kProtoUdp:
        SuccessOrExit(error = mUdp.UpdateChecksum(aMessage, checksum));
        break;

    case kProtoIcmp6:
        SuccessOrExit(error = mIcmp.UpdateChecksum(aMessage, checksum));
        break;

    default:
        break;
    }

exit:

    if (error == kThreadError_None)
    {
        aMessage.SetInterfaceId(aMessageInfo.GetInterfaceId());
        EnqueueDatagram(aMessage);
    }

    return error;
}

void Ip6::HandleSendQueue(void *aContext)
{
    static_cast<Ip6 *>(aContext)->HandleSendQueue();
}

void Ip6::HandleSendQueue(void)
{
    Message *message;

    while ((message = mSendQueue.GetHead()) != NULL)
    {
        mSendQueue.Dequeue(*message);
        HandleDatagram(*message, NULL, message->GetInterfaceId(), NULL, false);
    }
}

ThreadError Ip6::HandleOptions(Message &aMessage, Header &aHeader, bool &aForward)
{
    ThreadError error = kThreadError_None;
    HopByHopHeader hbhHeader;
    OptionHeader optionHeader;
    uint16_t endOffset;

    VerifyOrExit(aMessage.Read(aMessage.GetOffset(), sizeof(hbhHeader), &hbhHeader) == sizeof(hbhHeader),
                 error = kThreadError_Drop);
    endOffset = aMessage.GetOffset() + (hbhHeader.GetLength() + 1) * 8;

    VerifyOrExit(endOffset <= aMessage.GetLength(), error = kThreadError_Drop);

    aMessage.MoveOffset(sizeof(optionHeader));

    while (aMessage.GetOffset() < endOffset)
    {
        VerifyOrExit(aMessage.Read(aMessage.GetOffset(), sizeof(optionHeader), &optionHeader) == sizeof(optionHeader),
                     error = kThreadError_Drop);

        if (optionHeader.GetType() == OptionPad1::kType)
        {
            aMessage.MoveOffset(sizeof(OptionPad1));
            continue;
        }

        VerifyOrExit(aMessage.GetOffset() + sizeof(optionHeader) + optionHeader.GetLength() <= endOffset,
                     error = kThreadError_Drop);

        switch (optionHeader.GetType())
        {
        case OptionMpl::kType:
            SuccessOrExit(error = mMpl.ProcessOption(aMessage, aHeader.GetSource(), aForward));
            break;

        default:
            switch (optionHeader.GetAction())
            {
            case OptionHeader::kActionSkip:
                break;

            case OptionHeader::kActionDiscard:
                ExitNow(error = kThreadError_Drop);

            case OptionHeader::kActionForceIcmp:
                // TODO: send icmp error
                ExitNow(error = kThreadError_Drop);

            case OptionHeader::kActionIcmp:
                // TODO: send icmp error
                ExitNow(error = kThreadError_Drop);

            }

            break;
        }

        aMessage.MoveOffset(sizeof(optionHeader) + optionHeader.GetLength());
    }

exit:
    return error;
}

ThreadError Ip6::HandleFragment(Message &aMessage)
{
    ThreadError error = kThreadError_None;
    FragmentHeader fragmentHeader;

    VerifyOrExit(aMessage.Read(aMessage.GetOffset(), sizeof(fragmentHeader), &fragmentHeader) == sizeof(fragmentHeader),
                 error = kThreadError_Drop);

    VerifyOrExit(fragmentHeader.GetOffset() == 0 && fragmentHeader.IsMoreFlagSet() == false,
                 error = kThreadError_Drop);

    aMessage.MoveOffset(sizeof(fragmentHeader));

exit:
    return error;
}

ThreadError Ip6::HandleExtensionHeaders(Message &aMessage, Header &aHeader, uint8_t &aNextHeader, bool aForward,
                                        bool aReceive)
{
    ThreadError error = kThreadError_None;
    ExtensionHeader extHeader;

    while (aReceive == true || aNextHeader == kProtoHopOpts)
    {
        VerifyOrExit(aMessage.Read(aMessage.GetOffset(), sizeof(extHeader), &extHeader) == sizeof(extHeader),
                     error = kThreadError_Drop);

        switch (aNextHeader)
        {
        case kProtoHopOpts:
            SuccessOrExit(error = HandleOptions(aMessage, aHeader, aForward));
            break;

        case kProtoFragment:
            SuccessOrExit(error = HandleFragment(aMessage));
            break;

        case kProtoDstOpts:
            SuccessOrExit(error = HandleOptions(aMessage, aHeader, aForward));
            break;

        case kProtoIp6:
            ExitNow();

        case kProtoRouting:
        case kProtoNone:
            ExitNow(error = kThreadError_Drop);

        default:
            ExitNow();
        }

        aNextHeader = static_cast<uint8_t>(extHeader.GetNextHeader());
    }

exit:
    return error;
}

ThreadError Ip6::HandlePayload(Message &aMessage, MessageInfo &aMessageInfo, uint8_t aIpProto)
{
    ThreadError error = kThreadError_None;

    switch (aIpProto)
    {
    case kProtoUdp:
        ExitNow(error = mUdp.HandleMessage(aMessage, aMessageInfo));

    case kProtoIcmp6:
        ExitNow(error = mIcmp.HandleMessage(aMessage, aMessageInfo));
    }

exit:
    return error;
}

ThreadError Ip6::ProcessReceiveCallback(const Message &aMessage, const MessageInfo &aMessageInfo, uint8_t aIpProto,
                                        bool aFromNcpHost)
{
    ThreadError error = kThreadError_None;
    Message *messageCopy = NULL;

    VerifyOrExit(aFromNcpHost == false, error = kThreadError_Drop);
    VerifyOrExit(mReceiveIp6DatagramCallback != NULL, error = kThreadError_NoRoute);

    if (mIsReceiveIp6FilterEnabled)
    {
        // do not pass messages sent to/from an RLOC
        VerifyOrExit(!aMessageInfo.GetSockAddr().IsRoutingLocator() &&
                     !aMessageInfo.GetPeerAddr().IsRoutingLocator() &&
                     !aMessageInfo.GetSockAddr().IsAnycastRoutingLocator() &&
                     !aMessageInfo.GetPeerAddr().IsAnycastRoutingLocator(),
                     error = kThreadError_NoRoute);

        switch (aIpProto)
        {
        case kProtoIcmp6:
            if (mIcmp.IsEchoEnabled())
            {
                IcmpHeader icmp;
                aMessage.Read(aMessage.GetOffset(), sizeof(icmp), &icmp);

                // do not pass ICMP Echo Request messages
                VerifyOrExit(icmp.GetType() != kIcmp6TypeEchoRequest, error = kThreadError_NoRoute);
            }

            break;

        case kProtoUdp:
            if (aMessageInfo.GetSockAddr().IsLinkLocal() ||
                aMessageInfo.GetSockAddr().IsLinkLocalMulticast())
            {
                UdpHeader udp;
                aMessage.Read(aMessage.GetOffset(), sizeof(udp), &udp);

                // do not pass MLE messages
                VerifyOrExit(udp.GetDestinationPort() != Mle::kUdpPort, error = kThreadError_NoRoute);
            }

            break;

        default:
            break;
        }
    }

    // make a copy of the datagram to pass to host
    VerifyOrExit((messageCopy = aMessage.Clone()) != NULL, error = kThreadError_NoBufs);
    RemoveMplOption(*messageCopy);
    mReceiveIp6DatagramCallback(messageCopy, mReceiveIp6DatagramCallbackContext);

exit:

    switch (error)
    {
    case kThreadError_NoBufs:
        otLogInfoIp6(GetInstance(), "Failed to pass up message (len: %d) to host - out of message buffer.",
                     aMessage.GetLength());
        break;

    case kThreadError_Drop:
        otLogInfoIp6(GetInstance(), "Dropping message (len: %d) from local host since next hop is the host.",
                     aMessage.GetLength());
        break;

    default:
        break;
    }

    return error;
}

ThreadError Ip6::HandleDatagram(Message &aMessage, Netif *aNetif, int8_t aInterfaceId, const void *aLinkMessageInfo,
                                bool aFromNcpHost)
{
    ThreadError error = kThreadError_None;
    MessageInfo messageInfo;
    Header header;
    uint16_t payloadLength;
    bool receive = false;
    bool forward = false;
    bool tunnel = false;
    bool multicastPromiscuous = false;
    uint8_t nextHeader;
    uint8_t hopLimit;
    int8_t forwardInterfaceId;

    otLogFuncEntry();

#if 0
    uint8_t buf[1024];
    aMessage.Read(0, sizeof(buf), buf);
    dump("handle datagram", buf, aMessage.GetLength());
#endif

    // check aMessage length
    VerifyOrExit(aMessage.Read(0, sizeof(header), &header) == sizeof(header), error = kThreadError_Drop);
    payloadLength = header.GetPayloadLength();

    // check Version
    VerifyOrExit(header.IsVersion6(), error = kThreadError_Drop);

    // check Payload Length
    VerifyOrExit(sizeof(header) + payloadLength == aMessage.GetLength() &&
                 sizeof(header) + payloadLength <= Ip6::kMaxDatagramLength, error = kThreadError_Drop);

    messageInfo.SetPeerAddr(header.GetSource());
    messageInfo.SetSockAddr(header.GetDestination());
    messageInfo.SetInterfaceId(aInterfaceId);
    messageInfo.SetHopLimit(header.GetHopLimit());
    messageInfo.SetLinkInfo(aLinkMessageInfo);

    // determine destination of packet
    if (header.GetDestination().IsMulticast())
    {
        if (aNetif != NULL)
        {
            if (aNetif->IsMulticastSubscribed(header.GetDestination()))
            {
                receive = true;
            }
            else if (aNetif->IsMulticastPromiscuousEnabled())
            {
                multicastPromiscuous = true;
            }
        }
        else
        {
            forward = true;

            if (aFromNcpHost)
            {
                SuccessOrExit(error = InsertMplOption(aMessage, header, messageInfo));
            }
        }
    }
    else
    {
        if (IsUnicastAddress(header.GetDestination()))
        {
            receive = true;
        }
        else if (!header.GetDestination().IsLinkLocal())
        {
            forward = true;
        }
        else if (aNetif == NULL)
        {
            forward = true;
        }
    }

    aMessage.SetInterfaceId(aInterfaceId);
    aMessage.SetOffset(sizeof(header));

    // process IPv6 Extension Headers
    nextHeader = static_cast<uint8_t>(header.GetNextHeader());
    SuccessOrExit(error = HandleExtensionHeaders(aMessage, header, nextHeader, forward, receive));

    if (!mForwardingEnabled && aNetif != NULL)
    {
        forward = false;
    }

    // process IPv6 Payload
    if (receive)
    {
        if (nextHeader == kProtoIp6)
        {
            // Remove encapsulating header.
            aMessage.RemoveHeader(aMessage.GetOffset());

            HandleDatagram(aMessage, aNetif, aInterfaceId, aLinkMessageInfo, aFromNcpHost);
            ExitNow(tunnel = true);
        }

        ProcessReceiveCallback(aMessage, messageInfo, nextHeader, aFromNcpHost);

        SuccessOrExit(error = HandlePayload(aMessage, messageInfo, nextHeader));
    }
    else if (multicastPromiscuous)
    {
        ProcessReceiveCallback(aMessage, messageInfo, nextHeader, aFromNcpHost);
    }

    if (forward)
    {
        forwardInterfaceId = FindForwardInterfaceId(messageInfo);

        if (forwardInterfaceId == 0)
        {
            // try passing to host
            SuccessOrExit(error = ProcessReceiveCallback(aMessage, messageInfo, nextHeader, aFromNcpHost));

            // the caller transfers custody in the success case, so free the aMessage here
            aMessage.Free();

            ExitNow();
        }

        if (aNetif != NULL)
        {
            header.SetHopLimit(header.GetHopLimit() - 1);
        }

        if (header.GetHopLimit() == 0)
        {
            // send time exceeded
            ExitNow(error = kThreadError_Drop);
        }
        else
        {
            hopLimit = header.GetHopLimit();
            aMessage.Write(Header::GetHopLimitOffset(), Header::GetHopLimitSize(), &hopLimit);

            // submit aMessage to interface
            VerifyOrExit((aNetif = GetNetifById(forwardInterfaceId)) != NULL, error = kThreadError_NoRoute);
            SuccessOrExit(error = aNetif->SendMessage(aMessage));
        }
    }

exit:

    if (!tunnel && (error != kThreadError_None || !forward))
    {
        aMessage.Free();
    }

    otLogFuncExitErr(error);
    return error;
}

int8_t Ip6::FindForwardInterfaceId(const MessageInfo &aMessageInfo)
{
    int8_t interfaceId;

    otLogFuncEntry();

    if (aMessageInfo.GetSockAddr().IsMulticast())
    {
        // multicast
        interfaceId = aMessageInfo.mInterfaceId;
    }
    else if (aMessageInfo.GetSockAddr().IsLinkLocal())
    {
        // on-link link-local address
        interfaceId = aMessageInfo.mInterfaceId;
    }
    else if ((interfaceId = GetOnLinkNetif(aMessageInfo.GetSockAddr())) > 0)
    {
        // on-link global address
        ;
    }
    else if ((interfaceId = mRoutes.Lookup(aMessageInfo.GetPeerAddr(), aMessageInfo.GetSockAddr())) > 0)
    {
        // route
        ;
    }
    else
    {
        interfaceId = 0;
    }

    otLogFuncExit();

    return interfaceId;
}

ThreadError Ip6::AddNetif(Netif &aNetif)
{
    ThreadError error = kThreadError_None;
    Netif *netif;

    if (mNetifListHead == NULL)
    {
        mNetifListHead = &aNetif;
    }
    else
    {
        netif = mNetifListHead;

        do
        {
            if (netif == &aNetif || netif->mInterfaceId == aNetif.mInterfaceId)
            {
                ExitNow(error = kThreadError_Already);
            }
        }
        while (netif->mNext);

        netif->mNext = &aNetif;
    }

    aNetif.mNext = NULL;

exit:
    return error;
}

ThreadError Ip6::RemoveNetif(Netif &aNetif)
{
    ThreadError error = kThreadError_NotFound;

    VerifyOrExit(mNetifListHead != NULL, error = kThreadError_NotFound);

    if (mNetifListHead == &aNetif)
    {
        mNetifListHead = aNetif.mNext;
    }
    else
    {
        for (Netif *netif = mNetifListHead; netif->mNext; netif = netif->mNext)
        {
            if (netif->mNext != &aNetif)
            {
                continue;
            }

            netif->mNext = aNetif.mNext;
            error = kThreadError_None;
            break;
        }
    }

    aNetif.mNext = NULL;

exit:
    return error;
}

Netif *Ip6::GetNetifById(int8_t aInterfaceId)
{
    Netif *netif;

    for (netif = mNetifListHead; netif; netif = netif->mNext)
    {
        if (netif->GetInterfaceId() == aInterfaceId)
        {
            ExitNow();
        }
    }

exit:
    return netif;
}

bool Ip6::IsUnicastAddress(const Address &aAddress)
{
    bool rval = false;

    for (Netif *netif = mNetifListHead; netif; netif = netif->mNext)
    {
        rval = netif->IsUnicastAddress(aAddress);

        if (rval)
        {
            ExitNow();
        }
    }

exit:
    return rval;
}

const NetifUnicastAddress *Ip6::SelectSourceAddress(MessageInfo &aMessageInfo)
{
    Address *destination = &aMessageInfo.GetPeerAddr();
    int interfaceId = aMessageInfo.mInterfaceId;
    const NetifUnicastAddress *rvalAddr = NULL;
    const Address *candidateAddr;
    int8_t candidateId;
    int8_t rvalIface = 0;
    uint8_t rvalPrefixMatched = 0;
    uint8_t destinationScope = destination->GetScope();

    for (Netif *netif = GetNetifList(); netif; netif = netif->mNext)
    {
        candidateId = netif->GetInterfaceId();

        if (destination->IsLinkLocal() || destination->IsMulticast())
        {
            if (interfaceId != candidateId)
            {
                continue;
            }
        }

        for (const NetifUnicastAddress *addr = netif->mUnicastAddresses; addr; addr = addr->GetNext())
        {
            uint8_t overrideScope;
            uint8_t candidatePrefixMatched;

            candidateAddr = &addr->GetAddress();
            candidatePrefixMatched = destination->PrefixMatch(*candidateAddr);
            overrideScope = (candidatePrefixMatched >= addr->mPrefixLength) ? addr->GetScope() : destinationScope;

            if (candidateAddr->IsAnycastRoutingLocator())
            {
                // Don't use anycast address as source address.
                continue;
            }

            if (rvalAddr == NULL)
            {
                // Rule 0: Prefer any address
                rvalAddr = addr;
                rvalIface = candidateId;
                rvalPrefixMatched = candidatePrefixMatched;
            }
            else if (*candidateAddr == *destination)
            {
                // Rule 1: Prefer same address
                rvalAddr = addr;
                rvalIface = candidateId;
                ExitNow();
            }
            else if (addr->GetScope() < rvalAddr->GetScope())
            {
                // Rule 2: Prefer appropriate scope
                if (addr->GetScope() >= overrideScope)
                {
                    rvalAddr = addr;
                    rvalIface = candidateId;
                    rvalPrefixMatched = candidatePrefixMatched;
                }
            }
            else if (addr->GetScope() > rvalAddr->GetScope())
            {
                if (rvalAddr->GetScope() < overrideScope)
                {
                    rvalAddr = addr;
                    rvalIface = candidateId;
                    rvalPrefixMatched = candidatePrefixMatched;
                }
            }
            else if (rvalAddr->GetScope() == Address::kRealmLocalScope)
            {
                // Additional rule: Prefer appropriate realm local address
                if (overrideScope > Address::kRealmLocalScope)
                {
                    if (rvalAddr->GetAddress().IsRoutingLocator())
                    {
                        // Prefer EID if destination is not realm local.
                        rvalAddr = addr;
                        rvalIface = candidateId;
                        rvalPrefixMatched = candidatePrefixMatched;
                    }
                }
                else
                {
                    if (candidateAddr->IsRoutingLocator())
                    {
                        // Prefer RLOC if destination is realm local.
                        rvalAddr = addr;
                        rvalIface = candidateId;
                        rvalPrefixMatched = candidatePrefixMatched;
                    }
                }
            }
            else if (addr->mPreferred && !rvalAddr->mPreferred)
            {
                // Rule 3: Avoid deprecated addresses
                rvalAddr = addr;
                rvalIface = candidateId;
                rvalPrefixMatched = candidatePrefixMatched;
            }
            else if (aMessageInfo.mInterfaceId != 0 && aMessageInfo.mInterfaceId == candidateId &&
                     rvalIface != candidateId)
            {
                // Rule 4: Prefer home address
                // Rule 5: Prefer outgoing interface
                rvalAddr = addr;
                rvalIface = candidateId;
                rvalPrefixMatched = candidatePrefixMatched;
            }
            else if (candidatePrefixMatched > rvalPrefixMatched)
            {
                // Rule 6: Prefer matching label
                // Rule 7: Prefer public address
                // Rule 8: Use longest prefix matching
                rvalAddr = addr;
                rvalIface = candidateId;
                rvalPrefixMatched = candidatePrefixMatched;
            }
        }
    }

exit:
    aMessageInfo.mInterfaceId = rvalIface;
    return rvalAddr;
}

int8_t Ip6::GetOnLinkNetif(const Address &aAddress)
{
    int8_t rval = -1;

    for (Netif *netif = mNetifListHead; netif; netif = netif->mNext)
    {
        for (const NetifUnicastAddress *cur = netif->mUnicastAddresses; cur; cur = cur->GetNext())
        {
            if (cur->GetAddress().PrefixMatch(aAddress) >= cur->mPrefixLength)
            {
                ExitNow(rval = netif->GetInterfaceId());
            }
        }
    }

exit:
    return rval;
}

otInstance *Ip6::GetInstance(void)
{
    return otInstanceFromIp6(this);
}

const char *Ip6::IpProtoToString(IpProto aIpProto)
{
    const char *retval;

    switch (aIpProto)
    {
    case kProtoHopOpts:
        retval = "HopOpts";
        break;

    case kProtoTcp:
        retval = "TCP";
        break;

    case kProtoUdp:
        retval = "UDP";
        break;

    case kProtoIp6:
        retval = "IP6";
        break;

    case kProtoRouting:
        retval = "Routing";
        break;

    case kProtoFragment:
        retval = "Frag";
        break;

    case kProtoIcmp6:
        retval = "ICMP6";
        break;

    case kProtoNone:
        retval = "None";
        break;

    case kProtoDstOpts:
        retval = "DstOpts";
        break;

    default:
        retval = "Unknown";
        break;
    }

    return retval;
}

}  // namespace Ip6
}  // namespace ot
