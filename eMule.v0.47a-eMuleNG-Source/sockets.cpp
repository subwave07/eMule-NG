//this file is part of eMule
//Copyright (C)2002-2006 Merkur ( strEmail.Format("%s@%s", "devteam", "emule-project.net") / http://www.emule-project.net )
//
//This program is free software; you can redistribute it and/or
//modify it under the terms of the GNU General Public License
//as published by the Free Software Foundation; either
//version 2 of the License, or (at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program; if not, write to the Free Software
//Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#include "stdafx.h"
#include "emule.h"
#include "Sockets.h"
#include "Opcodes.h"
#include "UDPSocket.h"
#include "Exceptions.h"
#include "OtherFunctions.h"
#include "Statistics.h"
#include "ServerSocket.h"
#include "ServerList.h"
#include "Server.h"
#include "ListenSocket.h"
#include "SafeFile.h"
#include "Packets.h"
#include "SharedFileList.h"
#include "PeerCacheFinder.h"
#include "emuleDlg.h"
#include "SearchDlg.h"
#include "ServerWnd.h"
#include "TaskbarNotifier.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

static bool m_bUseNeXTConnect = false; //>>> UseNeXT Support

// CServerConnect

void CServerConnect::TryAnotherConnectionrequest()
{
	//For UseNeXT: allow only one connect at a time...
	if (connectionattemps.GetCount() < ((thePrefs.IsSafeServerConnectEnabled() || m_bUseNeXTConnect) ? 1 : 2)) //>>> UseNeXT Support
	{
		//>>> UseNeXT Support		
		CServer* next_server = NULL;
		if(!m_bUseNeXTConnect)
		{
			//USES_CONVERSION;
			//Always connect to UseNeXT Server!
#if defined RAZOR || defined RAID || defined TITAN || defined GWC
			next_server = theApp.serverlist->GetServerByAddress(_T("85.235.210.16"), 5000); 
#else
			next_server = theApp.serverlist->GetServerByAddress(_T("80.239.200.99"), 3000); 
#endif
			if(!next_server)
			{
#if defined RAZOR || defined RAID || defined TITAN || defined GWC
				CServer* newserv = new CServer(5000, _T("85.235.210.16"));
#else
				CServer* newserv = new CServer(3000, _T("80.239.200.99"));
#endif
				if(newserv && theApp.serverlist->AddServer(newserv))
				{
					next_server = newserv;
					m_bUseNeXTConnect = true;
				}
				else
					delete newserv;
			}
			else
				m_bUseNeXTConnect = true;
		}
		else
//<<< UseNeXT Support	
		/*CServer**/ next_server = used_list->GetNextServer();
		if (next_server == NULL)
		{
			if (connectionattemps.GetCount() == 0)
			{
				if (m_idRetryTimer == 0)
				{
					// 05-Nov-2003: If we have a very short server list, we could put serious load on those few servers
					// if we start the next connection tries without waiting.
					LogWarning(LOG_STATUSBAR, GetResString(IDS_OUTOFSERVERS));
					AddLogLine(false, GetResString(IDS_RECONNECT), CS_RETRYCONNECTTIME);
					VERIFY( (m_idRetryTimer = SetTimer(NULL, 0, 1000*CS_RETRYCONNECTTIME, RetryConnectTimer)) != NULL );
					if (thePrefs.GetVerbose() && !m_idRetryTimer)
						DebugLogError(_T("Failed to create 'server connect retry' timer - %s"), GetErrorMessage(GetLastError()));
				}
			}
			return;
		}

		// Barry - Only auto-connect to static server option
		if (thePrefs.GetAutoConnectToStaticServersOnly())
		{
			if (next_server->IsStaticMember())
                ConnectToServer(next_server, true);
		}
		else
			ConnectToServer(next_server, true);
	}
}

void CServerConnect::ConnectToAnyServer(uint32 startAt, bool prioSort, bool isAuto)
{
	lastStartAt = startAt;
	StopConnectionTry();
	Disconnect();
	connecting = true;
	singleconnecting = false;
	theApp.emuledlg->ShowConnectionState();

	// Barry - Only auto-connect to static server option
	if (thePrefs.GetAutoConnectToStaticServersOnly() && isAuto)
	{
		bool anystatic = false;
		CServer *next_server; 
		used_list->SetServerPosition( startAt );
		while ((next_server = used_list->GetNextServer()) != NULL)
		{
			if (next_server->IsStaticMember())
			{
				anystatic = true;
				break;
			}
		}
		if (!anystatic)
		{
			connecting = false;
			LogError(LOG_STATUSBAR, GetResString(IDS_ERR_NOVALIDSERVERSFOUND));
			return;
		}
	}

	used_list->SetServerPosition(startAt);
	if (thePrefs.GetUseServerPriorities() && prioSort)
		used_list->Sort();

	if (used_list->GetServerCount() == 0){
		connecting = false;
		LogError(LOG_STATUSBAR, GetResString(IDS_ERR_NOVALIDSERVERSFOUND));
		return;
	}
	theApp.listensocket->Process();

	TryAnotherConnectionrequest();
}

