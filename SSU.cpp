#include <string.h>
#include <boost/bind.hpp>
#include <cryptopp/dh.h>
#include <cryptopp/sha.h>
#include "CryptoConst.h"
#include "Log.h"
#include "Timestamp.h"
#include "RouterContext.h"
#include "Transports.h"
#include "hmac.h"
#include "SSU.h"

namespace i2p
{
namespace transport
{

	SSUSession::SSUSession (SSUServer& server, boost::asio::ip::udp::endpoint& remoteEndpoint,
		const i2p::data::RouterInfo * router, bool peerTest ): TransportSession (router), 
		m_Server (server), m_RemoteEndpoint (remoteEndpoint), 
		m_Timer (m_Server.GetService ()), m_PeerTest (peerTest),
 		m_State (eSessionStateUnknown), m_IsSessionKey (false), m_RelayTag (0),
		m_Data (*this), m_NumSentBytes (0), m_NumReceivedBytes (0)
	{
		m_CreationTime = i2p::util::GetSecondsSinceEpoch ();
		if (!router) // incoming session
			ScheduleConnectTimer ();
	}

	SSUSession::~SSUSession ()
	{		
	}	
	
	void SSUSession::CreateAESandMacKey (const uint8_t * pubKey)
	{
		CryptoPP::DH dh (i2p::crypto::elgp, i2p::crypto::elgg);
		uint8_t sharedKey[256];
		if (!dh.Agree (sharedKey, m_DHKeysPair->privateKey, pubKey))
		{    
		    LogPrint (eLogError, "Couldn't create shared key");
			return;
		};

		if (sharedKey[0] & 0x80)
		{
			m_SessionKey[0] = 0;
			memcpy (m_SessionKey + 1, sharedKey, 31);
			memcpy (m_MacKey, sharedKey + 31, 32);
		}	
		else if (sharedKey[0])
		{
			memcpy (m_SessionKey, sharedKey, 32);
			memcpy (m_MacKey, sharedKey + 32, 32);
		}	
		else
		{	
			// find first non-zero byte
			uint8_t * nonZero = sharedKey + 1;
			while (!*nonZero)
			{
				nonZero++;
				if (nonZero - sharedKey > 32)
				{
					LogPrint ("First 32 bytes of shared key is all zeros. Ignored");
					return;
				}	
			}
			
			memcpy (m_SessionKey, nonZero, 32);
			CryptoPP::SHA256().CalculateDigest(m_MacKey, nonZero, 64 - (nonZero - sharedKey));
		}
		m_IsSessionKey = true;
		m_SessionKeyEncryption.SetKey (m_SessionKey);
		m_SessionKeyDecryption.SetKey (m_SessionKey);
	}		

	void SSUSession::ProcessNextMessage (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint)
	{
		m_NumReceivedBytes += len;
		if (m_State == eSessionStateIntroduced)
		{
			// HolePunch received
			LogPrint ("SSU HolePunch of ", len, " bytes received");
			m_State = eSessionStateUnknown;
			Connect ();
		}
		else
		{
			if (!len) return; // ignore zero-length packets	
			if (m_State == eSessionStateEstablished)
				ScheduleTermination ();		
			
			if (m_IsSessionKey && Validate (buf, len, m_MacKey)) // try session key first
				DecryptSessionKey (buf, len);	
			else 
			{
				// try intro key depending on side
				auto introKey = GetIntroKey ();
				if (introKey && Validate (buf, len, introKey))
					Decrypt (buf, len, introKey);
				else
				{    
					// try own intro key
					auto address = i2p::context.GetRouterInfo ().GetSSUAddress ();
					if (!address)
					{
						LogPrint (eLogError, "SSU is not supported");
						return;
					}	
					if (Validate (buf, len, address->key))
						Decrypt (buf, len, address->key);
					else
					{
						LogPrint (eLogError, "MAC verification failed ", len, " bytes from ", senderEndpoint);
						m_Server.DeleteSession (this); 
						return;
					}	
				}	
			}	
			// successfully decrypted
			ProcessMessage (buf, len, senderEndpoint);
		}	
	}

	void SSUSession::ProcessMessage (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint)
	{
		SSUHeader * header = (SSUHeader *)buf;
		switch (header->GetPayloadType ())
		{
			case PAYLOAD_TYPE_DATA:
				LogPrint (eLogDebug, "SSU data received");
				ProcessData (buf + sizeof (SSUHeader), len - sizeof (SSUHeader));
			break;
			case PAYLOAD_TYPE_SESSION_REQUEST:
				ProcessSessionRequest (buf, len, senderEndpoint);				
			break;
			case PAYLOAD_TYPE_SESSION_CREATED:
				ProcessSessionCreated (buf, len);
			break;
			case PAYLOAD_TYPE_SESSION_CONFIRMED:
				ProcessSessionConfirmed (buf, len);
			break;	
			case PAYLOAD_TYPE_PEER_TEST:
				LogPrint (eLogDebug, "SSU peer test received");
				ProcessPeerTest (buf + sizeof (SSUHeader), len - sizeof (SSUHeader), senderEndpoint);
			break;
			case PAYLOAD_TYPE_SESSION_DESTROYED:
			{
				LogPrint (eLogDebug, "SSU session destroy received");
				m_Server.DeleteSession (this); // delete this 
				break;
			}	
			case PAYLOAD_TYPE_RELAY_RESPONSE:
				ProcessRelayResponse (buf, len);
				if (m_State != eSessionStateEstablished)
					m_Server.DeleteSession (this);
			break;
			case PAYLOAD_TYPE_RELAY_REQUEST:
				LogPrint (eLogDebug, "SSU relay request received");
				ProcessRelayRequest (buf + sizeof (SSUHeader), len - sizeof (SSUHeader), senderEndpoint);
			break;
			case PAYLOAD_TYPE_RELAY_INTRO:
				LogPrint (eLogDebug, "SSU relay intro received");
				ProcessRelayIntro (buf + sizeof (SSUHeader), len - sizeof (SSUHeader));
			break;
			default:
				LogPrint (eLogWarning, "Unexpected SSU payload type ", (int)header->GetPayloadType ());
		}
	}

