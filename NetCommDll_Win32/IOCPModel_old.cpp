#include "StdAfx.h"
#include "IOCPModel.h"
#include <atomic>
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

std::mutex             g_lockTask;
std::deque<NotifyMSG*> g_deqMsg;
std::atomic<bool>      g_bStart(false);
CIOCPModel::CIOCPModel(void):
							m_nThreads(0),
							m_hShutdownEvent(NULL),
							m_hIOCompletionPort(NULL),
							m_phWorkerThreads(NULL),
							m_strIP(DEFAULT_IP),
							m_nPort(DEFAULT_PORT),
							m_pMain(NULL),
							m_lpfnAcceptEx( NULL ),
							m_pListenContext( NULL ),
							m_pClinetContext( NULL ),
							m_pfnNotify(nullptr)
{
}


CIOCPModel::~CIOCPModel(void)
{
	// ȷ����Դ�����ͷ�
	//this->Stop();
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

	pIOCPModel->_ShowMessage(_T("�������߳�������ID: %d."),nThreadNo);

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
				}
			}
			break;
		}

		// �ж��Ƿ�����˴���
		if( !bReturn )  
		{  
			DWORD dwErr = GetLastError();
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

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
					pIOCPModel->_ShowMessage(_T("�ͻ��� %s:%d �Ͽ�����."), inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
					CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)), ntohs(pSocketContext->m_ClientAddr.sin_port)));
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
						pIOCPModel->_ShowMessage(_T("�ͻ��� %s:%d �Ͽ�����."), inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
						CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)), ntohs(pSocketContext->m_ClientAddr.sin_port)));
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
				//	{
				//					static int nSize = 0;
				//					nSize += pIoContext->m_wsendBuf.len;
				//					pIOCPModel->_ShowMessage(_T("�ۼƷ���%d�ֽ�"), nSize);
				//					::SetEvent(pIoContext->m_hEvent);
				//	}
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
										//if (pIoContextEx->m_wsendBuf.buf || pIoContextEx->m_wsendBuf.len)
										//{
										//	delete[] pIoContextEx->m_wsendBuf.buf;
										//	pIoContextEx->m_wsendBuf.buf = nullptr;
										//	pIoContextEx->m_wsendBuf.len = 0;
										//}
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

	TRACE(_T("�������߳� %d ���˳�.\n"),nThreadNo);

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
		this->_ShowMessage(_T("��ʼ��WinSock 2.2ʧ�ܣ�\n"));
		return false; 
	}
	m_pfnNotify = pfnBack;
	m_hHanderTask = ::CreateThread(0, 0, _TaskThread, (void *)this, 0, 0);
	m_hEventTask = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_hTaskOver = CreateEvent(NULL, FALSE, FALSE, NULL);
	return true;
}

//////////////////////////////////////////////////////////////////
//	����������
bool CIOCPModel::Start(bool bIsServer, char* pStrIP, int nPort)
{
	// ����ϵͳ�˳����¼�֪ͨ
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_bExit = false;
	CString str(pStrIP);
	m_strIP.Format(L"%s", str);
	m_nPort = nPort;
	m_bIsServer = bIsServer;
	// ��ʼ��IOCP
	if (false == _InitializeIOCP())
	{
		this->_ShowMessage(_T("��ʼ��IOCPʧ�ܣ�\n"));
		return false;
	}
	else
	{
		this->_ShowMessage(_T("\nIOCP��ʼ�����\n."));
	}
	// ��ʼ��Socket
	if (m_bIsServer)
	{
		if (false == _InitializeListenSocket())
		{
			this->_ShowMessage(_T("Listen Socket��ʼ��ʧ�ܣ�\n"));
			this->_DeInitialize();
			return false;
		}
		else
		{
			this->_ShowMessage(_T("Listen Socket��ʼ�����."));
		}
	}
	else
	{
		if (false == _InitializeClientSocket())
		{
			this->_ShowMessage(_T("Client Socket��ʼ��ʧ�ܣ�\n"));

			return false;
		}
		else
		{
			this->_ShowMessage(_T("Client Socket��ʼ�����."));
		}
	}

	this->_ShowMessage(_T("ϵͳ׼���������Ⱥ�����....\n"));

	g_bStart = true;

	Sleep(1000);

	return true;
}


