#include "StdAfx.h"
#include "IOCPModel.h"
//#include <winsock.h>

#pragma comment(lib,"mswsock.lib")

// ÿһ���������ϲ������ٸ��߳�(Ϊ������޶ȵ��������������ܣ���������ĵ�)
#define WORKER_THREADS_PER_PROCESSOR 2
// ͬʱͶ�ݵ�Accept���������(���Ҫ����ʵ�ʵ�����������)
#define MAX_POST_ACCEPT              10
// ���ݸ�Worker�̵߳��˳��ź�
#define EXIT_CODE                    NULL


// �ͷ�ָ��;����Դ�ĺ�

// �ͷ�ָ���
#define RELEASE(x)                      {if(x != NULL ){delete x;x=NULL;}}
// �ͷž����
#define RELEASE_HANDLE(x)               {if(x != NULL && x!=INVALID_HANDLE_VALUE){ CloseHandle(x);x = NULL;}}
// �ͷ�Socket��
#define RELEASE_SOCKET(x)               {if(x !=INVALID_SOCKET) { closesocket(x);x=INVALID_SOCKET;}}


CIOCPModel::CIOCPModel(void):
							m_nThreads(0),
							m_hShutdownEvent(NULL),
							m_hIOCompletionPort(NULL),
							m_phWorkerThreads(NULL),
							m_strIP(DEFAULT_IP),
							m_nPort(DEFAULT_PORT),
//							m_pMain(NULL),
							m_lpfnAcceptEx( NULL ),
							m_pListenContext( NULL ),
							m_pClinetContext( NULL ),
							m_pfnNotify(nullptr),
							m_bStart(false),
							m_dwHeartBeatTime(0)
{
	std::string dirName = "C:\\Log";
	ILogManage()->CreateLog(dirName.c_str(), "newwork");
}


CIOCPModel::~CIOCPModel(void)
{
}




///////////////////////////////////////////////////////////////////
// �������̣߳�  ΪIOCP�������Ĺ������߳�
//         Ҳ����ÿ����ɶ˿��ϳ�����������ݰ����ͽ�֮ȡ�������д�����߳�
///////////////////////////////////////////////////////////////////

DWORD WINAPI CIOCPModel::_WorkerThread(LPVOID lpParam)
{    
	THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
	CIOCPModel* pIOCPModel = (CIOCPModel*)pParam->pIOCPModel;
	int nThreadNo = (int)pParam->nThreadNo;

	OVERLAPPED           *pOverlapped = NULL;
	PER_SOCKET_CONTEXT   *pSocketContext = NULL;
	DWORD                dwBytesTransfered = 0;

	// ѭ����������֪�����յ�Shutdown��ϢΪֹ
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			pIOCPModel->m_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE);

		// ����յ������˳���־����ֱ���˳�
		if ( EXIT_CODE==(DWORD)pSocketContext )
		{
			if (!pOverlapped) break;
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			if (pIoContext && pIoContext->m_OpType >= ACCEPT_POSTED && pIoContext->m_OpType <= NULL_POSTED)
			{
				pIoContext->m_lockSend.lock();
				pIoContext->m_bSend = false;
				pIoContext->m_bLine = false;
				while (pIoContext->m_deqSend.size())
				{
					LPSendData pInfo = pIoContext->m_deqSend.front();
					delete pInfo;
					pIoContext->m_deqSend.pop_front();			
				}
				pIoContext->m_lockSend.unlock();
				pIOCPModel->m_dwHeartBeatTime = 0;
			}
			else
			{
				PER_IO_CONTEXT* pIoContextEx = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_OverlappedEx);
				if (pIoContextEx && pIoContextEx->m_OpTypeEx == SEND_POSTED)
				{
					pIoContextEx->m_lockSend.lock();
					pIoContextEx->m_bSend = false;
					pIoContextEx->m_bLine = false;
					while (pIoContextEx->m_deqSend.size())
					{
						LPSendData pInfo = pIoContextEx->m_deqSend.front();
						delete pInfo;
						pIoContextEx->m_deqSend.pop_front();
					}
					pIoContextEx->m_lockSend.unlock();
					pIOCPModel->m_dwHeartBeatTime = 0;
				}
			}
			break;
		}

		// �ж��Ƿ�����˴���
		if( !bReturn )  
		{  
			DWORD dwErr = GetLastError();
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			pIOCPModel->m_dwHeartBeatTime = 0;
			if (pIoContext && pIoContext->m_OpType >= ACCEPT_POSTED && pIoContext->m_OpType <= NULL_POSTED)
			{
				// ��ʾһ����ʾ��Ϣ
				if (!pIOCPModel->HandleError(pSocketContext, pIoContext, dwErr))
				{
					break;
				}

				continue;
			}
			else
			{
				// ��ʾһ����ʾ��Ϣ
				PER_IO_CONTEXT* pIoContextEx = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_OverlappedEx);
				if (pIoContextEx && pIoContextEx->m_OpTypeEx == SEND_POSTED && !pIOCPModel->HandleError(pSocketContext, pIoContextEx, dwErr))
				{
					break;
				}

				continue;
			}
		}  
		else  
		{  	
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			// �ж��Ƿ��пͻ��˶Ͽ���
			if (0 == dwBytesTransfered)
			{  
				pIOCPModel->m_dwHeartBeatTime = 0;
				if (pIoContext && pIoContext->m_OpType >= ACCEPT_POSTED && pIoContext->m_OpType <= NULL_POSTED )
				{
					pIoContext->m_lockSend.lock();
					pIoContext->m_OpTypeEx = NULL_POSTED;
					pIoContext->m_bLine = false;
					while (pIoContext->m_deqSend.size())
					{
						LPSendData pInfo = pIoContext->m_deqSend.front();
						delete pInfo;
						pIoContext->m_deqSend.pop_front();
					}
					pIoContext->m_bSend = false;
					pIoContext->m_lockSend.unlock();
					ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) �ͻ��� %s:%d �Ͽ�����.������:%ld,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
					CIOCPModel::AddTask(new NotifyMSG(pIOCPModel, TASK_DISCONNECT, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)), ntohs(pSocketContext->m_ClientAddr.sin_port)));
					if (0 == pIoContext->m_nType){
						if (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0)) pIOCPModel->_ReConnect(pIoContext, true);
					}
					else
						// �ͷŵ���Ӧ����Դ
						pIOCPModel->_RemoveContext(pSocketContext);

					continue;
				}
				else
				{
					PER_IO_CONTEXT* pIoContextEx = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_OverlappedEx);
					if (pIoContextEx && pIoContextEx->m_OpTypeEx == SEND_POSTED)
					{
						pIoContextEx->m_lockSend.lock();
						pIoContextEx->m_OpTypeEx = NULL_POSTED;
						pIoContextEx->m_bLine = false;
						while (pIoContextEx->m_deqSend.size())
						{
							LPSendData pInfo = pIoContextEx->m_deqSend.front();
							delete pInfo;
							pIoContextEx->m_deqSend.pop_front();
						}
						pIoContextEx->m_bSend = false;
						pIoContextEx->m_lockSend.unlock();
						ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) �ͻ��� %s:%d �Ͽ�����.������:%d,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
						CIOCPModel::AddTask(new NotifyMSG(pIOCPModel, TASK_DISCONNECT, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)), ntohs(pSocketContext->m_ClientAddr.sin_port)));
						if (0 == pIoContextEx->m_nType){
							if (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0)) pIOCPModel->_ReConnect(pIoContextEx, true);
						}
						else
							// �ͷŵ���Ӧ����Դ
							pIOCPModel->_RemoveContext(pSocketContext);

						continue;
					}
				}
			}  
			else
			{

				switch( pIoContext->m_OpType )  
				{  
					 // Accept  
				case ACCEPT_POSTED:
					{ 
						// Ϊ�����Ӵ���ɶ��ԣ�������ר�ŵ�_DoAccept�������д�����������
						pIOCPModel->_DoAccpet( pSocketContext, pIoContext );						

					}
					break;

					// RECV
				case RECV_POSTED:
					{
						// Ϊ�����Ӵ���ɶ��ԣ�������ר�ŵ�_DoRecv�������д����������
						pIoContext->m_wsaBuf.len = dwBytesTransfered;
						pIOCPModel->_DoRecv( pSocketContext,pIoContext );
					}
					break;

					// SEND
					// �����Թ���д�ˣ�Ҫ������̫���ˣ���������⣬Send�������������һЩ
				case NULL_POSTED:
					break;
				case CONNECT_POSTED:
				{
					pIOCPModel->_DoConnect(pSocketContext, pIoContext);
				}
					break;
				default:
					// ��ȡ����Ĳ���
					PER_IO_CONTEXT* pIoContextEx = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_OverlappedEx);
					switch (pIoContextEx->m_OpTypeEx)
					{
					case SEND_POSTED:
					{
						pIoContextEx->m_lockSend.lock();
						pIoContextEx->m_OpTypeEx = NULL_POSTED;
						pIOCPModel->m_dwHeartBeatTime = ::GetTickCount();
						if (pIoContextEx->m_deqSend.size() < 1)
						{
							pIoContextEx->m_bSend = false;
						}
						else
						{
							pIOCPModel->ReSendData(pIoContextEx);
						}
						pIoContextEx->m_lockSend.unlock();
						continue;
					}
						break;
					default:
						break;
					}
					break;
				} //switch
			}//if
		}//if

	}//while

