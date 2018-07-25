#pragma once

#include <QApplication>
#include <QTranslator>
#include <QPointer>

#include <obs.hpp>
#include <util/lexer.h>
#include <util/util.hpp>
#include <util/platform.h>
#include <obs-frontend-api.h>
#include <deque>

#include "window-login-main.h"
#include "window-student-main.h"
#include "json.h"

std::string CurrentTimeString();
std::string CurrentDateTimeString();
std::string GenerateTimeDateFilename(const char *extension, bool noSpace = false);
std::string GenerateSpecifiedFilename(const char *extension, bool noSpace, const char *format);

struct BaseLexer {
	lexer lex;
public:
	inline BaseLexer() { lexer_init(&lex); }
	inline ~BaseLexer() { lexer_free(&lex); }
	operator lexer*() { return &lex; }
};

class OBSTranslator : public QTranslator {
	Q_OBJECT
public:
	virtual bool isEmpty() const override { return false; }
	virtual QString translate(const char *context, const char *sourceText, const char *disambiguation, int n) const override;
};

class CWebThread;
class CStudentApp : public QApplication {
	Q_OBJECT
public:
	CStudentApp(int &argc, char **argv);
	~CStudentApp();
signals:
	void msgFromWebThread(int nMessageID, int nWParam, int nLParam);
	void doEnableCamera(OBSQTDisplay * lpNewDisplay);
public:
	void AppInit();
public:
	void doLoginInit();
	void doLogoutEvent();
	void doSaveFocus(OBSQTDisplay * lpNewDisplay);
	void doResetFocus(OBSQTDisplay * lpCurDisplay);
public:
	static char * GetServerOS();
	static char * GetServerDNSName();
	static string GetSystemVer();
	static string getJsonString(Json::Value & inValue);
	static string UTF8_ANSI(const char * lpUValue);
	static string ANSI_UTF8(const char * lpSValue);
public slots:
	void doLoginSuccess(string & strRoomID);
public:
	bool     GetAuthLicense() { return m_bAuthLicense; }
	int      GetAuthDays() { return m_nAuthDays; }
	string & GetAuthExpired() { return m_strAuthExpired; }
	string & GetMainName() { return m_strMainName; }
	int		 GetSliceVal() { return m_nSliceVal; }
	int		 GetInterVal() { return m_nInterVal; }
	int		 GetSnapVal() { return m_nSnapVal; }
	bool	 GetAutoLinkDVR() { return m_bAutoLinkDVR; }
	bool	 GetAutoLinkFDFS() { return m_bAutoLinkFDFS; }
	int		 GetMaxCamera() { return m_nMaxCamera; }
	string & GetWebVer() { return m_strWebVer; }
	string & GetWebTag() { return m_strWebTag; }
	string & GetWebName() { return m_strWebName; }
	int		 GetWebType() { return m_nWebType; }
	string & GetRemoteAddr() { return m_strRemoteAddr; }
	int		 GetRemotePort() { return m_nRemotePort; }
	string & GetTrackerAddr() { return m_strTrackerAddr; }
	int		 GetTrackerPort() { return m_nTrackerPort; }
	int      GetDBGatherID() { return m_nDBGatherID; }
	int      GetDBHaoYiUserID() { return m_nDBHaoYiUserID; }
	int      GetDBHaoYiNodeID() { return m_nDBHaoYiNodeID; }
	int      GetDBHaoYiGatherID() { return m_nDBHaoYiGatherID; }

	void	 SetRemoteAddr(const string & strAddr) { m_strRemoteAddr = strAddr; }
	void     SetRemotePort(int nPort) { m_nRemotePort = nPort; }
	void	 SetTrackerAddr(const string & strAddr) { m_strTrackerAddr = strAddr; }
	void     SetTrackerPort(int nPort) { m_nTrackerPort = nPort; }
	void	 SetWebType(int nWebType) { m_nWebType = nWebType; }
	void	 SetWebName(const string & strWebName) { m_strWebName = strWebName; }
	void	 SetWebTag(const string & strWebTag) { m_strWebTag = strWebTag; }
	void	 SetWebVer(const string & strWebVer) { m_strWebVer = strWebVer; }
	void	 SetDBGatherID(int nDBGatherID) { m_nDBGatherID = nDBGatherID; }
	void     SetDBHaoYiUserID(int nDBUserID) { m_nDBHaoYiUserID = nDBUserID; }
	void	 SetDBHaoYiNodeID(int nDBNodeID) { m_nDBHaoYiNodeID = nDBNodeID; }
	void     SetDBHaoYiGatherID(int nDBGatherID) { m_nDBHaoYiGatherID = nDBGatherID; }