////////////////////////////////////////////////////////////////////
//	��ʼ����ϵͳ�˳���Ϣ���˳���ɶ˿ں��߳���Դ
void CIOCPModel::Stop()
{
	if (!g_bStart) return;
	m_bExit = true;
	g_bStart = false;
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

		this->_ShowMessage(_T("ֹͣ����\n"));
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

		this->_ShowMessage(_T("ֹͣ����\n"));
	}
	CloseHandle(m_hShutdownEvent);
	CIOCPModel::ClearTask();
	Sleep(1000);
}


////////////////////////////////
// ��ʼ����ɶ˿�
bool CIOCPModel::_InitializeIOCP()
{
	// ������һ����ɶ˿�
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0 );
	g_deqMsg.clear();
	if ( NULL == m_hIOCompletionPort)
	{
		this->_ShowMessage(_T("������ɶ˿�ʧ�ܣ��������: %d!\n"), WSAGetLastError());
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
	TRACE(" ���� _WorkerThread %d ��.\n", m_nThreads);
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
		/*this->_ShowMessage*/TRACE(_T("��ʼ��Socketʧ�ܣ��������: %d.\n"), WSAGetLastError());
		return false;
	}
	else
	{
		TRACE("WSASocket() ���.\n");
	}

	// ��Listen Socket������ɶ˿���
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)m_pListenContext, 0))
	{  
		this->_ShowMessage(_T("�� Listen Socket����ɶ˿�ʧ�ܣ��������: %d/n"), WSAGetLastError());
		RELEASE_SOCKET( m_pListenContext->m_Socket );
		return false;
	}
	else
	{
		TRACE(_T("Listen Socket����ɶ˿� ���.\n"));
	}

	// ����ַ��Ϣ
	ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);    


	size_t len = wcslen(m_strIP.GetBuffer(0)) + 1;
	size_t converted = 0;
	char *CStr;
	CStr = (char*)malloc(len*sizeof(char));
	wcstombs_s(&converted, CStr, len, m_strIP.GetBuffer(0), _TRUNCATE);

	ServerAddress.sin_addr.s_addr = inet_addr(CStr);
	ServerAddress.sin_port = htons(m_nPort);                          
	free(CStr);
	// �󶨵�ַ�Ͷ˿�
	if (SOCKET_ERROR == bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress))) 
	{
		this->_ShowMessage(_T("bind()����ִ�д���.\n"));
		return false;
	}
	else
	{
		TRACE("bind() ���.\n");
	}

	// ��ʼ���м���
	if (SOCKET_ERROR == listen(m_pListenContext->m_Socket,SOMAXCONN))
	{
		this->_ShowMessage(_T("Listen()����ִ�г��ִ���.\n"));
		return false;
	}
	else
	{
		TRACE(_T("Listen() ���.\n"));
	}
	//const char chOpt = 1;
	//int nErr = setsockopt(m_pListenContext->m_Socket, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	// ʹ��AcceptEx��������Ϊ���������WinSock2�淶֮���΢�������ṩ����չ����
	// ������Ҫ�����ȡһ�º�����ָ�룬
	// ��ȡAcceptEx����ָ��
	//DWORD dwBytes = 0;  
	//if(SOCKET_ERROR == WSAIoctl(
	//	m_pListenContext->m_Socket, 
	//	SIO_GET_EXTENSION_FUNCTION_POINTER, 
	//	&GuidAcceptEx, 
	//	sizeof(GuidAcceptEx), 
	//	&m_lpfnAcceptEx, 
	//	sizeof(m_lpfnAcceptEx), 
	//	&dwBytes, 
	//	NULL, 
	//	NULL))  
	//{  
	//	this->_ShowMessage(_T("WSAIoctl δ�ܻ�ȡAcceptEx����ָ�롣�������: %d\n"), WSAGetLastError());
	//	this->_DeInitialize();
	//	return false;  
	//}  

	// ��ȡGetAcceptExSockAddrs����ָ�룬Ҳ��ͬ��
	//if(SOCKET_ERROR == WSAIoctl(
	//	m_pListenContext->m_Socket, 
	//	SIO_GET_EXTENSION_FUNCTION_POINTER, 
	//	&GuidGetAcceptExSockAddrs,
	//	sizeof(GuidGetAcceptExSockAddrs), 
	//	&m_lpfnGetAcceptExSockAddrs, 
	//	sizeof(m_lpfnGetAcceptExSockAddrs),   
	//	&dwBytes, 
	//	NULL, 
	//	NULL))  
	//{  
	//	this->_ShowMessage(_T("WSAIoctl δ�ܻ�ȡGuidGetAcceptExSockAddrs����ָ�롣�������: %d\n"), WSAGetLastError());
	//	this->_DeInitialize();
	//	return false; 
	//}  


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

	this->_ShowMessage( _T("Ͷ�� %d ��AcceptEx�������"),MAX_POST_ACCEPT );

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


	size_t len = wcslen(m_strIP.GetBuffer(0)) + 1;
	size_t converted = 0;
	char *CStr;
	CStr = (char*)malloc(len*sizeof(char));
	wcstombs_s(&converted, CStr, len, m_strIP.GetBuffer(0), _TRUNCATE);

	pConnectIoContext.get()->ServerAddress.sin_addr.s_addr = inet_addr(CStr);
	pConnectIoContext.get()->ServerAddress.sin_port = htons(m_nPort);
	free(CStr);

	m_pClinetContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pClinetContext->m_Socket)
	{
		_ShowMessage(_T("��������Client��Socketʧ�ܣ��������: %d"), WSAGetLastError());
		return false;
	}
	//const char chOpt = 1;
	//int nErr = setsockopt(m_pClinetContext->m_Socket, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	////ASSERT(INVALID_SOCKET != m_pClinetContext->m_Socket);

	//// ��Client Socket������ɶ˿���
	//if (NULL == CreateIoCompletionPort((HANDLE)m_pClinetContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)m_pClinetContext, 0))
	//{
	//	this->_ShowMessage(_T("�� Client Socket����ɶ˿�ʧ�ܣ��������: %d/n"), WSAGetLastError());
	//	RELEASE_SOCKET(m_pClinetContext->m_Socket);
	//	return false;
	//}
	//else
	//{
	//	TRACE(_T("Client Socket����ɶ˿� ���.\n"));
	//}

	SOCKADDR_IN local;
	local.sin_family = AF_INET;
	local.sin_addr.S_un.S_addr = INADDR_ANY;
	local.sin_port = 0;
	if (SOCKET_ERROR == bind(m_pClinetContext->m_Socket, (LPSOCKADDR)&local, sizeof(local)))
	{
		this->_ShowMessage(_T("���׽���ʧ��!\r\n"));
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

	this->_ShowMessage(_T("�ͷ���Դ���.\n"));
}