//	TRACE(_T("�������߳� %d ���˳�.\n"),nThreadNo);

	// �ͷ��̲߳���
	//RELEASE(lpParam);	

	return 0;
}



//====================================================================================
//
//				    ϵͳ��ʼ������ֹ
//
//====================================================================================




////////////////////////////////////////////////////////////////////
// ��ʼ��WinSock 2.2
bool CIOCPModel::LoadSocketLib(PUSER_CB_IOCP pfnBack)
{    
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	// ����(һ�㶼�����ܳ���)
	if (NO_ERROR != nResult)
	{
		//this->_ShowMessage(_T("��ʼ��WinSock 2.2ʧ�ܣ�\n"));
		return false; 
	}
	m_pfnNotify = pfnBack;
	m_hHanderTask = ::CreateThread(0, 0, _TaskThread, (void *)this, 0, 0);
	m_hEventTask = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_hTaskOver = CreateEvent(NULL, TRUE, FALSE, NULL);
	
	return true;
}

static std::wstring MultiToWideByte(const char* s, size_t len){
	size_t num = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
	if (num > len) num = len;

	wchar_t* pchar = new wchar_t[num];
	if (pchar == NULL)	return NULL;
	memset(pchar, 0, sizeof(*pchar)*num);

	if (MultiByteToWideChar(CP_ACP, 0, s, -1, pchar, num * sizeof(wchar_t)) < num){
		delete[] pchar;
		pchar = NULL;
		return NULL;
	}
	std::wstring str = pchar;
	delete[] pchar;
	return str;
}

//////////////////////////////////////////////////////////////////
//	����������
bool CIOCPModel::Start(bool bIsServer,const char* pStrIP, int nPort)
{
	// ����ϵͳ�˳����¼�֪ͨ
	ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) �汾��20180614", __FILE__, __LINE__);
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_bExit = false;

 //	CStringA str(pStrIP);
//	std::wstring str = MultiToWideByte(pStrIP, strlen(pStrIP) + 1);
//	m_strIP.Format(L"%s", str.c_str());
	m_strIP = pStrIP;
	m_nPort = nPort;
	m_bIsServer = bIsServer;
	m_dwHeartBeatTime = 0;
	// ��ʼ��IOCP
	if (false == _InitializeIOCP())
	{
		//this->_ShowMessage(_T("��ʼ��IOCPʧ�ܣ�\n"));
		return false;
	}
	else
	{
		//this->_ShowMessage(_T("\nIOCP��ʼ�����\n."));
	}
	// ��ʼ��Socket
	if (m_bIsServer)
	{
		if (false == _InitializeListenSocket())
		{
			//this->_ShowMessage(_T("Listen Socket��ʼ��ʧ�ܣ�\n"));
			this->_DeInitialize();
			return false;
		}
		else
		{
			//this->_ShowMessage(_T("Listen Socket��ʼ�����."));
		}
	}
	else
	{
		if (false == _InitializeClientSocket())
		{
			//this->_ShowMessage(_T("Client Socket��ʼ��ʧ�ܣ�\n"));

			return false;
		}
		else
		{
			//this->_ShowMessage(_T("Client Socket��ʼ�����."));
		}
	}

	//this->_ShowMessage(_T("ϵͳ׼���������Ⱥ�����....\n"));

	m_bStart = true;

	Sleep(1000);

	return true;
}


////////////////////////////////////////////////////////////////////
//	��ʼ����ϵͳ�˳���Ϣ���˳���ɶ˿ں��߳���Դ
void CIOCPModel::Stop()
{
	if (!m_bStart) return;
	m_bExit = true;
	m_bStart = false;
	if( m_pListenContext!=NULL && m_pListenContext->m_Socket!=INVALID_SOCKET )
	{
		// ����ر���Ϣ֪ͨ
		SetEvent(m_hShutdownEvent);

		for (int i = 0; i < m_nThreads; i++)
		{
			// ֪ͨ���е���ɶ˿ڲ����˳�
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}

		// �ȴ����еĿͻ�����Դ�˳�
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

		// ����ͻ����б���Ϣ
		this->_ClearContextList();

		// �ͷ�������Դ
		this->_DeInitialize();

		//this->_ShowMessage(_T("ֹͣ����\n"));
	}	

	if (m_pClinetContext != NULL)
	{
		// ����ر���Ϣ֪ͨ
		SetEvent(m_hShutdownEvent);

		for (int i = 0; i < m_nThreads; i++)
		{
			// ֪ͨ���е���ɶ˿ڲ����˳�
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}

		// �ȴ����еĿͻ�����Դ�˳�
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

		// ����ͻ����б���Ϣ
		this->_ClearContextList();

		// �ͷ�������Դ
		this->_DeInitialize();

		//this->_ShowMessage(_T("ֹͣ����\n"));
	}
	CloseHandle(m_hShutdownEvent);
	CIOCPModel::ClearTask(this);
	Sleep(1000);
}


////////////////////////////////
// ��ʼ����ɶ˿�
bool CIOCPModel::_InitializeIOCP()
{
	// ������һ����ɶ˿�
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0 );
	m_deqMsg.clear();
	if ( NULL == m_hIOCompletionPort)
	{
		ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ������ɶ˿�ʧ�ܣ��������: %d!\n", __FILE__, __LINE__, WSAGetLastError());
		return false;
	}
	// ���ݱ����еĴ�����������������Ӧ���߳���
	if (m_bIsServer)
		m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();
	else
		m_nThreads = WORKER_THREADS_PER_PROCESSOR;
	// Ϊ�������̳߳�ʼ�����
	m_phWorkerThreads = new HANDLE[m_nThreads];

	// ���ݼ�����������������������߳�
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; i++)
	{
		THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
		pThreadParams->pIOCPModel = this;
		pThreadParams->nThreadNo = i + 1;
		m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
	}
	m_bRelase = true;