	void SSUSession::ProcessSessionRequest (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint)
	{
		LogPrint (eLogDebug, "Session request received");	
		m_RemoteEndpoint = senderEndpoint;
		if (!m_DHKeysPair)
			m_DHKeysPair = transports.GetNextDHKeysPair ();
		CreateAESandMacKey (buf + sizeof (SSUHeader));
		SendSessionCreated (buf + sizeof (SSUHeader));
	}

	void SSUSession::ProcessSessionCreated (uint8_t * buf, size_t len)
	{
		if (!m_RemoteRouter || !m_DHKeysPair)
		{
			LogPrint (eLogWarning, "Unsolicited session created message");
			return;
		}

		LogPrint (eLogDebug, "Session created received");	
		m_Timer.cancel (); // connect timer
		SignedData s; // x,y, our IP, our port, remote IP, remote port, relayTag, signed on time 
		uint8_t * payload = buf + sizeof (SSUHeader);	
		uint8_t * y = payload;
		CreateAESandMacKey (y);
		s.Insert (m_DHKeysPair->publicKey, 256); // x
		s.Insert (y, 256); // y
		payload += 256;
		uint8_t addressSize = *payload;
		payload += 1; // size
		uint8_t * ourAddress = payload;
		boost::asio::ip::address ourIP;
		if (addressSize == 4) // v4
		{	
			boost::asio::ip::address_v4::bytes_type bytes;
			memcpy (bytes.data (), ourAddress, 4);
			ourIP = boost::asio::ip::address_v4 (bytes);
		}	
		else // v6
		{
			boost::asio::ip::address_v6::bytes_type bytes;
			memcpy (bytes.data (), ourAddress, 16);
			ourIP = boost::asio::ip::address_v6 (bytes);
		}	
		s.Insert (ourAddress, addressSize); // our IP 
		payload += addressSize; // address
		uint16_t ourPort = be16toh (*(uint16_t *)payload);
		s.Insert (payload, 2); // our port
		payload += 2; // port
		LogPrint ("Our external address is ", ourIP.to_string (), ":", ourPort);
		i2p::context.UpdateAddress (ourIP);
		if (m_RemoteEndpoint.address ().is_v4 ())
			s.Insert (m_RemoteEndpoint.address ().to_v4 ().to_bytes ().data (), 4); // remote IP v4
		else
			s.Insert (m_RemoteEndpoint.address ().to_v6 ().to_bytes ().data (), 16); // remote IP v6
		s.Insert (htobe16 (m_RemoteEndpoint.port ())); // remote port
		s.Insert (payload, 8); // relayTag and signed on time 
		m_RelayTag = be32toh (*(uint32_t *)payload);
		payload += 4; // relayTag
		payload += 4; // signed on time
		// decrypt signature
		size_t signatureLen = m_RemoteIdentity.GetSignatureLen ();
		size_t paddingSize = signatureLen & 0x0F; // %16
		if (paddingSize > 0) signatureLen += (16 - paddingSize);
		m_SessionKeyDecryption.SetIV (((SSUHeader *)buf)->iv);
		m_SessionKeyDecryption.Decrypt (payload, signatureLen, payload);
		// verify
		if (!s.Verify (m_RemoteIdentity, payload))
			LogPrint (eLogError, "SSU signature verification failed");
		
		SendSessionConfirmed (y, ourAddress, addressSize + 2);
	}	

	void SSUSession::ProcessSessionConfirmed (uint8_t * buf, size_t len)
	{
		LogPrint (eLogDebug, "Session confirmed received");	
		uint8_t * payload = buf + sizeof (SSUHeader);
		payload++; // identity fragment info
		uint16_t identitySize = be16toh (*(uint16_t *)payload);	
		payload += 2; // size of identity fragment
		m_RemoteIdentity.FromBuffer (payload, identitySize);
		m_Data.UpdatePacketSize (m_RemoteIdentity.GetIdentHash ());
		payload += identitySize; // identity	
		payload += 4; // signed-on time
		size_t paddingSize = (payload - buf) + m_RemoteIdentity.GetSignatureLen ();
		paddingSize &= 0x0F;  // %16
		if (paddingSize > 0) paddingSize = 16 - paddingSize;
		payload += paddingSize;
		// TODO: verify signature (need data from session request), payload points to signature
		SendI2NPMessage (CreateDeliveryStatusMsg (0));
		Established ();
	}

	void SSUSession::SendSessionRequest ()
	{
		auto introKey = GetIntroKey ();
		if (!introKey)
		{
			LogPrint (eLogError, "SSU is not supported");
			return;
		}
	
		uint8_t buf[320 + 18]; // 304 bytes for ipv4, 320 for ipv6
		uint8_t * payload = buf + sizeof (SSUHeader);
		memcpy (payload, m_DHKeysPair->publicKey, 256); // x
		bool isV4 = m_RemoteEndpoint.address ().is_v4 ();
		if (isV4)
		{
			payload[256] = 4; 
			memcpy (payload + 257, m_RemoteEndpoint.address ().to_v4 ().to_bytes ().data(), 4); 
		}
		else
		{
			payload[256] = 16; 
			memcpy (payload + 257, m_RemoteEndpoint.address ().to_v6 ().to_bytes ().data(), 16); 
		}	
		
		uint8_t iv[16];
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		rnd.GenerateBlock (iv, 16); // random iv
		FillHeaderAndEncrypt (PAYLOAD_TYPE_SESSION_REQUEST, buf, isV4 ? 304 : 320, introKey, iv, introKey);
		m_Server.Send (buf, isV4 ? 304 : 320, m_RemoteEndpoint);
	}

	void SSUSession::SendRelayRequest (uint32_t iTag, const uint8_t * iKey)
	{
		auto address = i2p::context.GetRouterInfo ().GetSSUAddress ();
		if (!address)
		{
			LogPrint (eLogError, "SSU is not supported");
			return;
		}
	
		uint8_t buf[96 + 18]; 
		uint8_t * payload = buf + sizeof (SSUHeader);
		*(uint32_t *)payload = htobe32 (iTag);
		payload += 4;
		*payload = 0; // no address
		payload++;
		*(uint16_t *)payload = 0; // port = 0
		payload += 2;
		*payload = 0; // challenge
		payload++;	
		memcpy (payload, (const uint8_t *)address->key, 32);
		payload += 32;
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		*(uint32_t *)payload = htobe32 (rnd.GenerateWord32 ()); // nonce	

		uint8_t iv[16];
		rnd.GenerateBlock (iv, 16); // random iv
		if (m_State == eSessionStateEstablished)
			FillHeaderAndEncrypt (PAYLOAD_TYPE_RELAY_REQUEST, buf, 96, m_SessionKey, iv, m_MacKey);
		else
			FillHeaderAndEncrypt (PAYLOAD_TYPE_RELAY_REQUEST, buf, 96, iKey, iv, iKey);			
		m_Server.Send (buf, 96, m_RemoteEndpoint);
	}

