#ifndef __TINY_CSGO_CLIENT_HEADER__
#define __TINY_CSGO_CLIENT_HEADER__

#ifdef _WIN32
#pragma once
#endif

#include <iostream>
#include <string>
#include <format>
#include <mutex>
#include <vector>
#include <ranges>
#include <asio.hpp>

//from hl2sdk-csgo
#include <bitbuf.h>
#include <steam_api.h>
#include <strtools.h>
#include <checksum_crc.h>
#include <mathlib/IceKey.H>
#include <utlmemory.h>
#include <lzss.h>
#include <vstdlib/random.h>

#include "common/proto_oob.h"
#include "common/protocol.h"
#include "netmessage/netmessages_signon.h"
#include "netmessage/netmessages.h"

using namespace asio::ip;

inline asio::io_context g_IoContext;

constexpr int NET_CRYPT_KEY_LENGTH = 16;
constexpr int NET_COMPRESSION_STACKBUF_SIZE = 4096;

class Client
{
public:
	Client() :
		m_WriteBuf(m_Buf, sizeof(m_Buf)), m_ReadBuf(m_Buf, sizeof(m_Buf))
	{
	}
	
	[[nodiscard("Steam api must be confirmed working before running the client!")]]
	bool PrepareSteamAPI()
	{
		if (!SteamAPI_IsSteamRunning())
		{
			std::cout << "Steam is not running!\n";
			return false;
		}
		
		if (!SteamAPI_Init())
		{
			std::cout << "Cannot initialize SteamAPI\n";
			return false;
		}
		if (!SteamUser())
		{
			std::cout << "Cannot initialize ISteamUser interface\n";
			return false;
		}

		return true;
	}

	void BindServer(const char* ip, const char* nickname, const char* password, uint16_t port)
	{
		m_Ip = ip;
		m_NickName = nickname;
		m_PassWord = password;
		m_Port = port;
		
		asio::co_spawn(g_IoContext, ConnectToServer(), asio::detached);
	}

	inline void RunClient() { g_IoContext.run(); }

	void InitCommandInput()
	{
		thread_local bool run = false;
		if (run)
			return;

		std::thread([this]() {
			std::string command;
			while (std::getline(std::cin, command))
			{
				std::lock_guard<std::mutex> lock(m_CommandVecLock);
				m_VecCommand.push_back(std::string(command));
			}

			}).detach();

		run = true;
	}

protected:
	asio::awaitable<void> ConnectToServer()
	{
		std::cout << std::format("Connecting to server {}:{}, using nickname {}, using password {}\n", 
			m_Ip, m_Port, m_NickName, m_PassWord);
		
		udp::socket socket(g_IoContext, udp::endpoint(udp::v4(), m_Port));
		auto remote_endpoint = udp::endpoint(make_address(m_Ip), m_Port);
		
		auto [success, challenge, auth_proto_ver, connect_proto_ver] = co_await GetChallenge(socket, remote_endpoint, 0);
		if (!success)
		{
			std::cout << "Connection failed after challenge response!\n";
			co_return;
		}
		m_HostVersion = connect_proto_ver;

		if(!(co_await SendConnectPacket(socket, remote_endpoint, challenge, auth_proto_ver, connect_proto_ver)))
			co_return;
		
		co_await HandleIncomingPacket(socket, remote_endpoint);

		co_return;
	}

