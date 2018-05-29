
#include "obs-app.hpp"
#include "FastSession.h"

void long2buff(int64_t n, char *buff)
{
	unsigned char *p;
	p = (unsigned char *)buff;
	*p++ = (n >> 56) & 0xFF;
	*p++ = (n >> 48) & 0xFF;
	*p++ = (n >> 40) & 0xFF;
	*p++ = (n >> 32) & 0xFF;
	*p++ = (n >> 24) & 0xFF;
	*p++ = (n >> 16) & 0xFF;
	*p++ = (n >> 8) & 0xFF;
	*p++ = n & 0xFF;
}

int64_t buff2long(const char *buff)
{
	unsigned char *p;
	p = (unsigned char *)buff;
	return  (((int64_t)(*p)) << 56) | \
		(((int64_t)(*(p + 1))) << 48) | \
		(((int64_t)(*(p + 2))) << 40) | \
		(((int64_t)(*(p + 3))) << 32) | \
		(((int64_t)(*(p + 4))) << 24) | \
		(((int64_t)(*(p + 5))) << 16) | \
		(((int64_t)(*(p + 6))) << 8) | \
		((int64_t)(*(p + 7)));
}

CFastSession::CFastSession()
  : m_TCPSocket(NULL)
  , m_nPort(0)
{
}

CFastSession::~CFastSession()
{
	this->closeSocket();
}

void CFastSession::closeSocket()
{
	// 这里必须同时先调用close()再调用disconnect，否则有内存泄漏...
	if (m_TCPSocket != NULL) {
		m_TCPSocket->close();
		m_TCPSocket->disconnectFromHost();
		delete m_TCPSocket;
		m_TCPSocket = NULL;
	}
}

bool CFastSession::InitSession(const char * lpszAddr, int nPort)
{
	if (m_TCPSocket != NULL) {
		delete m_TCPSocket;
		m_TCPSocket = NULL;
	}
	m_nPort = nPort;
	m_strAddress.assign(lpszAddr);
	m_TCPSocket = new QTcpSocket();
	connect(m_TCPSocket, SIGNAL(connected()), this, SLOT(onConnected()));
	connect(m_TCPSocket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
	connect(m_TCPSocket, SIGNAL(disconnected()), this, SLOT(onDisConnected()));
	connect(m_TCPSocket, SIGNAL(bytesWritten(qint64)), this, SLOT(onBytesWritten(qint64)));
	connect(m_TCPSocket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onError(QAbstractSocket::SocketError)));
	m_TCPSocket->connectToHost(QString::fromUtf8(lpszAddr), nPort);
	return true;
}

CTrackerSession::CTrackerSession()
{
	m_nGroupCount = 0;
	m_lpGroupStat = NULL;
	m_bIsConnected = false;
	memset(&m_NewStorage, 0, sizeof(m_NewStorage));
	memset(&m_TrackerCmd, 0, sizeof(m_TrackerCmd));
}

CTrackerSession::~CTrackerSession()
{
	// 先关闭套接字...
	this->closeSocket();
	// 再关闭分配的对象...
	if (m_lpGroupStat != NULL) {
		delete[] m_lpGroupStat;
		m_lpGroupStat = NULL;
		m_nGroupCount = 0;
	}
}

void CTrackerSession::SendCmd(char inCmd)
{
	int nSendSize = sizeof(m_TrackerCmd);
	memset(&m_TrackerCmd, 0, nSendSize);
	m_TrackerCmd.cmd = inCmd;
	m_TCPSocket->write((char*)&m_TrackerCmd, nSendSize);
}

void CTrackerSession::onConnected()
{
	this->SendCmd(TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITHOUT_GROUP_ONE);
}

