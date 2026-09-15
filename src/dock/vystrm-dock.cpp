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
#include <QSlider>
#include <QStackedWidget>
#include <QInputDialog>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QStringList>
#include <cstdio>
#include <QLineEdit>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <string>
#include "../qr_v4.h"
#include "../auth/auth-manager.h"
#include "../auth/login-widget.h"

extern "C" {
const char *vystrm_pairing_payload(void);
int vystrm_camera_count(void);
const char *vystrm_camera_name(int index);
const char *vystrm_camera_health(int index);
void vystrm_set_camera_control(int index, const char *control, const char *value);
bool vystrm_rename_camera(int index, const char *scene, const char *source);
void vystrm_select_camera(int index);
bool vystrm_talkback_start(void);
void vystrm_talkback_stop(void);
}

class HoldButton final : public QPushButton {
public:
  using QPushButton::QPushButton;
  explicit HoldButton(const QString &text, QWidget *parent = nullptr) : QPushButton(text, parent) {
    connect(this, &QPushButton::clicked, this, [this] {
      if (talking) {
        vystrm_talkback_stop(); talking = false; setText("TAP TO TALK");
      } else if (vystrm_talkback_start()) {
        talking = true; setText("TALKBACK ON · TAP TO DISABLE");
      }
      setProperty("talking", talking); style()->unpolish(this); style()->polish(this);
    });
  }
protected:
  /* Legacy press/release handlers are intentionally disabled; talkback is a toggle. */
  /* void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      grabMouse();
      if (vystrm_talkback_start()) {
        setProperty("talking", true); setText("TALKING — RELEASE TO STOP");
        style()->unpolish(this); style()->polish(this);
      }
      event->accept();
      return;
    }
    QPushButton::mousePressEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      releaseMouse();
      vystrm_talkback_stop(); setProperty("talking", false); setText("HOLD TO TALK");
      style()->unpolish(this); style()->polish(this);
      event->accept();
      return;
    }
    QPushButton::mouseReleaseEvent(event);
  } */
  bool talking = false;
};

class VyStreamDock;
static VyStreamDock *g_vystrm_dock = nullptr;