	void SSUSession::SendSessionCreated (const uint8_t * x)
	{
		auto introKey = GetIntroKey ();
		auto address = i2p::context.GetRouterInfo ().GetSSUAddress ();
		if (!introKey || !address)
		{
			LogPrint (eLogError, "SSU is not supported");
			return;
		}
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		SignedData s; // x,y, remote IP, remote port, our IP, our port, relayTag, signed on time 
		s.Insert (x, 256); // x

		uint8_t buf[384 + 18];	
		uint8_t * payload = buf + sizeof (SSUHeader);
		memcpy (payload, m_DHKeysPair->publicKey, 256);
		s.Insert (payload, 256); // y
		payload += 256;
		if (m_RemoteEndpoint.address ().is_v4 ())
		{
			// ipv4
			*payload = 4;
			payload++;
			memcpy (payload, m_RemoteEndpoint.address ().to_v4 ().to_bytes ().data(), 4); 
			s.Insert (payload, 4); // remote endpoint IP V4
			payload += 4;
		}
		else
		{
			// ipv6
			*payload = 16;
			payload++;
			memcpy (payload, m_RemoteEndpoint.address ().to_v6 ().to_bytes ().data(), 16); 
			s.Insert (payload, 16); // remote endpoint IP V6
			payload += 16;
		}
		*(uint16_t *)(payload) = htobe16 (m_RemoteEndpoint.port ());
		s.Insert (payload, 2); // remote port
		payload += 2;
		if (address->host.is_v4 ())
			s.Insert (address->host.to_v4 ().to_bytes ().data (), 4); // our IP V4
		else
			s.Insert (address->host.to_v6 ().to_bytes ().data (), 16); // our IP V6
		s.Insert (htobe16 (address->port)); // our port
		uint32_t relayTag = 0;
		if (i2p::context.GetRouterInfo ().IsIntroducer ())
		{
			relayTag = rnd.GenerateWord32 ();
			if (!relayTag) relayTag = 1;
			m_Server.AddRelay (relayTag, m_RemoteEndpoint);
		}
		*(uint32_t *)(payload) = htobe32 (relayTag); 
		payload += 4; // relay tag 
		*(uint32_t *)(payload) = htobe32 (i2p::util::GetSecondsSinceEpoch ()); // signed on time
		payload += 4;
		s.Insert (payload - 8, 8); // relayTag and signed on time 
		s.Sign (i2p::context.GetPrivateKeys (), payload); // DSA signature
		// TODO: fill padding with random data	

		uint8_t iv[16];
		rnd.GenerateBlock (iv, 16); // random iv
		// encrypt signature and padding with newly created session key	
		size_t signatureLen = i2p::context.GetIdentity ().GetSignatureLen ();
		size_t paddingSize = signatureLen & 0x0F; // %16
		if (paddingSize > 0) signatureLen += (16 - paddingSize);
		m_SessionKeyEncryption.SetIV (iv);
		m_SessionKeyEncryption.Encrypt (payload, signatureLen, payload);
		payload += signatureLen;
		size_t msgLen = payload - buf;
		
		// encrypt message with intro key
		FillHeaderAndEncrypt (PAYLOAD_TYPE_SESSION_CREATED, buf, msgLen, introKey, iv, introKey);	
		Send (buf, msgLen);
	}

	void SSUSession::SendSessionConfirmed (const uint8_t * y, const uint8_t * ourAddress, size_t ourAddressLen)
	{
		uint8_t buf[512 + 18];
		uint8_t * payload = buf + sizeof (SSUHeader);
		*payload = 1; // 1 fragment
		payload++; // info
		size_t identLen = i2p::context.GetIdentity ().GetFullLen (); // 387+ bytes
		*(uint16_t *)(payload) = htobe16 (identLen);
		payload += 2; // cursize
		i2p::context.GetIdentity ().ToBuffer (payload, identLen);
		payload += identLen;
		uint32_t signedOnTime = i2p::util::GetSecondsSinceEpoch ();
		*(uint32_t *)(payload) = htobe32 (signedOnTime); // signed on time
		payload += 4;
		auto signatureLen = i2p::context.GetIdentity ().GetSignatureLen ();
		size_t paddingSize = ((payload - buf) + signatureLen)%16;
		if (paddingSize > 0) paddingSize = 16 - paddingSize;
		// TODO: fill padding	
		payload += paddingSize; // padding size

		// signature		
		SignedData s; // x,y, our IP, our port, remote IP, remote port, relayTag, our signed on time 
		s.Insert (m_DHKeysPair->publicKey, 256); // x
		s.Insert (y, 256); // y
		s.Insert (ourAddress, ourAddressLen); // our address/port as seem by party
		if (m_RemoteEndpoint.address ().is_v4 ())
			s.Insert (m_RemoteEndpoint.address ().to_v4 ().to_bytes ().data (), 4); // remote IP V4
		else
			s.Insert (m_RemoteEndpoint.address ().to_v6 ().to_bytes ().data (), 16); // remote IP V6	
		s.Insert (htobe16 (m_RemoteEndpoint.port ())); // remote port
		s.Insert (htobe32 (m_RelayTag)); // relay tag
		s.Insert (htobe32 (signedOnTime)); // signed on time
		s.Sign (i2p::context.GetPrivateKeys (), payload); // DSA signature	
		payload += signatureLen;
		
		size_t msgLen = payload - buf;
		uint8_t iv[16];
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		rnd.GenerateBlock (iv, 16); // random iv
		// encrypt message with session key
		FillHeaderAndEncrypt (PAYLOAD_TYPE_SESSION_CONFIRMED, buf, msgLen, m_SessionKey, iv, m_MacKey);
		Send (buf, msgLen);
	}