	asio::awaitable<bool> SendConnectPacket(
		udp::socket& socket, 
		udp::endpoint& remote_endpoint, 
		uint32_t challenge, 
		uint32_t auth_proto_version, 
		uint32_t connect_proto_ver)
	{
		ResetWriteBuffer();
		m_WriteBuf.WriteLong(-1);
		m_WriteBuf.WriteByte(C2S_CONNECT);
		m_WriteBuf.WriteLong(connect_proto_ver);
		m_WriteBuf.WriteLong(auth_proto_version); 
		m_WriteBuf.WriteLong(challenge);
		m_WriteBuf.WriteString(m_NickName.c_str());
		m_WriteBuf.WriteString(m_PassWord.c_str());
		m_WriteBuf.WriteByte(0);
		m_WriteBuf.WriteOneBit(0); //low violence setting
		m_WriteBuf.WriteLongLong(0); //reservation cookie
		m_WriteBuf.WriteByte(1); //platform

		unsigned char steamkey[STEAM_KEYSIZE];
		unsigned int keysize = 0;

		CSteamID localsid = SteamUser()->GetSteamID();
		SteamUser()->GetAuthSessionTicket(steamkey, STEAM_KEYSIZE, &keysize);

		m_WriteBuf.WriteLong(0); //Encryption key index
		m_WriteBuf.WriteShort(keysize + sizeof(uint64));
		m_WriteBuf.WriteLongLong(localsid.ConvertToUint64());
		m_WriteBuf.WriteBytes(steamkey, keysize);
		
		co_await socket.async_send_to(asio::buffer(m_Buf, m_WriteBuf.GetNumBytesWritten()), remote_endpoint, asio::use_awaitable);

		//Wait for connect response, TO DO: add timeout support
		co_await socket.async_wait(socket.wait_read, asio::use_awaitable);
		co_await socket.async_receive_from(asio::buffer(m_Buf), remote_endpoint, asio::use_awaitable);

		ResetReadBuffer();
		if (m_ReadBuf.ReadLong() != -1)
			co_return false;

		auto c = m_ReadBuf.ReadByte();
		switch (c)
		{
		case S2C_CONNREJECT:
			char err[256];
			m_ReadBuf.ReadString(err, sizeof(err));

			std::cout << std::format("Connection refused! - {}\n", err);
			co_return false;

		case S2C_CONNECTION:
			//Tell server that we are connected, ready to receive netmessages
			co_await SetSignonState(socket, remote_endpoint, SIGNONSTATE_CONNECTED, -1);
			co_return true;

		default:
			std::cout << std::format("Connection error! Got response header - {:#04x}\n", c);
			co_return false;
		}
		
	}

	asio::awaitable<std::tuple<bool, uint32_t, uint32_t, uint32_t>> GetChallenge(
		udp::socket& socket, 
		udp::endpoint& remote_endpoint,
		uint32_t cached_challenge)
	{
		//Write request challenge packet
		ResetWriteBuffer();
		m_WriteBuf.WriteLong(-1);
		m_WriteBuf.WriteByte(A2S_GETCHALLENGE);
		
		auto numBytesWritten = m_WriteBuf.GetNumBytesWritten();
		auto len = snprintf((char*)(m_Buf + numBytesWritten), sizeof(m_Buf) - numBytesWritten, "connect0x%08X", cached_challenge);
		
		co_await socket.async_send_to(asio::buffer(m_Buf, m_WriteBuf.GetNumBytesWritten() + len+1), remote_endpoint, asio::use_awaitable);

		//Wait for challenge response, TO DO: add timeout support
		co_await socket.async_wait(socket.wait_read, asio::use_awaitable);
		co_await socket.async_receive_from(asio::buffer(m_Buf), remote_endpoint, asio::use_awaitable);
		
		auto failed_obj = std::make_tuple<bool, uint32_t, uint32_t, uint32_t>(false, 0, 0, 0);

		//Read information from buffer
		ResetReadBuffer();
		if (m_ReadBuf.ReadLong() != -1 || m_ReadBuf.ReadByte() != S2C_CHALLENGE)
			co_return failed_obj;
		
		auto challenge = m_ReadBuf.ReadLong();
		auto auth_protocol_version = m_ReadBuf.ReadLong();
		auto encrypt_key = m_ReadBuf.ReadShort();
		auto server_steamid = m_ReadBuf.ReadLongLong();
		auto vac = m_ReadBuf.ReadByte();

		char buf[48];
		m_ReadBuf.ReadString(buf, sizeof(buf));
		if (StringHasPrefix(buf, "connect"))
		{
			if (StringHasPrefix(buf, "connect-retry"))
			{
				co_return co_await GetChallenge(socket, remote_endpoint, challenge);
			}
			else if (StringHasPrefix(buf, "connect-lan-only"))
			{
				std::cout << "You cannot connect to this CS:GO server because it is restricted to LAN connections only.\n";
				co_return failed_obj;
			}
			else if (StringHasPrefix(buf, "connect-matchmaking-only"))
			{
				std::cout << "You must use matchmaking to connect to this CS:GO server.\n";
				co_return failed_obj;
			}
			//else if... don't cover other circumstances for now
		}
		else
		{
			std::cout << "Corrupted challenge response!\n";
			co_return failed_obj;
		}

		auto connect_protocol_version = m_ReadBuf.ReadLong();
		m_ReadBuf.ReadString(buf, sizeof(buf)); //lobby name
		auto require_pw = m_ReadBuf.ReadByte();
		auto lobby_id = m_ReadBuf.ReadLongLong();
		auto dcFriendsReqd = m_ReadBuf.ReadByte() != 0;
		auto officialValveServer = m_ReadBuf.ReadByte() != 0;
		
		std::cout << std::format("Server using '{}' lobbies, requiring pw {}, lobby id {:#010x}\n", 
			buf, require_pw ? "yes" : "no", (uint64_t)lobby_id);

		std::cout << std::format("Get server challenge number : {:#010x}, auth prorocol: {:#04x}, server steam: {}, vac {}, dcFriendsReqd {}, officialValveServer {}\n",
			challenge, auth_protocol_version, server_steamid, vac ? "on" : "off", dcFriendsReqd, officialValveServer);

		co_return std::make_tuple<bool, uint32_t, uint32_t, uint32_t>(true, 
			(uint32_t)challenge, (uint32_t)auth_protocol_version, (uint32_t)connect_protocol_version);
	}