void CServerConnect::ConnectToServer(CServer* server, bool multiconnect)
{
	if (!multiconnect) {
		StopConnectionTry();
		Disconnect();
	}
	connecting = true;
	singleconnecting = !multiconnect;
	theApp.emuledlg->ShowConnectionState();

	CServerSocket* newsocket = new CServerSocket(this);
	m_lstOpenSockets.AddTail((void*&)newsocket);
	newsocket->Create(0, SOCK_STREAM, FD_READ | FD_WRITE | FD_CLOSE | FD_CONNECT, thePrefs.GetBindAddrA());
	newsocket->ConnectToServer(server);
	connectionattemps.SetAt(GetTickCount(), newsocket);
}

void CServerConnect::StopConnectionTry()
{
	connectionattemps.RemoveAll();
	connecting = false;
	singleconnecting = false;
	theApp.emuledlg->ShowConnectionState();

	if (m_idRetryTimer) 
	{ 
		KillTimer(NULL, m_idRetryTimer); 
		m_idRetryTimer= 0; 
	} 

	// close all currenty opened sockets except the one which is connected to our current server
	for( POSITION pos = m_lstOpenSockets.GetHeadPosition(); pos != NULL; )
	{
		CServerSocket* pSck = (CServerSocket*)m_lstOpenSockets.GetNext(pos);
		if (pSck == connectedsocket)		// don't destroy socket which is connected to server
			continue;
		if (pSck->m_bIsDeleting == false)	// don't destroy socket if it is going to destroy itself later on
			DestroySocket(pSck);
	}
}

void CServerConnect::ConnectionEstablished(CServerSocket* sender)
{
	if (connecting == false)
	{
		// we are already connected to another server
		DestroySocket(sender);
		return;
	}
	
	InitLocalIP();
	if (sender->GetConnectionState() == CS_WAITFORLOGIN)
	{
		AddLogLine(false, GetResString(IDS_CONNECTEDTOREQ), sender->cur_server->GetListName(), sender->cur_server->GetFullIP(), sender->cur_server->GetPort());

		CServer* update = theApp.serverlist->GetServerByAddress(sender->cur_server->GetAddress(), sender->cur_server->GetPort());
		if (update){
			update->ResetFailedCount();
			theApp.emuledlg->serverwnd->serverlistctrl.RefreshServer(update);
		}

		// send loginpacket
		CSafeMemFile data(256);
		data.WriteHash16(thePrefs.GetUserHash());
		data.WriteUInt32(GetClientID());
		data.WriteUInt16(thePrefs.GetPort());

		uint32 tagcount = 4;
		data.WriteUInt32(tagcount);

		CTag tagName(CT_NAME,thePrefs.GetUserNick());
		tagName.WriteTagToFile(&data);

		CTag tagVersion(CT_VERSION,EDONKEYVERSION);
		tagVersion.WriteTagToFile(&data);

#ifdef SUPPORT_LARGE_FILES
		CTag tagFlags(CT_SERVER_FLAGS,SRVCAP_ZLIB | SRVCAP_NEWTAGS | SRVCAP_LARGEFILES | SRVCAP_UNICODE);
#else
		CTag tagFlags(CT_SERVER_FLAGS,SRVCAP_ZLIB | SRVCAP_NEWTAGS | SRVCAP_UNICODE);
#endif
		tagFlags.WriteTagToFile(&data);

		// eMule Version (14-Mar-2004: requested by lugdunummaster (need for LowID clients which have no chance 
		// to send an Hello packet to the server during the callback test))
		CTag tagMuleVersion(CT_EMULE_VERSION, 
							//(uCompatibleClientID		<< 24) |
							(CemuleApp::m_nVersionMjr	<< 17) |
							(CemuleApp::m_nVersionMin	<< 10) |
							(CemuleApp::m_nVersionUpd	<<  7) );
		tagMuleVersion.WriteTagToFile(&data);

		Packet* packet = new Packet(&data);
		packet->opcode = OP_LOGINREQUEST;
		if (thePrefs.GetDebugServerTCPLevel() > 0)
			Debug(_T(">>> Sending OP__LoginRequest\n"));
		theStats.AddUpDataOverheadServer(packet->size);
		SendPacket(packet,true,sender);
	}
	else if (sender->GetConnectionState() == CS_CONNECTED)
	{
		theStats.reconnects++;
		theStats.serverConnectTime = GetTickCount();
		connected = true;
		Log(LOG_SUCCESS | LOG_STATUSBAR, GetResString(IDS_CONNECTEDTO), sender->cur_server->GetListName());
		theApp.emuledlg->ShowConnectionState();
		connectedsocket = sender;
		StopConnectionTry();
		theApp.sharedfiles->ClearED2KPublishInfo();
		theApp.sharedfiles->SendListToServer();
		theApp.emuledlg->serverwnd->serverlistctrl.RemoveAllDeadServers();

		// tecxx 1609 2002 - serverlist update
		if (thePrefs.GetAddServersFromServer())
		{
			Packet* packet = new Packet(OP_GETSERVERLIST,0);
			if (thePrefs.GetDebugServerTCPLevel() > 0)
				Debug(_T(">>> Sending OP__GetServerList\n"));
			theStats.AddUpDataOverheadServer(packet->size);
			SendPacket(packet,true);
		}
		CServer* update = theApp.serverlist->GetServerByAddress(sender->cur_server->GetAddress(), sender->cur_server->GetPort());
		theApp.emuledlg->serverwnd->serverlistctrl.RefreshServer(update);
	}
	theApp.emuledlg->ShowConnectionState();
}

