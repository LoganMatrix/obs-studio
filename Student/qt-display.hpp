#pragma once

#include <QWidget>

class OBSQTDisplay : public QWidget {
	Q_OBJECT
protected:
	void   paintEvent(QPaintEvent *event) override;
public:
	void   setBKColor(QColor inColor);
public:
	OBSQTDisplay(QWidget *parent = 0, Qt::WindowFlags flags = 0);
	virtual void mouseDoubleClickEvent(QMouseEvent *event) override;
protected:
	QColor	m_bkColor;
};