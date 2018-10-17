
#pragma once

#include "HYDefine.h"
#include <QMouseEvent>
#include "qt-display.hpp"
#include <util/threading.h>

class CViewLeft;
class CWebrtcAEC;
class CDataThread;
class CUDPSendThread;
class CViewCamera : public OBSQTDisplay {
	Q_OBJECT
public:
	CViewCamera(QWidget *parent, int nDBCameraID);
	virtual ~CViewCamera();
signals:
	void		doTriggerReadyToRecvFrame();
protected slots:
	void		onTriggerReadyToRecvFrame();
public:
	bool		IsCameraOffLine() { return ((m_nCameraState == kCameraOffLine) ? true : false); }
	bool		IsCameraOnLine() { return ((m_nCameraState == kCameraOnLine) ? true : false); }
	bool		IsCameraPreview() { return m_bIsPreview; }
	int			GetDBCameraID() { return m_nDBCameraID; }
public:
	void        doEchoCancel(void * lpBufData, int nBufSize, int nSampleRate, int nChannelNum, int msInSndCardBuf);
	void        doPushAudioAEC(FMS_FRAME & inFrame);
	void		doPushFrame(FMS_FRAME & inFrame);
	void		doUdpSendThreadStart();
	void		doUdpSendThreadStop();
	void        onUdpRecvThreadStop();
	void		doReadyToRecvFrame();
	void		doTogglePreview();
	bool		doCameraStart();
	bool		doCameraStop();
private:
	void		DrawTitleArea();
	void		DrawRenderArea();
	void		DrawStatusText();
private:
	QString		GetRecvPullRate();
	QString		GetSendPushRate();
	QString		GetStreamPushUrl();
	bool		IsDataFinished();
	bool		IsFrameTimeout();
	void		CalcFlowKbps();
	void        BuildSendThread();
	void        ReBuildWebrtcAEC();
protected:
	void		paintEvent(QPaintEvent *event) override;
	void		timerEvent(QTimerEvent * inEvent) override;
	void		mousePressEvent(QMouseEvent *event) override;
private:
	CAMERA_STATE        m_nCameraState;		// 摄像头状态...
	uint32_t			m_dwTimeOutMS;		// 超时计时点...
	uint32_t			m_nCurRecvByte;		// 当前已接收字节数
	int					m_nRecvKbps;		// 拉流接收码率...
	int                 m_nFlowTimer;       // 码率检测时钟...
	int                 m_nDBCameraID;      // 通道在数据库中的编号...
	bool                m_bIsPreview;       // 通道是否正在预览画面...
	QRect               m_rcRenderRect;     // 窗口画面渲染的矩形区域...
	CViewLeft       *   m_lpViewLeft;       // 左侧父窗口对象...
	CWebrtcAEC      *   m_lpWebrtcAEC;      // 回音处理对象...
	CDataThread     *   m_lpDataThread;     // 数据基础类线程...
	CUDPSendThread  *   m_lpUDPSendThread;  // UDP推流线程...
	pthread_mutex_t     m_MutexAEC;         // 回音消除线程互斥体
};
