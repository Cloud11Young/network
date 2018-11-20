#include "StdAfx.h"
#include "IOCPModel.h"
#include <atomic>
// 每一个处理器上产生多少个线程(为了最大限度的提升服务器性能，详见配套文档)
#define WORKER_THREADS_PER_PROCESSOR 2
// 同时投递的Accept请求的数量(这个要根据实际的情况灵活设置)
#define MAX_POST_ACCEPT              10
// 传递给Worker线程的退出信号
#define EXIT_CODE                    NULL


// 释放指针和句柄资源的宏

// 释放指针宏
#define RELEASE(x)                      {if(x != NULL ){delete x;x=NULL;}}
// 释放句柄宏
#define RELEASE_HANDLE(x)               {if(x != NULL && x!=INVALID_HANDLE_VALUE){ CloseHandle(x);x = NULL;}}
// 释放Socket宏
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
	// 确保资源彻底释放
	//this->Stop();
}




///////////////////////////////////////////////////////////////////
// 工作者线程：  为IOCP请求服务的工作者线程
//         也就是每当完成端口上出现了完成数据包，就将之取出来进行处理的线程
///////////////////////////////////////////////////////////////////

DWORD WINAPI CIOCPModel::_WorkerThread(LPVOID lpParam)
{    
	THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
	CIOCPModel* pIOCPModel = (CIOCPModel*)pParam->pIOCPModel;
	int nThreadNo = (int)pParam->nThreadNo;

	pIOCPModel->_ShowMessage(_T("工作者线程启动，ID: %d."),nThreadNo);

	OVERLAPPED           *pOverlapped = NULL;
	PER_SOCKET_CONTEXT   *pSocketContext = NULL;
	DWORD                dwBytesTransfered = 0;

	// 循环处理请求，知道接收到Shutdown信息为止
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			pIOCPModel->m_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE);

		// 如果收到的是退出标志，则直接退出
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

		// 判断是否出现了错误
		if( !bReturn )  
		{  
			DWORD dwErr = GetLastError();
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

			if (pIoContext && pIoContext->m_OpType >= ACCEPT_POSTED && pIoContext->m_OpType <= NULL_POSTED)
			{
				// 显示一下提示信息
				if (!pIOCPModel->HandleError(pSocketContext, pIoContext, dwErr))
				{
					break;
				}

				continue;
			}
			else
			{
				// 显示一下提示信息
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
			// 判断是否有客户端断开了
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
					pIOCPModel->_ShowMessage(_T("客户端 %s:%d 断开连接."), inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
					CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)), ntohs(pSocketContext->m_ClientAddr.sin_port)));
					if (0 == pIoContext->m_nType){
						if (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0)) pIOCPModel->_ReConnect(pIoContext, true);
					}
					else
						// 释放掉对应的资源
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
						pIOCPModel->_ShowMessage(_T("客户端 %s:%d 断开连接."), inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
						CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)), ntohs(pSocketContext->m_ClientAddr.sin_port)));
						if (0 == pIoContextEx->m_nType){
							if (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0)) pIOCPModel->_ReConnect(pIoContextEx, true);
						}
						else
							// 释放掉对应的资源
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

						// 为了增加代码可读性，这里用专门的_DoAccept函数进行处理连入请求
						pIOCPModel->_DoAccpet( pSocketContext, pIoContext );						

					}
					break;

					// RECV
				case RECV_POSTED:
					{
						// 为了增加代码可读性，这里用专门的_DoRecv函数进行处理接收请求
						pIoContext->m_wsaBuf.len = dwBytesTransfered;
						pIOCPModel->_DoRecv( pSocketContext,pIoContext );
					}
					break;

					// SEND
					// 这里略过不写了，要不代码太多了，不容易理解，Send操作相对来讲简单一些
				case NULL_POSTED:
				//	{
				//					static int nSize = 0;
				//					nSize += pIoContext->m_wsendBuf.len;
				//					pIOCPModel->_ShowMessage(_T("累计发送%d字节"), nSize);
				//					::SetEvent(pIoContext->m_hEvent);
				//	}
					break;
				case CONNECT_POSTED:
				{
									   pIOCPModel->_DoConnect(pSocketContext, pIoContext);
				}
					break;
				default:
					// 读取传入的参数
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

	TRACE(_T("工作者线程 %d 号退出.\n"),nThreadNo);

	// 释放线程参数
	//RELEASE(lpParam);	

	return 0;
}



