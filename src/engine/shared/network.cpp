/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/system.h>


#include "config.h"
#include "network.h"
#include "huffman.h"

void CNetRecvUnpacker::Clear() {
	m_Valid = false;
}

void CNetRecvUnpacker::Start(const NETADDR *pAddr, CNetConnection *pConnection, int ClientID)
{
	m_Addr = *pAddr;
	m_pConnection = pConnection;
	m_ClientID = ClientID;
	m_CurrentChunk = 0;
	m_Valid = true;
}

// TODO: rename this function
int CNetRecvUnpacker::FetchChunk(CNetChunk *pChunk)
{
	CNetChunkHeader Header;
	unsigned char *pEnd = m_Data.m_aChunkData + m_Data.m_DataSize;

	while(1)
	{
		unsigned char *pData = m_Data.m_aChunkData;

		// check for old data to unpack
		if(!m_Valid || m_CurrentChunk >= m_Data.m_NumChunks)
		{
			Clear();
			return 0;
		}

		// TODO: add checking here so we don't read too far
		for(int i = 0; i < m_CurrentChunk; i++)
		{
			pData = Header.Unpack(pData);
			pData += Header.m_Size;
		}

		// unpack the header
		pData = Header.Unpack(pData);
		m_CurrentChunk++;

		if(pData+Header.m_Size > pEnd)
		{
			Clear();
			return 0;
		}

		// handle sequence stuff
		if(m_pConnection && (Header.m_Flags&NET_CHUNKFLAG_VITAL))
		{
			if(Header.m_Sequence == (m_pConnection->m_Ack+1)%NET_MAX_SEQUENCE)
			{
				// in sequence
				m_pConnection->m_Ack = (m_pConnection->m_Ack+1)%NET_MAX_SEQUENCE;
			}
			else
			{
				// old packet that we already got
				if(CNetBase::IsSeqInBackroom(Header.m_Sequence, m_pConnection->m_Ack))
					continue;

				// out of sequence, request resend
				if(g_Config.m_Debug)
					dbg_msg("conn", "asking for resend %d %d", Header.m_Sequence, (m_pConnection->m_Ack+1)%NET_MAX_SEQUENCE);
				m_pConnection->SignalResend();
				continue; // take the next chunk in the packet
			}
		}

		// fill in the info
		pChunk->m_ClientID = m_ClientID;
		pChunk->m_Address = m_Addr;
		pChunk->m_Flags = Header.m_Flags;
		pChunk->m_DataSize = Header.m_Size;
		pChunk->m_pData = pData;
		return 1;
	}
}

// packs the data tight and sends it
void CNetBase::SendPacketConnless(NETSOCKET Socket, const NETADDR *pAddr, int Version, TOKEN Token, TOKEN ResponseToken, const void *pData, int DataSize)
{
	unsigned char aBuffer[NET_MAX_PACKETSIZE];

	dbg_assert(NET_PACKETVERSION_LEGACY <= Version && Version <= NET_PACKETVERSION, "CNetBase::SendPacketConnless: packet version out of range");

	if(Version == NET_PACKETVERSION_LEGACY)
	{
		dbg_assert(DataSize + 6 <= NET_MAX_PACKETSIZE, "CNetBase::SendPacketConnless: packet data size too high");
		aBuffer[0] = 0xff;
		aBuffer[1] = 0xff;
		aBuffer[2] = 0xff;
		aBuffer[3] = 0xff;
		aBuffer[4] = 0xff;
		aBuffer[5] = 0xff;
		mem_copy(&aBuffer[6], pData, DataSize);
		net_udp_send(Socket, pAddr, aBuffer, 6+DataSize);
	}
	else
	{
		dbg_assert(DataSize + 15 <= NET_MAX_PACKETSIZE, "CNetBase::SendPacketConnless: packet data size too high");
		aBuffer[0] = 0x00; // padding
		aBuffer[1] = 0x00;
		aBuffer[2] = 0x00;
		aBuffer[3] = 0x01; // version
		aBuffer[4] = (Token>>24)&0xff; // token
		aBuffer[5] = (Token>>16)&0xff;
		aBuffer[6] = (Token>>8)&0xff;
		aBuffer[7] = (Token)&0xff;
		aBuffer[8] = 0xff; // flags and data for not connless packets
		aBuffer[9] = 0xff;
		aBuffer[10] = 0xff;
		aBuffer[11] = (ResponseToken>>24)&0xff; // response token
		aBuffer[12] = (ResponseToken>>16)&0xff;
		aBuffer[13] = (ResponseToken>>8)&0xff;
		aBuffer[14] = (ResponseToken)&0xff;

		mem_copy(&aBuffer[15], pData, DataSize);
		net_udp_send(Socket, pAddr, aBuffer, 15+DataSize);
	}
}

