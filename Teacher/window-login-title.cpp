
#include "window-login-title.h"
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QDebug>
#include <QFile>

#define BUTTON_HEIGHT 30		// ��ť�߶�;
#define BUTTON_WIDTH 30			// ��ť����;
#define TITLE_HEIGHT 30			// �������߶�;

MyTitleBar::MyTitleBar(QWidget *parent)
	: QWidget(parent)
	, m_isPressed(false)
	, m_isTransparent(false)
	, m_isMoveParentWindow(true)
	, m_bkColor(QColor(153, 153, 153))
{
	// ��ʼ��;
	initControl();
	initConnections();
	// ���ر�����ʽ�ļ�...
	loadStyleSheet(":/login/css/MyTitle.css");
}

MyTitleBar::~MyTitleBar()
{
}

// ��ʼ���ؼ�;
void MyTitleBar::initControl()
{
	m_pIcon = new QLabel;
	m_pTitleContent = new QLabel;

	m_pButtonMin = new QPushButton;
	m_pButtonClose = new QPushButton;

	m_pButtonMin->setFixedSize(QSize(BUTTON_WIDTH, BUTTON_HEIGHT));
	m_pButtonClose->setFixedSize(QSize(BUTTON_WIDTH, BUTTON_HEIGHT));

	m_pTitleContent->setObjectName("TitleContent");
	m_pButtonMin->setObjectName("ButtonMin");
	m_pButtonClose->setObjectName("ButtonClose");

	QHBoxLayout * mylayout = new QHBoxLayout(this);
	mylayout->addWidget(m_pIcon);
	mylayout->addWidget(m_pTitleContent);

	mylayout->addWidget(m_pButtonMin);
	mylayout->addWidget(m_pButtonClose);

	mylayout->setContentsMargins(5, 0, 0, 0);
	mylayout->setSpacing(0);

	m_pTitleContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	this->setFixedHeight(TITLE_HEIGHT);
	this->setWindowFlags(Qt::FramelessWindowHint);
}

// �źŲ۵İ�;
void MyTitleBar::initConnections()
{
	connect(m_pButtonMin, SIGNAL(clicked()), this, SLOT(onButtonMinClicked()));
	connect(m_pButtonClose, SIGNAL(clicked()), this, SLOT(onButtonCloseClicked()));
}

// ���ñ���������ɫ,��paintEvent�¼��н��л��Ʊ���������ɫ;
//�ڹ��캯���и���Ĭ��ֵ�������ⲿ������ɫֵ�ı����������ɫ;
void MyTitleBar::setBackgroundColor(QColor inColor, bool isTransparent)
{
	// ���洫�ݹ����Ĳ���...
	m_bkColor = inColor;
	m_isTransparent = isTransparent;
	// ���»��ƣ�����paintEvent�¼���;
	this->update();
}

// ���ñ�����ͼ��;
void MyTitleBar::setTitleIcon(QString filePath)
{
	QPixmap titleIcon(filePath);
	m_pIcon->setPixmap(titleIcon.scaled(25 , 25));
}

// ���ñ�������;
void MyTitleBar::setTitleContent(QString titleContent, int titleFontSize)
{
	// ���ñ��������С;
	QFont font = m_pTitleContent->font();
	font.setPointSize(titleFontSize);
	m_pTitleContent->setFont(font);
	// ���ñ�������;
	m_pTitleContent->setText(titleContent);
	m_titleContent = titleContent;
}

// ���ñ���������;
void MyTitleBar::setTitleWidth(int width)
{
	this->setFixedWidth(width);
}

// �����Ƿ�ͨ���������ƶ�����;
void MyTitleBar::setMoveParentWindowFlag(bool isMoveParentWindow)
{
	m_isMoveParentWindow = isMoveParentWindow;
}

