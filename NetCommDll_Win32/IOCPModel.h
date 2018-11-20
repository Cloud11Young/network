
/*
==========================================================================

Purpose:

	* �����CIOCPModel�Ǳ�����ĺ����࣬����˵��WinSock�������˱��ģ���е�
	  ��ɶ˿�(IOCP)��ʹ�÷�������ʹ��MFC�Ի�����������������ʵ���˻�����
	  ����������ͨ�ŵĹ��ܡ�

	* ���е�PER_IO_DATA�ṹ���Ƿ�װ������ÿһ���ص������Ĳ���
	  PER_HANDLE_DATA �Ƿ�װ������ÿһ��Socket�Ĳ�����Ҳ��������ÿһ����ɶ˿ڵĲ���

	* ��ϸ���ĵ�˵����ο� http://blog.csdn.net/PiggyXP

Notes:

	* ���彲���˷������˽�����ɶ˿ڡ������������̡߳�Ͷ��Recv����Ͷ��Accept����ķ�����
	  ���еĿͻ��������Socket����Ҫ�󶨵�IOCP�ϣ����дӿͻ��˷��������ݣ�����ʵʱ��ʾ��
	  ��������ȥ��

Author:

	* PiggyXP��С��

Date:

	* 2009/10/04

==========================================================================
*/

#pragma once