//	TRACE(" ���� _WorkerThread %d ��.\n", m_nThreads);
	return true;
}


/////////////////////////////////////////////////////////////////
// ��ʼ��Socket
bool CIOCPModel::_InitializeListenSocket()
{
	// AcceptEx �� GetAcceptExSockaddrs ��GUID�����ڵ�������ָ��
	GUID GuidAcceptEx = WSAID_ACCEPTEX;  
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS; 

	// ��������ַ��Ϣ�����ڰ�Socket
	struct sockaddr_in ServerAddress;

	// �������ڼ�����Socket����Ϣ
	m_pListenContext = new PER_SOCKET_CONTEXT;

	// ��Ҫʹ���ص�IO�������ʹ��WSASocket������Socket���ſ���֧���ص�IO����
	m_pListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_Socket) 
	{
//		/*this->_ShowMessage*/TRACE(_T("��ʼ��Socketʧ�ܣ��������: %d.\n"), WSAGetLastError());
		return false;
	}
	else
	{
		
//		TRACE("WSASocket() ���.\n");
	}

	// ��Listen Socket������ɶ˿���
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)m_pListenContext, 0))
	{  
		//this->_ShowMessage(_T("�� Listen Socket����ɶ˿�ʧ�ܣ��������: %d/n"), WSAGetLastError());
		RELEASE_SOCKET( m_pListenContext->m_Socket );
		return false;
	}
	else
	{
//		TRACE(_T("Listen Socket����ɶ˿� ���.\n"));
	}

	// ����ַ��Ϣ
	ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);    


// 	size_t len = wcslen(m_strIP.GetBuffer(0)) + 1;
// 	size_t converted = 0;
// 	char *CStr;
// 	CStr = (char*)malloc(len*sizeof(char));
// 	wcstombs_s(&converted, CStr, len, m_strIP.GetBuffer(0), _TRUNCATE);

	ServerAddress.sin_addr.s_addr = inet_addr(/*CStr*/m_strIP.c_str());
	ServerAddress.sin_port = htons(m_nPort);                          
//	free(CStr);
	// �󶨵�ַ�Ͷ˿�
	if (SOCKET_ERROR == bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress))) 
	{
		//this->_ShowMessage(_T("bind()����ִ�д���.\n"));
		return false;
	}
	else
	{
//		TRACE("bind() ���.\n");
	}

	// ��ʼ���м���
	if (SOCKET_ERROR == listen(m_pListenContext->m_Socket,SOMAXCONN))
	{
		//this->_ShowMessage(_T("Listen()����ִ�г��ִ���.\n"));
		return false;
	}
	else
	{
//		TRACE(_T("Listen() ���.\n"));
	}

	// ΪAcceptEx ׼��������Ȼ��Ͷ��AcceptEx I/O����
	for( int i=0;i<MAX_POST_ACCEPT;i++ )
	{
		// �½�һ��IO_CONTEXT
		std::shared_ptr<PER_IO_CONTEXT> pAcceptIoContext = m_pListenContext->GetNewIoContext();

		if( false==this->_PostAccept( pAcceptIoContext.get() ) )
		{
			m_pListenContext->RemoveContext(pAcceptIoContext.get());
			return false;
		}
	}

	//this->_ShowMessage( _T("Ͷ�� %d ��AcceptEx�������"),MAX_POST_ACCEPT );

	return true;
}

/////////////////////////////////////////////////////////////////
// ��ʼ��Socket
bool CIOCPModel::_InitializeClientSocket()
{
	// AcceptEx �� GetAcceptExSockaddrs ��GUID�����ڵ�������ָ��
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

	// ��������ַ��Ϣ�����ڰ�Socket
	

	// �������ڼ�����Socket����Ϣ
	m_pClinetContext = new PER_SOCKET_CONTEXT;

	std::shared_ptr<PER_IO_CONTEXT> pConnectIoContext = m_pClinetContext->GetNewIoContext();
	// ����ַ��Ϣ

	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);    



	pConnectIoContext.get()->ServerAddress.sin_addr.s_addr = inet_addr(/*CStr*/m_strIP.c_str());
	pConnectIoContext.get()->ServerAddress.sin_port = htons(m_nPort);
//	free(CStr);

	m_pClinetContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pClinetContext->m_Socket)
	{
		//_ShowMessage(_T("��������Client��Socketʧ�ܣ��������: %d"), WSAGetLastError());
		return false;
	}

	SOCKADDR_IN local;
	local.sin_family = AF_INET;
	local.sin_addr.S_un.S_addr = INADDR_ANY;
	local.sin_port = 0;
	if (SOCKET_ERROR == bind(m_pClinetContext->m_Socket, (LPSOCKADDR)&local, sizeof(local)))
	{
		//this->_ShowMessage(_T("���׽���ʧ��!\r\n"));
		return false;
	}
	this->_ClearContextList();
	this->_AddToContextList(m_pClinetContext);
	if (false == this->_PostConnect(pConnectIoContext.get(), true))
	{
		m_pClinetContext->RemoveContext(pConnectIoContext.get());
		return false;
	}

	return true;
}

////////////////////////////////////////////////////////////
//	����ͷŵ�������Դ
void CIOCPModel::_DeInitialize()
{
	// �ر�ϵͳ�˳��¼����
	RELEASE_HANDLE(m_hShutdownEvent);

	// �ͷŹ������߳̾��ָ��
	for( int i=0;i<m_nThreads;i++ )
	{
		if (m_bRelase)
			RELEASE_HANDLE(m_phWorkerThreads[i]);
	}
	m_bRelase = false;
	RELEASE(m_phWorkerThreads);

	// �ر�IOCP���
	RELEASE_HANDLE(m_hIOCompletionPort);

	// �رռ���Socket
	RELEASE(m_pListenContext);

	//RELEASE(m_pClinetContext);

	//this->_ShowMessage(_T("�ͷ���Դ���.\n"));
}

bool CIOCPModel::_ReConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine)
{
	// ����ַ��Ϣ
	pConnectIoContext->m_lockSend.lock();
	pConnectIoContext->m_bSend = false;
	pConnectIoContext->m_lockSend.unlock();
	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	m_bStart = false;

	pConnectIoContext->ServerAddress.sin_addr.s_addr = inet_addr(/*CStr*/m_strIP.c_str());
	pConnectIoContext->ServerAddress.sin_port = htons(m_nPort);
//	free(CStr);
	if (false == this->_PostConnect(pConnectIoContext, bReOnLine))
	{
		//m_pClinetContext->RemoveContext(pConnectIoContext);
		return false;
	}
	return true;
}

//====================================================================================
//
//				    Ͷ����ɶ˿�����
//
//====================================================================================