class VyStreamDock final : public QWidget {
  Q_OBJECT
public:
  VyStreamDock() {
    g_vystrm_dock = this;
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
    auth = new VystrmAuthManager(this);
    stack = new QStackedWidget(this);
    auto *login = new VystrmLoginWidget(auth, stack);
    auto *dashboard = new QWidget(stack);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0,0,0,0);
    outer->addWidget(stack);
    auto *root = new QVBoxLayout(dashboard); rootLayout=root; root->setContentsMargins(10,10,10,10); root->setSpacing(8);
    auto *brandRow = new QHBoxLayout;
    auto *logo = new QLabel; logo->setPixmap(QPixmap(":/vystrm/vystrm_camera_icon.png").scaled(44,44,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    auto *brand = new QLabel("<span style='color:#19b9ff'>VYS</span><span style='color:#f7f9ff'>TREA</span><span style='color:#ff8a00'>M</span>");
    brand->setObjectName("brand"); brand->setTextFormat(Qt::RichText); brandRow->addWidget(logo); brandRow->addWidget(brand); brandRow->addStretch();
    planBadge = new QLabel("FREE · 1 CAMERA · 720P");
    planBadge->setStyleSheet("color:#9fb1c8;font-size:9px;");
    auto *logout = new QPushButton("SIGN OUT");
    logout->setObjectName("listen");
    logout->setMaximumWidth(72);
    brandRow->addWidget(planBadge);
    brandRow->addWidget(logout);
    root->addLayout(brandRow);
    auto *sub = new QLabel("PROFESSIONAL BROADCAST CAMERA"); sub->setObjectName("sub"); root->addWidget(sub);
    status = new QLabel("● Waiting for VYSTREAM cameras"); status->setStyleSheet("color:#8d97a8;padding:4px;"); root->addWidget(status);
    auto *tabs = new QTabWidget(this); tabs->setDocumentMode(true); tabs->setObjectName("sections"); root->addWidget(tabs);
    auto *overview = new QWidget; auto *overviewLayout = new QVBoxLayout(overview);
    overviewLayout->addWidget(new QLabel("PRODUCTION OVERVIEW")); overviewLayout->addWidget(new QLabel("VYSTREAM Camera is ready for OBS.")); overviewLayout->addStretch(); tabs->addTab(overview, "OVERVIEW");
    auto *cameraPage = new QWidget; auto *cameraLayout = new QVBoxLayout(cameraPage);
    cameraLayout->addWidget(new QLabel("CAMERA HEALTH"));
    cameras = new QComboBox; cameraLayout->addWidget(cameras); health = new QLabel("Select a connected camera to view health."); health->setObjectName("card"); health->setWordWrap(true); cameraLayout->addWidget(health);
    cameraLayout->addWidget(new QLabel("REMOTE CAMERA CONTROLS"));
    zoom = addSlider(cameraLayout, "ZOOM", 10, 80, 10, "ZOOM"); exposure = addSlider(cameraLayout, "EXPOSURE", -6, 6, 0, "EXPOSURE"); brightness = addSlider(cameraLayout, "BRIGHTNESS", -6, 6, 0, "BRIGHTNESS");
    auto *rename = new QPushButton("RENAME CAMERA"); cameraLayout->addWidget(rename); cameraLayout->addStretch(); tabs->addTab(cameraPage, "CAMERAS");
    connect(cameras, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i){ vystrm_select_camera(i-1); refresh(); });
    connect(rename, &QPushButton::clicked, this, [this]{int i=cameras->currentIndex()-1;if(i<0)return;bool ok=false;QString scene=QInputDialog::getText(this,"Rename camera","OBS scene name:",QLineEdit::Normal,cameras->currentText()+" Scene",&ok);if(!ok||scene.trimmed().isEmpty())return;QString source=QInputDialog::getText(this,"Rename camera","OBS source name:",QLineEdit::Normal,cameras->currentText()+"-Cam",&ok);if(ok&&!source.trimmed().isEmpty()&&vystrm_rename_camera(i,scene.trimmed().toUtf8().constData(),source.trimmed().toUtf8().constData()))refresh();});
    auto *talkPage = new QWidget; auto *talkLayout = new QVBoxLayout(talkPage);
    talkLayout->addWidget(new QLabel("TALKBACK & INTERCOM"));
    talkLayout->addWidget(new QLabel("Send director talkback to:"));
    talkbackCameras = new QComboBox;
    talkbackCameras->addItem("All paired cameras");
    talkLayout->addWidget(talkbackCameras);
    connect(talkbackCameras, qOverload<int>(&QComboBox::currentIndexChanged), this, [](int i){ vystrm_select_camera(i-1); });
    talk = new HoldButton("HOLD TO TALK"); talk->setMinimumHeight(48); talkLayout->addWidget(talk);
    auto *listen = new QLabel("● CAMERA REPLY LISTENER ACTIVE"); listen->setAlignment(Qt::AlignCenter); listen->setStyleSheet("background:#202a26;color:#35d07f;border-radius:9px;padding:9px;font-weight:700;"); talkLayout->addWidget(listen); talkLayout->addStretch(); tabs->addTab(talkPage, "TALKBACK");
    auto *settings = new QWidget; auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->addWidget(new QLabel("PAIR & TRANSPORT"));
    qr = new QLabel; qr->setObjectName("card"); qr->setAlignment(Qt::AlignCenter); qr->setWordWrap(true); qr->setText("Waiting for pairing code…"); qr->setMinimumHeight(155); settingsLayout->addWidget(qr);
    settingsLayout->addWidget(new QLabel("Wi-Fi discovery: ACTIVE")); settingsLayout->addStretch(); tabs->addTab(settings, "SETTINGS");
    auto *footer = new QLabel("UDP 45990 discovery  •  46010/46011 talkback"); footer->setAlignment(Qt::AlignCenter); footer->setStyleSheet("color:#687386;font-size:9px;"); root->addWidget(footer);
    root->addStretch();

    stack->addWidget(login);
    stack->addWidget(dashboard);
    stack->setCurrentWidget(login);
    connect(auth, &VystrmAuthManager::authenticatedChanged, this, [this, login, dashboard](bool signedIn) {
      stack->setCurrentWidget(signedIn ? dashboard : login);
      refreshEntitlementLabel();
    });
    connect(auth, &VystrmAuthManager::entitlementsChanged, this, &VyStreamDock::refreshEntitlementLabel);
    connect(logout, &QPushButton::clicked, auth, &VystrmAuthManager::signOut);

    auto *timer = new QTimer(this); connect(timer,&QTimer::timeout,this,&VyStreamDock::refresh); timer->start(750);
    refresh();
    auth->restoreSession();
  }
  ~VyStreamDock() override { if(g_vystrm_dock==this)g_vystrm_dock=nullptr; }