//====================================================================================
//
//				    系统初始化和终止
//
//====================================================================================




////////////////////////////////////////////////////////////////////
// 初始化WinSock 2.2
bool CIOCPModel::LoadSocketLib(PUSER_CB_IOCP pfnBack)
{    
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	// 错误(一般都不可能出现)
	if (NO_ERROR != nResult)
	{
		this->_ShowMessage(_T("初始化WinSock 2.2失败！\n"));
		return false; 
	}
	m_pfnNotify = pfnBack;
	m_hHanderTask = ::CreateThread(0, 0, _TaskThread, (void *)this, 0, 0);
	m_hEventTask = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_hTaskOver = CreateEvent(NULL, FALSE, FALSE, NULL);
	return true;
}

//////////////////////////////////////////////////////////////////
//	启动服务器
bool CIOCPModel::Start(bool bIsServer, char* pStrIP, int nPort)
{
	// 建立系统退出的事件通知
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_bExit = false;
	CString str(pStrIP);
	m_strIP.Format(L"%s", str);
	m_nPort = nPort;
	m_bIsServer = bIsServer;
	// 初始化IOCP
	if (false == _InitializeIOCP())
	{
		this->_ShowMessage(_T("初始化IOCP失败！\n"));
		return false;
	}
	else
	{
		this->_ShowMessage(_T("\nIOCP初始化完毕\n."));
	}
	// 初始化Socket
	if (m_bIsServer)
	{
		if (false == _InitializeListenSocket())
		{
			this->_ShowMessage(_T("Listen Socket初始化失败！\n"));
			this->_DeInitialize();
			return false;
		}
		else
		{
			this->_ShowMessage(_T("Listen Socket初始化完毕."));
		}
	}
	else
	{
		if (false == _InitializeClientSocket())
		{
			this->_ShowMessage(_T("Client Socket初始化失败！\n"));

			return false;
		}
		else
		{
			this->_ShowMessage(_T("Client Socket初始化完毕."));
		}
	}

	this->_ShowMessage(_T("系统准备就绪，等候连接....\n"));

	g_bStart = true;

	Sleep(1000);

	return true;
}


////////////////////////////////////////////////////////////////////
//	开始发送系统退出消息，退出完成端口和线程资源
void CIOCPModel::Stop()
{
	if (!g_bStart) return;
	m_bExit = true;
	g_bStart = false;
	if( m_pListenContext!=NULL && m_pListenContext->m_Socket!=INVALID_SOCKET )
	{
		// 激活关闭消息通知
		SetEvent(m_hShutdownEvent);

		for (int i = 0; i < m_nThreads; i++)
		{
			// 通知所有的完成端口操作退出
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}

		// 等待所有的客户端资源退出
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

		// 清除客户端列表信息
		this->_ClearContextList();

		// 释放其他资源
		this->_DeInitialize();

		this->_ShowMessage(_T("停止监听\n"));
	}	

	if (m_pClinetContext != NULL)
	{
		// 激活关闭消息通知
		SetEvent(m_hShutdownEvent);

		for (int i = 0; i < m_nThreads; i++)
		{
			// 通知所有的完成端口操作退出
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}

		// 等待所有的客户端资源退出
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

		// 清除客户端列表信息
		this->_ClearContextList();

		// 释放其他资源
		this->_DeInitialize();

		this->_ShowMessage(_T("停止连接\n"));
	}
	CloseHandle(m_hShutdownEvent);
	CIOCPModel::ClearTask();
	Sleep(1000);
}


