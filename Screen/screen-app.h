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

#include "json.h"

using namespace std;

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

class CLoginMini;
class OBSBasicPreview;
class CScreenApp : public QApplication {
	Q_OBJECT
public:
	CScreenApp(int &argc, char **argv, profiler_name_store_t *store);
	~CScreenApp();
public:
	void AppInit();
	bool OBSInit();
	void doLoginInit();
	void doProcessCmdLine(int argc, char * argv[]);
public:
	static string getJsonString(Json::Value & inValue);
	static void RenderMain(void *data, uint32_t cx, uint32_t cy);
	static void ReceiveRawVideo(void *param, struct video_data *frame);
public slots:
	void onTriggerMiniSuccess();
public:
	string & GetWebClass() { return m_strWebClass; }
	string & GetWebCenter() { return m_strWebCenter; }
public:
	const char *GetRenderModule() const;
	std::string GetVersionString() const;
	bool TranslateString(const char *lookupVal, const char **out) const;
	inline config_t *GlobalConfig() const { return m_globalConfig; }
	inline lookup_t *GetTextLookup() const { return m_textLookup; }
	inline const char *GetString(const char *lookupVal) const {
		return m_textLookup.GetString(lookupVal);
	}
	profiler_name_store_t *GetProfilerNameStore() const {
		return profilerNameStore;
	}
private:
	void    LogScenes();
	int     ResetVideo();
	bool	InitLocale();
	void    ClearSceneData();
	void    doCreateDisplay();
	bool	InitGlobalConfig();
	bool	InitGlobalConfigDefaults();
	bool	doSaveMemJpg(video_data * frame);
	void    ResizePreview(uint32_t cx, uint32_t cy);
	bool    doSaveFileJpg(uint8_t * inData, int nSize);
	void    AddSceneItem(obs_scene_t *scene, obs_source_t *source);
private:
	std::deque<obs_frontend_translate_ui_cb> translatorHooks;
	profiler_name_store_t  *   profilerNameStore = nullptr;
	QPointer<CLoginMini>       m_LoginMini = nullptr;       // 小程序登录窗口
	QPointer<OBSBasicPreview>  m_preview = nullptr;         // 预览窗口对象...
	ConfigFile      m_globalConfig;               // 全局配置对象...
	TextLookup      m_textLookup;                 // 文字翻译对象...
	OBSContext      m_obsContext;                 // 释放结构对象...
	obs_source_t *  m_obsSource = nullptr;        // 唯一主数据源...
	obs_scene_t  *  m_obsScene = nullptr;         // 唯一主场景...
	string          m_strWebCenter;               // 访问中心网站地址...
	string          m_strWebClass;				  // 访问云教室网站地址...
	string          m_strLocale;                  // 本地语言 => zh-CN
	string          m_strJPGData;                 // JPG内存数据区 => 会被刷新...
	bool            m_bIsDebugMode = false;       // 是否是调试模式 => 挂载到调试服务器...
	bool            m_bIsSaveFile = false;        // 是否存放JPG文件标志 => 默认False...
	int             m_nFPS = 5;                   // 每秒帧率，降低CPU使用率...
	int             m_snap_second = 5;            // 截图间隔时间(秒)...
	float           m_qCompress = 0.5f;           // JPG压缩质量...
	uint64_t        m_start_time_ns = 0;          // 默认的启动时间...
	float           previewScale = 0.0f;          // 预览缩放率...
	int             previewX = 0, previewY = 0;   // 预览位置...
	int             previewCX = 0, previewCY = 0; // 预览大小...

	friend class CLoginMini;
};

inline CScreenApp *App() { return static_cast<CScreenApp*>(qApp); }
inline config_t *GetGlobalConfig() { return App()->GlobalConfig(); }
inline const char *Str(const char *lookup) { return App()->GetString(lookup); }
#define QTStr(lookupVal) QString::fromUtf8(Str(lookupVal))

int GetConfigPath(char *path, size_t size, const char *name);
char *GetConfigPathPtr(const char *name);
bool WindowPositionValid(QRect rect);