void CNetBase::SendPacket(NETSOCKET Socket, const NETADDR *pAddr, CNetPacketConstruct *pPacket)
{
	unsigned char aBuffer[NET_MAX_PACKETSIZE];
	int CompressedSize = -1;
	int FinalSize = -1;
	int HeaderSize;

	dbg_assert(NET_PACKETVERSION_LEGACY <= pPacket->m_Version && pPacket->m_Version <= NET_PACKETVERSION, "CNetBase::SendPacket: packet version out of range");

	// log the data
	if(ms_DataLogSent)
	{
		int Type = 1;
		io_write(ms_DataLogSent, &Type, sizeof(Type));
		io_write(ms_DataLogSent, &pPacket->m_DataSize, sizeof(pPacket->m_DataSize));
		io_write(ms_DataLogSent, &pPacket->m_aChunkData, pPacket->m_DataSize);
		io_flush(ms_DataLogSent);
	}

	if(pPacket->m_Version == NET_PACKETVERSION_LEGACY)
		HeaderSize = NET_PACKETHEADERSIZE_LEGACY;
	else
		HeaderSize = NET_PACKETHEADERSIZE;

	// compress
	CompressedSize = ms_Huffman.Compress(pPacket->m_aChunkData, pPacket->m_DataSize, &aBuffer[HeaderSize], NET_MAX_PACKETSIZE - HeaderSize - 1);

	// check if the compression was enabled, successful and good enough
	if(CompressedSize > 0 && CompressedSize < pPacket->m_DataSize)
	{
		FinalSize = CompressedSize;
		pPacket->m_Flags |= NET_PACKETFLAG_COMPRESSION;
	}
	else
	{
		// use uncompressed data
		FinalSize = pPacket->m_DataSize;
		mem_copy(&aBuffer[HeaderSize], pPacket->m_aChunkData, pPacket->m_DataSize);
		pPacket->m_Flags &= ~NET_PACKETFLAG_COMPRESSION;
	}

	// set header and send the packet if all things are good
	if(FinalSize >= 0)
	{
		FinalSize += HeaderSize;
		if(pPacket->m_Version == NET_PACKETVERSION_LEGACY)
		{
			aBuffer[0] = ((pPacket->m_Flags<<4)&0xf0)|((pPacket->m_Ack>>8)&0xf);
			aBuffer[1] = pPacket->m_Ack&0xff;
			aBuffer[2] = pPacket->m_NumChunks;
		}
		else
		{
			aBuffer[0] = 0x00; // padding
			aBuffer[1] = 0x00;
			aBuffer[2] = 0x00;
			aBuffer[3] = NET_PACKETVERSION; // version
			aBuffer[4] = (pPacket->m_Token>>24)&0xff; // token
			aBuffer[5] = (pPacket->m_Token>>16)&0xff;
			aBuffer[6] = (pPacket->m_Token>>8)&0xff;
			aBuffer[7] = (pPacket->m_Token)&0xff;
			aBuffer[8] = ((pPacket->m_Flags<<4)&0xf0)|((pPacket->m_Ack>>8)&0xf); // flags & ack
			aBuffer[9] = pPacket->m_Ack&0xff; // ack
			aBuffer[10] = pPacket->m_NumChunks; // num_chunks
		}

		net_udp_send(Socket, pAddr, aBuffer, FinalSize);

		// log raw socket data
		if(ms_DataLogSent)
		{
			int Type = 0;
			io_write(ms_DataLogSent, &Type, sizeof(Type));
			io_write(ms_DataLogSent, &FinalSize, sizeof(FinalSize));
			io_write(ms_DataLogSent, aBuffer, FinalSize);
			io_flush(ms_DataLogSent);
		}
	}
}