	asio::awaitable<void> HandleIncomingPacket(udp::socket& socket, udp::endpoint& remote_endpoint)
	{
		//Process all incoming packet here after we established connection.
		while (true)
		{
			co_await HandleStringCommand(socket, remote_endpoint);
			co_await socket.async_wait(socket.wait_read, asio::use_awaitable);
			size_t n = co_await socket.async_receive_from(asio::buffer(m_Buf), remote_endpoint, asio::use_awaitable);

			//TO DO : Process all other packets here
			ResetReadBuffer();

			if (ReadBufferHeaderInt32() == -1)
			{
				m_ReadBuf.ReadLong(); //-1

				ProcessConnectionlessPacket();
				continue;
			}

			ProcessPacket(n);
		}

		co_return;
	}

	asio::awaitable<void> HandleStringCommand(udp::socket& socket, udp::endpoint& remote_endpoint)
	{
		auto trim = [](std::string str) {
			str.erase(0, str.find_first_not_of(" "));
			str.erase(str.find_last_not_of(" ") + 1);
			return str;
		};

		auto isEmpty = [](std::string str) { return !str.empty(); };

		{
			std::lock_guard<std::mutex> lock(m_CommandVecLock);

			for (std::string command : m_VecCommand | std::views::transform(trim) | std::views::filter(isEmpty))
			{
				co_await SendStringCommand(socket, remote_endpoint, command);
			}

			m_VecCommand.clear();
		}
	}
	

	void ProcessPacket(int packetSize)
	{
		//Message decryption, for now we only support default encryption key
		CUtlMemoryFixedGrowable< byte, NET_COMPRESSION_STACKBUF_SIZE > memDecryptedAll(NET_COMPRESSION_STACKBUF_SIZE);
		IceKey iceKey(2);
		iceKey.set(GetEncryptionKey());

		if ((packetSize % iceKey.blockSize()) == 0)
		{
			// Decrypt the message
			memDecryptedAll.EnsureCapacity(packetSize);
			unsigned char* pchCryptoBuffer = (unsigned char*)stackalloc(iceKey.blockSize());
			for (int k = 0; k < (int)packetSize; k += iceKey.blockSize())
			{
				iceKey.decrypt((const unsigned char*)(m_Buf + k), pchCryptoBuffer);
				Q_memcpy(memDecryptedAll.Base() + k, pchCryptoBuffer, iceKey.blockSize());
			}

			// Check how much random fudge we have
			int numRandomFudgeBytes = *memDecryptedAll.Base();
			if ((numRandomFudgeBytes > 0) && (int(numRandomFudgeBytes + 1 + sizeof(int32)) < packetSize))
			{
				// Fetch the size of the encrypted message
				int32 numBytesWrittenWire = 0;
				Q_memcpy(&numBytesWrittenWire, memDecryptedAll.Base() + 1 + numRandomFudgeBytes, sizeof(int32));
				int32 const numBytesWritten = BigLong(numBytesWrittenWire);	// byteswap from the wire

				// Make sure the total size of the message matches the expectations
				if (int(numRandomFudgeBytes + 1 + sizeof(int32) + numBytesWritten) == packetSize)
				{
					// Fix the packet to point at decrypted data!
					packetSize = numBytesWritten;
					Q_memcpy(m_Buf, memDecryptedAll.Base() + 1 + numRandomFudgeBytes + sizeof(int32), packetSize);
				}
			}
		}
		
		//Is this message compressed?
		constexpr auto NET_HEADER_FLAG_COMPRESSEDPACKET = -3;
		if (ReadBufferHeaderInt32() == NET_HEADER_FLAG_COMPRESSEDPACKET)
		{
			byte* pCompressedData = (byte*)m_Buf + sizeof(unsigned int);

			CLZSS lzss;
			// Decompress
			int actualSize = lzss.GetActualSize(pCompressedData);
			if (actualSize <= 0 || actualSize > NET_MAX_PAYLOAD)
				return;

			MEM_ALLOC_CREDIT();
			CUtlMemoryFixedGrowable< byte, NET_COMPRESSION_STACKBUF_SIZE > memDecompressed(NET_COMPRESSION_STACKBUF_SIZE);
			memDecompressed.EnsureCapacity(actualSize);

			unsigned int uDecompressedSize = lzss.SafeUncompress(pCompressedData, memDecompressed.Base(), actualSize);
			if (uDecompressedSize == 0 || ((unsigned int)actualSize) != uDecompressedSize)
			{
				return;
			}

			// packet->wiresize is already set
			Q_memcpy(m_Buf, memDecompressed.Base(), uDecompressedSize);

			packetSize = uDecompressedSize;
		}

		PrintRecvBuffer(packetSize);

		int flags = ProcessPacketHeader(m_ReadBuf);

		if (flags == -1)
			return;

		//TO DO: Handle netmessages here
	}

