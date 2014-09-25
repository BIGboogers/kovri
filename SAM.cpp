#include <string.h>
#include <boost/bind.hpp>
#include "base64.h"
#include "Log.h"
#include "SAM.h"

namespace i2p
{
namespace stream
{
	SAMSocket::SAMSocket (SAMBridge& owner): 
		m_Owner (owner), m_Socket (m_Owner.GetService ()), m_SocketType (eSAMSocketTypeUnknown),
		m_Stream (nullptr)
	{
	}

	SAMSocket::~SAMSocket ()
	{
		if (m_Stream)
		{
			m_Stream->Close ();
			DeleteStream (m_Stream);
		}
	}	

	void SAMSocket::Terminate ()
	{
		if (m_Stream)
		{
			m_Stream->Close ();
			DeleteStream (m_Stream);
			m_Stream = nullptr;
		}
		if (m_SocketType == eSAMSocketTypeSession)
			m_Owner.CloseSession (m_ID);
		delete this;
	}

	void SAMSocket::ReceiveHandshake ()
	{
		m_Socket.async_read_some (boost::asio::buffer(m_Buffer, SAM_SOCKET_BUFFER_SIZE),                
			boost::bind(&SAMSocket::HandleHandshakeReceived, this, 
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}

	void SAMSocket::HandleHandshakeReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("SAM handshake read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			m_Buffer[bytes_transferred] = 0;
			LogPrint ("SAM handshake ", m_Buffer);
			if (!memcmp (m_Buffer, SAM_HANDSHAKE, sizeof (SAM_HANDSHAKE)))
			{
				// TODO: check version
				boost::asio::async_write (m_Socket, boost::asio::buffer (SAM_HANDSHAKE_REPLY, sizeof (SAM_HANDSHAKE_REPLY)), boost::asio::transfer_all (),
        			boost::bind(&SAMSocket::HandleHandshakeReplySent, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
			}
			else
			{
				LogPrint ("SAM hannshake mismatch");
				Terminate ();
			}
		}
	}

	void SAMSocket::HandleHandshakeReplySent (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("SAM handshake reply send error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{
			m_Socket.async_read_some (boost::asio::buffer(m_Buffer, SAM_SOCKET_BUFFER_SIZE),                
				boost::bind(&SAMSocket::HandleMessage, this, 
				boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));	
		}	
	}

	void SAMSocket::SendMessageReply (const char * msg, size_t len, bool close)
	{
		boost::asio::async_write (m_Socket, boost::asio::buffer (msg, len), boost::asio::transfer_all (),
			boost::bind(&SAMSocket::HandleMessageReplySent, this, 
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, close));
	}

	void SAMSocket::HandleMessageReplySent (const boost::system::error_code& ecode, std::size_t bytes_transferred, bool close)
	{
		if (ecode)
        {
			LogPrint ("SAM reply send error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{
			if (close)
				Terminate ();
			else
				Receive ();	
		}	
	}

	void SAMSocket::HandleMessage (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("SAM read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{
			m_Buffer[bytes_transferred] = 0;
			char * eol = strchr (m_Buffer, '\n');
			if (eol)
			{
				*eol = 0;
				if (!strcmp (m_Buffer, SAM_SESSION_CREATE))
					ProcessSessionCreate (eol + 1, bytes_transferred - (eol - m_Buffer) - 1);
				else if (!strcmp (m_Buffer, SAM_STREAM_CONNECT))
					ProcessStreamConnect (eol + 1, bytes_transferred - (eol - m_Buffer) - 1);
				else		
				{	
					LogPrint ("SAM unexpected message ", m_Buffer);		
					Terminate ();
				}
			}
			else
			{	
				LogPrint ("SAM malformed message");
				Terminate ();
			}
		}
	}

	void SAMSocket::ProcessSessionCreate (char * buf, size_t len)
	{
		std::map<std::string, std::string> params;
		ExtractParams (buf, len, params);
		std::string& id = params[SAM_PARAM_ID];
		std::string& destination = params[SAM_PARAM_DESTINATION];
		m_ID = id;
		auto session = m_Owner.CreateSession (id, destination == SAM_VALUE_TRANSIENT ? "" : destination); 
		if (session)
		{
			memcpy (m_Buffer, SAM_SESSION_CREATE_REPLY_OK, sizeof (SAM_SESSION_CREATE_REPLY_OK));
			uint8_t ident[1024];
			size_t l = session->localDestination->GetPrivateKeys ().ToBuffer (ident, 1024);
			size_t l1 = i2p::data::ByteStreamToBase64 (ident, l, m_Buffer + sizeof (SAM_SESSION_CREATE_REPLY_OK), 
				SAM_SOCKET_BUFFER_SIZE - sizeof (SAM_SESSION_CREATE_REPLY_OK));
			SendMessageReply (m_Buffer, sizeof (SAM_SESSION_CREATE_REPLY_OK) + l1, false);
		}
		else
			SendMessageReply (SAM_SESSION_CREATE_DUPLICATED_ID, sizeof(SAM_SESSION_CREATE_DUPLICATED_ID), true);
	}

	void SAMSocket::ProcessStreamConnect (char * buf, size_t len)
	{
		Receive ();
	}

	void SAMSocket::ExtractParams (char * buf, size_t len, std::map<std::string, std::string>& params)
	{
		while (char * eol = strchr (buf, '\n'))
		{
			*eol = 0;
			char * value = strchr (buf, '=');
			if (value)
			{
				*value = 0;
				value++;
				params[buf] = value;
			}	
			buf = eol + 1;
		}
	}	

	void SAMSocket::Receive ()
	{
		m_Socket.async_read_some (boost::asio::buffer(m_Buffer, SAM_SOCKET_BUFFER_SIZE),                
			boost::bind(&SAMSocket::HandleReceived, this, 
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}

	void SAMSocket::HandleReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("SAM read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{
			if (m_Stream)
				m_Stream->Send ((uint8_t *)m_Buffer, bytes_transferred, 0);
			Receive ();
		}
	}

	void SAMSocket::StreamReceive ()
	{
		if (m_Stream)
			m_Stream->AsyncReceive (boost::asio::buffer (m_StreamBuffer, SAM_SOCKET_BUFFER_SIZE),
				boost::bind (&SAMSocket::HandleStreamReceive, this,
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred),
				SAM_SOCKET_CONNECTION_MAX_IDLE);
	}	

	void SAMSocket::HandleStreamReceive (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
		{
			LogPrint ("SAM stream read error: ", ecode.message ());
			Terminate ();
		}
		else
		{
			boost::asio::async_write (m_Socket, boost::asio::buffer (m_StreamBuffer, bytes_transferred),
        		boost::bind (&SAMSocket::HandleWriteStreamData, this, boost::asio::placeholders::error));
		}
	}

	void SAMSocket::HandleWriteStreamData (const boost::system::error_code& ecode)
	{
		if (ecode)
		{
			LogPrint ("SAM socket write error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
			StreamReceive ();
	}

	SAMBridge::SAMBridge (int port):
		m_IsRunning (false), m_Thread (nullptr),
		m_Acceptor (m_Service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
		m_NewSocket	(nullptr)
	{
	}

	SAMBridge::~SAMBridge ()
	{
		Stop ();
		delete m_NewSocket;
	}	

	void SAMBridge::Start ()
	{
		Accept ();
		m_Thread = new std::thread (std::bind (&SAMBridge::Run, this));
	}

	void SAMBridge::Stop ()
	{
		m_IsRunning = false;
		m_Service.stop ();
		if (m_Thread)
		{	
			m_Thread->join (); 
			delete m_Thread;
			m_Thread = nullptr;
		}	
	}

	void SAMBridge::Run () 
	{ 
		while (m_IsRunning)
		{
			try
			{	
				m_Service.run ();
			}
			catch (std::exception& ex)
			{
				LogPrint ("SAM: ", ex.what ());
			}	
		}	
	}

	void SAMBridge::Accept ()
	{
		m_NewSocket = new SAMSocket (*this);
		m_Acceptor.async_accept (m_NewSocket->GetSocket (), boost::bind (&SAMBridge::HandleAccept, this,
			boost::asio::placeholders::error));
	}

	void SAMBridge::HandleAccept(const boost::system::error_code& ecode)
	{
		if (!ecode)
		{
			LogPrint ("New SAM connection from ", m_NewSocket->GetSocket ().remote_endpoint ());
			m_NewSocket->ReceiveHandshake ();		
		}
		else
		{
			delete m_NewSocket;
			m_NewSocket = nullptr;	
		}

		if (ecode != boost::asio::error::operation_aborted)
			Accept ();
	}

	SAMSession * SAMBridge::CreateSession (const std::string& id, const std::string& destination)
	{
		if (m_Sessions.find (id) != m_Sessions.end ()) // session exists
 			return nullptr;

		StreamingDestination * localDestination = nullptr; 
		if (destination != "")
		{
			uint8_t * buf = new uint8_t[destination.length ()];
			size_t l = i2p::data::Base64ToByteStream (destination.c_str (), destination.length (), buf, destination.length ());
			i2p::data::PrivateKeys keys;
			keys.FromBuffer (buf, l);
			delete[] buf;
			localDestination = GetLocalDestination (keys);
		}
		else // transient
			localDestination = CreateNewLocalDestination (); 
		if (localDestination)
		{
			SAMSession session;
			session.localDestination = localDestination;
			session.isTransient = destination == "";
			m_Sessions[id] = session;
			return &m_Sessions[id];
		}
		return nullptr;
	}

	void SAMBridge::CloseSession (const std::string& id)
	{
		auto it = m_Sessions.find (id);
		if (it != m_Sessions.end ())
		{
			for (auto it1 : it->second.sockets)
				delete it1;
			it->second.sockets.clear ();
			if (it->second.isTransient)
				DeleteLocalDestination (it->second.localDestination);
			m_Sessions.erase (it);
		}
	}

	SAMSession * SAMBridge::FindSession (const std::string& id)
	{
		auto it = m_Sessions.find (id);
		if (it != m_Sessions.end ())
			return &it->second;
		return nullptr;
	}
}
}
