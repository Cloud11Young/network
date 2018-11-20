
/*
==========================================================================

Purpose:

	* 这个类CIOCPModel是本代码的核心类，用于说明WinSock服务器端编程模型中的
	  完成端口(IOCP)的使用方法，并使用MFC对话框程序来调用这个类实现了基本的
	  服务器网络通信的功能。

	* 其中的PER_IO_DATA结构体是封装了用于每一个重叠操作的参数
	  PER_HANDLE_DATA 是封装了用于每一个Socket的参数，也就是用于每一个完成端口的参数

	* 详细的文档说明请参考 http://blog.csdn.net/PiggyXP

Notes:

	* 具体讲明了服务器端建立完成端口、建立工作者线程、投递Recv请求、投递Accept请求的方法，
	  所有的客户端连入的Socket都需要绑定到IOCP上，所有从客户端发来的数据，都会实时显示到
	  主界面中去。

Author:

	* PiggyXP【小猪】

Date:

	* 2009/10/04

==========================================================================
*/

#pragma once

// winsock 2 的头文件和库
//#include <winsock2.h>
//#include <MSWSock.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <map>
#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include <deque>
#include <list>
#include <mstcpip.h>
#include <atomic>

#include "MD5App.hpp"
#include "LogManage.hpp"

#pragma comment(lib,"ws2_32.lib")

#define ADDRESSLEN    sizeof(SOCKADDR_IN) + 16
#define BUF_SIZE      (1024*8-sizeof(DWORD)*2-32)
#define QUEUE_SIZE    160
#define REQUEST_S     'S'
#define REQUEST_O     'O'
#define REQUEST_C     'C'
#define REQUEST_K     'K'
#define REQUEST_E     'E'
#define PACKET_FIRST  'F'
#define PACKET_END    'D'
#define STR_CLIENT    "CLIENT"	
#define STR_SERVER    "SERVER"					
#define STR_UNLIMITED "UNLIMITED"	
// 缓冲区长度 (1024*8)
// 之所以为什么设置8K，也是一个江湖上的经验值
// 如果确实客户端发来的每组数据都比较少，那么就设置得小一些，省内存
#define MAX_REC_SEND_BUFFER_LEN  (1024*1024*8)  
#define MAX_BUFFER_LEN        (1024*8)  
// 默认端口
#define DEFAULT_PORT          12345    
// 默认IP地址
#define DEFAULT_IP            "127.0.0.1"

#define IOCP_NETWORK_MSG     WM_USER + 147
#define HEARTBEAT_TIME       2000
#define HEARTBEAT_VERIFY     9999
#pragma pack(push)
#pragma pack(4)
enum E_TASK_TYPE
{
	TASK_NOMAL = 0,
	TASK_DATA,
	TASK_CONNECT,
	TASK_DISCONNECT,
	TASK_REC_ERROR,
	TASK_SND_ERROR,
};

enum E_TASK_GRADE
{
	GRADE_NOMAL = 0,
	GRADE_HIGH,
	GRADE_LOW,
};

typedef struct _NotifyMSG
{
	_NotifyMSG() :_eType(TASK_NOMAL),
	_dwLen(0),
	_pStr(nullptr),
	_dwPersist(0),
	_pVoid(nullptr)
	{memset(_cIP, 0, 48);}
	_NotifyMSG(void* pVoid, E_TASK_TYPE eType, char* pStr, DWORD dwLen, DWORD dwPersist, char* pStrIP = nullptr) :_eType(eType),
		_dwLen(dwLen),
		_dwPersist(dwPersist),
		_pStr(nullptr),
		_pVoid(pVoid)
	{
		if (!_pStr && dwLen){
			_pStr = new char[dwLen + 1];
			memset(_pStr, 0, dwLen + 1);
			memcpy(_pStr, pStr, dwLen);
			memset(_cIP, 0, 48);
			if (pStrIP)
				memcpy(_cIP, pStrIP, strlen(pStrIP));
		}
	}
	~_NotifyMSG()
	{
		if (_pStr){
			delete[] _pStr;
			_pStr = nullptr;
			_dwLen = 0;
			_eType = TASK_NOMAL;
			_dwPersist = 0;
			memset(_cIP, 0, 48);
		}
	}
	E_TASK_TYPE _eType;
	DWORD       _dwLen;
	char*       _pStr;
	DWORD       _dwPersist;
	char        _cIP[48];
	void*       _pVoid;
}NotifyMSG, *LPNotifyMSG;