	//Incomplete!!!
	int ProcessPacketHeader(bf_read& msg)
	{
		ResetReadBuffer();
		int sequence = msg.ReadLong();
		int sequence_ack = msg.ReadLong();
		int flags = msg.ReadByte();

		//TO DO: Check crc? inReliable, chocked

		m_nInSequenceNr = sequence;
		m_nOutSequenceNrAck = sequence_ack;

		return flags;
	}

	void ProcessConnectionlessPacket()
	{
		char c = m_ReadBuf.ReadByte();
		std::cout << std::format("Get connectionless packet : {:#04x}\n", (uint8_t)c);
	}

	asio::awaitable<void> SetSignonState(udp::socket& socket, udp::endpoint& remote_endpoint, int state, int count)
	{
		CNETMsg_SignonState_t signonState(state, count);
		co_await SendProtobufMessage(socket, remote_endpoint, signonState);
	}

	asio::awaitable<void> SendStringCommand(udp::socket& socket, udp::endpoint& remote_endpoint, std::string& command)
	{
		CNETMsg_StringCmd_t stringCmd(command.c_str());

		if (command == "disconnect")
		{
			CNETMsg_Disconnect_t disconnect;
			disconnect.set_text("Disconnect.");
			co_await SendProtobufMessage(socket, remote_endpoint, disconnect);
		}

		co_await SendProtobufMessage(socket, remote_endpoint, stringCmd);
	}

	//TO DO: Setup a netmessage stream instead of one netmessage a datagram
	asio::awaitable<void> SendProtobufMessage(udp::socket& socket, udp::endpoint& remote_endpoint, INetMessage& msg)
	{
		ResetWriteBuffer();
		m_WriteBuf.WriteLong(m_nOutSequenceNr++); //out sequence
		m_WriteBuf.WriteLong(m_nInSequenceNr); //In sequence
		bf_write flagsPos = m_WriteBuf;
		m_WriteBuf.WriteByte(0); //Flag place holder
		m_WriteBuf.WriteShort(0);//checksum place holder

		int nCheckSumStart = m_WriteBuf.GetNumBytesWritten();

		m_WriteBuf.WriteByte(0); //InReliableState

		msg.WriteToBuffer(m_WriteBuf);
		
		flagsPos.WriteByte(0);

		const void* pvData = m_WriteBuf.GetData() + nCheckSumStart;
		int nCheckSumBytes = m_WriteBuf.GetNumBytesWritten() - nCheckSumStart;
		unsigned short usCheckSum = BufferToShortChecksum(pvData, nCheckSumBytes);
		flagsPos.WriteUBitLong(usCheckSum, 16);

		auto length = EncryptDatagram();
		co_await socket.async_send_to(asio::buffer(m_Buf, length), remote_endpoint, asio::use_awaitable);
	}