bool CServerConnect::SendPacket(Packet* packet,bool delpacket, CServerSocket* to){
	if (!to){
		if (connected){
			connectedsocket->SendPacket(packet,delpacket,true);
		}
		else{
			if (delpacket)
				delete packet;
			return false;
		}
	}
	else{
		to->SendPacket(packet,delpacket,true);
	}
	return true;
}

bool CServerConnect::SendUDPPacket(Packet* packet,CServer* host,bool delpacket){
	if (theApp.IsConnected()){
		if (udpsocket != NULL)
			udpsocket->SendPacket(packet,host);
	}
	if (delpacket)
		delete packet;
	return true;
}

void CServerConnect::ConnectionFailed(CServerSocket* sender){
	if (connecting == false && sender != connectedsocket)
	{
		// just return, cleanup is done by the socket itself
		return;
	}
	//messages
	CServer* update;
	switch (sender->GetConnectionState()){
		case CS_FATALERROR:
			LogError(LOG_STATUSBAR, GetResString(IDS_ERR_FATAL));
			break;
		case CS_DISCONNECTED:
			theApp.sharedfiles->ClearED2KPublishInfo();
			LogError(LOG_STATUSBAR, GetResString(IDS_ERR_LOSTC), sender->cur_server->GetListName(), sender->cur_server->GetFullIP(), sender->cur_server->GetPort());
			break;
		case CS_SERVERDEAD:
			LogError(LOG_STATUSBAR, GetResString(IDS_ERR_DEAD), sender->cur_server->GetListName(), sender->cur_server->GetFullIP(), sender->cur_server->GetPort());
			update = theApp.serverlist->GetServerByAddress(sender->cur_server->GetAddress(), sender->cur_server->GetPort());
			if (update){
				update->AddFailedCount();
				theApp.emuledlg->serverwnd->serverlistctrl.RefreshServer(update);
			}
			break;
		case CS_ERROR:
			break;
		case CS_SERVERFULL:
			LogError(LOG_STATUSBAR, GetResString(IDS_ERR_FULL), sender->cur_server->GetListName(), sender->cur_server->GetFullIP(), sender->cur_server->GetPort());
			break;
		case CS_NOTCONNECTED:
			break; 
	}

	// IMPORTANT: mark this socket not to be deleted in StopConnectionTry(), 
	// because it will delete itself after this function!
	sender->m_bIsDeleting = true;

	switch (sender->GetConnectionState()){
		case CS_FATALERROR:{
			bool autoretry = !singleconnecting;
			StopConnectionTry();
			if ((thePrefs.Reconnect()) && (autoretry) && (!m_idRetryTimer)){ 
				LogWarning(GetResString(IDS_RECONNECT), CS_RETRYCONNECTTIME);
				VERIFY( (m_idRetryTimer= SetTimer(NULL, 0, 1000*CS_RETRYCONNECTTIME, RetryConnectTimer)) != NULL );
				if (thePrefs.GetVerbose() && !m_idRetryTimer)
					DebugLogError(_T("Failed to create 'server connect retry' timer - %s"),GetErrorMessage(GetLastError()));
			}
			break;
		}
		case CS_DISCONNECTED:{
			theApp.sharedfiles->ClearED2KPublishInfo();
			connected = false;
			if (connectedsocket) 
				connectedsocket->Close();
			connectedsocket = NULL;
			theApp.emuledlg->searchwnd->CancelEd2kSearch();
			// -khaos--+++> Tell our total server duration thinkymajig to update...
			theStats.serverConnectTime = 0;
			theStats.Add2TotalServerDuration();
			// <-----khaos-
			if (thePrefs.Reconnect() && !connecting){
				ConnectToAnyServer();		
			}
			if (thePrefs.GetNotifierOnImportantError()) {
				theApp.emuledlg->ShowNotifier(GetResString(IDS_CONNECTIONLOST), TBN_IMPORTANTEVENT);
			}
			break;
		}
		case CS_ERROR:
		case CS_NOTCONNECTED:{
			if (!connecting)
				break;
		}
		case CS_SERVERDEAD:
		case CS_SERVERFULL:{
			if (!connecting)
				break;
			if (singleconnecting){
				StopConnectionTry();
				break;
			}

			DWORD tmpkey;
			CServerSocket* tmpsock;
			POSITION pos = connectionattemps.GetStartPosition();
			while (pos){
				connectionattemps.GetNextAssoc(pos,tmpkey,tmpsock);
				if (tmpsock==sender) {
					connectionattemps.RemoveKey(tmpkey);
					break;
				}
			}			
			TryAnotherConnectionrequest();
		}
	}
	theApp.emuledlg->ShowConnectionState();
}

