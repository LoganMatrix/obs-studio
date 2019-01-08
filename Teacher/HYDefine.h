
#pragma once

#include <assert.h>

#ifndef ASSERT
#define ASSERT assert 
#endif // ASSERT

#define DEF_WEB_CENTER				"https://www.myhaoyi.com"	// 默认中心网站(443) => 必须是 https:// 兼容小程序接口...
#define DEF_WEB_CLASS				"https://edu.ihaoyi.cn"		// 默认云教室网站(443) => 必须是 https:// 兼容小程序接口...

enum AUTH_STATE
{
	kAuthRegister = 1,		// 网站注册授权
	kAuthExpired = 2,		// 授权过期验证
};

enum STREAM_PROP
{
	kStreamDevice = 0,		// 摄像头设备流类型 => camera
	kStreamMP4File = 1,		// MP4文件流类型    => .mp4
	kStreamUrlLink = 2,		// URL链接流类型    => rtsp/rtmp
};

enum CAMERA_STATE
{
	kCameraWait = 0,		// 未连接
	kCameraRun = 1,			// 运行中
	kCameraRec = 2,			// 录像中
};

// define Mini QRCode type...
enum {
	kQRCodeShop       = 1,  // 门店小程序码
	kQRCodeStudent    = 2,  // 学生端小程序码
	kQRCodeTeacher    = 3,  // 讲师端小程序码
};

// define client type...
enum {
	kClientPHP         = 1,
	kClientStudent     = 2,
	kClientTeacher     = 3,
	kClientUdpServer   = 4,
};

// define command id...
enum {
	kCmd_Student_Login		  = 1,
	kCmd_Student_OnLine		  = 2,
	kCmd_Teacher_Login		  = 3,
	kCmd_Teacher_OnLine       = 4,
	kCmd_UDP_Logout			  = 5,
	kCmd_Camera_PullStart     = 6,
	kCmd_Camera_PullStop      = 7,
	kCmd_Camera_OnLineList    = 8,
	kCmd_Camera_LiveStart     = 9,
	kCmd_Camera_LiveStop      = 10,
	kCmd_UdpServer_Login      = 11,
	kCmd_UdpServer_OnLine     = 12,
	kCmd_UdpServer_AddTeacher = 13,
	kCmd_UdpServer_DelTeacher = 14,
	kCmd_UdpServer_AddStudent = 15,
	kCmd_UdpServer_DelStudent = 16,
	kCmd_PHP_GetUdpServer     = 17,
};

// define the command header...
typedef struct {
	int   m_pkg_len;    // body size...
	int   m_type;       // client type...
	int   m_cmd;        // command id...
	int   m_sock;       // php sock in transmit...
} Cmd_Header;