bool CIOCPModel::_ReConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine)
{
	//m_pClinetContext->RemoveContext(pConnectIoContext);
	//std::shared_ptr<PER_IO_CONTEXT> pConnectIoContext1 = m_pClinetContext->GetNewIoContext();
	// ����ַ��Ϣ
	pConnectIoContext->m_lockSend.lock();
	pConnectIoContext->m_bSend = false;
	pConnectIoContext->m_lockSend.unlock();
	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	g_bStart = false;
	size_t len = wcslen(m_strIP.GetBuffer(0)) + 1;
	size_t converted = 0;
	char *CStr;
	CStr = (char*)malloc(len*sizeof(char));
	wcstombs_s(&converted, CStr, len, m_strIP.GetBuffer(0), _TRUNCATE);

	pConnectIoContext->ServerAddress.sin_addr.s_addr = inet_addr(CStr);
	pConnectIoContext->ServerAddress.sin_port = htons(m_nPort);
	free(CStr);
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
			_ShowMessage(_T("��������Client��Socketʧ�ܣ��������: %d"), WSAGetLastError());
			return false;
		}
		const char chOpt = 1;
		//int nErr = setsockopt(pConnectIoContext->m_sockAccept, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
		if (NULL == CreateIoCompletionPort((HANDLE)pConnectIoContext->m_sockAccept, m_hIOCompletionPort, (ULONG_PTR)m_pClinetContext, 0))
		{
			this->_ShowMessage(_T("�� Client Socket����ɶ˿�ʧ�ܣ��������: %d/n"), WSAGetLastError());
			RELEASE_SOCKET(pConnectIoContext->m_sockAccept);
			return false;
		}
		else
		{
			TRACE(_T("Client Socket����ɶ˿� ���.\n"));
		}

		SOCKADDR_IN local;
		local.sin_family = AF_INET;
		local.sin_addr.S_un.S_addr = INADDR_ANY;
		local.sin_port = 0;
		if (SOCKET_ERROR == bind(pConnectIoContext->m_sockAccept, (LPSOCKADDR)&local, sizeof(local)))
		{
			this->_ShowMessage(_T("���׽���ʧ��!\r\n"));
			return false;
		}
	}
	if (SOCKET_ERROR == WSAIoctl(pConnectIoContext->m_sockAccept, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidConnectEx, sizeof(guidConnectEx), &ConnectEx, sizeof(ConnectEx), &dwBytes, NULL, NULL))
	{
		this->_ShowMessage(_T("�õ���չ����ָ��ʧ��!\r\n"));
		return false;
	}
	
	if (!ConnectEx(pConnectIoContext->m_sockAccept, (const sockaddr*)&pConnectIoContext->ServerAddress, sizeof(pConnectIoContext->ServerAddress), szBuffer, strlen(szBuffer), &dwSend, p_ol))
	{
		DWORD dwError = WSAGetLastError();
		if (ERROR_IO_PENDING != dwError)
		{

			this->_ShowMessage(_T("���ӷ�����ʧ��\r\n"));
			return false;
		}
	}
	//DWORD dwFlag = 0, dwTrans;

	//if (!WSAGetOverlappedResult(pConnectIoContext->m_sockAccept, p_ol, &dwTrans, TRUE, &dwFlag))
	//{
	//	this->_ShowMessage(_T("�ȴ��첽���ʧ��\r\n"));
	//	return false;

	//}
	//DWORD dwError = WSAGetLastError();
	// //�������ֵ���󣬲��Ҵ���Ĵ��벢����Pending�Ļ����Ǿ�˵������ص�����ʧ����
	//if (/*(SOCKET_ERROR == nBytesRecv) && */(WSA_IO_PENDING != WSAGetLastError()))
	//{
	//	this->_ShowMessage(_T("Ͷ�ݵ�һ��WSAConnectʧ�ܣ�"));
	//	return false;
	//}
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
		_ShowMessage(_T("��������Accept��Socketʧ�ܣ��������: %d"), WSAGetLastError());
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
			_ShowMessage(_T("Ͷ�� AcceptEx ����ʧ�ܣ��������: %d"), WSAGetLastError());
			return false;  
		}  
	} 
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
	//int len = sizeof(sockaddr_in);
	//int ret = getsockname(pIoContext->m_sockAccept, (sockaddr*)&(pIoContext->ServerAddress), &len);
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
	g_bStart = true;
	CIOCPModel::AddTask(new NotifyMSG(TASK_CONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port), cHostName));
	this->_ShowMessage(_T("���ӳɹ���"));
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
	this->_ShowMessage( _T("�ͻ��� %s:%d ����."), inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port) );
	this->_ShowMessage( _T("�ͻ��� %s:%d ��Ϣ��%s."),inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port),pIoContext->m_wsaBuf.buf );


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
	g_bStart = true;

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

	CIOCPModel::AddTask(new NotifyMSG(TASK_CONNECT, inet_ntoa(ClientAddr->sin_addr), strlen(inet_ntoa(ClientAddr->sin_addr)), ntohs(ClientAddr->sin_port), pIoContext->m_wsaBuf.buf));

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
	BOOL bKeepAlive = TRUE;
	int nErr = ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*)&bKeepAlive, sizeof(bKeepAlive));
	if (nErr == SOCKET_ERROR)
	{
		return false;
	}
	// ����KeepAlive����
	tcp_keepalive alive_in = { 0 };
	tcp_keepalive alive_out = { 0 };
	alive_in.keepalivetime = 1000; // ��ʼ�״�KeepAlive̽��ǰ��TCP�ձ�ʱ��
	alive_in.keepaliveinterval = 500; // ����KeepAlive̽����ʱ����
	alive_in.onoff = TRUE;
	unsigned long ulBytesReturn = 0;
	nErr = WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in),
		&alive_out, sizeof(alive_out), &ulBytesReturn, NULL, NULL);
	if (nErr == SOCKET_ERROR)
	{
		return false;
	}

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
	WSABUF *p_wbuf   = &pIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;

	// ��ʼ����ɺ󣬣�Ͷ��WSARecv����
	int nBytesRecv = WSARecv( pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL );

	// �������ֵ���󣬲��Ҵ���Ĵ��벢����Pending�Ļ����Ǿ�˵������ص�����ʧ����
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		this->_ShowMessage(_T("Ͷ�ݵ�һ��WSARecvʧ�ܣ�"));
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
	this->_ShowMessage( _T("�յ�  %s:%d ��Ϣ��%s"),inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port),pIoContext->m_wsaBuf.buf );
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
		if (sp.get() && g_bStart)
		{
			if (!g_bStart || !pIoContextEx->m_bLine) {
				return FALSE;
			}
			sp.get()->m_OpTypeEx = SEND_POSTED;
			DWORD dwBytesSend = 0;
			//if (sp.get()->m_wsendBuf.buf || sp.get()->m_wsendBuf.len)
			//{
			//	delete[] sp.get()->m_wsendBuf.buf;
			//	sp.get()->m_wsendBuf.buf = nullptr;
			//	sp.get()->m_wsendBuf.len = 0;
			//}
			memset(sp.get()->m_wsendBuf.buf, 0, MAX_BUFFER_LEN);
			//char* str = new char[pInfo->_nLen];
			//memcpy(str, pInfo->_pStr, pInfo->_nLen);
			memcpy(sp.get()->m_wsendBuf.buf, pInfo->_pStr, pInfo->_nLen);
			//sp.get()->m_wsendBuf.buf = str;
			sp.get()->m_wsendBuf.len = pInfo->_nLen;
			int nBytesSend = WSASend(sp.get()->m_sockAccept, &sp.get()->m_wsendBuf, 1, &dwBytesSend, 0, &(sp.get()->m_OverlappedEx), NULL);
			if ((SOCKET_ERROR == nBytesSend) && (WSA_IO_PENDING != WSAGetLastError()))
			{
				char cError[64] = { 0 };
				sprintf_s(cError, "%d", WSAGetLastError());
				CIOCPModel::AddTask(new NotifyMSG(TASK_SND_ERROR, cError, strlen(cError), pInfo->_dwPort, pInfo->_pIP), GRADE_HIGH);
				this->_ShowMessage(_T("Ͷ�ݵ�һ��WSASendʧ�ܣ�"));
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

BOOL CIOCPModel::SendData(DWORD dwType, char* pStr, int nLen, char* pStrIP, int nPort)
{
	std::shared_ptr<_PER_IO_CONTEXT> sp = _GetSocketContext(pStrIP, nPort);
	if (sp.get() && g_bStart)
	{
			static int nSize = 0;
			sp.get()->m_lockSend.lock();
			if (!g_bStart || !sp.get()->m_bLine) {
				sp.get()->m_lockSend.unlock();
				return FALSE;
			}
			if (nLen <= BUF_SIZE)
			{
				//DWORD dwBytesSend = 0;
				//if (sp.get()->m_wsendBuf.buf || sp.get()->m_wsendBuf.len)
				//{
				//	delete[] sp.get()->m_wsendBuf.buf;
				//	sp.get()->m_wsendBuf.buf = nullptr;
				//	sp.get()->m_wsendBuf.len = 0;
				//}
				int len = nLen + sizeof(DWORD)* 2 + 32;
				char* str = new char[len];
				*(DWORD*)(str) = dwType;
				std::string sMd5 = GetMD5Verify(pStr, nLen);
				for (int i = 0; i < sMd5.length() && i < 32; i++)
				{
					*(BYTE*)(str + sizeof(DWORD) + i) = sMd5.at(i);
				}
				*(DWORD*)(str + sizeof(DWORD) + 32) = nLen;
				memcpy(str + sizeof(DWORD)* 2 + 32, pStr, nLen);
				WSABUF wsb;
				wsb.buf = str;
				wsb.len = len;
				sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType));
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
					//DWORD dwBytesSend = 0;
					//if (sp.get()->m_wsendBuf.buf || sp.get()->m_wsendBuf.len)
					//{
					//	delete[] sp.get()->m_wsendBuf.buf;
					//	sp.get()->m_wsendBuf.buf = nullptr;
					//	sp.get()->m_wsendBuf.len = 0;
					//}
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
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType));
						delete[] wsb.buf;
					}
					else if (nCounet - 1 == i && nSurplus)
					{
						char* str = new char[nSurplus];
						memcpy(str, pStr + i*BUF_SIZE, nSurplus);
						WSABUF wsb;
						wsb.buf = str;
						wsb.len = nSurplus;
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType));
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
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType));
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
						sp.get()->m_deqSend.push_back(new _SendData(wsb.buf, wsb.len, pStrIP, nPort, dwType));
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
		TRACE("���ݶ�ȡ�쳣!\n");
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
					TRACE("���ݶ�ȡ�쳣!\n");
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
				TRACE("���ݶ�ȡ�쳣!\n");
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
					TRACE("���ݶ�ȡ�쳣!\n");
					return;
				}
				static int nSize = 0;
				nSize += pContext->nDesLen;
				static char sMsg[1024] = { 0 };
				memset(sMsg, 0, 1024);
				if (m_pMain != NULL)
				{
					sprintf_s(sMsg, "%d", nSize);
					m_pMain->PostMessageW(IOCP_NETWORK_MSG, (WPARAM)sMsg, 1);
				}
				memcpy(pContext->wsBuf.buf + pContext->nCurLen, pStr, nLess);
				bool bFlags = false;
				//pContext->secLock.Lock();
				///*for (int i = 0;i < pNet->nBufSize;i++)
				//{
				//if (IsHeaderOverlapped(pNet->wsBuf.buf,pNet->queBuf[i].buf))
				//{
				//bFlags = true;
				//pNet->queBuf[i].len = 0;
				//delete[] pNet->queBuf[i].buf;
				//break;
				//}
				//}
				//if (bFlags)
				//{
				//ClearOverlapped(pNet);
				//}else */if (pContext->nBufSize >= QUEUE_SIZE){
				//	pContext->queBuf[0].len = 0;
				//	delete[] pContext->queBuf[0].buf;
				//	ClearOverlapped(pContext);
				//}
				//pContext->queBuf[pContext->nBufSize].len = pContext->wsBuf.len;
				//pContext->queBuf[pContext->nBufSize].buf = new char[pContext->wsBuf.len];
				//memcpy(pContext->queBuf[pContext->nBufSize].buf, pContext->wsBuf.buf, pContext->wsBuf.len);
				//pContext->nBufSize++;
				//pContext->secLock.UnLock();
				pContext->nDesLen = 0;
				pContext->nCurLen = 0;
				std::string sDesMd5 = GetMD5Verify(pContext->wsBuf.buf, pContext->wsBuf.len);
				std::string sMd5(pContext->btMD5);
				if (strcmp(sDesMd5.c_str(), sMd5.c_str()) == 0)
					CIOCPModel::AddTask(new NotifyMSG(TASK_DATA, pContext->wsBuf.buf, pContext->wsBuf.len, ntohs(pContext->ServerAddress.sin_port), inet_ntoa(pContext->ServerAddress.sin_addr)));
				else
					CIOCPModel::AddTask(new NotifyMSG(TASK_REC_ERROR, nullptr, pContext->wsBuf.len, ntohs(pContext->ServerAddress.sin_port), inet_ntoa(pContext->ServerAddress.sin_addr)));
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

