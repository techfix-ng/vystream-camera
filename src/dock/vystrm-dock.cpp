#include <obs-frontend-api.h>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QImage>
#include <QPixmap>
#include "../qr_v4.h"

extern "C" {
const char *vystrm_pairing_payload(void);
int vystrm_camera_count(void);
const char *vystrm_camera_name(int index);
void vystrm_select_camera(int index);
bool vystrm_talkback_start(void);
void vystrm_talkback_stop(void);
}

class HoldButton final : public QPushButton {
public:
  using QPushButton::QPushButton;
protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && vystrm_talkback_start()) {
      setProperty("talking", true); setText("TALKING — RELEASE TO STOP");
      style()->unpolish(this); style()->polish(this);
    }
    QPushButton::mousePressEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent *event) override {
    vystrm_talkback_stop(); setProperty("talking", false); setText("HOLD TO TALK");
    style()->unpolish(this); style()->polish(this);
    QPushButton::mouseReleaseEvent(event);
  }
};

class VyStreamDock final : public QWidget {
  Q_OBJECT
public:
  VyStreamDock() {
    setObjectName("VyStreamCameraDock");
    setMinimumWidth(260);
    setStyleSheet(R"CSS(
      #VyStreamCameraDock { background:#17191f; color:#f5f7fb; }
      QLabel#brand { color:#f7f9ff; font-size:20px; font-weight:800; letter-spacing:2px; }
      QLabel#sub { color:#8d97a8; font-size:10px; letter-spacing:1px; }
      QLabel#card { background:#20242c; border:1px solid #303642; border-radius:12px; padding:10px; }
      QComboBox { background:#242a34; border:1px solid #394150; border-radius:8px; padding:7px; color:#fff; }
      QPushButton { background:#1769e0; border:0; border-radius:9px; padding:10px; color:white; font-weight:700; }
      QPushButton[talking="true"] { background:#e3473f; }
      QPushButton#listen { background:#282e38; border:1px solid #3b4351; }
    )CSS");
    auto *root = new QVBoxLayout(this); root->setContentsMargins(12,12,12,12); root->setSpacing(10);
    auto *brandRow = new QHBoxLayout;
    auto *logo = new QLabel; logo->setPixmap(QPixmap(":/vystrm/vystrm_camera_icon.png").scaled(44,44,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    auto *brand = new QLabel("VY<span style='color:#19b9ff'>STR</span><span style='color:#ff8a00'>M</span>");
    brand->setObjectName("brand"); brand->setTextFormat(Qt::RichText); brandRow->addWidget(logo); brandRow->addWidget(brand); brandRow->addStretch(); root->addLayout(brandRow);
    auto *sub = new QLabel("PROFESSIONAL BROADCAST CAMERA"); sub->setObjectName("sub"); root->addWidget(sub);
    qr = new QLabel; qr->setObjectName("card"); qr->setAlignment(Qt::AlignCenter); qr->setWordWrap(true);
    qr->setText("PAIR CAMERA\n\nOpen VyStream Camera and scan this dock's stable pairing code.\n\nWi-Fi discovery: ACTIVE");
    qr->setMinimumHeight(155); root->addWidget(qr);
    status = new QLabel("● Waiting for VyStream cameras"); status->setStyleSheet("color:#8d97a8;padding:4px;"); root->addWidget(status);
    cameras = new QComboBox; root->addWidget(cameras);
    connect(cameras, qOverload<int>(&QComboBox::currentIndexChanged), this, [](int i){ vystrm_select_camera(i-1); });
    talk = new HoldButton("HOLD TO TALK"); root->addWidget(talk);
    auto *listen = new QLabel("● CAMERA REPLY LISTENER ACTIVE"); listen->setAlignment(Qt::AlignCenter); listen->setStyleSheet("background:#202a26;color:#35d07f;border-radius:9px;padding:9px;font-weight:700;"); root->addWidget(listen);
    auto *relay = new QLabel("● CREW-TO-CREW RELAY ACTIVE"); relay->setAlignment(Qt::AlignCenter); relay->setStyleSheet("background:#1d2633;color:#43a5ff;border-radius:9px;padding:9px;font-weight:700;"); root->addWidget(relay);
    auto *footer = new QLabel("UDP 45990 discovery  •  46010/46011 talkback"); footer->setAlignment(Qt::AlignCenter); footer->setStyleSheet("color:#687386;font-size:9px;"); root->addWidget(footer);
    root->addStretch();
    auto *timer = new QTimer(this); connect(timer,&QTimer::timeout,this,&VyStreamDock::refresh); timer->start(750); refresh();
  }
private slots:
  void refresh() {
    const int count=vystrm_camera_count();
    if (count+1 != cameras->count()) {
      cameras->blockSignals(true); cameras->clear(); cameras->addItem("All connected cameras");
      for(int i=0;i<count;i++) cameras->addItem(QString::fromUtf8(vystrm_camera_name(i)));
      cameras->blockSignals(false);
    }
    status->setText(count ? QString("● %1 camera%2 connected").arg(count).arg(count==1?"":"s") : "● Waiting for VyStream cameras");
    status->setStyleSheet(count ? "color:#35d07f;padding:4px;" : "color:#8d97a8;padding:4px;");
    const char *payload=vystrm_pairing_payload();
    const QString current=payload?QString::fromUtf8(payload):QString();
    if (!current.isEmpty() && current != pairingPayload) {
      pairingPayload=current; VystrmQrV4 code;
      if(code.encode(current.toStdString())) {
        constexpr int quiet=4, scale=5; const int side=(VystrmQrV4::size+quiet*2)*scale;
        QImage image(side,side,QImage::Format_RGB32); image.fill(Qt::white);
        for(int y=0;y<VystrmQrV4::size;y++) for(int x=0;x<VystrmQrV4::size;x++) if(code.module(x,y))
          for(int yy=0;yy<scale;yy++) for(int xx=0;xx<scale;xx++) image.setPixelColor((x+quiet)*scale+xx,(y+quiet)*scale+yy,Qt::black);
        qr->setPixmap(QPixmap::fromImage(image)); qr->setMinimumHeight(side+20);
      }
    }
    qr->setToolTip(current);
  }
private:
  QLabel *qr{},*status{}; QComboBox *cameras{}; HoldButton *talk{}; QString pairingPayload;
};

static bool vystrm_dock_registered = false;

static void vystrm_frontend_event(enum obs_frontend_event event, void *) {
  if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || vystrm_dock_registered)
    return;

  auto *dock = new VyStreamDock;
  vystrm_dock_registered =
      obs_frontend_add_dock_by_id("vystrm-camera-dock", "VyStream Camera", dock);
  if (!vystrm_dock_registered) {
    delete dock;
    return;
  }

  // OBS owns a QDockWidget wrapper around the supplied widget. Explicitly
  // enable every dock area and attach it on first load instead of leaving it
  // as a floating utility window. The user can still drag or float it later.
  QTimer::singleShot(0, dock, [dock] {
    auto *wrapper = qobject_cast<QDockWidget *>(dock->parentWidget());
    auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!wrapper || !mainWindow)
      return;
    wrapper->setAllowedAreas(Qt::AllDockWidgetAreas);
    wrapper->setFeatures(QDockWidget::DockWidgetClosable |
                         QDockWidget::DockWidgetMovable |
                         QDockWidget::DockWidgetFloatable);
    mainWindow->addDockWidget(Qt::RightDockWidgetArea, wrapper);
    wrapper->setFloating(false);
    wrapper->show();
  });
}

extern "C" void vystrm_register_dock(void) {
  obs_frontend_add_event_callback(vystrm_frontend_event, nullptr);
}

#include "vystrm-dock.moc"