// TODO: rename this function
int CNetBase::UnpackPacket(unsigned char *pBuffer, int Size, CNetPacketConstruct *pPacket)
{
	// check the size
	if(Size < NET_PACKETHEADERSIZE_LEGACY || Size > NET_MAX_PACKETSIZE)
	{
		if(g_Config.m_Debug)
			dbg_msg("network", "packet too small, size=%d", Size);
		return -1;
	}

	// log the data
	if(ms_DataLogRecv)
	{
		int Type = 0;
		io_write(ms_DataLogRecv, &Type, sizeof(Type));
		io_write(ms_DataLogRecv, &Size, sizeof(Size));
		io_write(ms_DataLogRecv, pBuffer, Size);
		io_flush(ms_DataLogRecv);
	}

	// determine protocol version
	if(pBuffer[0] != 0x00 || pBuffer[1] != 0x00 || pBuffer[2] != 0x00)
	{
		// legacy packet

		// read the packet
		pPacket->m_Version = NET_PACKETVERSION_LEGACY;
		pPacket->m_Token = NET_TOKEN_NONE;
		pPacket->m_ResponseToken = NET_TOKEN_NONE;

		pPacket->m_Flags = pBuffer[0]>>4;
		pPacket->m_Ack = ((pBuffer[0]&0xf)<<8) | pBuffer[1];
		pPacket->m_NumChunks = pBuffer[2];
		pPacket->m_DataSize = Size - NET_PACKETHEADERSIZE_LEGACY;
	}
	else
	{
		// new-style packet

		// check size first
		if(Size < NET_PACKETHEADERSIZE_LEGACY + 1)
		{
			if(g_Config.m_Debug)
				dbg_msg("network", "new-style packet too small for version checking, size=%d", Size);
			return -1;
		}

		if(pBuffer[3] != NET_PACKETVERSION)
			return -1; // wrong version, silent ignore

		// check size again
		if(Size < NET_PACKETHEADERSIZE)
		{
			if(g_Config.m_Debug)
				dbg_msg("network", "packet too small, size=%d", Size);
			return -1;
		}

		// finally read the packet
		pPacket->m_Version = NET_PACKETVERSION;
		pPacket->m_Token = (pBuffer[4]<<24) | (pBuffer[5]<<16) | (pBuffer[6]<<8) | pBuffer[7];
		pPacket->m_ResponseToken = NET_TOKEN_NONE;

		pPacket->m_Flags = pBuffer[8]>>4;
		pPacket->m_Ack = ((pBuffer[8]&0xf)<<8) | pBuffer[9];
		pPacket->m_NumChunks = pBuffer[10];
		pPacket->m_DataSize = Size - NET_PACKETHEADERSIZE;
	}


	if(pPacket->m_Flags&NET_PACKETFLAG_CONNLESS)
	{
		pPacket->m_Flags = NET_PACKETFLAG_CONNLESS;
		pPacket->m_Ack = 0;
		pPacket->m_NumChunks = 0;
		if(pPacket->m_Version == NET_PACKETVERSION_LEGACY)
		{
			if(Size < 6)
			{
				if(g_Config.m_Debug)
					dbg_msg("net", "connless packet too small, size=%d", Size);
				return -1;
			}

			pPacket->m_DataSize = Size - 6;
			mem_copy(pPacket->m_aChunkData, &pBuffer[6], pPacket->m_DataSize);
		}
		else
		{
			if(Size < 15)
			{
				if(g_Config.m_Debug)
					dbg_msg("net", "new-style connless packet too small, size=%d", Size);
				return -1;
			}

			pPacket->m_DataSize = Size - 15;
			pPacket->m_ResponseToken = (pBuffer[11]<<24) | (pBuffer[12]<<16) | (pBuffer[13]<<8) | pBuffer[14];
			mem_copy(pPacket->m_aChunkData, &pBuffer[15], pPacket->m_DataSize);
		}
	}
	else
	{
		int DataStart;
		if(pPacket->m_Version == NET_PACKETVERSION_LEGACY)
			DataStart = NET_PACKETHEADERSIZE_LEGACY;
		else
			DataStart = NET_PACKETHEADERSIZE;

		if(Size - DataStart > NET_MAX_PAYLOAD)
		{
			if(g_Config.m_Debug)
				dbg_msg("network", "packet payload too big, size=%d", Size);
			return -1;
		}

		if(pPacket->m_Flags&NET_PACKETFLAG_COMPRESSION)
			pPacket->m_DataSize = ms_Huffman.Decompress(&pBuffer[DataStart], pPacket->m_DataSize, pPacket->m_aChunkData, sizeof(pPacket->m_aChunkData));
		else
			mem_copy(pPacket->m_aChunkData, &pBuffer[DataStart], pPacket->m_DataSize);
	}

	// check for errors
	if(pPacket->m_DataSize < 0)
	{
		if(g_Config.m_Debug)
			dbg_msg("network", "error during packet decoding");
		return -1;
	}

	// set the response token (a bit hacky because this function doesn't know about control packets)
	if(pPacket->m_Version == NET_PACKETVERSION && (pPacket->m_Flags&NET_PACKETFLAG_CONTROL))
	{
		if(pPacket->m_DataSize >= 5) // control byte + token
		{
			if(pPacket->m_aChunkData[0] == NET_CTRLMSG_CONNECT
				|| pPacket->m_aChunkData[0] == NET_CTRLMSG_TOKEN)
			{
				pPacket->m_ResponseToken = (pPacket->m_aChunkData[1]<<24) | (pPacket->m_aChunkData[2]<<16)
					| (pPacket->m_aChunkData[3]<<8) | pPacket->m_aChunkData[4];
			}
		}
	}

	// log the data
	if(ms_DataLogRecv)
	{
		int Type = 1;
		io_write(ms_DataLogRecv, &Type, sizeof(Type));
		io_write(ms_DataLogRecv, &pPacket->m_DataSize, sizeof(pPacket->m_DataSize));
		io_write(ms_DataLogRecv, pPacket->m_aChunkData, pPacket->m_DataSize);
		io_flush(ms_DataLogRecv);
	}

	// return success
	return 0;
}