typedef void(__stdcall *LPCONNECT_CALLBACK_IOCP)(void* pThis, std::string strIP, size_t dwPort, std::string strPcName);
typedef void(__stdcall *LPDISCONNECT_CALLBACK_IOCP)(void* pThis, std::string strIP, size_t dwPort);
typedef void(__stdcall *LPRECVMSG_CALLBACK_IOCP)(void* pThis, PVOID pMsg, size_t dwMsgLen, std::string strIP, size_t dwPort);
typedef void(__stdcall *LPERROR_SNDDATA_CALLBACK)(void* pThis, std::string strLineInfo, size_t dwErrorCode);
typedef void(__stdcall *LPPOSTAUTO_CONNECT_CALLBACK_IOCP)(void* pThis, std::string strIP, size_t dwPort, BOOL bOK);

typedef struct _USER_CB_IOCP
{
	_USER_CB_IOCP() :lpConnectCB(nullptr),
	lpDisconnectCB(nullptr),
	lpRecvMsgCB(nullptr),
	lpErrorSendCB(nullptr),
	lpPostAutoConnectCB(nullptr),
	lpCallBackData(nullptr)
	{}
	LPCONNECT_CALLBACK_IOCP		     lpConnectCB;
	LPDISCONNECT_CALLBACK_IOCP	     lpDisconnectCB;
	LPRECVMSG_CALLBACK_IOCP		     lpRecvMsgCB;
	PVOID					         lpCallBackData;
	LPERROR_SNDDATA_CALLBACK         lpErrorSendCB;
	LPPOSTAUTO_CONNECT_CALLBACK_IOCP lpPostAutoConnectCB;
}USER_CB_IOCP, *PUSER_CB_IOCP;

class CSecLock
{
private:
	CRITICAL_SECTION sLock;
public:
	CSecLock()          { ::InitializeCriticalSection(&sLock); }
	virtual ~CSecLock() { ::DeleteCriticalSection(&sLock); }
public:
	void Lock()         { ::EnterCriticalSection(&sLock); }
	void UnLock()       { ::LeaveCriticalSection(&sLock); }
};

//////////////////////////////////////////////////////////////////
// 在完成端口上投递的I/O操作的类型
typedef enum _OPERATION_TYPE  
{  
	ACCEPT_POSTED = 10,                     // 标志投递的Accept操作
	CONNECT_POSTED,
	SEND_POSTED,                       // 标志投递的是发送操作
	RECV_POSTED,                       // 标志投递的是接收操作
	NULL_POSTED                        // 用于初始化，无意义

}OPERATION_TYPE;

enum E_PACK_PART
{
	PART_NOMAL = 0,
	PART_HEAD,
	PART_MID,
	PART_END,
};

typedef struct _HEARTBEAT_SYN
{
	_HEARTBEAT_SYN(){
		cHeartBeat[0] = 's';
		cHeartBeat[1] = 'y';
		cHeartBeat[2] = 'n';
	}
	char cHeartBeat[3];
}HEARTBEAT_SYN, *LPHEARTBEAT_SYN;
//====================================================================================
//
//				单IO数据结构体定义(用于每一个重叠操作的参数)
//
//====================================================================================
typedef struct _SendData
{
	_SendData() :_pStr(nullptr), _nLen(0), _pIP(nullptr), _dwPort(0), _dwType(0), _ePart(PART_NOMAL){}
	_SendData(char* pStr, int nLen, char* pIP, DWORD dwPort, DWORD dwType, E_PACK_PART ePart)
	{
		if (pStr || nLen > 0)
		{
			_pStr = new char[nLen];
			memcpy(_pStr, pStr, nLen);
			_nLen = nLen;
		}
		if (strlen(pIP) > 0)
		{
			_pIP = new char[strlen(pIP)+1];
			memcpy(_pIP, pIP, strlen(pIP));
			_pIP[strlen(pIP)] = '\0';
			_dwPort = dwPort;
			_dwType = dwType;
		}
		_ePart = ePart;
	}
	~_SendData()
	{
		if (_pStr)
		{
			delete[] _pStr;
			_nLen = 0;
			_pStr = nullptr;
		}
		if (_pIP)
		{
			delete[] _pIP;
			_pIP = nullptr;
		}
		_ePart = PART_NOMAL;
	}
	char*       _pStr;
	int         _nLen;
	char*       _pIP;
	DWORD       _dwPort;
	DWORD       _dwType;
	E_PACK_PART _ePart;
}SendData, *LPSendData;

typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED     m_Overlapped;                               // 每一个重叠网络操作的重叠结构(针对每一个Socket的每一个操作，都要有一个) 
	OVERLAPPED     m_OverlappedEx;
	SOCKET         m_sockAccept;                               // 这个网络操作所使用的Socket
	WSABUF         m_wsaBuf;                                   // WSA类型的缓冲区，用于给重叠操作传参数的
	WSABUF         m_wsendBuf;
	char*          m_szBuffer;                 // 这个是WSABUF里具体存字符的缓冲区
	char*          m_sdBuffer;
	OPERATION_TYPE m_OpType;                                   // 标识网络操作的类型(对应上面的枚举)
	OPERATION_TYPE m_OpTypeEx;
	sockaddr_in    ServerAddress;
	std::mutex     m_lockSend;
	bool           m_bLine;
	std::deque<LPSendData> m_deqSend;
	bool           m_bSend;
	int            m_nType;
	DWORD          m_nHeartBeat;
	// 初始化
	_PER_IO_CONTEXT()
	{
		m_szBuffer = new char[MAX_BUFFER_LEN];
		m_sdBuffer = new char[MAX_BUFFER_LEN];
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));  
		ZeroMemory(&m_OverlappedEx, sizeof(m_OverlappedEx));
		ZeroMemory( m_szBuffer,MAX_BUFFER_LEN );
		ZeroMemory(&wsTempBuf, sizeof(WSABUF));
		ZeroMemory(&wsBuf, sizeof(WSABUF));
		ZeroMemory(&m_wsendBuf, sizeof(WSABUF));
		ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
		ServerAddress.sin_family = AF_INET;
		nDesLen = 0;
		nCurLen = 0;
		nBufSize = 0;
		m_sockAccept = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_wsendBuf.buf = m_sdBuffer;
		m_wsendBuf.len = 0;
		m_OpType     = NULL_POSTED;
		m_OpTypeEx   = NULL_POSTED;
		m_nType = 0;
		m_bSend = false;
		m_bLine = true;
		m_deqSend.clear();
		wsTempBuf.buf = nullptr;
		wsTempBuf.len = 0;
		m_nHeartBeat = 0;
	}
	// 释放掉Socket
	~_PER_IO_CONTEXT()
	{
		if( m_sockAccept!=INVALID_SOCKET )
		{
			shutdown(m_sockAccept, SD_SEND);
			closesocket(m_sockAccept);
			m_sockAccept = INVALID_SOCKET;
		}
		if (m_szBuffer)
		{
			delete[] m_szBuffer;
			m_szBuffer = nullptr;
		}
		if (m_sdBuffer)
		{
			delete[] m_sdBuffer;
			m_sdBuffer = nullptr;
		}
	}
	// 重置缓冲区内容
	void ResetBuffer()
	{
		ZeroMemory( m_szBuffer,MAX_BUFFER_LEN );
	}

	WSABUF    wsTempBuf;
	WSABUF    wsBuf;
	int       nDesLen;
	int       nCurLen;
	int       nBufSize;
	WSABUF    queBuf[QUEUE_SIZE];
	char      btMD5[33];
	CSecLock  secLock;
} PER_IO_CONTEXT, *PPER_IO_CONTEXT;


//====================================================================================
//
//				单句柄数据结构体定义(用于每一个完成端口，也就是每一个Socket的参数)
//
//====================================================================================