	void SSUSession::ProcessRelayRequest (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& from)
	{
		uint32_t relayTag = be32toh (*(uint32_t *)buf);
		auto session = m_Server.FindRelaySession (relayTag);
		if (session)
		{
			buf += 4; // relay tag	
			uint8_t size = *buf;
			buf++; // size
			buf += size; // address
			buf += 2; // port
			uint8_t challengeSize = *buf;
			buf++; // challenge size
			buf += challengeSize;
			uint8_t * introKey = buf;
			buf += 32; // introkey
			uint32_t nonce = be32toh (*(uint32_t *)buf);
			SendRelayResponse (nonce, from, introKey, session->m_RemoteEndpoint);
			SendRelayIntro (session, from);
		}	
	}

	void SSUSession::SendRelayResponse (uint32_t nonce, const boost::asio::ip::udp::endpoint& from,
		const uint8_t * introKey, const boost::asio::ip::udp::endpoint& to)
	{
		uint8_t buf[80 + 18]; // 64 Alice's ipv4 and 80 Alice's ipv6
		uint8_t * payload = buf + sizeof (SSUHeader);
		// Charlie's address always v4
		if (!to.address ().is_v4 ())
		{
			LogPrint (eLogError, "Charlie's IP must be v4");
			return;
		}
		*payload = 4;
		payload++; // size
		*(uint32_t *)payload = htobe32 (to.address ().to_v4 ().to_ulong ()); // Charlie's IP
		payload += 4; // address	
		*(uint16_t *)payload = htobe16 (to.port ()); // Charlie's port
		payload += 2; // port
		// Alice
		bool isV4 = from.address ().is_v4 (); // Alice's
		if (isV4)
		{
			*payload = 4;
			payload++; // size
			memcpy (payload, from.address ().to_v4 ().to_bytes ().data (), 4); // Alice's IP V4
			payload += 4; // address	
		}
		else
		{
			*payload = 16;
			payload++; // size
			memcpy (payload, from.address ().to_v6 ().to_bytes ().data (), 16); // Alice's IP V6
			payload += 16; // address	
		}
		*(uint16_t *)payload = htobe16 (from.port ()); // Alice's port
		payload += 2; // port
		*(uint32_t *)payload = htobe32 (nonce);		

		if (m_State == eSessionStateEstablished)
		{	
			// encrypt with session key
			FillHeaderAndEncrypt (PAYLOAD_TYPE_RELAY_RESPONSE, buf, isV4 ? 64 : 80);
			Send (buf, isV4 ? 64 : 80);
		}	
		else
		{
			// ecrypt with Alice's intro key
			uint8_t iv[16];
			CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
			rnd.GenerateBlock (iv, 16); // random iv
			FillHeaderAndEncrypt (PAYLOAD_TYPE_RELAY_RESPONSE, buf, isV4 ? 64 : 80, introKey, iv, introKey);
			m_Server.Send (buf, isV4 ? 64 : 80, from);
		}	
		LogPrint (eLogDebug, "SSU relay response sent");
	}	

	void SSUSession::SendRelayIntro (SSUSession * session, const boost::asio::ip::udp::endpoint& from)
	{
		if (!session) return;	
		// Alice's address always v4
		if (!from.address ().is_v4 ())
		{
			LogPrint (eLogError, "Alice's IP must be v4");
			return;
		}	
		uint8_t buf[48 + 18];
		uint8_t * payload = buf + sizeof (SSUHeader);
		*payload = 4;
		payload++; // size
		*(uint32_t *)payload = htobe32 (from.address ().to_v4 ().to_ulong ()); // Alice's IP
		payload += 4; // address	
		*(uint16_t *)payload = htobe16 (from.port ()); // Alice's port
		payload += 2; // port
		*payload = 0; // challenge size	
		uint8_t iv[16];
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		rnd.GenerateBlock (iv, 16); // random iv
		FillHeaderAndEncrypt (PAYLOAD_TYPE_RELAY_INTRO, buf, 48, session->m_SessionKey, iv, session->m_MacKey);
		m_Server.Send (buf, 48, session->m_RemoteEndpoint);
		LogPrint (eLogDebug, "SSU relay intro sent");
	}
	
	void SSUSession::ProcessRelayResponse (uint8_t * buf, size_t len)
	{
		LogPrint (eLogDebug, "Relay response received");		
		uint8_t * payload = buf + sizeof (SSUHeader);
		uint8_t remoteSize = *payload; 
		payload++; // remote size
		//boost::asio::ip::address_v4 remoteIP (be32toh (*(uint32_t* )(payload)));
		payload += remoteSize; // remote address
		//uint16_t remotePort = be16toh (*(uint16_t *)(payload));
		payload += 2; // remote port
		uint8_t ourSize = *payload; 
		payload++; // our size
		boost::asio::ip::address ourIP;
		if (ourSize == 4)
		{
			boost::asio::ip::address_v4::bytes_type bytes;
			memcpy (bytes.data (), payload, 4);
			ourIP = boost::asio::ip::address_v4 (bytes);
		}
		else
		{
			boost::asio::ip::address_v6::bytes_type bytes;
			memcpy (bytes.data (), payload, 16);
			ourIP = boost::asio::ip::address_v6 (bytes);
		}
		payload += ourSize; // our address
		uint16_t ourPort = be16toh (*(uint16_t *)(payload));
		payload += 2; // our port
		LogPrint ("Our external address is ", ourIP.to_string (), ":", ourPort);
		i2p::context.UpdateAddress (ourIP);
	}

	void SSUSession::ProcessRelayIntro (uint8_t * buf, size_t len)
	{
		uint8_t size = *buf;
		if (size == 4)
		{
			buf++; // size
			boost::asio::ip::address_v4 address (be32toh (*(uint32_t* )buf));
			buf += 4; // address
			uint16_t port = be16toh (*(uint16_t *)buf);
			// send hole punch of 1 byte
			m_Server.Send (buf, 0, boost::asio::ip::udp::endpoint (address, port));
		}
		else
			LogPrint (eLogWarning, "Address size ", size, " is not supported"); 	
	}		