void CNetBase::SendControlMsg(NETSOCKET Socket, const NETADDR *pAddr, int Version, TOKEN Token, int Ack, int ControlMsg, const void *pExtra, int ExtraSize)
{
	CNetPacketConstruct Construct;
	Construct.m_Version = Version;
	Construct.m_Token = Token;
	Construct.m_Flags = NET_PACKETFLAG_CONTROL;
	Construct.m_Ack = Ack;
	Construct.m_NumChunks = 0;
	Construct.m_DataSize = 1+ExtraSize;
	Construct.m_aChunkData[0] = ControlMsg;
	mem_copy(&Construct.m_aChunkData[1], pExtra, ExtraSize);

	// send the control message
	CNetBase::SendPacket(Socket, pAddr, &Construct);
}


void CNetBase::SendControlMsgWithToken(NETSOCKET Socket, const NETADDR *pAddr, TOKEN Token, int Ack, int ControlMsg, TOKEN MyToken)
{
	char aToken[4];
	aToken[0] = (MyToken>>24)&0xff;
	aToken[1] = (MyToken>>16)&0xff;
	aToken[2] = (MyToken>>8)&0xff;
	aToken[3] = (MyToken)&0xff;

	SendControlMsg(Socket, pAddr, NET_PACKETVERSION, Token, 0, ControlMsg, aToken, sizeof(aToken));
}

unsigned char *CNetChunkHeader::Pack(unsigned char *pData)
{
	pData[0] = ((m_Flags&0x03)<<6) | ((m_Size>>6)&0x3F);
	pData[1] = (m_Size&0x3F);
	if(m_Flags&NET_CHUNKFLAG_VITAL)
	{
		pData[1] |= (m_Sequence>>2)&0xC0;
		pData[2] = m_Sequence&0xFF;
		return pData + 3;
	}
	return pData + 2;
}