typedef struct _PER_SOCKET_CONTEXT
{  
	SOCKET      m_Socket;                                  // 每一个客户端连接的Socket
	SOCKADDR_IN m_ClientAddr;                              // 客户端的地址
	std::list<std::shared_ptr<_PER_IO_CONTEXT>> m_arrayIoContext;             // 客户端网络操作的上下文数据，
	                                                       // 也就是说对于每一个客户端Socket，是可以在上面同时投递多个IO请求的
	// 初始化
	_PER_SOCKET_CONTEXT() :_nCount(0)
	{
		m_Socket = INVALID_SOCKET;
		memset(&m_ClientAddr, 0, sizeof(m_ClientAddr)); 
	}

	// 释放资源
	~_PER_SOCKET_CONTEXT()
	{
		if( m_Socket!=INVALID_SOCKET )
		{
			shutdown(m_Socket, SD_SEND);
			closesocket( m_Socket );
		    m_Socket = INVALID_SOCKET;
		}
		// 释放掉所有的IO上下文数据
		//for (int i = 0; i < m_arrayIoContext.GetCount(); i++)
		//{
		//	delete m_arrayIoContext.GetAt(i);
		//}
		m_arrayIoContext.clear();
	}
	std::shared_ptr<_PER_IO_CONTEXT> GetIoContext(char* pStrIP, int nPort)
	{
		//for (int i = 0; i < m_arrayIoContext.GetCount(); i++)
		for (std::list<std::shared_ptr<_PER_IO_CONTEXT>>::iterator it = m_arrayIoContext.begin(); it != m_arrayIoContext.end(); it++)
		{
			if (strcmp(inet_ntoa(it->get()->ServerAddress.sin_addr), pStrIP) == 0 && nPort == ntohs(it->get()->ServerAddress.sin_port))
				return *it;
		}
		return std::shared_ptr<_PER_IO_CONTEXT>();
	}
	std::shared_ptr<_PER_IO_CONTEXT> GetIoContext(_PER_IO_CONTEXT* pContext)
	{
		//for (int i = 0; i < m_arrayIoContext.GetCount(); i++)
		for (std::list<std::shared_ptr<_PER_IO_CONTEXT>>::iterator it = m_arrayIoContext.begin(); it != m_arrayIoContext.end(); it++)
		{
			if (pContext == it->get())
				return *it;
		}
		return std::shared_ptr<_PER_IO_CONTEXT>();
	}
	// 获取一个新的IoContext
	std::shared_ptr<_PER_IO_CONTEXT> GetNewIoContext()
	{
		std::shared_ptr<_PER_IO_CONTEXT> sp(new _PER_IO_CONTEXT);

		m_arrayIoContext.push_back( sp );

		return sp;
	}

	// 从数组中移除一个指定的IoContext
	void RemoveContext( _PER_IO_CONTEXT* pContext )
	{
		ASSERT( pContext!=NULL );

		for (std::list<std::shared_ptr<_PER_IO_CONTEXT>>::iterator it = m_arrayIoContext.begin(); it != m_arrayIoContext.end(); it++)
		{
			if( pContext==it->get())
			{
				//delete pContext;
				//pContext = NULL;
				m_arrayIoContext.erase(it);
				break;
			}
		}
	}
	std::atomic<int> _nCount;
} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;




//====================================================================================
//
//				CIOCPModel类定义
//
//====================================================================================

// 工作者线程的线程参数
class CIOCPModel;
typedef struct _tagThreadParams_WORKER
{
	CIOCPModel* pIOCPModel;                                   // 类指针，用于调用类中的函数
	int         nThreadNo;                                    // 线程编号

} THREADPARAMS_WORKER,*PTHREADPARAM_WORKER; 
#pragma pack(pop) 
// CIOCPModel类
class CIOCPModel
{
public:
	CIOCPModel(void);
	~CIOCPModel(void);

public:

	// 启动服务器
	bool Start(bool bIsServer,const char* pStrIP = "127.0.0.1", int nPort = 5001);

	//	停止服务器
	void Stop();

	// 加载Socket库
	bool LoadSocketLib(PUSER_CB_IOCP pfnBack = nullptr);

	// 卸载Socket库，彻底完事
	void UnloadSocketLib()
	{ 
		if (m_hEventTask && m_hHanderTask)
		{
			::SetEvent(m_hEventTask);
			WaitForSingleObject(m_hTaskOver, INFINITY);
			CloseHandle(m_hHanderTask);
			CloseHandle(m_hEventTask);
			WSACleanup();
			m_hHanderTask = NULL;
			m_hEventTask = NULL;
		}
	}

	// 获得本机的IP地址
	std::string GetLocalIP();

	// 设置监听端口
	void SetPort( const int& nPort ) { m_nPort=nPort; }

	// 设置主界面的指针，用于调用显示信息到界面中
//	void SetMainDlg( CDialog* p ) { m_pMain=p; }

	BOOL SendData(DWORD dwType, char* pStr, int nLen, char* pStrIP, int nPort, E_TASK_GRADE eGrade = GRADE_LOW);

	BOOL ReSendData(PER_IO_CONTEXT* pIoContextEx);
protected:

	// 初始化IOCP
	bool _InitializeIOCP();

	// 初始化Socket
	bool _InitializeListenSocket();

	// 初始化Socket
	bool _InitializeClientSocket();

	// 最后释放资源
	void _DeInitialize();