	void SSUSession::FillHeaderAndEncrypt (uint8_t payloadType, uint8_t * buf, size_t len, 
		const uint8_t * aesKey, const uint8_t * iv, const uint8_t * macKey)
	{	
		if (len < sizeof (SSUHeader))
		{
			LogPrint (eLogError, "Unexpected SSU packet length ", len);
			return;
		}
		SSUHeader * header = (SSUHeader *)buf;
		memcpy (header->iv, iv, 16);
		header->flag = payloadType << 4; // MSB is 0
		header->time = htobe32 (i2p::util::GetSecondsSinceEpoch ());
		uint8_t * encrypted = &header->flag;
		uint16_t encryptedLen = len - (encrypted - buf);
		i2p::crypto::CBCEncryption encryption;
		encryption.SetKey (aesKey);
		encryption.SetIV (iv);
		encryption.Encrypt (encrypted, encryptedLen, encrypted);
		// assume actual buffer size is 18 (16 + 2) bytes more
		memcpy (buf + len, iv, 16);
		*(uint16_t *)(buf + len + 16) = htobe16 (encryptedLen);
		i2p::crypto::HMACMD5Digest (encrypted, encryptedLen + 18, macKey, header->mac);
	}

	void SSUSession::FillHeaderAndEncrypt (uint8_t payloadType, uint8_t * buf, size_t len)
	{
		if (len < sizeof (SSUHeader))
		{
			LogPrint (eLogError, "Unexpected SSU packet length ", len);
			return;
		}
		SSUHeader * header = (SSUHeader *)buf;
		i2p::context.GetRandomNumberGenerator ().GenerateBlock (header->iv, 16); // random iv
		m_SessionKeyEncryption.SetIV (header->iv);
		header->flag = payloadType << 4; // MSB is 0
		header->time = htobe32 (i2p::util::GetSecondsSinceEpoch ());
		uint8_t * encrypted = &header->flag;
		uint16_t encryptedLen = len - (encrypted - buf);
		m_SessionKeyEncryption.Encrypt (encrypted, encryptedLen, encrypted);
		// assume actual buffer size is 18 (16 + 2) bytes more
		memcpy (buf + len, header->iv, 16);
		*(uint16_t *)(buf + len + 16) = htobe16 (encryptedLen);
		i2p::crypto::HMACMD5Digest (encrypted, encryptedLen + 18, m_MacKey, header->mac);
	}	
		
	void SSUSession::Decrypt (uint8_t * buf, size_t len, const uint8_t * aesKey)
	{
		if (len < sizeof (SSUHeader))
		{
			LogPrint (eLogError, "Unexpected SSU packet length ", len);
			return;
		}
		SSUHeader * header = (SSUHeader *)buf;
		uint8_t * encrypted = &header->flag;
		uint16_t encryptedLen = len - (encrypted - buf);	
		i2p::crypto::CBCDecryption decryption;
		decryption.SetKey (aesKey);
		decryption.SetIV (header->iv);
		decryption.Decrypt (encrypted, encryptedLen, encrypted);
	}

	void SSUSession::DecryptSessionKey (uint8_t * buf, size_t len)
	{
		if (len < sizeof (SSUHeader))
		{
			LogPrint (eLogError, "Unexpected SSU packet length ", len);
			return;
		}
		SSUHeader * header = (SSUHeader *)buf;
		uint8_t * encrypted = &header->flag;
		uint16_t encryptedLen = len - (encrypted - buf);	
		if (encryptedLen > 0)
		{	
			m_SessionKeyDecryption.SetIV (header->iv);
			m_SessionKeyDecryption.Decrypt (encrypted, encryptedLen, encrypted);
		}	
	}	
		
	bool SSUSession::Validate (uint8_t * buf, size_t len, const uint8_t * macKey)
	{
		if (len < sizeof (SSUHeader))
		{
			LogPrint (eLogError, "Unexpected SSU packet length ", len);
			return false;
		}
		SSUHeader * header = (SSUHeader *)buf;
		uint8_t * encrypted = &header->flag;
		uint16_t encryptedLen = len - (encrypted - buf);
		// assume actual buffer size is 18 (16 + 2) bytes more
		memcpy (buf + len, header->iv, 16);
		*(uint16_t *)(buf + len + 16) = htobe16 (encryptedLen);
		uint8_t digest[16];
		i2p::crypto::HMACMD5Digest (encrypted, encryptedLen + 18, macKey, digest);
		return !memcmp (header->mac, digest, 16);
	}

	void SSUSession::Connect ()
	{
		if (m_State == eSessionStateUnknown)
		{	
			// set connect timer
			ScheduleConnectTimer ();
			m_DHKeysPair = transports.GetNextDHKeysPair ();
			SendSessionRequest ();
		}	
	}

	void SSUSession::ScheduleConnectTimer ()
	{
		m_Timer.cancel ();
		m_Timer.expires_from_now (boost::posix_time::seconds(SSU_CONNECT_TIMEOUT));
		m_Timer.async_wait (boost::bind (&SSUSession::HandleConnectTimer,
			this, boost::asio::placeholders::error));	
}

	void SSUSession::HandleConnectTimer (const boost::system::error_code& ecode)
	{
		if (!ecode)
		{
			// timeout expired
			LogPrint ("SSU session was not established after ", SSU_CONNECT_TIMEOUT, " second");
			Failed ();
		}	
	}	
	
	void SSUSession::Introduce (uint32_t iTag, const uint8_t * iKey)
	{
		if (m_State == eSessionStateUnknown)
		{	
			// set connect timer
			m_Timer.expires_from_now (boost::posix_time::seconds(SSU_CONNECT_TIMEOUT));
			m_Timer.async_wait (boost::bind (&SSUSession::HandleConnectTimer,
				this, boost::asio::placeholders::error));
		}	
		SendRelayRequest (iTag, iKey);
	}

	void SSUSession::WaitForIntroduction ()
	{
		m_State = eSessionStateIntroduced;
		// set connect timer
		m_Timer.expires_from_now (boost::posix_time::seconds(SSU_CONNECT_TIMEOUT));
		m_Timer.async_wait (boost::bind (&SSUSession::HandleConnectTimer,
			this, boost::asio::placeholders::error));			
	}

	void SSUSession::Close ()
	{
		SendSesionDestroyed ();
		if (!m_DelayedMessages.empty ())
		{
			for (auto it :m_DelayedMessages)
				DeleteI2NPMessage (it);
			m_DelayedMessages.clear ();
		}	
	}	