////////////////////////////////
// 初始化完成端口
bool CIOCPModel::_InitializeIOCP()
{
	// 建立第一个完成端口
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0 );
	g_deqMsg.clear();
	if ( NULL == m_hIOCompletionPort)
	{
		this->_ShowMessage(_T("建立完成端口失败！错误代码: %d!\n"), WSAGetLastError());
		return false;
	}
	// 根据本机中的处理器数量，建立对应的线程数
	if (m_bIsServer)
		m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();
	else
		m_nThreads = WORKER_THREADS_PER_PROCESSOR;
	// 为工作者线程初始化句柄
	m_phWorkerThreads = new HANDLE[m_nThreads];

	// 根据计算出来的数量建立工作者线程
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; i++)
	{
		THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
		pThreadParams->pIOCPModel = this;
		pThreadParams->nThreadNo = i + 1;
		m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
	}
	m_bRelase = true;
	TRACE(" 建立 _WorkerThread %d 个.\n", m_nThreads);
	return true;
}


/////////////////////////////////////////////////////////////////
// 初始化Socket
bool CIOCPModel::_InitializeListenSocket()
{
	// AcceptEx 和 GetAcceptExSockaddrs 的GUID，用于导出函数指针
	GUID GuidAcceptEx = WSAID_ACCEPTEX;  
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS; 

	// 服务器地址信息，用于绑定Socket
	struct sockaddr_in ServerAddress;

	// 生成用于监听的Socket的信息
	m_pListenContext = new PER_SOCKET_CONTEXT;

	// 需要使用重叠IO，必须得使用WSASocket来建立Socket，才可以支持重叠IO操作
	m_pListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_Socket) 
	{
		/*this->_ShowMessage*/TRACE(_T("初始化Socket失败，错误代码: %d.\n"), WSAGetLastError());
		return false;
	}
	else
	{
		TRACE("WSASocket() 完成.\n");
	}

	// 将Listen Socket绑定至完成端口中
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)m_pListenContext, 0))
	{  
		this->_ShowMessage(_T("绑定 Listen Socket至完成端口失败！错误代码: %d/n"), WSAGetLastError());
		RELEASE_SOCKET( m_pListenContext->m_Socket );
		return false;
	}
	else
	{
		TRACE(_T("Listen Socket绑定完成端口 完成.\n"));
	}

	// 填充地址信息
	ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	// 这里可以绑定任何可用的IP地址，或者绑定一个指定的IP地址 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);    


	size_t len = wcslen(m_strIP.GetBuffer(0)) + 1;
	size_t converted = 0;
	char *CStr;
	CStr = (char*)malloc(len*sizeof(char));
	wcstombs_s(&converted, CStr, len, m_strIP.GetBuffer(0), _TRUNCATE);

	ServerAddress.sin_addr.s_addr = inet_addr(CStr);
	ServerAddress.sin_port = htons(m_nPort);                          
	free(CStr);
	// 绑定地址和端口
	if (SOCKET_ERROR == bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress))) 
	{
		this->_ShowMessage(_T("bind()函数执行错误.\n"));
		return false;
	}
	else
	{
		TRACE("bind() 完成.\n");
	}

	// 开始进行监听
	if (SOCKET_ERROR == listen(m_pListenContext->m_Socket,SOMAXCONN))
	{
		this->_ShowMessage(_T("Listen()函数执行出现错误.\n"));
		return false;
	}
	else
	{
		TRACE(_T("Listen() 完成.\n"));
	}
	//const char chOpt = 1;
	//int nErr = setsockopt(m_pListenContext->m_Socket, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	// 使用AcceptEx函数，因为这个是属于WinSock2规范之外的微软另外提供的扩展函数
	// 所以需要额外获取一下函数的指针，
	// 获取AcceptEx函数指针
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
	//	this->_ShowMessage(_T("WSAIoctl 未能获取AcceptEx函数指针。错误代码: %d\n"), WSAGetLastError());
	//	this->_DeInitialize();
	//	return false;  
	//}  

	// 获取GetAcceptExSockAddrs函数指针，也是同理
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
	//	this->_ShowMessage(_T("WSAIoctl 未能获取GuidGetAcceptExSockAddrs函数指针。错误代码: %d\n"), WSAGetLastError());
	//	this->_DeInitialize();
	//	return false; 
	//}  


	// 为AcceptEx 准备参数，然后投递AcceptEx I/O请求
	for( int i=0;i<MAX_POST_ACCEPT;i++ )
	{
		// 新建一个IO_CONTEXT
		std::shared_ptr<PER_IO_CONTEXT> pAcceptIoContext = m_pListenContext->GetNewIoContext();

		if( false==this->_PostAccept( pAcceptIoContext.get() ) )
		{
			m_pListenContext->RemoveContext(pAcceptIoContext.get());
			return false;
		}
	}

	this->_ShowMessage( _T("投递 %d 个AcceptEx请求完毕"),MAX_POST_ACCEPT );

	return true;
}