// ���ñ������ϰ�ť����;
// ���ڲ�ͬ���ڱ������ϵİ�ť����һ�������Կ����Զ���������еİ�ť;
// �����ṩ���ĸ���ť���ֱ�Ϊ��С������ԭ����󻯡��رհ�ť�������Ҫ������ť��������������;
/*void MyTitleBar::setButtonType(ButtonType buttonType)
{
	m_buttonType = buttonType;

	switch (buttonType)
	{
	case MIN_BUTTON:
		{
			m_pButtonRestore->setVisible(false);
			m_pButtonMax->setVisible(false);
		}
		break;
	case MIN_MAX_BUTTON:
		{
			m_pButtonRestore->setVisible(false);
		}
		break;
	case ONLY_CLOSE_BUTTON:
		{
			m_pButtonMin->setVisible(false);
			m_pButtonRestore->setVisible(false);
			m_pButtonMax->setVisible(false);
		}
		break;
	default:
		break;
	}
}

// ���ñ������еı����Ƿ���Զ������������Ƶ�Ч��;
// һ������±������еı��������ǲ������ģ����Ǽ�Ȼ�Զ���Ϳ��Լ���Ҫ�����ô��ƾ���ô��O(��_��)O��
void MyTitleBar::setTitleRoll()
{
	connect(&m_titleRollTimer, SIGNAL(timeout()), this, SLOT(onRollTitle()));
	m_titleRollTimer.start(200);
}*/

// ���Ʊ���������ɫ;
void MyTitleBar::paintEvent(QPaintEvent *event)
{
	if (!m_isTransparent)
	{
		// ���ñ���ɫ;
		QPainter painter(this);
		QPainterPath pathBack;
		pathBack.setFillRule(Qt::WindingFill);
		pathBack.addRoundedRect(QRect(0, 0, this->width(), this->height()), 3, 3);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.fillPath(pathBack, QBrush(m_bkColor));
	}	

	QWidget::paintEvent(event);
}

// ˫����Ӧ�¼�����Ҫ��ʵ��˫��������������󻯺���С������;
void MyTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
	// ֻ�д�����󻯡���ԭ��ťʱ˫������Ч;
	/*if (m_buttonType == MIN_MAX_BUTTON)
	{
		// ͨ����󻯰�ť��״̬�жϵ�ǰ�����Ǵ�����󻯻���ԭʼ��С״̬;
		// ����ͨ���������ñ�������ʾ��ǰ����״̬;
		if (m_pButtonMax->isVisible())
		{
			onButtonMaxClicked();
		}
		else
		{
			onButtonRestoreClicked();
		}
	}*/

	return QWidget::mouseDoubleClickEvent(event);
}
//
// ����ͨ�� mousePressEvent | mouseMoveEvent | mouseReleaseEvent �����¼�ʵ��������϶��������ƶ����ڵ�Ч��;
void MyTitleBar::mousePressEvent(QMouseEvent *event)
{
	m_isPressed = true;
	m_startMovePos = event->globalPos();
	qDebug() << "MyTitleBar::mousePressEvent" << m_startMovePos.x() << m_startMovePos.y();
	
	return QWidget::mousePressEvent(event);
}

void MyTitleBar::mouseMoveEvent(QMouseEvent *event)
{
	if (m_isPressed && m_isMoveParentWindow && this->parentWidget()) {
		QPoint movePoint = event->globalPos() - m_startMovePos;
		QPoint widgetPos = this->parentWidget()->pos() + movePoint;
		m_startMovePos = event->globalPos();
		qDebug() << "MyTitleBar::mouseMoveEvent" << widgetPos.x() << widgetPos.y();
		this->parentWidget()->move(widgetPos.x(), widgetPos.y());
	}
	return QWidget::mouseMoveEvent(event);
}

void MyTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
	m_isPressed = false;
	return QWidget::mouseReleaseEvent(event);
}

// ���ر�����ʽ�ļ�;
// ���Խ���ʽֱ��д���ļ��У���������ʱֱ�Ӽ��ؽ���;
void MyTitleBar::loadStyleSheet(const QString &sheetName)
{
	QFile file(sheetName);
	file.open(QFile::ReadOnly);
	if (file.isOpen())
	{
		QString styleSheet = this->styleSheet();
		styleSheet += QLatin1String(file.readAll());
		this->setStyleSheet(styleSheet);
	}
}

// ����Ϊ��ť������Ӧ�Ĳ�;
void MyTitleBar::onButtonMinClicked()
{
	emit signalButtonMinClicked();
}

void MyTitleBar::onButtonCloseClicked()
{
	emit signalButtonCloseClicked();
}

// �÷�����Ҫ���ñ������еı�����ʾΪ������Ч��;
/*void MyTitleBar::onRollTitle()
{
	static int nPos = 0;
	QString titleContent = m_titleContent;
	// ����ȡ��λ�ñ��ַ�����ʱ����ͷ��ʼ;
	if (nPos > titleContent.length())
		nPos = 0;

	m_pTitleContent->setText(titleContent.mid(nPos));
	nPos++;
}*/