unsigned char *CNetChunkHeader::Unpack(unsigned char *pData)
{
	m_Flags = (pData[0]>>6)&0x03;
	m_Size = ((pData[0]&0x3F)<<6) | (pData[1]&0x3F);
	m_Sequence = -1;
	if(m_Flags&NET_CHUNKFLAG_VITAL)
	{
		m_Sequence = ((pData[1]&0xC0)<<2) | pData[2];
		return pData + 3;
	}
	return pData + 2;
}


int CNetBase::IsSeqInBackroom(int Seq, int Ack)
{
	int Bottom = (Ack-NET_MAX_SEQUENCE/2);
	if(Bottom < 0)
	{
		if(Seq <= Ack)
			return 1;
		if(Seq >= (Bottom + NET_MAX_SEQUENCE))
			return 1;
	}
	else
	{
		if(Seq <= Ack && Seq >= Bottom)
			return 1;
	}

	return 0;
}

IOHANDLE CNetBase::ms_DataLogSent = 0;
IOHANDLE CNetBase::ms_DataLogRecv = 0;
CHuffman CNetBase::ms_Huffman;


void CNetBase::OpenLog(IOHANDLE DataLogSent, IOHANDLE DataLogRecv)
{
	if(DataLogSent)
	{
		ms_DataLogSent = DataLogSent;
		dbg_msg("network", "logging sent packages");
	}
	else
		dbg_msg("network", "failed to start logging sent packages");

	if(DataLogRecv)
	{
		ms_DataLogRecv = DataLogRecv;
		dbg_msg("network", "logging recv packages");
	}
	else
		dbg_msg("network", "failed to start logging recv packages");
}

void CNetBase::CloseLog()
{
	if(ms_DataLogSent)
	{
		dbg_msg("network", "stopped logging sent packages");
		io_close(ms_DataLogSent);
		ms_DataLogSent = 0;
	}

	if(ms_DataLogRecv)
	{
		dbg_msg("network", "stopped logging recv packages");
		io_close(ms_DataLogRecv);
		ms_DataLogRecv = 0;
	}
}

int CNetBase::Compress(const void *pData, int DataSize, void *pOutput, int OutputSize)
{
	return ms_Huffman.Compress(pData, DataSize, pOutput, OutputSize);
}

int CNetBase::Decompress(const void *pData, int DataSize, void *pOutput, int OutputSize)
{
	return ms_Huffman.Decompress(pData, DataSize, pOutput, OutputSize);
}


static const unsigned gs_aFreqTable[256+1] = {
	1<<30,4545,2657,431,1950,919,444,482,2244,617,838,542,715,1814,304,240,754,212,647,186,
	283,131,146,166,543,164,167,136,179,859,363,113,157,154,204,108,137,180,202,176,
	872,404,168,134,151,111,113,109,120,126,129,100,41,20,16,22,18,18,17,19,
	16,37,13,21,362,166,99,78,95,88,81,70,83,284,91,187,77,68,52,68,
	59,66,61,638,71,157,50,46,69,43,11,24,13,19,10,12,12,20,14,9,
	20,20,10,10,15,15,12,12,7,19,15,14,13,18,35,19,17,14,8,5,
	15,17,9,15,14,18,8,10,2173,134,157,68,188,60,170,60,194,62,175,71,
	148,67,167,78,211,67,156,69,1674,90,174,53,147,89,181,51,174,63,163,80,
	167,94,128,122,223,153,218,77,200,110,190,73,174,69,145,66,277,143,141,60,
	136,53,180,57,142,57,158,61,166,112,152,92,26,22,21,28,20,26,30,21,
	32,27,20,17,23,21,30,22,22,21,27,25,17,27,23,18,39,26,15,21,
	12,18,18,27,20,18,15,19,11,17,33,12,18,15,19,18,16,26,17,18,
	9,10,25,22,22,17,20,16,6,16,15,20,14,18,24,335,1517};

void CNetBase::Init()
{
	ms_Huffman.Init(gs_aFreqTable);
}