/////////////////////////////////////////////////////////////////
// 初始化Socket
bool CIOCPModel::_InitializeClientSocket()
{
	// AcceptEx 和 GetAcceptExSockaddrs 的GUID，用于导出函数指针
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

	// 服务器地址信息，用于绑定Socket
	

	// 生成用于监听的Socket的信息
	m_pClinetContext = new PER_SOCKET_CONTEXT;

	std::shared_ptr<PER_IO_CONTEXT> pConnectIoContext = m_pClinetContext->GetNewIoContext();
	// 填充地址信息

	// 这里可以绑定任何可用的IP地址，或者绑定一个指定的IP地址 
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
		_ShowMessage(_T("创建用于Client的Socket失败！错误代码: %d"), WSAGetLastError());
		return false;
	}
	//const char chOpt = 1;
	//int nErr = setsockopt(m_pClinetContext->m_Socket, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	////ASSERT(INVALID_SOCKET != m_pClinetContext->m_Socket);

	//// 将Client Socket绑定至完成端口中
	//if (NULL == CreateIoCompletionPort((HANDLE)m_pClinetContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)m_pClinetContext, 0))
	//{
	//	this->_ShowMessage(_T("绑定 Client Socket至完成端口失败！错误代码: %d/n"), WSAGetLastError());
	//	RELEASE_SOCKET(m_pClinetContext->m_Socket);
	//	return false;
	//}
	//else
	//{
	//	TRACE(_T("Client Socket绑定完成端口 完成.\n"));
	//}

	SOCKADDR_IN local;
	local.sin_family = AF_INET;
	local.sin_addr.S_un.S_addr = INADDR_ANY;
	local.sin_port = 0;
	if (SOCKET_ERROR == bind(m_pClinetContext->m_Socket, (LPSOCKADDR)&local, sizeof(local)))
	{
		this->_ShowMessage(_T("绑定套接字失败!\r\n"));
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
//	最后释放掉所有资源
void CIOCPModel::_DeInitialize()
{
	// 关闭系统退出事件句柄
	RELEASE_HANDLE(m_hShutdownEvent);

	// 释放工作者线程句柄指针
	for( int i=0;i<m_nThreads;i++ )
	{
		if (m_bRelase)
			RELEASE_HANDLE(m_phWorkerThreads[i]);
	}
	m_bRelase = false;
	RELEASE(m_phWorkerThreads);

	// 关闭IOCP句柄
	RELEASE_HANDLE(m_hIOCompletionPort);

	// 关闭监听Socket
	RELEASE(m_pListenContext);

	//RELEASE(m_pClinetContext);

	this->_ShowMessage(_T("释放资源完毕.\n"));
}

bool CIOCPModel::_ReConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine)
{
	//m_pClinetContext->RemoveContext(pConnectIoContext);
	//std::shared_ptr<PER_IO_CONTEXT> pConnectIoContext1 = m_pClinetContext->GetNewIoContext();
	// 填充地址信息
	pConnectIoContext->m_lockSend.lock();
	pConnectIoContext->m_bSend = false;
	pConnectIoContext->m_lockSend.unlock();
	// 这里可以绑定任何可用的IP地址，或者绑定一个指定的IP地址 
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
//				    投递完成端口请求
//
//====================================================================================

bool CIOCPModel::_PostConnect(PER_IO_CONTEXT* pConnectIoContext, bool bReOnLine)
{
	// 准备参数
	DWORD dwSend = 0;
	char szBuffer[256] = {0};
	gethostname(szBuffer, 256);
	pConnectIoContext->m_OpType = CONNECT_POSTED;
	WSABUF *p_wbuf = &pConnectIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pConnectIoContext->m_Overlapped;
	ZeroMemory(p_ol, sizeof(OVERLAPPED));
	// 初始化完成后，，投递WSARecv请求
	LPFN_CONNECTEX ConnectEx;
	DWORD dwBytes = 0;
	GUID guidConnectEx = WSAID_CONNECTEX;
	pConnectIoContext->m_nType = 0;
	// 为以后新连入的客户端先准备好Socket( 这个是与传统accept最大的区别 ) 
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
			_ShowMessage(_T("创建用于Client的Socket失败！错误代码: %d"), WSAGetLastError());
			return false;
		}
		const char chOpt = 1;
		//int nErr = setsockopt(pConnectIoContext->m_sockAccept, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
		if (NULL == CreateIoCompletionPort((HANDLE)pConnectIoContext->m_sockAccept, m_hIOCompletionPort, (ULONG_PTR)m_pClinetContext, 0))
		{
			this->_ShowMessage(_T("绑定 Client Socket至完成端口失败！错误代码: %d/n"), WSAGetLastError());
			RELEASE_SOCKET(pConnectIoContext->m_sockAccept);
			return false;
		}
		else
		{
			TRACE(_T("Client Socket绑定完成端口 完成.\n"));
		}

		SOCKADDR_IN local;
		local.sin_family = AF_INET;
		local.sin_addr.S_un.S_addr = INADDR_ANY;
		local.sin_port = 0;
		if (SOCKET_ERROR == bind(pConnectIoContext->m_sockAccept, (LPSOCKADDR)&local, sizeof(local)))
		{
			this->_ShowMessage(_T("绑定套接字失败!\r\n"));
			return false;
		}
	}
	if (SOCKET_ERROR == WSAIoctl(pConnectIoContext->m_sockAccept, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidConnectEx, sizeof(guidConnectEx), &ConnectEx, sizeof(ConnectEx), &dwBytes, NULL, NULL))
	{
		this->_ShowMessage(_T("得到扩展函数指针失败!\r\n"));
		return false;
	}
	
	if (!ConnectEx(pConnectIoContext->m_sockAccept, (const sockaddr*)&pConnectIoContext->ServerAddress, sizeof(pConnectIoContext->ServerAddress), szBuffer, strlen(szBuffer), &dwSend, p_ol))
	{
		DWORD dwError = WSAGetLastError();
		if (ERROR_IO_PENDING != dwError)
		{

			this->_ShowMessage(_T("连接服务器失败\r\n"));
			return false;
		}
	}
	//DWORD dwFlag = 0, dwTrans;

	//if (!WSAGetOverlappedResult(pConnectIoContext->m_sockAccept, p_ol, &dwTrans, TRUE, &dwFlag))
	//{
	//	this->_ShowMessage(_T("等待异步结果失败\r\n"));
	//	return false;

	//}
	//DWORD dwError = WSAGetLastError();
	// //如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
	//if (/*(SOCKET_ERROR == nBytesRecv) && */(WSA_IO_PENDING != WSAGetLastError()))
	//{
	//	this->_ShowMessage(_T("投递第一个WSAConnect失败！"));
	//	return false;
	//}
	return true;
}

