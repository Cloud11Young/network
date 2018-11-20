#include "stdafx.h"
#include "NetWork.h"


CNetWork::CNetWork()
{
}


CNetWork::~CNetWork()
{
}

void CNetWork::Release()
{
	m_netWork.Stop();
}

BOOL CNetWork::Initialize(void* pThis, PUSER_CB callback, size_t dwPort, std::string strIp)
{
	if (!m_netWork.LoadSocketLib((PUSER_CB_IOCP)callback))
		return FALSE;

	char IP[256];
	memset(IP, 0, sizeof(char)* 256);
	strcpy_s(IP, strIp.c_str());

	if (!m_netWork.Start(true, IP, dwPort))
		return FALSE;

	return TRUE;
}

BOOL CNetWork::Initialize(PVOID pThis, PUSER_CB callback)
{
	if (!m_netWork.LoadSocketLib((PUSER_CB_IOCP)callback))
		return FALSE;

	return TRUE;
}

BOOL CNetWork::GetStatus(BOOL &bIsServer, BOOL &bIsClient)
{

	return FALSE;
}

BOOL CNetWork::ConnectTo(std::string pIP, size_t uPort, BOOL bAutoReconnect)
{
	char ip[256];
	memset(ip, 0, sizeof(char)* 256);
	strcpy_s(ip, pIP.c_str());
	if (!m_netWork.Start(false, ip, uPort))
		return FALSE;

	return TRUE;
}

BOOL CNetWork::Disconnect(std::string pIP, size_t uPort)
{
	m_netWork.Stop();

	return FALSE;
}

BOOL CNetWork::SendMsg(void* pMsg, size_t dwMsgLen, std::string pIP, size_t uPort, DWORD dwWay)
{
	char ip[256];
	memset(ip, 0, sizeof(char)* 256);
	strcpy_s(ip, pIP.c_str());
	return m_netWork.SendData(0, (char*)pMsg, dwMsgLen, ip, uPort, (E_TASK_GRADE)dwWay);
}

BOOL CNetWork::GetSocket(std::string pIP, size_t uPort, list<HANDLE> SocketList)
{

	return FALSE;
}

BOOL CNetWork::Uninitialize()
{
	m_netWork.Stop();
	m_netWork.UnloadSocketLib();
	return FALSE;
}