// winsock 2 ��ͷ�ļ��Ϳ�
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
// ���������� (1024*8)
// ֮����Ϊʲô����8K��Ҳ��һ�������ϵľ���ֵ
// ���ȷʵ�ͻ��˷�����ÿ�����ݶ��Ƚ��٣���ô�����õ�СһЩ��ʡ�ڴ�
#define MAX_REC_SEND_BUFFER_LEN  (1024*1024*8)  
#define MAX_BUFFER_LEN        (1024*8)  
// Ĭ�϶˿�
#define DEFAULT_PORT          12345    
// Ĭ��IP��ַ
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
// ����ɶ˿���Ͷ�ݵ�I/O����������
typedef enum _OPERATION_TYPE  
{  
	ACCEPT_POSTED = 10,                     // ��־Ͷ�ݵ�Accept����
	CONNECT_POSTED,
	SEND_POSTED,                       // ��־Ͷ�ݵ��Ƿ��Ͳ���
	RECV_POSTED,                       // ��־Ͷ�ݵ��ǽ��ղ���
	NULL_POSTED                        // ���ڳ�ʼ����������

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
//				��IO���ݽṹ�嶨��(����ÿһ���ص������Ĳ���)
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
	OVERLAPPED     m_Overlapped;                               // ÿһ���ص�����������ص��ṹ(���ÿһ��Socket��ÿһ����������Ҫ��һ��) 
	OVERLAPPED     m_OverlappedEx;
	SOCKET         m_sockAccept;                               // ������������ʹ�õ�Socket
	WSABUF         m_wsaBuf;                                   // WSA���͵Ļ����������ڸ��ص�������������
	WSABUF         m_wsendBuf;
	char*          m_szBuffer;                 // �����WSABUF�������ַ��Ļ�����
	char*          m_sdBuffer;
	OPERATION_TYPE m_OpType;                                   // ��ʶ�������������(��Ӧ�����ö��)
	OPERATION_TYPE m_OpTypeEx;
	sockaddr_in    ServerAddress;
	std::mutex     m_lockSend;
	bool           m_bLine;
	std::deque<LPSendData> m_deqSend;
	bool           m_bSend;
	int            m_nType;
	DWORD          m_nHeartBeat;
	// ��ʼ��
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
	// �ͷŵ�Socket
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
	// ���û���������
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
//				��������ݽṹ�嶨��(����ÿһ����ɶ˿ڣ�Ҳ����ÿһ��Socket�Ĳ���)
//
//====================================================================================

typedef struct _PER_SOCKET_CONTEXT
{  
	SOCKET      m_Socket;                                  // ÿһ���ͻ������ӵ�Socket
	SOCKADDR_IN m_ClientAddr;                              // �ͻ��˵ĵ�ַ
	std::list<std::shared_ptr<_PER_IO_CONTEXT>> m_arrayIoContext;             // �ͻ���������������������ݣ�
	                                                       // Ҳ����˵����ÿһ���ͻ���Socket���ǿ���������ͬʱͶ�ݶ��IO�����
	// ��ʼ��
	_PER_SOCKET_CONTEXT() :_nCount(0)
	{
		m_Socket = INVALID_SOCKET;
		memset(&m_ClientAddr, 0, sizeof(m_ClientAddr)); 
	}

	// �ͷ���Դ
	~_PER_SOCKET_CONTEXT()
	{
		if( m_Socket!=INVALID_SOCKET )
		{
			shutdown(m_Socket, SD_SEND);
			closesocket( m_Socket );
		    m_Socket = INVALID_SOCKET;
		}
		// �ͷŵ����е�IO����������
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
	// ��ȡһ���µ�IoContext
	std::shared_ptr<_PER_IO_CONTEXT> GetNewIoContext()
	{
		std::shared_ptr<_PER_IO_CONTEXT> sp(new _PER_IO_CONTEXT);

		m_arrayIoContext.push_back( sp );

		return sp;
	}

	// ���������Ƴ�һ��ָ����IoContext
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
//				CIOCPModel�ඨ��
//
//====================================================================================

// �������̵߳��̲߳���
class CIOCPModel;
typedef struct _tagThreadParams_WORKER
{
	CIOCPModel* pIOCPModel;                                   // ��ָ�룬���ڵ������еĺ���
	int         nThreadNo;                                    // �̱߳��

} THREADPARAMS_WORKER,*PTHREADPARAM_WORKER; 
#pragma pack(pop) 
// CIOCPModel��
class CIOCPModel
{
public:
	CIOCPModel(void);
	~CIOCPModel(void);

public:

	// ����������
	bool Start(bool bIsServer,const char* pStrIP = "127.0.0.1", int nPort = 5001);

	//	ֹͣ������
	void Stop();

	// ����Socket��
	bool LoadSocketLib(PUSER_CB_IOCP pfnBack = nullptr);

	// ж��Socket�⣬��������
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

	// ��ñ�����IP��ַ
	std::string GetLocalIP();

	// ���ü����˿�
	void SetPort( const int& nPort ) { m_nPort=nPort; }

	// �����������ָ�룬���ڵ�����ʾ��Ϣ��������
//	void SetMainDlg( CDialog* p ) { m_pMain=p; }

	BOOL SendData(DWORD dwType, char* pStr, int nLen, char* pStrIP, int nPort, E_TASK_GRADE eGrade = GRADE_LOW);

	BOOL ReSendData(PER_IO_CONTEXT* pIoContextEx);
protected:

	// ��ʼ��IOCP
	bool _InitializeIOCP();

	// ��ʼ��Socket
	bool _InitializeListenSocket();

	// ��ʼ��Socket
	bool _InitializeClientSocket();

	// ����ͷ���Դ
	void _DeInitialize();

	// Ͷ��Accept����
	bool _PostAccept( PER_IO_CONTEXT* pAcceptIoContext ); 

	// Ͷ��Connect����
	bool _PostConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine = false);

	bool _ReConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine = false);

	// Ͷ�ݽ�����������
	bool _PostRecv( PER_IO_CONTEXT* pIoContext );

	bool _DoConnect(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// ���пͻ��������ʱ�򣬽��д���
	bool _DoAccpet( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

	// ���н��յ����ݵ����ʱ�򣬽��д���
	bool _DoRecv( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

	// ���ͻ��˵������Ϣ�洢��������
	void _AddToContextList( PER_SOCKET_CONTEXT *pSocketContext );

	// ���ͻ��˵���Ϣ���������Ƴ�
	void _RemoveContext( PER_SOCKET_CONTEXT *pSocketContext );

	std::shared_ptr<_PER_IO_CONTEXT> CIOCPModel::_GetSocketContext(char* pStrIP, int nPort);

	// ��տͻ�����Ϣ
	void _ClearContextList();

	// ������󶨵���ɶ˿���
	bool _AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext);

	// ������ɶ˿��ϵĴ���
	bool HandleError(PER_SOCKET_CONTEXT *pContext, PER_IO_CONTEXT* pIoContext, const DWORD& dwErr);

	// �̺߳�����ΪIOCP�������Ĺ������߳�
	static DWORD WINAPI _WorkerThread(LPVOID lpParam);

	static DWORD WINAPI _TaskThread(LPVOID lpParam);

	// ��ñ����Ĵ���������
	int _GetNoOfProcessors();

	// �жϿͻ���Socket�Ƿ��Ѿ��Ͽ�
	bool _IsSocketAlive(SOCKET s);

	bool _SetSocketOpt(SOCKET& s);

	// ������������ʾ��Ϣ
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

	HANDLE                       m_hShutdownEvent;              // ����֪ͨ�߳�ϵͳ�˳����¼���Ϊ���ܹ����õ��˳��߳�

	HANDLE                       m_hIOCompletionPort;           // ��ɶ˿ڵľ��

	HANDLE*                      m_phWorkerThreads;             // �������̵߳ľ��ָ��

	HANDLE                       m_hHanderTask;

	HANDLE                       m_hEventTask;

	HANDLE                       m_hTaskOver;

	bool                         m_bExit;

	int		                     m_nThreads;                    // ���ɵ��߳�����

	std::string                  m_strIP;                       // �������˵�IP��ַ

	int                          m_nPort;                       // �������˵ļ����˿�

//	CDialog*                     m_pMain;                       // ������Ľ���ָ�룬����������������ʾ��Ϣ

	std::mutex                  m_csContextList;               // ����Worker�߳�ͬ���Ļ�����

//	CArray<std::shared_ptr<PER_SOCKET_CONTEXT>>  m_arrayClientContext;          // �ͻ���Socket��Context��Ϣ  
	std::vector<std::shared_ptr<PER_SOCKET_CONTEXT>>  m_arrayClientContext;          // �ͻ���Socket��Context��Ϣ        


	PER_SOCKET_CONTEXT*          m_pListenContext;              // ���ڼ�����Socket��Context��Ϣ

	PER_SOCKET_CONTEXT*          m_pClinetContext;              // �������ӵ�Socket��Context��Ϣ

	LPFN_ACCEPTEX                m_lpfnAcceptEx;                // AcceptEx �� GetAcceptExSockaddrs �ĺ���ָ�룬���ڵ�����������չ����

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