  bool chooseCameraNames(const QString &suggested,const QStringList &sceneNames,const QStringList &srtSourceNames,const QStringList &allSourceNames,QString &sceneResult,QString &sourceResult) {
    if(!rootLayout||setupActive)return false;setupActive=true;
    auto *card=new QFrame(this);card->setObjectName("setupCard");card->setStyleSheet("#setupCard{background:#20242c;border:1px solid #1769e0;border-radius:12px;padding:8px;}");
    auto *layout=new QVBoxLayout(card);auto *title=new QLabel(QString("SET UP NEW CAMERA · %1").arg(suggested));title->setStyleSheet("font-weight:800;color:#19b9ff;");layout->addWidget(title);
    auto *hint=new QLabel("Use an existing OBS scene and SRT source, or type a new name.");hint->setWordWrap(true);layout->addWidget(hint);
    layout->addWidget(new QLabel("Scene"));
    auto *sceneBox=new QComboBox;sceneBox->setEditable(true);sceneBox->addItems(sceneNames);sceneBox->setEditText(suggested+" Scene");layout->addWidget(sceneBox);
    layout->addWidget(new QLabel("SRT source"));
    auto *sourceBox=new QComboBox;sourceBox->setEditable(true);sourceBox->addItems(srtSourceNames);sourceBox->setEditText(suggested+"-Cam");layout->addWidget(sourceBox);
    auto *sourceHint=new QLabel(srtSourceNames.isEmpty()?"No existing SRT source found. Enter a new source name.":"Choose an existing SRT source or enter a new source name.");sourceHint->setWordWrap(true);sourceHint->setStyleSheet("color:#9fb1c8;");layout->addWidget(sourceHint);
    auto *error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#ff6b6b;font-weight:700;");layout->addWidget(error);
    auto *buttons=new QHBoxLayout;auto *cancel=new QPushButton("CANCEL");cancel->setObjectName("listen");auto *apply=new QPushButton("USE SELECTION / CREATE MISSING");buttons->addWidget(cancel);buttons->addWidget(apply);layout->addLayout(buttons);
    rootLayout->insertWidget(3,card);
    QEventLoop loop;bool accepted=false;
    connect(cancel,&QPushButton::clicked,&loop,&QEventLoop::quit);
    connect(apply,&QPushButton::clicked,&loop,[&]{
      const QString scene=sceneBox->currentText().trimmed(),source=sourceBox->currentText().trimmed();
      if(scene.isEmpty()||source.isEmpty()){error->setText("Select an existing item or enter a new name in both fields.");return;}
      if(scene.compare(source,Qt::CaseInsensitive)==0){error->setText("Scene and source names must be different.");return;}
      const bool sourceExists=allSourceNames.contains(source,Qt::CaseInsensitive);
      const bool existingSrt=srtSourceNames.contains(source,Qt::CaseInsensitive);
      if(sourceExists&&!existingSrt){error->setText("That name belongs to a non-SRT source. Choose an SRT source or type a different new name.");return;}
      sceneResult=scene;sourceResult=source;accepted=true;loop.quit();
    });
    loop.exec();rootLayout->removeWidget(card);delete card;setupActive=false;return accepted;
  }
private slots:
  void refresh() {
    const int count=vystrm_camera_count();
    if (count+1 != cameras->count()) {
      cameras->blockSignals(true); cameras->clear(); cameras->addItem("All connected cameras");
      for(int i=0;i<count;i++) cameras->addItem(QString::fromUtf8(vystrm_camera_name(i)));
      cameras->blockSignals(false);
    }
    if (count+1 != talkbackCameras->count()) {
      talkbackCameras->blockSignals(true); talkbackCameras->clear(); talkbackCameras->addItem("All paired cameras");
      for(int i=0;i<count;i++) talkbackCameras->addItem(QString::fromUtf8(vystrm_camera_name(i)));
      talkbackCameras->blockSignals(false);
    }
    const int selected = cameras->currentIndex() - 1;
    health->setText(selected >= 0 && selected < count ? QString::fromUtf8(vystrm_camera_health(selected)) : "Select a connected camera to view health.");
    status->setText(count ? QString("● %1 camera%2 connected").arg(count).arg(count==1?"":"s") : "● Waiting for VYSTREAM cameras");
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
  void refreshEntitlementLabel() {
    const auto value = auth->entitlements();
    planBadge->setText(QString("%1 · %2 CAMERA%3 · UP TO %4P")
      .arg(value.plan.toUpper())
      .arg(value.maxCameras)
      .arg(value.maxCameras == 1 ? "" : "S")
      .arg(value.maxHeight));
  }

  QSlider *addSlider(QVBoxLayout *layout, const QString &label, int min, int max, int value, const char *control) { auto *title=new QLabel(label); layout->addWidget(title); auto *s=new QSlider(Qt::Horizontal); s->setRange(min,max); s->setValue(value); layout->addWidget(s); connect(s,&QSlider::valueChanged,this,[this,control](int v){int i=cameras->currentIndex()-1;if(i>=0)vystrm_set_camera_control(i,control,std::to_string(v).c_str());}); return s; }
  QVBoxLayout *rootLayout{};
  bool setupActive=false;
  VystrmAuthManager *auth{};
  QStackedWidget *stack{};
  QLabel *planBadge{};
  QLabel *qr{},*status{},*health{}; QComboBox *cameras{},*talkbackCameras{}; HoldButton *talk{}; QString pairingPayload;
  QSlider *zoom{},*exposure{},*brightness{};
};

extern "C" bool vystrm_dock_choose_camera_names(const char *suggested,const char *scenes,const char *srt_sources,const char *all_sources,char *scene_out,int scene_size,char *source_out,int source_size) {
  if(!g_vystrm_dock||!scene_out||!source_out||scene_size<2||source_size<2)return false;
  QString sceneResult,sourceResult;bool accepted=false;
  const QString suggestedValue=QString::fromUtf8(suggested?suggested:"VYSTREAM Camera");
  const QStringList sceneNames=QString::fromUtf8(scenes?scenes:"").split('\n',Qt::SkipEmptyParts);
  const QStringList srtNames=QString::fromUtf8(srt_sources?srt_sources:"").split('\n',Qt::SkipEmptyParts);
  const QStringList allNames=QString::fromUtf8(all_sources?all_sources:"").split('\n',Qt::SkipEmptyParts);
  auto choose=[&]{accepted=g_vystrm_dock&&g_vystrm_dock->chooseCameraNames(suggestedValue,sceneNames,srtNames,allNames,sceneResult,sourceResult);};
  if(QThread::currentThread()==g_vystrm_dock->thread())choose();else QMetaObject::invokeMethod(g_vystrm_dock,choose,Qt::BlockingQueuedConnection);
  if(!accepted)return false;const QByteArray sceneUtf8=sceneResult.toUtf8(),sourceUtf8=sourceResult.toUtf8();
  std::snprintf(scene_out,static_cast<size_t>(scene_size),"%s",sceneUtf8.constData());std::snprintf(source_out,static_cast<size_t>(source_size),"%s",sourceUtf8.constData());return true;
}

static bool vystrm_dock_registered = false;

static void vystrm_frontend_event(enum obs_frontend_event event, void *) {
  if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || vystrm_dock_registered)
    return;

  auto *dock = new VyStreamDock;
  vystrm_dock_registered =
      obs_frontend_add_dock_by_id("vystrm-camera-dock", "VYSTREAM Camera", dock);
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

extern "C" void vystrm_unregister_dock(void) {
  obs_frontend_remove_event_callback(vystrm_frontend_event, nullptr);
  vystrm_dock_registered = false;
}

#include "vystrm-dock.moc"