void CTrackerSession::onReadyRead()
{
	// 从网络层读取所有的缓冲区，并将缓冲区连接起来...
	QByteArray theBuffer = m_TCPSocket->readAll();
	m_strRecv.append(theBuffer.toStdString());
	// 得到的数据长度不够，直接返回，等待新数据...
	int nCmdLength = sizeof(TrackerHeader);
	if (m_strRecv.size() < nCmdLength)
		return;
	// 对命令头进行分发处理...
	if (m_TrackerCmd.cmd == TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITHOUT_GROUP_ONE) {
		// 得到的数据长度不够，直接报错，等待新的链接接入，Storage上线后不会主动汇报...
		if (m_strRecv.size() < (nCmdLength + TRACKER_QUERY_STORAGE_STORE_BODY_LEN))
			return;
		// 对获取的数据进行转移处理...
		char * in_buff = (char*)m_strRecv.c_str() + nCmdLength;
		memset(&m_NewStorage, 0, sizeof(m_NewStorage));
		memcpy(m_NewStorage.group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
		memcpy(m_NewStorage.ip_addr, in_buff + FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE - 1);
		m_NewStorage.port = (int)buff2long(in_buff + FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1);
		m_NewStorage.store_path_index = (int)(*(in_buff + FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1 + FDFS_PROTO_PKG_LEN_SIZE)); // 内存中只有一个字节...
		blog(LOG_INFO, "Group = %s, Storage = %s:%d, PathIndex = %d", m_NewStorage.group_name, m_NewStorage.ip_addr, m_NewStorage.port, m_NewStorage.store_path_index);
		// 将缓冲区进行减少处理...
		m_strRecv.erase(0, nCmdLength + TRACKER_QUERY_STORAGE_STORE_BODY_LEN);
		// 发起新的命令 => 查询所有的组列表...
		return this->SendCmd(TRACKER_PROTO_CMD_SERVER_LIST_ALL_GROUPS);
	} else if (m_TrackerCmd.cmd == TRACKER_PROTO_CMD_SERVER_LIST_ALL_GROUPS) {
		// 得到的数据长度不够，直接返回，等待新数据...
		int in_bytes = m_strRecv.size() - nCmdLength;
		if (in_bytes % sizeof(TrackerGroupStat) != 0)
			return;
		m_nGroupCount = in_bytes / sizeof(TrackerGroupStat);
		m_lpGroupStat = new FDFSGroupStat[m_nGroupCount];
		memset(m_lpGroupStat, 0, sizeof(FDFSGroupStat) * m_nGroupCount);
		TrackerGroupStat * pSrc = (TrackerGroupStat*)(m_strRecv.c_str() + nCmdLength);
		TrackerGroupStat * pEnd = pSrc + m_nGroupCount;
		FDFSGroupStat * pDest = m_lpGroupStat;
		for (; pSrc < pEnd; pSrc++)
		{
			memcpy(pDest->group_name, pSrc->group_name, FDFS_GROUP_NAME_MAX_LEN);
			pDest->total_mb = buff2long(pSrc->sz_total_mb);
			pDest->free_mb = buff2long(pSrc->sz_free_mb);
			pDest->trunk_free_mb = buff2long(pSrc->sz_trunk_free_mb);
			pDest->count = buff2long(pSrc->sz_count);
			pDest->storage_port = buff2long(pSrc->sz_storage_port);
			pDest->storage_http_port = buff2long(pSrc->sz_storage_http_port);
			pDest->active_count = buff2long(pSrc->sz_active_count);
			pDest->current_write_server = buff2long(pSrc->sz_current_write_server);
			pDest->store_path_count = buff2long(pSrc->sz_store_path_count);
			pDest->subdir_count_per_path = buff2long(pSrc->sz_subdir_count_per_path);
			pDest->current_trunk_file_id = buff2long(pSrc->sz_current_trunk_file_id);

			pDest++;
		}
		// 设置已连接标志...
		m_bIsConnected = true;
	}
}

void CTrackerSession::onDisConnected()
{
}

void CTrackerSession::onBytesWritten(qint64 nBytes)
{
}

void CTrackerSession::onError(QAbstractSocket::SocketError nError)
{
}

CStorageSession::CStorageSession()
  : m_bCanReBuild(false)
  , m_llFileSize(0)
  , m_llLeftSize(0)
  , m_lpFile(NULL)
{
	memset(&m_NewStorage, 0, sizeof(m_NewStorage));
}

void CStorageSession::CloseUpFile()
{
	if (m_lpFile != NULL) {
		fclose(m_lpFile);
		m_lpFile = NULL;
	}
}

CStorageSession::~CStorageSession()
{
	// 先关闭套接字...
	this->closeSocket();
	// 再关闭文件对象...
	this->CloseUpFile();
	// 打印当前正在上传的文件和位置...
	// 没有上传完毕的文件，fdfs-storage会自动(回滚)删除服务器端副本，下次再上传时重新上传。。。
	// STORAGE_PROTO_CMD_UPLOAD_FILE 与 STORAGE_PROTO_CMD_APPEND_FILE 无区别...
	if (m_llLeftSize > 0) {
		blog(LOG_INFO, "~CStorageSession: File = %s, Left = %I64d, Size = %I64d", m_strFilePath.c_str(), m_llLeftSize, m_llFileSize);
	}
}
//
// 重建会话...
bool CStorageSession::ReBuildSession(StorageServer * lpStorage, const char * lpszFilePath)
{
	// 判断输入参数是否有效...
	if (lpStorage == NULL || lpszFilePath == NULL) {
		blog(LOG_INFO, "ReBuildSession: input param is NULL");
		return false;
	}
	// 关闭套接字和文件...
	this->closeSocket();
	this->CloseUpFile();
	// 保存数据，打开文件...
	m_NewStorage = *lpStorage;
	m_strFilePath.assign(lpszFilePath);
	m_strExtend.assign(strrchr(lpszFilePath, '.') + 1);
	m_lpFile = os_fopen(m_strFilePath.c_str(), "rb");
	if (m_lpFile == NULL) {
		blog(LOG_INFO, "ReBuildSession: os_fopen is NULL");
		return false;
	}
	// 保存文件总长度...
	m_llFileSize = os_fgetsize(m_lpFile);
	m_llLeftSize = m_llFileSize;
	// 初始化上传会话对象失败，复原重建标志...
	if (!this->InitSession(m_NewStorage.ip_addr, m_NewStorage.port)) {
		this->m_bCanReBuild = true;
		return false;
	}
	// 重建成功，返回正常...
	return true;
}
//
// 发送指令信息头数据包...
bool CStorageSession::SendCmdHeader()
{
	// 准备需要的变量...
	const char * lpszExt = m_strExtend.c_str();
	int64_t  llSize = m_llFileSize;
	qint64	 nReturn = 0;
	qint64   nLength = 0;
	char  *  lpBuf = NULL;
	char     szBuf[_MAX_PATH] = { 0 };
	int      n_pkg_len = 1 + sizeof(int64_t) + FDFS_FILE_EXT_NAME_MAX_LEN;
	// 指令包长度 => 目录编号(1) + 文件长度(8) + 文件扩展名(6)

	// 填充指令头 => 指令编号 + 状态 + 指令包长度(不含指令头)...
	// STORAGE_PROTO_CMD_UPLOAD_FILE 与 STORAGE_PROTO_CMD_APPEND_FILE 无区别...
	// 没有上传完毕的文件，fdfs-storage会自动(回滚)删除服务器端副本，下次再上传时重新上传。。。
	TrackerHeader * lpHeader = (TrackerHeader*)szBuf;
	lpHeader->cmd = STORAGE_PROTO_CMD_UPLOAD_FILE;
	lpHeader->status = 0;
	long2buff(llSize + n_pkg_len, lpHeader->pkg_len);

	// 填充指令包数据...
	lpBuf = szBuf + sizeof(TrackerHeader);							// 将指针定位到数据区...
	lpBuf[0] = m_NewStorage.store_path_index;						// 目录编号   => 0 - 1  => 1个字节
	long2buff(llSize, lpBuf + 1);									// 文件长度   => 1 - 9  => 8个字节 ULONGLONG
	memcpy(lpBuf + 1 + sizeof(int64_t), lpszExt, strlen(lpszExt));	// 文件扩展名 => 9 - 15 => 6个字节 FDFS_FILE_EXT_NAME_MAX_LEN

	// 发送上传指令头和指令包数据 => 命令头 + 数据长度...
	nLength = sizeof(TrackerHeader) + n_pkg_len;
	nReturn = m_TCPSocket->write(szBuf, nLength);
	// 当前正在发送的缓冲区大小...
	m_strCurData.append(szBuf, nLength);
	m_llLeftSize += nLength;
	return true;
}

void CStorageSession::onConnected()
{
	this->SendCmdHeader();
}

void CStorageSession::onReadyRead()
{
	// 如果已经处于重建状态，直接返回...
	if (m_bCanReBuild) return;
	// 从网络层读取所有的缓冲区，并将缓冲区连接起来...
	QByteArray theBuffer = m_TCPSocket->readAll();
	m_strRecv.append(theBuffer.toStdString());
	// 得到的数据长度不够，等待新的接收命令...
	int nCmdLength = sizeof(TrackerHeader);
	if (m_strRecv.size() < nCmdLength)
		return;
	int nDataSize = m_strRecv.size() - nCmdLength;
	const char * lpszDataBuf = m_strRecv.c_str() + nCmdLength;
	TrackerHeader * lpHeader = (TrackerHeader*)m_strRecv.c_str();
	// 判断服务器返回的状态是否正确...
	if (lpHeader->status != 0) {
		blog(LOG_INFO, "onReadyRead: status error");
		m_bCanReBuild = true;
		return;
	}
	// 判断返回的数据区长度是否正确 => 必须大于 FDFS_GROUP_NAME_MAX_LEN
	if (nDataSize <= FDFS_GROUP_NAME_MAX_LEN) {
		blog(LOG_INFO, "onReadyRead: GROUP NAME error");
		m_bCanReBuild = true;
		return;
	}
	char szFileFDFS[_MAX_PATH] = { 0 };
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1] = { 0 };
	char remote_filename[FDFS_REMOTE_NAME_MAX_SIZE] = { 0 };
	memcpy(group_name, lpszDataBuf, FDFS_GROUP_NAME_MAX_LEN);
	memcpy(remote_filename, lpszDataBuf + FDFS_GROUP_NAME_MAX_LEN, nDataSize - FDFS_GROUP_NAME_MAX_LEN + 1);
	sprintf(szFileFDFS, "%s/%s", group_name, remote_filename);
	// 关闭上传文件句柄...
	this->CloseUpFile();
	// 向网站汇报并保存FDFS文件记录...
	App()->doWebSaveFDFS((char*)m_strFilePath.c_str(), szFileFDFS, m_llFileSize);
	// 打印上传结果，删除已上传文件...
	blog(LOG_INFO, "Local = %s, Remote = %s, Size = %I64d\n", m_strFilePath.c_str(), szFileFDFS, m_llFileSize);
	if (os_unlink(m_strFilePath.c_str()) != 0) {
		blog(LOG_INFO, "DeleteFile failed!");
	}
	// 重置接收缓冲区，等待新的数据...
	m_strRecv.clear();
	// 进行复位操作，等待新的截图到达...
	m_bCanReBuild = true;
}