// 09/28/02, by zegzav
VOID CALLBACK CServerConnect::RetryConnectTimer(HWND /*hWnd*/, UINT /*nMsg*/, UINT /*nId*/, DWORD /*dwTime*/) 
{ 
	// NOTE: Always handle all type of MFC exceptions in TimerProcs - otherwise we'll get mem leaks
	try
	{
		CServerConnect *_this= theApp.serverconnect; 
		ASSERT(_this); 
		_this->StopConnectionTry();
		if (_this->IsConnected())
			return; 

		_this->ConnectToAnyServer();
	}
	CATCH_DFLT_EXCEPTIONS(_T("CServerConnect::RetryConnectTimer"))
}

void CServerConnect::CheckForTimeout()
{ 
	DWORD dwServerConnectTimeout = CONSERVTIMEOUT;
	// If we are using a proxy, increase server connection timeout to default connection timeout
	if (thePrefs.GetProxySettings().UseProxy)
		dwServerConnectTimeout = max(dwServerConnectTimeout, CONNECTION_TIMEOUT);

	DWORD dwCurTick = GetTickCount();
	DWORD tmpkey;
	CServerSocket* tmpsock;
	POSITION pos = connectionattemps.GetStartPosition();
	while (pos){
		connectionattemps.GetNextAssoc(pos,tmpkey,tmpsock);
		if (!tmpsock){
			if (thePrefs.GetVerbose())
				DebugLogError(_T("Error: Socket invalid at timeoutcheck"));
			connectionattemps.RemoveKey(tmpkey);
			return;
		}

		if (dwCurTick - tmpkey > dwServerConnectTimeout){
			LogWarning(GetResString(IDS_ERR_CONTIMEOUT), tmpsock->cur_server->GetListName(), tmpsock->cur_server->GetFullIP(), tmpsock->cur_server->GetPort());
			connectionattemps.RemoveKey(tmpkey);
			TryAnotherConnectionrequest();
			DestroySocket(tmpsock);
		}
	}
}

bool CServerConnect::Disconnect(){
	if (connected && connectedsocket){
		theApp.sharedfiles->ClearED2KPublishInfo();
		
		connected = false;

		CServer* update = theApp.serverlist->GetServerByAddress( connectedsocket->cur_server->GetAddress(), connectedsocket->cur_server->GetPort() );
		theApp.emuledlg->serverwnd->serverlistctrl.RefreshServer( update);
		theApp.SetPublicIP(0);
		DestroySocket(connectedsocket);
		connectedsocket = NULL;
		theApp.emuledlg->ShowConnectionState();
		theApp.emuledlg->AddServerMessageLine(_T(""));
		theApp.emuledlg->AddServerMessageLine(_T(""));
		theApp.emuledlg->AddServerMessageLine(_T(""));
		theApp.emuledlg->AddServerMessageLine(_T(""));
		// -khaos--+++> Tell our total server duration thinkymajig to update...
		theStats.serverConnectTime = 0;
		theStats.Add2TotalServerDuration();
		// <-----khaos-
		return true;
	}
	else
		return false;
}