bool CIOCPModel::_PostConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine)
{
	// ׼������
	DWORD dwSend = 0;
	char szBuffer[256] = {0};
	gethostname(szBuffer, 256);
	pConnectIoContext->m_OpType = CONNECT_POSTED;
	WSABUF *p_wbuf = &pConnectIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pConnectIoContext->m_Overlapped;
	ZeroMemory(p_ol, sizeof(OVERLAPPED));
	// ��ʼ����ɺ󣬣�Ͷ��WSARecv����
	LPFN_CONNECTEX ConnectEx;
	DWORD dwBytes = 0;
	GUID guidConnectEx = WSAID_CONNECTEX;
	pConnectIoContext->m_nType = 0;
	// Ϊ�Ժ�������Ŀͻ�����׼����Socket( ������봫ͳaccept�������� ) 
	if (bReOnLine){
		if (pConnectIoContext->m_sockAccept != INVALID_SOCKET)
		{
			shutdown(pConnectIoContext->m_sockAccept, SD_SEND);
			closesocket(pConnectIoContext->m_sockAccept);
			pConnectIoContext->m_sockAccept = INVALID_SOCKET;
		}
		pConnectIoContext->m_sockAccept = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (INVALID_SOCKET == pConnectIoContext->m_sockAccept)
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��������Client��Socketʧ�ܣ��������: %d", __FILE__, __LINE__, WSAGetLastError());
			return false;
		}
		const char chOpt = 1;
		//int nErr = setsockopt(pConnectIoContext->m_sockAccept, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
		if (NULL == CreateIoCompletionPort((HANDLE)pConnectIoContext->m_sockAccept, m_hIOCompletionPort, (ULONG_PTR)m_pClinetContext, 0))
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) �� Client Socket����ɶ˿�ʧ�ܣ��������: %d/n", __FILE__, __LINE__, WSAGetLastError());
			RELEASE_SOCKET(pConnectIoContext->m_sockAccept);
			return false;
		}
		else
		{
			//TRACE(_T("Client Socket����ɶ˿� ���.\n"));
		}

		SOCKADDR_IN local;
		local.sin_family = AF_INET;
		local.sin_addr.S_un.S_addr = INADDR_ANY;
		local.sin_port = 0;
		if (SOCKET_ERROR == bind(pConnectIoContext->m_sockAccept, (LPSOCKADDR)&local, sizeof(local)))
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ���׽���ʧ��!\r\n", __FILE__, __LINE__);
			return false;
		}
	}
	if (SOCKET_ERROR == WSAIoctl(pConnectIoContext->m_sockAccept, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidConnectEx, sizeof(guidConnectEx), &ConnectEx, sizeof(ConnectEx), &dwBytes, NULL, NULL))
	{
		ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) �õ���չ����ָ��ʧ��!\r\n", __FILE__, __LINE__);
		return false;
	}
	
	if (!ConnectEx(pConnectIoContext->m_sockAccept, (const sockaddr*)&pConnectIoContext->ServerAddress, sizeof(pConnectIoContext->ServerAddress), szBuffer, strlen(szBuffer), &dwSend, p_ol))
	{
		DWORD dwError = WSAGetLastError();
		if (ERROR_IO_PENDING != dwError)
		{

			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ���ӷ�����ʧ��\r\n", __FILE__, __LINE__);
			return false;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////
// Ͷ��Accept����
bool CIOCPModel::_PostAccept( PER_IO_CONTEXT* pAcceptIoContext )
{
	ASSERT( INVALID_SOCKET!=m_pListenContext->m_Socket );
	// ׼������
	DWORD dwBytes = 0;  
	pAcceptIoContext->m_OpType = ACCEPT_POSTED;  
	WSABUF *p_wbuf   = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pAcceptIoContext->m_Overlapped;
	pAcceptIoContext->m_nType = 1;
	// Ϊ�Ժ�������Ŀͻ�����׼����Socket( ������봫ͳaccept�������� ) 
	pAcceptIoContext->m_sockAccept  = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);  
	if( INVALID_SOCKET==pAcceptIoContext->m_sockAccept )  
	{  
		ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��������Accept��Socketʧ�ܣ��������: %d", __FILE__, __LINE__, WSAGetLastError());
		return false;  
	} 
	const char chOpt = 1;
	//int nErr = setsockopt(pAcceptIoContext->m_sockAccept, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	// Ͷ��AcceptEx
	//if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN)+16) * 2),
	//	sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, &dwBytes, p_ol))
	if (FALSE == AcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN)+16) * 2),
		sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, &dwBytes, p_ol))
	{  
		if(WSA_IO_PENDING != WSAGetLastError())  
		{  
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) Ͷ�� AcceptEx ����ʧ�ܣ��������: %d", __FILE__, __LINE__, WSAGetLastError());
			return false;  
		}  
	} 
	++m_pListenContext->_nCount;
	return true;
}

bool CIOCPModel::_DoConnect(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* LocalAddr = NULL;
	
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);
	// 3. �������������µ�IoContext�����������Socket��Ͷ�ݵ�һ��Recv��������
	//PER_IO_CONTEXT* pNewIoContext = pSocketContext->GetNewIoContext();
	pIoContext->m_OpType = RECV_POSTED;
	//pIoContext->m_sockAccept = pSocketContext->m_Socket;
	//memcpy(&(pIoContext->ServerAddress), &(pIoContext->ServerAddress), sizeof(SOCKADDR_IN));
	memcpy(&(pSocketContext->m_ClientAddr), &(pIoContext->ServerAddress), sizeof(SOCKADDR_IN));
	const char chOpt = 1;
	if (!this->_SetSocketOpt(pIoContext->m_sockAccept))
		return false;
	char cHostName[24] = { 0 };
	int ret = gethostname(cHostName, 24);
	if (ret != 0)
	{
		return false;
	}
	if (false == this->_PostRecv(pIoContext))
	{
		//pSocketContext->RemoveContext(pIoContext);
		return false;
	}

	/////////////////////////////////////////////////////////////////////////////////////////////////
	// 4. ���Ͷ�ݳɹ�����ô�Ͱ������Ч�Ŀͻ�����Ϣ�����뵽ContextList��ȥ(��Ҫͳһ���������ͷ���Դ)
	//this->_AddToContextList(pSocketContext);
	m_bStart = true;
	CIOCPModel::AddTask(new NotifyMSG(this, TASK_CONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port), cHostName));
	//this->_ShowMessage(_T("���ӳɹ���"));
	pIoContext->m_bLine = true;
}
////////////////////////////////////////////////////////////
// ���пͻ��������ʱ�򣬽��д���
// �����е㸴�ӣ���Ҫ�ǿ������Ļ����Ϳ����׵��ĵ���....
// ������������Ļ�����ɶ˿ڵĻ������������һ�����

// ��֮��Ҫ֪�����������ListenSocket��Context��������Ҫ����һ�ݳ������������Socket��
// ԭ����Context����Ҫ���������Ͷ����һ��Accept����
//
bool CIOCPModel::_DoAccpet( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext )
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* LocalAddr = NULL;  
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);  
	///////////////////////////////////////////////////////////////////////////
	// 1. ����ȡ������ͻ��˵ĵ�ַ��Ϣ
	// ��� m_lpfnGetAcceptExSockAddrs �����˰�~~~~~~
	// ��������ȡ�ÿͻ��˺ͱ��ض˵ĵ�ַ��Ϣ������˳��ȡ���ͻ��˷����ĵ�һ�����ݣ���ǿ����...
	//this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN)+16)*2),  
	//	sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);  
	GetAcceptExSockaddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN)+16) * 2),
		sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);


	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// 2. ������Ҫע�⣬���ﴫ��������ListenSocket�ϵ�Context�����Context���ǻ���Ҫ���ڼ�����һ������
	// �����һ���Ҫ��ListenSocket�ϵ�Context���Ƴ���һ��Ϊ�������Socket�½�һ��SocketContext

	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_Socket           = pIoContext->m_sockAccept;
	memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));
	
	// ����������ϣ������Socket����ɶ˿ڰ�(��Ҳ��һ���ؼ�����)
	if( false==this->_AssociateWithIOCP( pNewSocketContext ) )
	{
		RELEASE( pNewSocketContext );
		return false;
	}  
	m_bStart = true;

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// 3. �������������µ�IoContext�����������Socket��Ͷ�ݵ�һ��Recv��������
	std::shared_ptr<PER_IO_CONTEXT> pNewIoContext = pNewSocketContext->GetNewIoContext();
	pNewIoContext.get()->m_OpType       = RECV_POSTED;
	pNewIoContext.get()->m_sockAccept   = pNewSocketContext->m_Socket;
	pNewIoContext.get()->m_nType = 1;
	if (!this->_SetSocketOpt(pNewIoContext.get()->m_sockAccept)){
		return false;
	}
	// ���Buffer��Ҫ���������Լ�����һ�ݳ���
	//memcpy( pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN );
	memcpy(&(pNewIoContext.get()->ServerAddress), ClientAddr, sizeof(SOCKADDR_IN));
	// �����֮�󣬾Ϳ��Կ�ʼ�����Socket��Ͷ�����������
	if( false==this->_PostRecv( pNewIoContext.get()) )
	{
		pNewSocketContext->RemoveContext( pNewIoContext.get() );
		return false;
	}

	CIOCPModel::AddTask(new NotifyMSG(this, TASK_CONNECT, inet_ntoa(ClientAddr->sin_addr), strlen(inet_ntoa(ClientAddr->sin_addr)), ntohs(ClientAddr->sin_port), pIoContext->m_wsaBuf.buf));

	/////////////////////////////////////////////////////////////////////////////////////////////////
	// 4. ���Ͷ�ݳɹ�����ô�Ͱ������Ч�Ŀͻ�����Ϣ�����뵽ContextList��ȥ(��Ҫͳһ���������ͷ���Դ)
	this->_AddToContextList( pNewSocketContext );

	////////////////////////////////////////////////////////////////////////////////////////////////
	// 5. ʹ�����֮�󣬰�Listen Socket���Ǹ�IoContext���ã�Ȼ��׼��Ͷ���µ�AcceptEx
	pIoContext->ResetBuffer();
	pIoContext->m_bLine = true;
	return this->_PostAccept( pIoContext ); 	
}

bool CIOCPModel::_SetSocketOpt(SOCKET& s)
{
	const char chOpt = 1;
	//int nErr = ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	//if (nErr == SOCKET_ERROR)
	//{
	//	return false;
	//}
	//BOOL bKeepAlive = TRUE;
	//int nErr = ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*)&bKeepAlive, sizeof(bKeepAlive));
	//if (nErr == SOCKET_ERROR)
	//{
	//	return false;
	//}
	//// ����KeepAlive����
	//tcp_keepalive alive_in = { 0 };
	//tcp_keepalive alive_out = { 0 };
	//alive_in.keepalivetime = 1000; // ��ʼ�״�KeepAlive̽��ǰ��TCP�ձ�ʱ��
	//alive_in.keepaliveinterval = 500; // ����KeepAlive̽����ʱ����
	//alive_in.onoff = TRUE;
	//unsigned long ulBytesReturn = 0;
	//nErr = WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in),
	//	&alive_out, sizeof(alive_out), &ulBytesReturn, NULL, NULL);
	//if (nErr == SOCKET_ERROR)
	//{
	//	return false;
	//}

	return true;
}

////////////////////////////////////////////////////////////////////
// Ͷ�ݽ�����������
bool CIOCPModel::_PostRecv( PER_IO_CONTEXT* pIoContext )
{
	// ��ʼ������
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	pIoContext->m_wsaBuf.len = MAX_BUFFER_LEN;
	pIoContext->m_nHeartBeat = ::GetTickCount();
	if (!m_dwHeartBeatTime) 
		m_dwHeartBeatTime = ::GetTickCount();
	WSABUF *p_wbuf   = &pIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;

	// ��ʼ����ɺ󣬣�Ͷ��WSARecv����
	int nBytesRecv = WSARecv( pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL );

	// �������ֵ���󣬲��Ҵ���Ĵ��벢����Pending�Ļ����Ǿ�˵������ص�����ʧ����
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		//this->_ShowMessage(_T("Ͷ�ݵ�һ��WSARecvʧ�ܣ�"));
		ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) Ͷ�ݵ�һ��WSARecvʧ�ܣ�������룺%d", __FILE__, __LINE__, WSAGetLastError());
		if (pIoContext && 0 == pIoContext->m_nType)
		{
			//CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port)));
			if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) {
				this->_ReConnect(pIoContext, true);
				return true;
			}
		}
		return false;
	}
	return true;
}

/////////////////////////////////////////////////////////////////
// ���н��յ����ݵ����ʱ�򣬽��д���
bool CIOCPModel::_DoRecv( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext )
{
	// �Ȱ���һ�ε�������ʾ���֣�Ȼ�������״̬��������һ��Recv����
	SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
	//this->_ShowMessage( _T("�յ�  %s:%d ��Ϣ��%s"),inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port),pIoContext->m_wsaBuf.buf );
	Save(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len, pIoContext);
	// Ȼ��ʼͶ����һ��WSARecv����
	return _PostRecv( pIoContext );
}

void CIOCPModel::ClearOverlapped(PPER_IO_CONTEXT pInfo)
{
	int i = 0;
	for (; i < QUEUE_SIZE - 1; i++)
	{
		if (!pInfo->queBuf[i].buf)
		{
			pInfo->nBufSize--;
			break;
		}
	}
	for (; i < QUEUE_SIZE - 1; i++)
	{
		pInfo->queBuf[i] = pInfo->queBuf[i + 1];
	}
}

DWORD CIOCPModel::GetVerifyBytes(char* pStr, int nLen) 
{
	if (nLen > 0)
	{
		//if (pStr[0] == PACKET_FIRST)
		{
			return *(DWORD*)&(pStr[sizeof(DWORD) + 32]);
		}
	}
	return -1;
}

BOOL CIOCPModel::ReSendData(PER_IO_CONTEXT* pIoContextEx)
{
	if (pIoContextEx->m_deqSend.size())
	{
		LPSendData pInfo = pIoContextEx->m_deqSend.front();
		if (!pInfo) return FALSE;
		pIoContextEx->m_deqSend.pop_front();
		std::shared_ptr<_PER_IO_CONTEXT> sp = _GetSocketContext(pInfo->_pIP, pInfo->_dwPort);
		if (sp.get() && m_bStart)
		{
			if (!m_bStart || !pIoContextEx->m_bLine) {
				return FALSE;
			}
			sp.get()->m_OpTypeEx = SEND_POSTED;
			DWORD dwBytesSend = 0;
			memset(sp.get()->m_wsendBuf.buf, 0, MAX_BUFFER_LEN);
			memcpy(sp.get()->m_wsendBuf.buf, pInfo->_pStr, pInfo->_nLen);
			sp.get()->m_wsendBuf.len = pInfo->_nLen;
			int nBytesSend = WSASend(sp.get()->m_sockAccept, &sp.get()->m_wsendBuf, 1, &dwBytesSend, 0, &(sp.get()->m_OverlappedEx), NULL);
			if ((SOCKET_ERROR == nBytesSend) && (WSA_IO_PENDING != WSAGetLastError()))
			{
				char cError[64] = { 0 };
				sprintf_s(cError, "%d", WSAGetLastError());
				CIOCPModel::AddTask(new NotifyMSG(this, TASK_SND_ERROR, cError, strlen(cError), pInfo->_dwPort, pInfo->_pIP), GRADE_HIGH);
				ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) Ͷ�ݵ�һ��WSASendʧ�ܣ��������:%d", __FILE__, __LINE__, WSAGetLastError());
				delete pInfo;
				return FALSE;
			}
			else
				sp.get()->m_bSend = true;
			delete pInfo;
			return TRUE;
		}
		delete pInfo;
	}

	return false;
}