//////////////////////////////////////////////////////////////////
// 投递Accept请求
bool CIOCPModel::_PostAccept( PER_IO_CONTEXT* pAcceptIoContext )
{
	ASSERT( INVALID_SOCKET!=m_pListenContext->m_Socket );
	// 准备参数
	DWORD dwBytes = 0;  
	pAcceptIoContext->m_OpType = ACCEPT_POSTED;  
	WSABUF *p_wbuf   = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pAcceptIoContext->m_Overlapped;
	pAcceptIoContext->m_nType = 1;
	// 为以后新连入的客户端先准备好Socket( 这个是与传统accept最大的区别 ) 
	pAcceptIoContext->m_sockAccept  = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);  
	if( INVALID_SOCKET==pAcceptIoContext->m_sockAccept )  
	{  
		_ShowMessage(_T("创建用于Accept的Socket失败！错误代码: %d"), WSAGetLastError());
		return false;  
	} 
	const char chOpt = 1;
	//int nErr = setsockopt(pAcceptIoContext->m_sockAccept, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
	// 投递AcceptEx
	//if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN)+16) * 2),
	//	sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, &dwBytes, p_ol))
	if (FALSE == AcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN)+16) * 2),
		sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, &dwBytes, p_ol))
	{  
		if(WSA_IO_PENDING != WSAGetLastError())  
		{  
			_ShowMessage(_T("投递 AcceptEx 请求失败，错误代码: %d"), WSAGetLastError());
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
	// 3. 继续，建立其下的IoContext，用于在这个Socket上投递第一个Recv数据请求
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
	// 4. 如果投递成功，那么就把这个有效的客户端信息，加入到ContextList中去(需要统一管理，方便释放资源)
	//this->_AddToContextList(pSocketContext);
	g_bStart = true;
	CIOCPModel::AddTask(new NotifyMSG(TASK_CONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port), cHostName));
	this->_ShowMessage(_T("连接成功！"));
	pIoContext->m_bLine = true;
}
////////////////////////////////////////////////////////////
// 在有客户端连入的时候，进行处理
// 流程有点复杂，你要是看不懂的话，就看配套的文档吧....
// 如果能理解这里的话，完成端口的机制你就消化了一大半了