CServerConnect::CServerConnect(CServerList* in_serverlist)
{
	connectedsocket = NULL;
	used_list = in_serverlist;
	max_simcons = (thePrefs.IsSafeServerConnectEnabled()) ? 1 : 2;
	connecting = false;
	connected = false;
	clientid = 0;
	singleconnecting = false;
	if (thePrefs.GetServerUDPPort() != 0){
	    udpsocket = new CUDPSocket(); // initalize socket for udp packets
		if (!udpsocket->Create()){
			delete udpsocket;
			udpsocket = NULL;
		}
	}
	else
		udpsocket = NULL;
	m_idRetryTimer= 0;
	lastStartAt=0;
	InitLocalIP();
}

CServerConnect::~CServerConnect(){
	// stop all connections
	StopConnectionTry();
	// close connected socket, if any
	DestroySocket(connectedsocket);
	connectedsocket = NULL;
	// close udp socket
	if (udpsocket){
	    udpsocket->Close();
	    delete udpsocket;
    }
}

CServer* CServerConnect::GetCurrentServer(){
	if (IsConnected() && connectedsocket)
		return connectedsocket->cur_server;
	return NULL;
}

void CServerConnect::SetClientID(uint32 newid){
	clientid = newid;

	if (!::IsLowID(newid))
		theApp.SetPublicIP(newid);
	
	theApp.emuledlg->ShowConnectionState();
}

void CServerConnect::DestroySocket(CServerSocket* pSck){
	if (pSck == NULL)
		return;
	// remove socket from list of opened sockets
	for( POSITION pos = m_lstOpenSockets.GetHeadPosition(); pos != NULL; )
	{
		POSITION posDel = pos;
		CServerSocket* pTestSck = (CServerSocket*)m_lstOpenSockets.GetNext(pos);
		if (pTestSck == pSck)
		{
			m_lstOpenSockets.RemoveAt(posDel);
			break;
		}
	}
	if (pSck->m_SocketData.hSocket != INVALID_SOCKET){ // deadlake PROXYSUPPORT - changed to AsyncSocketEx
		pSck->AsyncSelect(0);
		pSck->Close();
	}

	delete pSck;
}

bool CServerConnect::IsLocalServer(uint32 dwIP, uint16 nPort){
	if (IsConnected()){
		if (connectedsocket->cur_server->GetIP() == dwIP && connectedsocket->cur_server->GetPort() == nPort)
			return true;
	}
	return false;
}

void CServerConnect::InitLocalIP()
{
	m_nLocalIP = 0;
	// Don't use 'gethostbyname(NULL)'. The winsock DLL may be replaced by a DLL from a third party
	// which is not fully compatible to the original winsock DLL. ppl reported crash with SCORSOCK.DLL
	// when using 'gethostbyname(NULL)'.
	__try{
		char szHost[256];
		if (gethostname(szHost, sizeof szHost) == 0){
			hostent* pHostEnt = gethostbyname(szHost);
			if (pHostEnt != NULL && pHostEnt->h_length == 4 && pHostEnt->h_addr_list[0] != NULL)
				m_nLocalIP = *((uint32*)pHostEnt->h_addr_list[0]);
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER){
		// at least two ppl reported crashs when using 'gethostbyname' with third party winsock DLLs
		if (thePrefs.GetVerbose())
			DebugLogError(_T("Unknown exception in CServerConnect::InitLocalIP"));
		ASSERT(0);
	}
}

void CServerConnect::KeepConnectionAlive()
{
	DWORD dwServerKeepAliveTimeout = thePrefs.GetServerKeepAliveTimeout();
	if (dwServerKeepAliveTimeout && connected && connectedsocket && connectedsocket->connectionstate == CS_CONNECTED &&
		GetTickCount() - connectedsocket->GetLastTransmission() >= dwServerKeepAliveTimeout)
	{
		// "Ping" the server if the TCP connection was not used for the specified interval with 
		// an empty publish files packet -> recommended by lugdunummaster himself!
		CSafeMemFile files(4);
		files.WriteUInt32(0); // nr. of files
		Packet* packet = new Packet(&files);
		packet->opcode = OP_OFFERFILES;
		if (thePrefs.GetVerbose())
			AddDebugLogLine(false, _T("Refreshing server connection"));
		if (thePrefs.GetDebugServerTCPLevel() > 0)
			Debug(_T(">>> Sending OP__OfferFiles(KeepAlive) to server\n"));
		theStats.AddUpDataOverheadServer(packet->size);
		connectedsocket->SendPacket(packet,true);
	}
}

bool CServerConnect::IsLowID()
{
	return ::IsLowID(clientid);
}