BOOL CIOCPModel::SendData(DWORD dwType, char* pStr, int nLen, char* pStrIP, int nPort, E_TASK_GRADE eGrade)
{
	std::shared_ptr<_PER_IO_CONTEXT> sp = _GetSocketContext(pStrIP, nPort);
	if (sp.get() && m_bStart)
	{
			static int nSize = 0;
			sp.get()->m_lockSend.lock();
			if (!m_bStart || !sp.get()->m_bLine) {
				sp.get()->m_lockSend.unlock();
				return FALSE;
			}
			if (nLen <= BUF_SIZE)
			{
				int len = nLen + sizeof(DWORD)* 2 + 32;
				char* str = new char[len];
				*(DWORD*)(str) = dwType;
				*(DWORD*)(str + sizeof(DWORD) + 32) = nLen;
				memcpy(str + sizeof(DWORD)* 2 + 32, pStr, nLen);
				WSABUF wsb;
				wsb.buf = str;
				wsb.len = len;
				if (GRADE_LOW == eGrade)
					sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_END));
				else{
					if (sp.get()->m_deqSend.size() < 1)
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_END));
					else
					for (auto it = sp.get()->m_deqSend.begin(); it != sp.get()->m_deqSend.end(); it++)
					{
						if ((*it)->_ePart == PART_END){
							it++;
							if (it == sp.get()->m_deqSend.end())
								sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_END));
							else
								sp.get()->m_deqSend.insert(it, new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_END));
							break;
						}
					}
				}
				delete[] wsb.buf;
				if (!sp.get()->m_bSend){
					this->ReSendData(sp.get());
				}
				sp.get()->m_lockSend.unlock();
				return TRUE;
			}
			else
			{
				int nCounet = nLen / BUF_SIZE;
				int nSurplus = nLen % BUF_SIZE;
				if (nSurplus) ++nCounet;
				for (int i = 0; i < nCounet; i++)
				{
					if (0 == i)
					{
						int len = BUF_SIZE + sizeof(DWORD)* 2 + 32;
						char* str = new char[len];
						memset(str, 0, len);
						*(DWORD*)(str) = dwType;
						std::string sMd5 = GetMD5Verify(pStr, nLen);
						for (int i = 0; i < sMd5.length() && i < 32; i++)
						{
							*(BYTE*)(str + sizeof(DWORD)+i) = sMd5.at(i);
						}
						*(DWORD*)(str + sizeof(DWORD) + 32) = nLen;
						memcpy(str + sizeof(DWORD)* 2 + 32, pStr, BUF_SIZE);
						WSABUF wsb;
						wsb.buf = str;
						wsb.len = len;
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_HEAD));
						delete[] wsb.buf;
					}
					else if (nCounet - 1 == i && nSurplus)
					{
						char* str = new char[nSurplus];
						memcpy(str, pStr + i*BUF_SIZE, nSurplus);
						WSABUF wsb;
						wsb.buf = str;
						wsb.len = nSurplus;
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_END));
						delete[] wsb.buf;
						if (!sp.get()->m_bSend){
							this->ReSendData(sp.get());
						}
					}
					else if (nCounet - 1 == i)
					{
						char* str = new char[BUF_SIZE];
						memcpy(str, pStr + i*BUF_SIZE, BUF_SIZE);
						WSABUF wsb;
						wsb.buf = str;
						wsb.len = BUF_SIZE;
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_END));
						delete[] wsb.buf;
						if (!sp.get()->m_bSend){
							this->ReSendData(sp.get());
						}
					}
					else
					{
						char* str = new char[BUF_SIZE];
						memcpy(str, pStr + i*BUF_SIZE, BUF_SIZE);
						WSABUF wsb;
						wsb.buf = str;
						wsb.len = BUF_SIZE;
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType, PART_MID));
						delete[] wsb.buf;
					}
				}
				sp->m_lockSend.unlock();

				return true;
			}	
	}
	return false;
}

std::string CIOCPModel::GetMD5Verify(char* pStr, DWORD nLen)
{
	Md5Encode encode;
	return encode.Encode((BYTE*)pStr, nLen);
}

void CIOCPModel::Save(char* pStr, int nLen, PPER_IO_CONTEXT pContext)
{
	if (nLen <= 0)
	{
		pContext->nDesLen = 0;
		pContext->nCurLen = 0;
		return;
	}
	if (pContext->wsTempBuf.len > 0)
	{
		WSABUF wBuf;
		wBuf.len = pContext->wsTempBuf.len + nLen;
		wBuf.buf = new char[wBuf.len];
		memcpy(wBuf.buf, pContext->wsTempBuf.buf, pContext->wsTempBuf.len);
		delete pContext->wsTempBuf.buf;
		memcpy(wBuf.buf + pContext->wsTempBuf.len, pStr, nLen);
		pContext->wsTempBuf.len = 0;
		Save(wBuf.buf, wBuf.len, pContext);
		delete wBuf.buf;
		return;
	}
	int nLess = pContext->nDesLen - pContext->nCurLen;
	if (nLen > sizeof(DWORD)* 2 + 32 || nLess != 0)
	{
		if (pContext->nDesLen == 0)
		{
			pContext->nDesLen = GetVerifyBytes(pStr, nLen);
			if (pContext->nDesLen != -1)
			{
				if (pContext->nDesLen < 0)
				{
					pContext->nDesLen = 0;
					pContext->nCurLen = 0;
					return;
				}
				pContext->wsBuf.len = pContext->nDesLen;
				pContext->wsBuf.buf = new char[pContext->wsBuf.len];
				pContext->nCurLen = 0;
				memset(pContext->btMD5, 0, 33);
				memcpy(pContext->btMD5, pStr + sizeof(DWORD), 32);
				Save(pStr + sizeof(DWORD)* 2 + 32, nLen - sizeof(DWORD)* 2 - 32, pContext);
			}
			else
			{
				pContext->nDesLen = 0;
				pContext->nCurLen = 0;
				return;
			}
		}
		else
		{
			if (nLess > nLen)
			{
				memcpy(pContext->wsBuf.buf + pContext->nCurLen, pStr, nLen);
				pContext->nCurLen += nLen;
			}
			else
			{
				if (nLess < 0){
					pContext->nDesLen = 0;
					pContext->nCurLen = 0;
					delete[] pContext->wsBuf.buf;
					return;
				}
				static int nSize = 0;
				nSize += pContext->nDesLen;
				static char sMsg[1024] = { 0 };
				memset(sMsg, 0, 1024);
				memcpy(pContext->wsBuf.buf + pContext->nCurLen, pStr, nLess);
				bool bFlags = false;
				pContext->nDesLen = 0;
				pContext->nCurLen = 0;
				{
					m_dwHeartBeatTime = ::GetTickCount();
					if (IsVerifyHeartBeat(pContext->wsBuf.buf, pContext->wsBuf.len))
						pContext->m_nHeartBeat = ::GetTickCount();
					else
						CIOCPModel::AddTask(new NotifyMSG(this, TASK_DATA, pContext->wsBuf.buf, pContext->wsBuf.len, ntohs(pContext->ServerAddress.sin_port), inet_ntoa(pContext->ServerAddress.sin_addr)));
//						CStringA strTemp;					
					char tmp[1024] = { 0 };
					sprintf_s(tmp, "%s_%d", inet_ntoa(pContext->ServerAddress.sin_addr), ntohs(pContext->ServerAddress.sin_port));
					std::string strTemp(tmp);
					m_mapHeartBeat[strTemp] |= m_dwHeartBeatTime<<32;
						
				}
				delete[] pContext->wsBuf.buf;
				if (nLen - nLess > 0)
					Save(pStr + nLess, nLen - nLess, pContext);
			}
		}
	}
	else
	{
		pContext->wsTempBuf.len = nLen;
		pContext->wsTempBuf.buf = new char[nLen];
		memcpy(pContext->wsTempBuf.buf, pStr, nLen);
	}
}