	// 投递Accept请求
	bool _PostAccept( PER_IO_CONTEXT* pAcceptIoContext ); 

	// 投递Connect请求
	bool _PostConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine = false);

	bool _ReConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine = false);

	// 投递接收数据请求
	bool _PostRecv( PER_IO_CONTEXT* pIoContext );

	bool _DoConnect(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// 在有客户端连入的时候，进行处理
	bool _DoAccpet( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

	// 在有接收的数据到达的时候，进行处理
	bool _DoRecv( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

	// 将客户端的相关信息存储到数组中
	void _AddToContextList( PER_SOCKET_CONTEXT *pSocketContext );

	// 将客户端的信息从数组中移除
	void _RemoveContext( PER_SOCKET_CONTEXT *pSocketContext );

	std::shared_ptr<_PER_IO_CONTEXT> CIOCPModel::_GetSocketContext(char* pStrIP, int nPort);

	// 清空客户端信息
	void _ClearContextList();

	// 将句柄绑定到完成端口中
	bool _AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext);

	// 处理完成端口上的错误
	bool HandleError(PER_SOCKET_CONTEXT *pContext, PER_IO_CONTEXT* pIoContext, const DWORD& dwErr);

	// 线程函数，为IOCP请求服务的工作者线程
	static DWORD WINAPI _WorkerThread(LPVOID lpParam);

	static DWORD WINAPI _TaskThread(LPVOID lpParam);

	// 获得本机的处理器数量
	int _GetNoOfProcessors();

	// 判断客户端Socket是否已经断开
	bool _IsSocketAlive(SOCKET s);

	bool _SetSocketOpt(SOCKET& s);

	// 在主界面中显示信息
	//void _ShowMessage( const CString szFormat,...);

	void Save(char* pStr, int nLen, PPER_IO_CONTEXT pContext);

	DWORD GetVerifyBytes(char* pStr, int nLen);

	void ClearOverlapped(PPER_IO_CONTEXT pInfo);

	bool IsVerifyHeartBeat(char* pStr, int nLen);

	static void AddTask(NotifyMSG* info, E_TASK_GRADE eGrade = GRADE_LOW);

	static NotifyMSG* PopTask(void* pVoid);

	static void ClearTask(void* pVoid);

	static std::string GetMD5Verify(char* pStr, DWORD nLen);
private:

	HANDLE                       m_hShutdownEvent;              // 用来通知线程系统退出的事件，为了能够更好的退出线程

	HANDLE                       m_hIOCompletionPort;           // 完成端口的句柄

	HANDLE*                      m_phWorkerThreads;             // 工作者线程的句柄指针

	HANDLE                       m_hHanderTask;

	HANDLE                       m_hEventTask;

	HANDLE                       m_hTaskOver;

	bool                         m_bExit;

	int		                     m_nThreads;                    // 生成的线程数量

	std::string                  m_strIP;                       // 服务器端的IP地址

	int                          m_nPort;                       // 服务器端的监听端口

//	CDialog*                     m_pMain;                       // 主界面的界面指针，用于在主界面中显示消息

	std::mutex                  m_csContextList;               // 用于Worker线程同步的互斥量

//	CArray<std::shared_ptr<PER_SOCKET_CONTEXT>>  m_arrayClientContext;          // 客户端Socket的Context信息  
	std::vector<std::shared_ptr<PER_SOCKET_CONTEXT>>  m_arrayClientContext;          // 客户端Socket的Context信息        


	PER_SOCKET_CONTEXT*          m_pListenContext;              // 用于监听的Socket的Context信息

	PER_SOCKET_CONTEXT*          m_pClinetContext;              // 用于连接的Socket的Context信息

	LPFN_ACCEPTEX                m_lpfnAcceptEx;                // AcceptEx 和 GetAcceptExSockaddrs 的函数指针，用于调用这两个扩展函数

	LPFN_GETACCEPTEXSOCKADDRS    m_lpfnGetAcceptExSockAddrs; 

	bool                         m_bIsServer;

	bool                         m_bRelase;

	PUSER_CB_IOCP                m_pfnNotify;

	std::mutex                   m_lockTask;
						         
	std::deque<NotifyMSG*>       m_deqMsg;
						         
	std::atomic<bool>            m_bStart;

	std::map<std::string, int64_t>  m_mapHeartBeat;

	int64_t                      m_dwHeartBeatTime;

//	CFile                        m_file;

	std::mutex                   m_mutexfile;

	bool                         m_bOpen;
};

