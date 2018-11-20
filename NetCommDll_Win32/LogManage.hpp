#pragma once

#include <mutex>
#include <stdarg.h>
#include <fileapi.h>
#include <sysinfoapi.h>


class CLogManage
{
private:
	CLogManage(){}
	~CLogManage(){
		return;
		if (m_bOpen)
		{
			fclose(m_pFile);
//			m_file.Close();
		}
	}
public:
	static CLogManage* GetInstance()
	{
		static CLogManage pub;
		return &pub;
	}

	void WriteLog(const char* szFormat, ...){
		// ���ݴ���Ĳ�����ʽ���ַ���
		return;
//		CStringA   strMessage;
		va_list   arglist;

		// ����䳤����
		va_start(arglist, szFormat);
//		strMessage.FormatV(szFormat, arglist);
		va_end(arglist);
		char tmp[30];//����ʱ����Ϣ�洢λ��
		time_t t = time(0);//ϵͳʱ��
		struct tm t_tm;
		localtime_s(&t_tm, &t);
		strftime(tmp, sizeof(tmp), "%Y-%m-%d %X", &t_tm/*localtime_s(&t)*/); //����-��-�� ʱ:��:�뱣����tmp��
		char sMsg[1024] = { 0 };
		memset(sMsg, 0, 1024);
//		sprintf_s(sMsg, "%s %s\r\n", tmp, strMessage.GetBuffer());
		if (m_bOpen){
			m_mutexfile.lock();
//			m_file.Write(sMsg, strlen(sMsg));
			fwrite(sMsg, strlen(sMsg) + 1, 1, m_pFile);
			m_mutexfile.unlock();
		}
	}
	void CreateLog(const char* dirName, const char* sFileName)
	{
		return;
		if (!m_bOpen) return;
//		CStringA sFile;
		char sFile[256] = { 0 };
		CreateDirectoryA(dirName, NULL);
		char cPath[128] = { 0 };
		char tmp[48] = { 0 };
		time_t t = time(0);
		struct tm t_tm;
		localtime_s(&t_tm, &t);
		strftime(tmp, sizeof(tmp), "%Y-%m-%d", &t_tm);// localtime(&t));
//		sFile.Format("%s\\%s_%s-%ld.txt", dirName, sFileName, tmp, ::GetTickCount());
		sprintf_s(sFile, "%s\\%s_%s-%ld.txt", dirName, sFileName, tmp, ::GetTickCount());
		fopen_s(&m_pFile, sFile, "w+");
		m_bOpen = m_pFile == NULL ? false : true;
		
// 		CString sPathFile(sFile);
// 		m_bOpen = m_file.Open(sPathFile, CFile::modeCreate | CFile::modeWrite | CFile::modeNoTruncate);
// 		if (m_bOpen) m_file.SeekToEnd();
	}
private:
	FILE* m_pFile;
//	CFile                        m_file;
	std::mutex                   m_mutexfile;
	bool                         m_bOpen;
};
#define ILogManage() CLogManage::GetInstance()