bool CIOCPModel::IsVerifyHeartBeat(char* pStr, int nLen)
{
	return nLen == 3 && pStr[0] == 's' && pStr[1] == 'y' && pStr[2] == 'n';
}

/////////////////////////////////////////////////////
// �����(Socket)�󶨵���ɶ˿���
bool CIOCPModel::_AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext )
{
	// �����ںͿͻ���ͨ�ŵ�SOCKET�󶨵���ɶ˿���
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)pContext, 0);

	if (NULL == hTemp)
	{
		//this->_ShowMessage(_T("ִ��CreateIoCompletionPort()���ִ���.������룺%d"), WSAGetLastError());
		return false;
	}

	return true;
}




//====================================================================================
//
//				    ContextList ��ز���
//
//====================================================================================


//////////////////////////////////////////////////////////////
// ���ͻ��˵������Ϣ�洢��������
void CIOCPModel::_AddToContextList( PER_SOCKET_CONTEXT *pHandleData )
{
	m_csContextList.lock();
	std::shared_ptr<PER_SOCKET_CONTEXT> sp(pHandleData);
//	m_arrayClientContext.Add(sp);
	m_arrayClientContext.push_back(sp);
	m_csContextList.unlock();
}

std::shared_ptr<_PER_IO_CONTEXT> CIOCPModel::_GetSocketContext(char* pStrIP, int nPort)
{
	m_csContextList.lock();
	for (int i = 0; i < m_arrayClientContext.size(); i++)
	{
		std::shared_ptr<_PER_IO_CONTEXT> sp = m_arrayClientContext[i].get()->GetIoContext(pStrIP, nPort);
		if (sp.get()){
			m_csContextList.unlock();
			return sp;
		}
	}
	m_csContextList.unlock();
	return std::shared_ptr<_PER_IO_CONTEXT>();
}

////////////////////////////////////////////////////////////////
//	�Ƴ�ĳ���ض���Context
void CIOCPModel::_RemoveContext( PER_SOCKET_CONTEXT *pSocketContext )
{
	m_csContextList.lock();

	for( int i=0;i<m_arrayClientContext.size();i++ )
	{
		if( pSocketContext==m_arrayClientContext[i].get() )
		{
			//	RELEASE(pSocketContext);	
			//m_arrayClientContext.RemoveAt(i);		
			m_arrayClientContext.erase(m_arrayClientContext.begin() + i);
			break;
		}
	}
	
	m_csContextList.unlock();
}

////////////////////////////////////////////////////////////////
// ��տͻ�����Ϣ
void CIOCPModel::_ClearContextList()
{
	m_csContextList.lock();

	m_arrayClientContext.clear();

	m_csContextList.unlock();
}



//====================================================================================
//
//				       ����������������
//
//====================================================================================



////////////////////////////////////////////////////////////////////
// ��ñ�����IP��ַ
std::string CIOCPModel::GetLocalIP()
{
	// ��ñ���������
	char hostname[MAX_PATH] = {0};
	gethostname(hostname,MAX_PATH);                
	struct hostent FAR* lpHostEnt = gethostbyname(hostname);
	if(lpHostEnt == NULL)
	{
		return DEFAULT_IP;
	}

	// ȡ��IP��ַ�б��еĵ�һ��Ϊ���ص�IP(��Ϊһ̨�������ܻ�󶨶��IP)
	LPSTR lpAddr = lpHostEnt->h_addr_list[0];      

	// ��IP��ַת�����ַ�����ʽ
	struct in_addr inAddr;
	memmove(&inAddr,lpAddr,4);
	m_strIP = inet_ntoa(inAddr);        

	return m_strIP;
}

///////////////////////////////////////////////////////////////////
// ��ñ����д�����������
int CIOCPModel::_GetNoOfProcessors()
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	return si.dwNumberOfProcessors;
}
#include <time.h>

/////////////////////////////////////////////////////////////////////
// �жϿͻ���Socket�Ƿ��Ѿ��Ͽ���������һ����Ч��Socket��Ͷ��WSARecv����������쳣
// ʹ�õķ����ǳ��������socket�������ݣ��ж����socket���õķ���ֵ
// ��Ϊ����ͻ��������쳣�Ͽ�(����ͻ��˱������߰ε����ߵ�)��ʱ�򣬷����������޷��յ��ͻ��˶Ͽ���֪ͨ��

bool CIOCPModel::_IsSocketAlive(SOCKET s)
{
	int nByteSent=send(s,"",0,0);
	if (-1 == nByteSent) return false;
	return true;
}