	//can't use stackalloc in a awaiter function, extract it out
	int EncryptDatagram()
	{
		CUtlMemoryFixedGrowable< byte, NET_COMPRESSION_STACKBUF_SIZE > memEncryptedAll(NET_COMPRESSION_STACKBUF_SIZE);
		int length = m_WriteBuf.GetNumBytesWritten();

		IceKey iceKey(2);
		iceKey.set(GetEncryptionKey());

		// Generate some random fudge, ICE operates on 64-bit blocks, so make sure our total size is a multiple of 8 bytes
		int numRandomFudgeBytes = RandomInt(16, 72);
		int numTotalEncryptedBytes = 1 + numRandomFudgeBytes + sizeof(int32) + length;
		numRandomFudgeBytes += iceKey.blockSize() - (numTotalEncryptedBytes % iceKey.blockSize());
		numTotalEncryptedBytes = 1 + numRandomFudgeBytes + sizeof(int32) + length;

		char* pchRandomFudgeBytes = (char*)stackalloc(numRandomFudgeBytes);
		for (int k = 0; k < numRandomFudgeBytes; ++k)
			pchRandomFudgeBytes[k] = RandomInt(16, 250);

		// Prepare the encrypted memory
		memEncryptedAll.EnsureCapacity(numTotalEncryptedBytes);
		*memEncryptedAll.Base() = numRandomFudgeBytes;
		Q_memcpy(memEncryptedAll.Base() + 1, pchRandomFudgeBytes, numRandomFudgeBytes);

		int32 const numBytesWrittenWire = BigLong(length);	// byteswap for the wire
		Q_memcpy(memEncryptedAll.Base() + 1 + numRandomFudgeBytes, &numBytesWrittenWire, sizeof(numBytesWrittenWire));
		Q_memcpy(memEncryptedAll.Base() + 1 + numRandomFudgeBytes + sizeof(int32), m_Buf, length);

		// Encrypt the message
		unsigned char* pchCryptoBuffer = (unsigned char*)stackalloc(iceKey.blockSize());
		for (int k = 0; k < numTotalEncryptedBytes; k += iceKey.blockSize())
		{
			iceKey.encrypt((const unsigned char*)(memEncryptedAll.Base() + k), pchCryptoBuffer);
			Q_memcpy(memEncryptedAll.Base() + k, pchCryptoBuffer, iceKey.blockSize());
		}

		Q_memcpy(m_Buf, memEncryptedAll.Base(), numTotalEncryptedBytes);

		return numTotalEncryptedBytes;
	}

private:

	inline byte* GetEncryptionKey()
	{
		static uint32 unHostVersion = m_HostVersion;
		static byte pubEncryptionKey[NET_CRYPT_KEY_LENGTH] = {
			'C', 'S', 'G', 'O',
			byte((unHostVersion >> 0) & 0xFF), byte((unHostVersion >> 8) & 0xFF), byte((unHostVersion >> 16) & 0xFF), byte((unHostVersion >> 24) & 0xFF),
			byte((unHostVersion >> 2) & 0xFF), byte((unHostVersion >> 10) & 0xFF), byte((unHostVersion >> 18) & 0xFF), byte((unHostVersion >> 26) & 0xFF),
			byte((unHostVersion >> 4) & 0xFF), byte((unHostVersion >> 12) & 0xFF), byte((unHostVersion >> 20) & 0xFF), byte((unHostVersion >> 28) & 0xFF),
		};

		return pubEncryptionKey;
	}

	inline unsigned short BufferToShortChecksum(const void* pvData, size_t nLength)
	{
		CRC32_t crc = CRC32_ProcessSingleBuffer(pvData, nLength);

		unsigned short lowpart = (crc & 0xffff);
		unsigned short highpart = ((crc >> 16) & 0xffff);

		return (unsigned short)(lowpart ^ highpart);
	}

	inline void ResetWriteBuffer()
	{
		m_WriteBuf.Reset();
	}

	inline void ResetReadBuffer()
	{
		m_ReadBuf.Seek(0);
	}

	inline int ReadBufferHeaderInt32()
	{
		return *(int*)m_Buf;
	}

	inline void PrintRecvBuffer(size_t bytes)
	{
		for (size_t i = 0; i < bytes; i++)
		{
			printf("%02X ", m_Buf[i] & 0xFF);
		}
		printf("\n");
	}

private:
	char m_Buf[10240];
	bf_write m_WriteBuf;
	bf_read m_ReadBuf;

	//commandline
	std::mutex m_CommandVecLock;
	std::vector<std::string> m_VecCommand;

	//host information
	int m_HostVersion;

	//Net channel
	int m_nInSequenceNr = 0;
	int m_nOutSequenceNr = 1;
	int m_nOutSequenceNrAck = 0;

	//server info
	std::string m_Ip;
	std::string m_NickName;
	std::string m_PassWord;
	uint16_t m_Port;
};

#endif // !__TINY_CSGO_CLIENT_HEADER__