/////////////////////////////////////////////////////
// �����(Socket)�󶨵���ɶ˿���
bool CIOCPModel::_AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext )
{
	// �����ںͿͻ���ͨ�ŵ�SOCKET�󶨵���ɶ˿���
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)pContext, 0);

	if (NULL == hTemp)
	{
		this->_ShowMessage(_T("ִ��CreateIoCompletionPort()���ִ���.������룺%d"), GetLastError());
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
	m_arrayClientContext.Add(sp);
	m_csContextList.unlock();
}

std::shared_ptr<_PER_IO_CONTEXT> CIOCPModel::_GetSocketContext(char* pStrIP, int nPort)
{
	m_csContextList.lock();
	for (int i = 0; i < m_arrayClientContext.GetCount(); i++)
	{
		std::shared_ptr<_PER_IO_CONTEXT> sp = m_arrayClientContext.GetAt(i).get()->GetIoContext(pStrIP, nPort);
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

	for( int i=0;i<m_arrayClientContext.GetCount();i++ )
	{
		if( pSocketContext==m_arrayClientContext.GetAt(i).get() )
		{
			//	RELEASE(pSocketContext);	
			m_arrayClientContext.RemoveAt(i);			
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

	//for( int i=0;i<m_arrayClientContext.GetCount();i++ )
	//{
	//	delete m_arrayClientContext.GetAt(i);
	//}

	m_arrayClientContext.RemoveAll();

	m_csContextList.unlock();
}



//====================================================================================
//
//				       ����������������
//
//====================================================================================



////////////////////////////////////////////////////////////////////
// ��ñ�����IP��ַ
CString CIOCPModel::GetLocalIP()
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
	m_strIP = CString( inet_ntoa(inAddr) );        

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

/////////////////////////////////////////////////////////////////////
// ������������ʾ��ʾ��Ϣ
void CIOCPModel::_ShowMessage(const CString szFormat,...) const
{
	// ���ݴ���Ĳ�����ʽ���ַ���
	return;
	CString   strMessage;
	va_list   arglist;

	// ����䳤����
	va_start(arglist, szFormat);
	strMessage.FormatV(szFormat,arglist);
	va_end(arglist);
	static char sMsg[1024] = { 0 };
	memset(sMsg, 0, 1024);
	// ������������ʾ
	if( m_pMain!=NULL )
	{	
		CStringA str(strMessage);
		memcpy(sMsg, str.GetBuffer(0), str.GetLength());
		m_pMain->PostMessageW(IOCP_NETWORK_MSG, (WPARAM)sMsg, 0);
	}	
}

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
	Sleep(1000);
	// ����ǳ�ʱ�ˣ����ټ����Ȱ�  
	if(WAIT_TIMEOUT == dwErr)  
	{  	
		// ȷ�Ͽͻ����Ƿ񻹻���...
		if( !_IsSocketAlive( pContext->m_Socket) )
		{
			CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			this->_ShowMessage( _T("��⵽�ͻ����쳣�˳���") );
			if (0 == pIoContext->m_nType){
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
			}
			return true;
		}
		else
		{
			CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			this->_ShowMessage( _T("���������ʱ��������...") );
			if (0 == pIoContext->m_nType) 
			{
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
			}
			return true;
		}
	}  

	// �����ǿͻ����쳣�˳���
	else if( ERROR_NETNAME_DELETED==dwErr || 121 == dwErr || 10053 == dwErr)
	{
		if (pIoContext)
		{
			if (0 == pIoContext->m_nType)
			{
				CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port)));
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
				this->_ShowMessage(_T("��⵽�ͻ����쳣�˳���"));
			}
			else
			{
				CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
				this->_ShowMessage(_T("��⵽������쳣�˳���"));
				this->_RemoveContext(pContext);
			}
		}
		return true;
	}

	else
	{
		if (pIoContext && 0 == pIoContext->m_nType)
		{
			//CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port)));
			if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) {
				this->_ReConnect(pIoContext);
				return true;
			}
		}
		this->_ShowMessage( _T("��ɶ˿ڲ������ִ����߳��˳���������룺%d"),dwErr );
		return false;
	}
}