// 总之你要知道，传入的是ListenSocket的Context，我们需要复制一份出来给新连入的Socket用
// 原来的Context还是要在上面继续投递下一个Accept请求
//
bool CIOCPModel::_DoAccpet( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext )
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* LocalAddr = NULL;  
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);  
	///////////////////////////////////////////////////////////////////////////
	// 1. 首先取得连入客户端的地址信息
	// 这个 m_lpfnGetAcceptExSockAddrs 不得了啊~~~~~~
	// 不但可以取得客户端和本地端的地址信息，还能顺便取出客户端发来的第一组数据，老强大了...
	//this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN)+16)*2),  
	//	sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);  
	GetAcceptExSockaddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN)+16) * 2),
		sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);
	this->_ShowMessage( _T("客户端 %s:%d 连入."), inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port) );
	this->_ShowMessage( _T("客户额 %s:%d 信息：%s."),inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port),pIoContext->m_wsaBuf.buf );


	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// 2. 这里需要注意，这里传入的这个是ListenSocket上的Context，这个Context我们还需要用于监听下一个连接
	// 所以我还得要将ListenSocket上的Context复制出来一份为新连入的Socket新建一个SocketContext

	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_Socket           = pIoContext->m_sockAccept;
	memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));
	
	// 参数设置完毕，将这个Socket和完成端口绑定(这也是一个关键步骤)
	if( false==this->_AssociateWithIOCP( pNewSocketContext ) )
	{
		RELEASE( pNewSocketContext );
		return false;
	}  
	g_bStart = true;

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// 3. 继续，建立其下的IoContext，用于在这个Socket上投递第一个Recv数据请求
	std::shared_ptr<PER_IO_CONTEXT> pNewIoContext = pNewSocketContext->GetNewIoContext();
	pNewIoContext.get()->m_OpType       = RECV_POSTED;
	pNewIoContext.get()->m_sockAccept   = pNewSocketContext->m_Socket;
	pNewIoContext.get()->m_nType = 1;
	if (!this->_SetSocketOpt(pNewIoContext.get()->m_sockAccept)){
		return false;
	}
	// 如果Buffer需要保留，就自己拷贝一份出来
	//memcpy( pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN );
	memcpy(&(pNewIoContext.get()->ServerAddress), ClientAddr, sizeof(SOCKADDR_IN));
	// 绑定完毕之后，就可以开始在这个Socket上投递完成请求了
	if( false==this->_PostRecv( pNewIoContext.get()) )
	{
		pNewSocketContext->RemoveContext( pNewIoContext.get() );
		return false;
	}

	CIOCPModel::AddTask(new NotifyMSG(TASK_CONNECT, inet_ntoa(ClientAddr->sin_addr), strlen(inet_ntoa(ClientAddr->sin_addr)), ntohs(ClientAddr->sin_port), pIoContext->m_wsaBuf.buf));

	/////////////////////////////////////////////////////////////////////////////////////////////////
	// 4. 如果投递成功，那么就把这个有效的客户端信息，加入到ContextList中去(需要统一管理，方便释放资源)
	this->_AddToContextList( pNewSocketContext );

	////////////////////////////////////////////////////////////////////////////////////////////////
	// 5. 使用完毕之后，把Listen Socket的那个IoContext重置，然后准备投递新的AcceptEx
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
	// 设置KeepAlive参数
	tcp_keepalive alive_in = { 0 };
	tcp_keepalive alive_out = { 0 };
	alive_in.keepalivetime = 1000; // 开始首次KeepAlive探测前的TCP空闭时间
	alive_in.keepaliveinterval = 500; // 两次KeepAlive探测间的时间间隔
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
// 投递接收数据请求
bool CIOCPModel::_PostRecv( PER_IO_CONTEXT* pIoContext )
{
	// 初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	pIoContext->m_wsaBuf.len = MAX_BUFFER_LEN;
	WSABUF *p_wbuf   = &pIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;

	// 初始化完成后，，投递WSARecv请求
	int nBytesRecv = WSARecv( pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL );

	// 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		this->_ShowMessage(_T("投递第一个WSARecv失败！"));
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
// 在有接收的数据到达的时候，进行处理
bool CIOCPModel::_DoRecv( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext )
{
	// 先把上一次的数据显示出现，然后就重置状态，发出下一个Recv请求
	SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
	this->_ShowMessage( _T("收到  %s:%d 信息：%s"),inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port),pIoContext->m_wsaBuf.buf );
	Save(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len, pIoContext);
	// 然后开始投递下一个WSARecv请求
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
				this->_ShowMessage(_T("投递第一个WSASend失败！"));
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
		TRACE("数据读取异常!\n");
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
					TRACE("数据读取异常!\n");
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
				TRACE("数据读取异常!\n");
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
					TRACE("数据读取异常!\n");
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
// 将句柄(Socket)绑定到完成端口中
bool CIOCPModel::_AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext )
{
	// 将用于和客户端通信的SOCKET绑定到完成端口中
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR)pContext, 0);

	if (NULL == hTemp)
	{
		this->_ShowMessage(_T("执行CreateIoCompletionPort()出现错误.错误代码：%d"), GetLastError());
		return false;
	}

	return true;
}