	void SSUSession::Established ()
	{
		m_State = eSessionStateEstablished;
		if (m_DHKeysPair)
		{
			delete m_DHKeysPair;
			m_DHKeysPair = nullptr;
		}
		SendI2NPMessage (CreateDatabaseStoreMsg ());
		if (!m_DelayedMessages.empty ())
		{
			for (auto it :m_DelayedMessages)
				m_Data.Send (it);
			m_DelayedMessages.clear ();
		}
		if (m_PeerTest && (m_RemoteRouter && m_RemoteRouter->IsPeerTesting ()))
			SendPeerTest ();
		ScheduleTermination ();
	}	

	void SSUSession::Failed ()
	{
		if (m_State != eSessionStateFailed)
		{	
			m_State = eSessionStateFailed;
			m_Server.DeleteSession (this); // delete this 
		}	
	}	

	void SSUSession::ScheduleTermination ()
	{
		m_Timer.cancel ();
		m_Timer.expires_from_now (boost::posix_time::seconds(SSU_TERMINATION_TIMEOUT));
		m_Timer.async_wait (boost::bind (&SSUSession::HandleTerminationTimer,
			this, boost::asio::placeholders::error));
	}

	void SSUSession::HandleTerminationTimer (const boost::system::error_code& ecode)
	{
		if (ecode != boost::asio::error::operation_aborted)
		{	
			LogPrint ("SSU no activity fo ", SSU_TERMINATION_TIMEOUT, " seconds");
			Failed ();
		}	
	}	
	
	const uint8_t * SSUSession::GetIntroKey () const
	{
		if (m_RemoteRouter)
		{
			// we are client
			auto address = m_RemoteRouter->GetSSUAddress ();
			return address ? (const uint8_t *)address->key : nullptr;
		}
		else
		{
			// we are server
			auto address = i2p::context.GetRouterInfo ().GetSSUAddress ();
			return address ? (const uint8_t *)address->key : nullptr;
		}
	}	

	void SSUSession::SendI2NPMessage (I2NPMessage * msg)
	{
		m_Server.GetService ().post (boost::bind (&SSUSession::PostI2NPMessage, this, msg));    
	}	

	void SSUSession::PostI2NPMessage (I2NPMessage * msg)
	{
		if (msg)
		{	
			if (m_State == eSessionStateEstablished)
				m_Data.Send (msg);
			else
				m_DelayedMessages.push_back (msg);
		}	
	}		
		
	void SSUSession::ProcessData (uint8_t * buf, size_t len)
	{
		m_Data.ProcessMessage (buf, len);
	}


	void SSUSession::ProcessPeerTest (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint)
	{
		uint8_t * buf1 = buf;
		uint32_t nonce = be32toh (*(uint32_t *)buf);
		buf += 4; // nonce
		uint8_t size = *buf;
		buf++; // size
		uint32_t address = (size == 4) ? *(uint32_t *)buf : 0; // use it as is
		buf += size; // address
		uint16_t port = *(uint16_t *)buf; // use it as is
		buf += 2; // port
		uint8_t * introKey = buf;
		if (port && !address)
		{
			LogPrint (eLogWarning, "Address of ", size, " bytes not supported");	
			return;
		}	
		if (m_PeerTestNonces.count (nonce) > 0)
		{
			// existing test
			if (m_PeerTest)
			{
				LogPrint (eLogDebug, "SSU peer test from Bob. We are Alice");
				m_PeerTestNonces.erase (nonce);
				m_PeerTest = false;
			}
			else if (port)
			{
				LogPrint (eLogDebug, "SSU peer test from Charlie. We are Bob");
				boost::asio::ip::udp::endpoint ep (boost::asio::ip::address_v4 (be32toh (address)), be16toh (port)); // Alice's address/port
				auto session = m_Server.FindSession (ep); // find session with Alice
				if (session)
					session->Send (PAYLOAD_TYPE_PEER_TEST, buf1, len); // back to Alice
			}
			else
			{
				LogPrint (eLogDebug, "SSU peer test from Alice. We are Charlie");
				SendPeerTest (nonce, senderEndpoint.address ().to_v4 ().to_ulong (),
						senderEndpoint.port (), introKey); // to Alice
			}
		}
		else
		{
			if (m_State == eSessionStateEstablished)
			{
				// new test
				m_PeerTestNonces.insert (nonce);
				if (port)
				{
					LogPrint (eLogDebug, "SSU peer test from Bob. We are Charlie");
					Send (PAYLOAD_TYPE_PEER_TEST, buf1, len); // back to Bob
					SendPeerTest (nonce, be32toh (address), be16toh (port), introKey); // to Alice
				}
				else
				{
					LogPrint (eLogDebug, "SSU peer test from Alice. We are Bob");
					auto session = m_Server.GetRandomEstablishedSession (this); // charlie
					if (session)
						session->SendPeerTest (nonce, senderEndpoint.address ().to_v4 ().to_ulong (),
							senderEndpoint.port (), introKey, false); 		
				}
			}
			else
				LogPrint (eLogDebug, "SSU peer test from Charlie. We are Alice");
		}	
	}
	
	void SSUSession::SendPeerTest (uint32_t nonce, uint32_t address, uint16_t port, 
		const uint8_t * introKey, bool toAddress)
	{
		uint8_t buf[80 + 18];
		uint8_t iv[16];
		uint8_t * payload = buf + sizeof (SSUHeader);
		*(uint32_t *)payload = htobe32 (nonce);
		payload += 4; // nonce	
		if (address)
		{					
			*payload = 4;
			payload++; // size
			*(uint32_t *)payload = htobe32 (address);
			payload += 4; // address
		}
		else
		{
			*payload = 0;
			payload++; //size
		}
		*(uint16_t *)payload = htobe16 (port);
		payload += 2; // port
		memcpy (payload, introKey, 32); // intro key

		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		rnd.GenerateBlock (iv, 16); // random iv
		if (toAddress)
		{	
			// encrypt message with specified intro key
			FillHeaderAndEncrypt (PAYLOAD_TYPE_PEER_TEST, buf, 80, introKey, iv, introKey);
			boost::asio::ip::udp::endpoint e (boost::asio::ip::address_v4 (address), port);
			m_Server.Send (buf, 80, e);
		}	
		else
		{
			// encrypt message with session key
			FillHeaderAndEncrypt (PAYLOAD_TYPE_PEER_TEST, buf, 80);
			Send (buf, 80);
		}	
	}	