void CStorageSession::onDisConnected()
{
	m_bCanReBuild = true;
	blog(LOG_INFO, "onDisConnected");
}

void CStorageSession::onBytesWritten(qint64 nBytes)
{
	// 如果已经处于重建状态，直接返回...
	if (m_bCanReBuild) return;
	// 如果发送数据包失败，进行复位操作，等待新的截图到达...
	if (!this->SendNextPacket(nBytes)) {
		m_bCanReBuild = true;
	}
}
//
// 发送一个有效数据包...
bool CStorageSession::SendNextPacket(int64_t inLastBytes)
{
	// 没有数据要发送了...
	if (m_llLeftSize <= 0)
		return true;
	// 如果文件句柄已经关闭了...
	if (m_lpFile == NULL) {
		blog(LOG_INFO, "SendNextPacket: File = %s, Left = %I64d, Size = %I64d", m_strFilePath.c_str(), m_llLeftSize, m_llFileSize);
		return false;
	}
	// 先删除已经发送成功的缓冲区...
	m_strCurData.erase(0, inLastBytes);
	m_llLeftSize -= inLastBytes;
	// 如果已经发送完毕，直接返回，等待通知...
	if (m_llLeftSize <= 0)
		return true;
	// 从文件中读取一个数据包的数据量...
	char szBuffer[kPackSize] = { 0 };
	int nReadSize = fread(szBuffer, 1, kPackSize, m_lpFile);
	// 如果文件读取失败，返回错误...
	if (nReadSize <= 0) {
		blog(LOG_INFO, "SendNextPacket: fread error");
		return false;
	}
	// 保存缓冲区，计算要发送的长度...
	m_strCurData.append(szBuffer, nReadSize);
	int nCurSize = m_strCurData.size();
	int nSendSize = ((nCurSize >= kPackSize) ? kPackSize : nCurSize);
	// 发起新的发送任务...
	int nReturn = m_TCPSocket->write(m_strCurData.c_str(), nSendSize);
	return true;
}

void CStorageSession::onError(QAbstractSocket::SocketError nError)
{
	m_bCanReBuild = true;
	blog(LOG_INFO, "onError: %d", nError);
}