//====================================================================================
//
//				    ContextList 相关操作
//
//====================================================================================


//////////////////////////////////////////////////////////////
// 将客户端的相关信息存储到数组中
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
//	移除某个特定的Context
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
// 清空客户端信息
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
//				       其他辅助函数定义
//
//====================================================================================



////////////////////////////////////////////////////////////////////
// 获得本机的IP地址
CString CIOCPModel::GetLocalIP()
{
	// 获得本机主机名
	char hostname[MAX_PATH] = {0};
	gethostname(hostname,MAX_PATH);                
	struct hostent FAR* lpHostEnt = gethostbyname(hostname);
	if(lpHostEnt == NULL)
	{
		return DEFAULT_IP;
	}

	// 取得IP地址列表中的第一个为返回的IP(因为一台主机可能会绑定多个IP)
	LPSTR lpAddr = lpHostEnt->h_addr_list[0];      

	// 将IP地址转化成字符串形式
	struct in_addr inAddr;
	memmove(&inAddr,lpAddr,4);
	m_strIP = CString( inet_ntoa(inAddr) );        

	return m_strIP;
}

///////////////////////////////////////////////////////////////////
// 获得本机中处理器的数量
int CIOCPModel::_GetNoOfProcessors()
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	return si.dwNumberOfProcessors;
}