	void	 SetAuthDays(const int nAuthDays) { m_nAuthDays = nAuthDays; }
	void	 SetAuthExpired(const string & strExpired) { m_strAuthExpired = strExpired; }
	void	 SetAuthLicense(bool bLicense) { m_bAuthLicense = bLicense; }
	void	 SetMainName(const string & strName) { m_strMainName = strName; }
	void	 SetSliceVal(int nSliceVal) { m_nSliceVal = nSliceVal; }
	void	 SetInterVal(int nInterVal) { m_nInterVal = nInterVal; }
	void	 SetSnapVal(int nSnapVal) { m_nSnapVal = nSnapVal; }
	void	 SetAutoLinkDVR(bool bAutoLinkDVR) { m_bAutoLinkDVR = bAutoLinkDVR; }
	void	 SetAutoLinkFDFS(bool bAutoLinkFDFS) { m_bAutoLinkFDFS = bAutoLinkFDFS; }
	void	 SetMaxCamera(int nMaxCamera) { m_nMaxCamera = nMaxCamera; }

	QString  GetCameraName(int nDBCameraID);
	QString  GetCameraPullUrl(int nDBCameraID);
	string   GetCameraDeviceSN(int nDBCameraID);
	void     doDelCamera(int nDBCameraID);

	void	 SetCamera(int nDBCameraID, GM_MapData & inMapData) { m_MapNodeCamera[nDBCameraID] = inMapData; }
	void	 GetCamera(int nDBCameraID, GM_MapData & outMapData) { outMapData = m_MapNodeCamera[nDBCameraID]; }
	GM_MapNodeCamera & GetNodeCamera() { return m_MapNodeCamera; }
	CWebThread * GetWebThread() { return m_lpWebThread; }

	int      GetWebPort() { return m_nWebPort; }
	string & GetWebAddr() { return m_strWebAddr; }
	string & GetLocalIPAddr() { return m_strIPAddr; }
	string & GetLocalMacAddr() { return m_strMacAddr; }

	bool TranslateString(const char *lookupVal, const char **out) const;
	inline config_t *GlobalConfig() const { return m_globalConfig; }
	inline lookup_t *GetTextLookup() const { return m_textLookup; }
	inline const char *GetString(const char *lookupVal) const
	{
		return m_textLookup.GetString(lookupVal);
	}

	inline void PushUITranslation(obs_frontend_translate_ui_cb cb)
	{
		translatorHooks.emplace_front(cb);
	}

	inline void PopUITranslation()
	{
		translatorHooks.pop_front();
	}
private:
	bool	InitLocale();
	bool	InitGlobalConfig();
	bool	InitGlobalConfigDefaults();
	bool	InitMacIPAddr();
private:
	std::deque<obs_frontend_translate_ui_cb> translatorHooks;
	QPointer<StudentWindow>    m_studentWindow;
	QPointer<LoginWindow>      m_loginWindow;
	ConfigFile                 m_globalConfig;
	TextLookup                 m_textLookup;
	OBSQTDisplay      *        m_lpFocusDisplay;
	CWebThread        *        m_lpWebThread;				// 网站相关线程...
	string                     m_strMacAddr;				// 本机MAC地址...
	string                     m_strIPAddr;					// 本机IP地址...
	string                     m_strWebAddr;				// 访问节点网站地址...
	int                        m_nWebPort;					// 访问节点网站端口...
	GM_MapNodeCamera           m_MapNodeCamera;				// 监控通道配置信息(数据库CameraID）
	int					m_nMaxCamera;					// 能够支持的最大摄像头数（默认为8个）
	string				m_strWebVer;					// 注册是获取的网站版本
	string				m_strWebTag;					// 注册时获取的网站标志
	string				m_strWebName;					// 注册时获取的网站名称
	int					m_nWebType;						// 注册时获取的网站类型
	string				m_strRemoteAddr;				// 远程中转服务器的IP地址...
	int					m_nRemotePort;					// 远程中转服务器的端口地址...
	string				m_strTrackerAddr;				// FDFS-Tracker的IP地址...
	int					m_nTrackerPort;					// FDFS-Tracker的端口地址...
	int                 m_nDBGatherID;					// 数据库中采集端编号...
	int					m_nDBHaoYiUserID;				// 在中心服务器上的绑定用户编号...
	int					m_nDBHaoYiNodeID;				// 在中心服务器上的节点编号...
	int                 m_nDBHaoYiGatherID;				// 在中心服务器上的采集端编号...
	string				m_strAuthExpired;				// 中心服务器反馈的授权过期时间...
	bool				m_bAuthLicense;					// 中心服务器反馈的永久授权标志...
	int					m_nAuthDays;					// 中心服务器反馈的剩余授权天数...
	string				m_strMainName;					// 主窗口标题名称...
	int					m_nSliceVal;					// 录像切片时间(0~30分钟)
	int					m_nInterVal;					// 切片交错关键帧(0~3个)
	int				    m_nSnapVal;						// 通道截图间隔(1~10分钟)
	bool				m_bAutoLinkDVR;					// 自动重连DVR摄像头...
	bool				m_bAutoLinkFDFS;				// 自动重连FDFS服务器...
};

inline CStudentApp *App() { return static_cast<CStudentApp*>(qApp); }
inline config_t *GetGlobalConfig() { return App()->GlobalConfig(); }
inline const char *Str(const char *lookup) { return App()->GetString(lookup); }
#define QTStr(lookupVal) QString::fromUtf8(Str(lookupVal))

bool WindowPositionValid(QRect rect);
