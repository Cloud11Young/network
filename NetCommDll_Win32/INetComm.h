// INetComm.h: interface for the INetComm class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_INETCOMM_H__ED63B975_9DA9_4238_AACD_10F7EC3D58C9__INCLUDED_)
#define AFX_INETCOMM_H__ED63B975_9DA9_4238_AACD_10F7EC3D58C9__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
#include <list>
#include <afxv_dll.h>
#include <windows.h>

#ifdef NET_EXPORT
#define NET_API _declspec(dllexport)
#else
#define NET_API _declspec(dllimport)
#endif

using namespace std;

#define SEND_SYN  0  // 同步
#define SEND_ASYN 1  // 异步

typedef void (__stdcall *LPCONNECT_CALLBACK)(void* pThis, std::string strIP, size_t dwPort, std::string strPcName);
typedef void (__stdcall *LPDISCONNECT_CALLBACK)(void* pThis, std::string strIP, size_t dwPort);
typedef void (__stdcall *LPRECVMSG_CALLBACK)(void* pThis, void* pMsg, size_t dwMsgLen, std::string strIP, size_t dwPort);


typedef struct _USER_CB
{
	LPCONNECT_CALLBACK		lpConnectCB;
	LPDISCONNECT_CALLBACK	lpDisconnectCB;
	LPRECVMSG_CALLBACK		lpRecvMsgCB;
	void*					lpCallBackData;
}USER_CB, *PUSER_CB;

#ifndef NET_LIB
class NET_API INetComm
{
#else
class INetComm
{
#endif
public:
	static int CreateInstance(INetComm **ppINetComm);
	virtual void Release() = 0;

	virtual int Initialize(void* pThis, PUSER_CB callback, size_t dwPort, std::string strIp) = 0;//需要提供Server服务
	virtual int Initialize(void* pThis, PUSER_CB callback) = 0;//不需要提供Server服务
	virtual int GetStatus(int &bIsServer, int &bIsClient) = 0;
	virtual int ConnectTo(std::string pIP, size_t uPort, int bAutoReconnect = TRUE) = 0;
	virtual int Disconnect(std::string pIP, size_t uPort) = 0;

	virtual int SendMsg(void* pMsg, size_t dwMsgLen, std::string pIP, size_t uPort, DWORD dwWay = SEND_ASYN) = 0;
	virtual int GetSocket(std::string pIP, size_t uPort, list<HANDLE> SocketList) = 0;

	virtual int Uninitialize() = 0;
};

#endif // !defined(AFX_INETCOMM_H__ED63B975_9DA9_4238_AACD_10F7EC3D58C9__INCLUDED_)