/////////////////////////////////////////////////////////////////////
// 在主界面中显示提示信息
void CIOCPModel::_ShowMessage(const CString szFormat,...) const
{
	// 根据传入的参数格式化字符串
	return;
	CString   strMessage;
	va_list   arglist;

	// 处理变长参数
	va_start(arglist, szFormat);
	strMessage.FormatV(szFormat,arglist);
	va_end(arglist);
	static char sMsg[1024] = { 0 };
	memset(sMsg, 0, 1024);
	// 在主界面中显示
	if( m_pMain!=NULL )
	{	
		CStringA str(strMessage);
		memcpy(sMsg, str.GetBuffer(0), str.GetLength());
		m_pMain->PostMessageW(IOCP_NETWORK_MSG, (WPARAM)sMsg, 0);
	}	
}

/////////////////////////////////////////////////////////////////////
// 判断客户端Socket是否已经断开，否则在一个无效的Socket上投递WSARecv操作会出现异常
// 使用的方法是尝试向这个socket发送数据，判断这个socket调用的返回值
// 因为如果客户端网络异常断开(例如客户端崩溃或者拔掉网线等)的时候，服务器端是无法收到客户端断开的通知的

bool CIOCPModel::_IsSocketAlive(SOCKET s)
{
	int nByteSent=send(s,"",0,0);
	if (-1 == nByteSent) return false;
	return true;
}

///////////////////////////////////////////////////////////////////
// 显示并处理完成端口上的错误
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
	// 如果是超时了，就再继续等吧  
	if(WAIT_TIMEOUT == dwErr)  
	{  	
		// 确认客户端是否还活着...
		if( !_IsSocketAlive( pContext->m_Socket) )
		{
			CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			this->_ShowMessage( _T("检测到客户端异常退出！") );
			if (0 == pIoContext->m_nType){
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
			}
			return true;
		}
		else
		{
			CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
			this->_ShowMessage( _T("网络操作超时！重试中...") );
			if (0 == pIoContext->m_nType) 
			{
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
			}
			return true;
		}
	}  

	// 可能是客户端异常退出了
	else if( ERROR_NETNAME_DELETED==dwErr || 121 == dwErr || 10053 == dwErr)
	{
		if (pIoContext)
		{
			if (0 == pIoContext->m_nType)
			{
				CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pIoContext->ServerAddress.sin_addr), strlen(inet_ntoa(pIoContext->ServerAddress.sin_addr)), ntohs(pIoContext->ServerAddress.sin_port)));
				if (WAIT_OBJECT_0 != WaitForSingleObject(this->m_hShutdownEvent, 0)) this->_ReConnect(pIoContext, true);
				this->_ShowMessage(_T("检测到客户端异常退出！"));
			}
			else
			{
				CIOCPModel::AddTask(new NotifyMSG(TASK_DISCONNECT, inet_ntoa(pContext->m_ClientAddr.sin_addr), strlen(inet_ntoa(pContext->m_ClientAddr.sin_addr)), ntohs(pContext->m_ClientAddr.sin_port)));
				this->_ShowMessage(_T("检测到服务端异常退出！"));
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
		this->_ShowMessage( _T("完成端口操作出现错误，线程退出。错误代码：%d"),dwErr );
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