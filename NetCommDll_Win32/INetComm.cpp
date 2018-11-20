#include "stdafx.h"
#include "INetComm.h"
#include "NetWork.h"

BOOL INetComm::CreateInstance(INetComm **ppINetComm)
{
	if (*ppINetComm == NULL)
	{
		*ppINetComm = new CNetWork;
	}
	return TRUE;
}