	void SSUSession::SendPeerTest ()
	{
		LogPrint (eLogDebug, "SSU sending peer test");
		auto address = i2p::context.GetRouterInfo ().GetSSUAddress ();
		if (!address)
		{
			LogPrint (eLogError, "SSU is not supported. Can't send peer test");
			return;
		}
		uint32_t nonce = i2p::context.GetRandomNumberGenerator ().GenerateWord32 ();
		if (!nonce) nonce = 1;
		m_PeerTestNonces.insert (nonce);
		SendPeerTest (nonce, 0, 0, address->key, false); // address and port always zero for Alice
	}	

	void SSUSession::SendKeepAlive ()
	{
		if (m_State == eSessionStateEstablished)
		{	
			uint8_t buf[48 + 18];	
			uint8_t	* payload = buf + sizeof (SSUHeader);
			*payload = 0; // flags
			payload++;
			*payload = 0; // num fragments  
			// encrypt message with session key
			FillHeaderAndEncrypt (PAYLOAD_TYPE_DATA, buf, 48);
			Send (buf, 48);
			LogPrint (eLogDebug, "SSU keep-alive sent");
			ScheduleTermination ();
		}	
	}

	void SSUSession::SendSesionDestroyed ()
	{
		if (m_IsSessionKey)
		{
			uint8_t buf[48 + 18];
			// encrypt message with session key
			FillHeaderAndEncrypt (PAYLOAD_TYPE_SESSION_DESTROYED, buf, 48);
			Send (buf, 48);
			LogPrint (eLogDebug, "SSU session destroyed sent");
		}
	}	

	void SSUSession::Send (uint8_t type, const uint8_t * payload, size_t len)
	{
		uint8_t buf[SSU_MTU_V4 + 18];
		size_t msgSize = len + sizeof (SSUHeader); 
		if (msgSize > SSU_MTU_V4)
		{
			LogPrint (eLogWarning, "SSU payload size ", msgSize, " exceeds MTU");
			return;
		} 
		memcpy (buf + sizeof (SSUHeader), payload, len);
		// encrypt message with session key
		FillHeaderAndEncrypt (type, buf, msgSize);
		Send (buf, msgSize);
	}			

	void SSUSession::Send (const uint8_t * buf, size_t size)
	{
		m_NumSentBytes += size;
		m_Server.Send (buf, size, m_RemoteEndpoint);
	}	


	SSUServer::SSUServer (int port): m_Thread (nullptr), m_Work (m_Service),
		m_Endpoint (boost::asio::ip::udp::v4 (), port), m_Socket (m_Service, m_Endpoint),
		m_IntroducersUpdateTimer (m_Service)	
	{
		m_Socket.set_option (boost::asio::socket_base::receive_buffer_size (65535));
		m_Socket.set_option (boost::asio::socket_base::send_buffer_size (65535));
	}
	
	SSUServer::~SSUServer ()
	{
		for (auto it: m_Sessions)
			delete it.second;
	}

	void SSUServer::Start ()
	{
		m_IsRunning = true;
		m_Thread = new std::thread (std::bind (&SSUServer::Run, this));
		m_Service.post (boost::bind (&SSUServer::Receive, this));  
		if (i2p::context.IsUnreachable ())
			ScheduleIntroducersUpdateTimer ();
	}

	void SSUServer::Stop ()
	{
		DeleteAllSessions ();
		m_IsRunning = false;
		m_Service.stop ();
		m_Socket.close ();
		if (m_Thread)
		{	
			m_Thread->join (); 
			delete m_Thread;
			m_Thread = 0;
		}	
	}

	void SSUServer::Run () 
	{ 
		while (m_IsRunning)
		{
			try
			{	
				m_Service.run ();
			}
			catch (std::exception& ex)
			{
				LogPrint (eLogError, "SSU server: ", ex.what ());
			}	
		}	
	}
		
	void SSUServer::AddRelay (uint32_t tag, const boost::asio::ip::udp::endpoint& relay)
	{
		m_Relays[tag] = relay;
	}	

	SSUSession * SSUServer::FindRelaySession (uint32_t tag)
	{
		auto it = m_Relays.find (tag);
		if (it != m_Relays.end ())
			return FindSession (it->second);
		return nullptr;
	}

	void SSUServer::Send (const uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& to)
	{
		m_Socket.send_to (boost::asio::buffer (buf, len), to);
	}	

	void SSUServer::Receive ()
	{
		m_Socket.async_receive_from (boost::asio::buffer (m_ReceiveBuffer, SSU_MTU_V4), m_SenderEndpoint,
			boost::bind (&SSUServer::HandleReceivedFrom, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)); 
	}

	void SSUServer::HandleReceivedFrom (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (!ecode)
		{
			SSUSession * session = nullptr;
			auto it = m_Sessions.find (m_SenderEndpoint);
			if (it != m_Sessions.end ())
				session = it->second;
			if (!session)
			{
				session = new SSUSession (*this, m_SenderEndpoint);
				m_Sessions[m_SenderEndpoint] = session;
				LogPrint ("New SSU session from ", m_SenderEndpoint.address ().to_string (), ":", m_SenderEndpoint.port (), " created");
			}
			session->ProcessNextMessage (m_ReceiveBuffer, bytes_transferred, m_SenderEndpoint);
			Receive ();
		}
		else
			LogPrint ("SSU receive error: ", ecode.message ());
	}

	SSUSession * SSUServer::FindSession (const i2p::data::RouterInfo * router)
	{
		if (!router) return nullptr;
		auto address = router->GetSSUAddress ();
		if (!address) return nullptr;
		return FindSession (boost::asio::ip::udp::endpoint (address->host, address->port));
	}	

	SSUSession * SSUServer::FindSession (const boost::asio::ip::udp::endpoint& e)
	{
		auto it = m_Sessions.find (e);
		if (it != m_Sessions.end ())
			return it->second;
		else
			return nullptr;
	}
		
