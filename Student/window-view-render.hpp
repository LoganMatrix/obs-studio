#pragma once

#include "qt-display.hpp"

class CViewTeacher;
class CViewRender : public OBSQTDisplay {
	Q_OBJECT
public:
	CViewRender(QWidget *parent, Qt::WindowFlags flags = 0);
	virtual ~CViewRender();
public:
	void    doUpdateNotice(const QString & strNotice, bool bIsDrawImage = false);
	bool    IsChangeScreen() { return m_bIsChangeScreen; }
	bool    IsDrawImage() { return m_bIsDrawImage; }
	QRect & GetRenderRect() { return m_rcRenderRect; }
	HWND    GetRenderHWnd() { return m_hRenderWnd; }
	void    doResizeRect(const QRect & rcInRect);
	bool    GetAndResetRenderFlag();
	void    onFullScreenAction();
protected:
	void    paintEvent(QPaintEvent *event) override;
	void    keyPressEvent(QKeyEvent *event) override;
	void    mouseDoubleClickEvent(QMouseEvent *event) override;
private:
	CViewTeacher   *   m_lpViewTeacher;     // �����ڶ���...
	HWND               m_hRenderWnd;        // ��Ⱦ���洰�ھ��...
	QRect              m_rcNoramlRect;      // ���ڵ�ȫ��ǰ�ľ�������...
	QRect              m_rcRenderRect;      // ���ڻ�����Ⱦ�ľ�������...
	bool               m_bRectChanged;      // ��Ⱦ�����������仯...
	bool               m_bIsChangeScreen;   // ���ڴ���ȫ����ԭ����...
	bool               m_bIsDrawImage;      // �Ƿ����ڻ���ͼƬ��־...
	QString            m_strNoticeText;     // ��ʾ����ʾ������Ϣ...
};