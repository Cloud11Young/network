#pragma once
#include "INetComm.h"
#include "IOCPModel.h"
class CNetWork :public INetComm
{
public:
	CNetWork();
	~CNetWork();
public:
	virtual void Release();
	virtual BOOL Initialize(void* pThis, PUSER_CB callback, size_t dwPort, std::string strIp);//��Ҫ�ṩServer����
	virtual BOOL Initialize(void* pThis, PUSER_CB callback) ;//����Ҫ�ṩServer����
	virtual BOOL GetStatus(BOOL &bIsServer, BOOL &bIsClient);
	virtual BOOL ConnectTo(std::string pIP, size_t uPort, BOOL bAutoReconnect = TRUE);
	virtual BOOL Disconnect(std::string pIP, size_t uPort);
	virtual BOOL SendMsg(void* pMsg, size_t dwMsgLen, std::string pIP, size_t uPort, DWORD dwWay = SEND_ASYN);
	virtual BOOL GetSocket(std::string pIP, size_t uPort, list<HANDLE> SocketList);

	virtual BOOL Uninitialize();
private:
	CIOCPModel m_netWork;
};