	SSUSession * SSUServer::GetSession (const i2p::data::RouterInfo * router, bool peerTest)
	{
		SSUSession * session = nullptr;
		if (router)
		{
			auto address = router->GetSSUAddress ();
			if (address)
			{
				boost::asio::ip::udp::endpoint remoteEndpoint (address->host, address->port);
				auto it = m_Sessions.find (remoteEndpoint);
				if (it != m_Sessions.end ())
					session = it->second;
				else
				{
					// otherwise create new session					
					session = new SSUSession (*this, remoteEndpoint, router, peerTest);
					m_Sessions[remoteEndpoint] = session;
					
					if (!router->UsesIntroducer ())
					{
						// connect directly						
						LogPrint ("Creating new SSU session to [", router->GetIdentHashAbbreviation (), "] ",
							remoteEndpoint.address ().to_string (), ":", remoteEndpoint.port ());
						session->Connect ();
					}
					else
					{
						// connect through introducer
						int numIntroducers = address->introducers.size ();
						if (numIntroducers > 0)
						{
							SSUSession * introducerSession = nullptr;
							const i2p::data::RouterInfo::Introducer * introducer = nullptr;
							// we might have a session to introducer already
							for (int i = 0; i < numIntroducers; i++)
							{
								introducer = &(address->introducers[i]);
								it = m_Sessions.find (boost::asio::ip::udp::endpoint (introducer->iHost, introducer->iPort));
								if (it != m_Sessions.end ())
								{
									introducerSession = it->second;
									break; 
								}	
							}

							if (introducerSession) // session found 
								LogPrint ("Session to introducer already exists");
							else // create new
							{
								LogPrint ("Creating new session to introducer");
								introducer = &(address->introducers[0]); // TODO:
								boost::asio::ip::udp::endpoint introducerEndpoint (introducer->iHost, introducer->iPort);
								introducerSession = new SSUSession (*this, introducerEndpoint, router);
								m_Sessions[introducerEndpoint] = introducerSession;													
							}	
							// introduce
							LogPrint ("Introduce new SSU session to [", router->GetIdentHashAbbreviation (), 
									"] through introducer ", introducer->iHost, ":", introducer->iPort);
							session->WaitForIntroduction ();	
							if (i2p::context.GetRouterInfo ().UsesIntroducer ()) // if we are unreachable
								Send (m_ReceiveBuffer, 0, remoteEndpoint); // send HolePunch
							introducerSession->Introduce (introducer->iTag, introducer->iKey);
						}
						else
						{	
							LogPrint (eLogWarning, "Can't connect to unreachable router. No introducers presented");
							m_Sessions.erase (remoteEndpoint);
							delete session;
							session = nullptr;
						}	
					}
				}
			}
			else
				LogPrint (eLogWarning, "Router ", router->GetIdentHashAbbreviation (), " doesn't have SSU address");
		}
		return session;
	}

	void SSUServer::DeleteSession (SSUSession * session)
	{
		if (session)
		{
			session->Close ();
			m_Sessions.erase (session->GetRemoteEndpoint ());
			delete session;
		}	
	}	

	void SSUServer::DeleteAllSessions ()
	{
		for (auto it: m_Sessions)
		{
			it.second->Close ();
			delete it.second;			
		}	
		m_Sessions.clear ();
	}

	template<typename Filter>
	SSUSession * SSUServer::GetRandomSession (Filter filter)
	{
		std::vector<SSUSession *> filteredSessions;
		for (auto s :m_Sessions)
			if (filter (s.second)) filteredSessions.push_back (s.second);
		if (filteredSessions.size () > 0)
		{
			auto ind = i2p::context.GetRandomNumberGenerator ().GenerateWord32 (0, filteredSessions.size ()-1);
			return filteredSessions[ind];
		}
		return nullptr;	
	}

	SSUSession * SSUServer::GetRandomEstablishedSession (const SSUSession * excluded)
	{
		return GetRandomSession (
			[excluded](SSUSession * session)->bool 
			{ 
				return session->GetState () == eSessionStateEstablished &&
					session != excluded; 
			}
								);
	}

	std::set<SSUSession *> SSUServer::FindIntroducers (int maxNumIntroducers)
	{
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		std::set<SSUSession *> ret;
		for (int i = 0; i < maxNumIntroducers; i++)
		{
			auto session = GetRandomSession (
				[&ret, ts](SSUSession * session)->bool 
				{ 
					return session->GetRelayTag () && !ret.count (session) &&
						session->GetState () == eSessionStateEstablished &&
						ts < session->GetCreationTime () + SSU_TO_INTRODUCER_SESSION_DURATION; 
				}
											);	
			if (session)
			{
				ret.insert (session);
				break;
			}	
		}
		return ret;
	}

	void SSUServer::ScheduleIntroducersUpdateTimer ()
	{
		m_IntroducersUpdateTimer.expires_from_now (boost::posix_time::seconds(SSU_KEEP_ALIVE_INTERVAL));
		m_IntroducersUpdateTimer.async_wait (boost::bind (&SSUServer::HandleIntroducersUpdateTimer,
			this, boost::asio::placeholders::error));	
	}

	void SSUServer::HandleIntroducersUpdateTimer (const boost::system::error_code& ecode)
	{
		if (!ecode)
		{
			// timeout expired
			std::list<boost::asio::ip::udp::endpoint> newList;
			size_t numIntroducers = 0;
			uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
			for (auto it :m_Introducers)
			{	
				auto session = FindSession (it);
				if (session && ts < session->GetCreationTime () + SSU_TO_INTRODUCER_SESSION_DURATION)
				{
					session->SendKeepAlive ();
					newList.push_back (it);
					numIntroducers++;
				}
				else	
					i2p::context.RemoveIntroducer (it);
			}

			if (numIntroducers < SSU_MAX_NUM_INTRODUCERS)
			{
				// create new
				auto introducers = FindIntroducers (SSU_MAX_NUM_INTRODUCERS);
				if (introducers.size () > 0)
				{
					for (auto it1: introducers)
					{
						auto router = it1->GetRemoteRouter ();
						if (router && i2p::context.AddIntroducer (*router, it1->GetRelayTag ()))
						{	
							newList.push_back (it1->GetRemoteEndpoint ());
							if (newList.size () >= SSU_MAX_NUM_INTRODUCERS) break;
						}	
					}	
				}	
			}	
			m_Introducers = newList;
			ScheduleIntroducersUpdateTimer ();
		}	
	}	
}
}