///////////////////////////////////////////////////////////////////
// ��ʾ��������ɶ˿��ϵĴ���
bool CIOCPModel::HandleError(PER_SOCKET_CONTEXT *pContext, PER_IO_CONTEXT* pIoContext, const DWORD& dwErr)
{
	pIoContext->m_lockSend.lock();
	pIoContext->m_OpTypeEx = NULL_POSTED;
	pIoContext->m_bLine = false;
	while (pIoContext->m_deqSend.size())
	{
		LPSendData pInfo = pIoContext->m_deqSend.front();
		pIoContext->m_deqSend.pop_front();
		delete pInfo;
	}
	pIoContext->m_bSend = false;
	pIoContext->m_lockSend.unlock();
	m_dwHeartBeatTime = 0;
	Sleep(1000);
	// ����ǳ�ʱ�ˣ����ټ����Ȱ�  
	if(WAIT_TIMEOUT == dwErr)  
	{  	
		// ȷ�Ͽͻ����Ƿ񻹻���...
		if( !_IsSocketAlive( pContext->m_Socket) )
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��⵽�ͻ���:IP %s, PORT:%d�쳣�˳���������룺%ld,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pContext->m_ClientAddr.sin_addr), ntohs(pContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
			CIOCPModel::AddTask(new NotifyMSG(this, TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			if (0 == pIoContext->m_nType){
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
			}
			return true;
		}
		else
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��⵽������쳣�˳�:IP %s, PORT:%d�쳣�˳���������룺%ld,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pContext->m_ClientAddr.sin_addr), ntohs(pContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
			CIOCPModel::AddTask(new NotifyMSG(this, TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			//this->_ShowMessage( _T("���������ʱ��������...") );
			if (0 == pIoContext->m_nType) 
			{
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
			}
			else
			{
				if (--m_pListenContext->_nCount <= 2)
				{
					if (m_pfnNotify->lpDisconnectCB) m_pfnNotify->lpDisconnectCB(m_pfnNotify->lpCallBackData, std::string("-1"), -1);
				}
			}
			return true;
		}
	}  
	else if (1225 == dwErr || ERROR_NETNAME_DELETED == dwErr /*|| 121 == dwErr || 10053 == dwErr*/)
	{
		if (pIoContext && 0 == pIoContext->m_nType)
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��⵽�ͻ���:IP %s, PORT:%d�쳣�˳���������룺%ld,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pContext->m_ClientAddr.sin_addr), ntohs(pContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
			if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) {
				if (ERROR_NETNAME_DELETED == dwErr){
					CIOCPModel::AddTask(new NotifyMSG(this, TASK_DISCONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port)));
					this->_ReConnect(pIoContext, true);
				}
				else
					this->_ReConnect(pIoContext);
			}
		}
		else
		{
			ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��⵽�ͻ���:IP %s, PORT:%d�쳣�˳���������룺%ld,�������ʹ���S:%ld", __FILE__, __LINE__, inet_ntoa(pContext->m_ClientAddr.sin_addr), ntohs(pContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
			CIOCPModel::AddTask(new NotifyMSG(this, TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			this->_RemoveContext(pContext);
		}
		return true;
	}
	// �����ǿͻ����쳣�˳���
	else
	{
		if (pIoContext)
		{
			if (0 == pIoContext->m_nType)
			{
				ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��⵽�ͻ���:IP %s, PORT:%d�쳣�˳���������룺%ld,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pContext->m_ClientAddr.sin_addr), ntohs(pContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
				if (121 != dwErr)
					CIOCPModel::AddTask(new NotifyMSG(this, TASK_DISCONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port)));
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
				return true;
			}
			else
			{
				ILogManage()->WriteLog("(�ļ���:%s_�к�:%d) ��⵽������쳣�˳�:IP %s, PORT:%d�쳣�˳���������룺%ld,�������ʹ���:%ld", __FILE__, __LINE__, inet_ntoa(pContext->m_ClientAddr.sin_addr), ntohs(pContext->m_ClientAddr.sin_port), WSAGetLastError(), GetLastError());
				CIOCPModel::AddTask(new NotifyMSG(this, TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
				this->_RemoveContext(pContext);
				if (--m_pListenContext->_nCount <= 2)
				{
					if (m_pfnNotify->lpDisconnectCB) m_pfnNotify->lpDisconnectCB(m_pfnNotify->lpCallBackData, std::string("-1"), -1);
				}
			}
		}
		return false;
	}
}

DWORD WINAPI CIOCPModel::_TaskThread(LPVOID lpParam)
{
	CIOCPModel* pIOCPModel = (CIOCPModel*)lpParam;
	while (WaitForSingleObject(pIOCPModel->m_hEventTask, 50) != WAIT_OBJECT_0)
	{
// 		if (WAIT_OBJECT_0 == WaitForSingleObject(pIOCPModel->m_hEventTask, 50))
// 			break;
		NotifyMSG* pInfo = PopTask(pIOCPModel);
		static int64_t dwTime = ::GetTickCount();
		int64_t iTime = 0;
		int64_t timeout = 0;
		int64_t losttime = 0;
//		CStringA strTemp;
		std::string strTemp;
		if (pInfo)
		{
			if (pIOCPModel->m_pfnNotify)
			{
				switch (pInfo->_eType)
				{
				case TASK_DATA:
					timeout = ::GetTickCount();
					if (pIOCPModel->m_pfnNotify->lpRecvMsgCB) pIOCPModel->m_pfnNotify->lpRecvMsgCB(pIOCPModel->m_pfnNotify->lpCallBackData, (void*)pInfo->_pStr, pInfo->_dwLen, std::string(pInfo->_cIP), pInfo->_dwPersist);
					losttime = ::GetTickCount() - timeout;
					break;
				case  TASK_CONNECT:
				{
					if (pIOCPModel->m_pfnNotify->lpConnectCB) pIOCPModel->m_pfnNotify->lpConnectCB(pIOCPModel->m_pfnNotify->lpCallBackData, std::string(pInfo->_pStr), pInfo->_dwPersist, std::string(pInfo->_cIP));
					//strTemp.Format("%s_%d", pInfo->_pStr, pInfo->_dwPersist);
					char tmp[512] = { 0 };
					sprintf_s(tmp, "%s_%d", pInfo->_pStr, pInfo->_dwPersist);
					strTemp = tmp;
					pIOCPModel->m_mapHeartBeat[strTemp] = 0x00000000ffffffff;
					//iTime = ::GetTickCount();
					//pIOCPModel->m_mapHeartBeat[strTemp] |= (iTime << 32);
					break;
				}
				case TASK_DISCONNECT:
				{
					if (pIOCPModel->m_pfnNotify->lpDisconnectCB) pIOCPModel->m_pfnNotify->lpDisconnectCB(pIOCPModel->m_pfnNotify->lpCallBackData, std::string(pInfo->_pStr), pInfo->_dwPersist);
					//strTemp.Format("%s_%d", pInfo->_pStr, pInfo->_dwPersist);
					char tmp[512] = { 0 };
					sprintf_s(tmp, "%s_%d", pInfo->_pStr, pInfo->_dwPersist);
					strTemp = tmp;
					pIOCPModel->m_mapHeartBeat[strTemp] = 0x0;
					break;
				}					
				case TASK_REC_ERROR:
					if (pIOCPModel->m_pfnNotify->lpRecvMsgCB) pIOCPModel->m_pfnNotify->lpRecvMsgCB(pIOCPModel->m_pfnNotify->lpCallBackData, nullptr, pInfo->_dwLen, std::string(pInfo->_cIP), pInfo->_dwPersist);
					break;
				case TASK_SND_ERROR:
				{
					//CStringA str;
					//str.Format("ip:%s,port:%d", pInfo->_cIP, pInfo->_dwPersist);
					char str[512] = { 0 };
					sprintf_s(str, "ip:%s,port:%d", pInfo->_cIP, pInfo->_dwPersist);
					if (pIOCPModel->m_pfnNotify->lpErrorSendCB) pIOCPModel->m_pfnNotify->lpErrorSendCB(pIOCPModel->m_pfnNotify->lpCallBackData, std::string(str), atol(pInfo->_pStr));
				}
					break;
				default:
					break;
				}
			}
			delete pInfo;
		}
	}
	SetEvent(pIOCPModel->m_hTaskOver);
	return 0;
}

void CIOCPModel::AddTask(NotifyMSG* info, E_TASK_GRADE eGrade)
{
	CIOCPModel* pThis = (CIOCPModel*)info->_pVoid;
	if (!pThis) return;
	pThis->m_lockTask.lock();
	if (GRADE_LOW == eGrade)
		pThis->m_deqMsg.push_back(info);
	else
		pThis->m_deqMsg.push_front(info);
	pThis->m_lockTask.unlock();
}

void CIOCPModel::ClearTask(void* pVoid)
{
	CIOCPModel* pThis = (CIOCPModel*)pVoid;
	if (!pThis) return;
	pThis->m_lockTask.lock();
	while (pThis->m_deqMsg.size())
	{
		NotifyMSG *pInfo = pThis->m_deqMsg.front();
		delete pInfo;
		pThis->m_deqMsg.pop_front();
	}
	pThis->m_lockTask.unlock();
}

NotifyMSG* CIOCPModel::PopTask(void* pVoid)
{
	CIOCPModel* pThis = (CIOCPModel*)pVoid;
	if (!pThis) return nullptr;
	NotifyMSG *pInfo = NULL;
	pThis->m_lockTask.lock();
	if (pThis->m_deqMsg.size()){
		pInfo = pThis->m_deqMsg.front();
		pThis->m_deqMsg.pop_front();
	}
	pThis->m_lockTask.unlock();
	return pInfo;
}