DWORD WINAPI CIOCPModel::_TaskThread(LPVOID lpParam)
{
	CIOCPModel* pIOCPModel = (CIOCPModel*)lpParam;
	while (TRUE)
	{
		if (WAIT_OBJECT_0 == WaitForSingleObject(pIOCPModel->m_hEventTask, 0))
			break;
		NotifyMSG* pInfo = PopTask();
		if (pInfo)
		{
			if (pIOCPModel->m_pfnNotify)
			{
				switch (pInfo->_eType)
				{
				case TASK_DATA:
					if (pIOCPModel->m_pfnNotify->lpRecvMsgCB) pIOCPModel->m_pfnNotify->lpRecvMsgCB(pIOCPModel->m_pfnNotify->lpCallBackData, (void*)pInfo->_pStr, pInfo->_dwLen, CString(pInfo->_cIP), pInfo->_dwPersist);
					break;
				case  TASK_CONNECT:
					if (pIOCPModel->m_pfnNotify->lpConnectCB) pIOCPModel->m_pfnNotify->lpConnectCB(pIOCPModel->m_pfnNotify->lpCallBackData, CString(pInfo->_pStr), pInfo->_dwPersist, CString(pInfo->_cIP));
					break;
				case TASK_DISCONNECT:
					if (pIOCPModel->m_pfnNotify->lpDisconnectCB) pIOCPModel->m_pfnNotify->lpDisconnectCB(pIOCPModel->m_pfnNotify->lpCallBackData, CString(pInfo->_pStr), pInfo->_dwPersist);
					break;
				case TASK_REC_ERROR:
					if (pIOCPModel->m_pfnNotify->lpRecvMsgCB) pIOCPModel->m_pfnNotify->lpRecvMsgCB(pIOCPModel->m_pfnNotify->lpCallBackData, nullptr, pInfo->_dwLen, CString(pInfo->_cIP), pInfo->_dwPersist);
					break;
				case TASK_SND_ERROR:
				{
					CStringA str;
					str.Format("ip:%s,port:%d", pInfo->_cIP, pInfo->_dwPersist);
					if (pIOCPModel->m_pfnNotify->lpErrorSendCB) pIOCPModel->m_pfnNotify->lpErrorSendCB(pIOCPModel->m_pfnNotify->lpCallBackData, CString(str), atol(pInfo->_pStr));
				}
					break;
				default:
					break;
				}
			}
			delete pInfo;
		}
		Sleep(500);
	}
	SetEvent(pIOCPModel->m_hTaskOver);
	return 0;
}

void CIOCPModel::AddTask(NotifyMSG* info, E_TASK_GRADE eGrade)
{
	g_lockTask.lock();
	if (GRADE_LOW == eGrade)
		g_deqMsg.push_back(info);
	else
		g_deqMsg.push_front(info);
	g_lockTask.unlock();
}

void CIOCPModel::ClearTask()
{
	g_lockTask.lock();
	while (g_deqMsg.size())
	{
		NotifyMSG *pInfo = g_deqMsg.front();
		delete pInfo;
		g_deqMsg.pop_front();
	}
	g_lockTask.unlock();
}

NotifyMSG* CIOCPModel::PopTask()
{
	NotifyMSG *pInfo = NULL;
	g_lockTask.lock();
	if (g_deqMsg.size()){
		pInfo = g_deqMsg.front();
		g_deqMsg.pop_front();
	}
	g_lockTask.unlock();
	return pInfo;
}