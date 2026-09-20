#include <obs-frontend-api.h>
#include <obs.h>
#include <QApplication>
#include <QCheckBox>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QSizePolicy>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QTextStream>
#include <QRegularExpression>
#include <string>
#include "../qr_v4.h"
#include "../auth/auth-manager.h"
#include "../auth/login-widget.h"
#include "../bible/bible-assistant.h"

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
void vystrm_send_tally_states(void);
bool vystrm_apply_bible_to_scene(bool preview, const QString &reference,
                                  const QString &text, const QString &skin,
                                  int opacity);
bool vystrm_clear_bible_from_scenes(void);
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
static bool append_obs_audio_source(void *data, obs_source_t *source) {
  auto *combo = static_cast<QComboBox *>(data);
  const char *name = obs_source_get_name(source);
  if (!combo || !name || !*name) return true;
  if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0 &&
      combo->findText(QString::fromUtf8(name)) < 0) {
    combo->addItem(QString::fromUtf8(name));
  }
  return true;
}


class VyStreamDock final : public QWidget {
  Q_OBJECT
public:
  VyStreamDock() {
    g_vystrm_dock = this;
    setObjectName("VyStreamCameraDock");
    setMinimumWidth(340);
    setMaximumWidth(366);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setStyleSheet(R"CSS(
      #VyStreamCameraDock { background:#171b22; color:#f5f7fb; }
      QLabel#brand { color:#f7f9ff; font-size:19px; font-weight:800; letter-spacing:2px; }
      QLabel#sub { color:#8d97a8; font-size:9px; letter-spacing:1px; }
      QLabel#card { background:#202733; border:1px solid #344257; border-radius:10px; padding:8px; }
      QTabWidget::pane { border:1px solid #344257; background:#151a21; }
      QTabBar::tab { background:#202733; color:#d9e5f4; border:1px solid #344257; padding:7px 9px; }
      QTabBar::tab:selected { background:#1769e0; color:#fff; }
      QComboBox, QLineEdit { background:#0f2236; border:1px solid #2e6fa9; border-radius:7px; padding:6px; color:#fff; }
      QComboBox { font-size:10px; }
      QPushButton { background:#1769e0; border:0; border-radius:8px; padding:8px; color:white; font-weight:700; }
      QPushButton[talking="true"] { background:#e3473f; }
      QPushButton#listen { background:#202b3a; border:1px solid #41617f; }
      QCheckBox { color:#eef5ff; }
      QSlider::groove:horizontal { height:4px; background:#2b3b50; border-radius:2px; }
      QSlider::handle:horizontal { width:12px; margin:-5px 0; border-radius:6px; background:#19b9ff; }
    )CSS");
    auth = new VystrmAuthManager(this);
    bibleNetwork = new QNetworkAccessManager(this);
    stack = new QStackedWidget(this);
#ifndef __APPLE__
    auto *login = new VystrmLoginWidget(auth, stack);
#endif
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
    overviewLayout->setContentsMargins(6,6,6,6); overviewLayout->setSpacing(6);
    auto *production = new QLabel("PRODUCTION OVERVIEW");
    production->setStyleSheet("font-weight:800;font-size:13px;");
    overviewLayout->addWidget(production);

    auto *bibleCard = new QFrame; bibleCard->setObjectName("bibleCard");
    bibleCard->setStyleSheet("#bibleCard{background:#20242c;border:1px solid #303642;border-radius:10px;padding:6px;}");
    auto *bibleLayout = new QVBoxLayout(bibleCard); bibleLayout->setContentsMargins(7,7,7,7); bibleLayout->setSpacing(5);
    auto *bibleHeader = new QHBoxLayout;
    auto *bibleTitle = new QLabel("📖  BIBLE LIVE ASSISTANT");
    bibleTitle->setStyleSheet("font-weight:800;color:#f5f7fb;");
    bibleTranslation = new QComboBox;
    bibleTranslation->setToolTip("Bible text and translation");
    bibleTranslation->setMaximumWidth(112);
    for (const auto &translation : bible.translations())
      bibleTranslation->addItem(translation.label, translation.code);
    bibleSkin = new QComboBox;
    bibleSkin->setToolTip("Choose the lower-third verse graphic skin");
    bibleSkin->setMinimumWidth(132);
    bibleSkin->setMaximumWidth(156);
    for (const auto &skin : bible.skins())
      bibleSkin->addItem(skin.label, skin.code);
    bibleHeader->addWidget(bibleTitle);
    bibleHeader->addStretch();
    bibleHeader->addWidget(bibleTranslation);
    bibleLayout->addLayout(bibleHeader);

    auto *skinRow = new QHBoxLayout;
    auto *skinLabel = new QLabel("VERSE SKIN");
    skinLabel->setStyleSheet("color:#9fb1c8;font-size:10px;font-weight:700;");
    skinRow->addWidget(skinLabel);
    skinRow->addStretch();
    skinRow->addWidget(bibleSkin);
    bibleLayout->addLayout(skinRow);

    auto *listenerRow = new QHBoxLayout;
    bibleListener = new QCheckBox("Live Listener");
    bibleListener->setToolTip("Listen to the selected OBS audio source for Bible references.");
    bibleListenerStatus = new QLabel("● Listener Off");
    bibleListenerStatus->setStyleSheet("color:#8d97a8;font-size:10px;");
    listenerRow->addWidget(bibleListener); listenerRow->addStretch(); listenerRow->addWidget(bibleListenerStatus);
    bibleLayout->addLayout(listenerRow);

    auto *audioLabel = new QLabel("Audio Source");
    audioLabel->setStyleSheet("color:#9fb1c8;font-size:10px;");
    bibleLayout->addWidget(audioLabel);
    bibleAudioSource = new QComboBox;
    bibleAudioSource->addItems({"Mic/Aux (Default)", "Desktop Audio"});
    obs_enum_sources(append_obs_audio_source, bibleAudioSource);
    bibleLayout->addWidget(bibleAudioSource);

    bibleHeard = new QLabel("Heard: waiting for speech…");
    bibleHeard->setWordWrap(true); bibleHeard->setStyleSheet("color:#9fb1c8;font-size:10px;");
    bibleLayout->addWidget(bibleHeard);

    bibleReference = new QLabel("No reference staged");
    bibleReference->setStyleSheet("font-size:16px;font-weight:800;color:#fff;");
    bibleLayout->addWidget(bibleReference);
    bibleText = new QLabel("Detected references will appear here. Nothing is sent to Program automatically.");
    bibleText->setWordWrap(true); bibleText->setObjectName("card");
    bibleText->setStyleSheet("color:#d8e2f1;background:#242a34;border:1px solid #394150;border-radius:7px;padding:7px;");
    bibleLayout->addWidget(bibleText);

    auto *bibleButtons = new QHBoxLayout;
    biblePreview = new QPushButton("PREVIEW"); biblePreview->setObjectName("listen");
    biblePush = new QPushButton("PUSH TO PROGRAM"); biblePush->setEnabled(false);
    bibleClear = new QPushButton("CLEAR"); bibleClear->setObjectName("listen");
    bibleButtons->addWidget(biblePreview); bibleButtons->addWidget(biblePush); bibleButtons->addWidget(bibleClear);
    bibleLayout->addLayout(bibleButtons);

    auto *findLabel = new QLabel("Find a verse"); findLabel->setStyleSheet("color:#9fb1c8;font-size:10px;");
    bibleLayout->addWidget(findLabel);
    auto *findRow = new QHBoxLayout;
    bibleSearch = new QLineEdit; bibleSearch->setPlaceholderText("John 3:16, Johanu 3:16, Romans 8:28");
    auto *findButton = new QPushButton("→"); findButton->setMaximumWidth(36);
    findRow->addWidget(bibleSearch); findRow->addWidget(findButton); bibleLayout->addLayout(findRow);

    bibleRecent = new QLabel("Recent: —");
    bibleRecent->setWordWrap(true); bibleRecent->setStyleSheet("color:#9fb1c8;font-size:10px;");
    bibleLayout->addWidget(bibleRecent);
    overviewLayout->addWidget(bibleCard);
    auto *overviewStatus = new QLabel("Camera: Waiting  •  Talkback: Ready");
    overviewStatus->setStyleSheet("color:#8d97a8;font-size:10px;");
    overviewLayout->addWidget(overviewStatus);
    overviewLayout->addStretch();
    tabs->addTab(overview, "OVERVIEW");

    auto stageBibleQuery = [this](const QString &query) {
      const QString code = bibleTranslation->currentData().toString();
      stagedBibleVerse = bible.lookup(query, code);
      bibleHeard->setText(QString("Heard: \"%1\"").arg(query.trimmed()));
      renderStagedBible();
      if (stagedBibleVerse.referenceRecognized &&
          !stagedBibleVerse.textAvailable && code != "kjv")
        fetchBibleVerse(code);
    };
    connect(findButton, &QPushButton::clicked, this, [this, stageBibleQuery] { stageBibleQuery(bibleSearch->text()); });
    connect(bibleSearch, &QLineEdit::returnPressed, this, [this, stageBibleQuery] { stageBibleQuery(bibleSearch->text()); });
    connect(bibleListener, &QCheckBox::toggled, this, [this](bool enabled) {
      const QString source = bibleAudioSource->currentText();
      bibleListenerStatus->setText(enabled ? QString("● Listening — %1").arg(source) : "● Listener Off");
      bibleListenerStatus->setStyleSheet(enabled ? "color:#35d07f;font-size:10px;" : "color:#8d97a8;font-size:10px;");
      if (!enabled) bibleHeard->setText("Heard: listener stopped");
    });
    connect(bibleAudioSource, &QComboBox::currentTextChanged, this, [this](const QString &source) {
      if (bibleListener->isChecked()) bibleListenerStatus->setText(QString("● Listening — %1").arg(source));
    });
    connect(bibleTranslation, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, stageBibleQuery](int index) {
      if (bibleSettingsTranslation && bibleSettingsTranslation->currentIndex() != index)
        bibleSettingsTranslation->setCurrentIndex(index);
      if (!stagedBibleVerse.reference.isEmpty()) stageBibleQuery(stagedBibleVerse.reference);
    });
    connect(biblePreview, &QPushButton::clicked, this, [this] {
      if (!stagedBibleVerse.textAvailable) return;
      if (vystrm_apply_bible_to_scene(true, stagedBibleVerse.reference,
                                   stagedBibleVerse.text,
                                   bibleSkin->currentData().toString(),
                                   bibleOpacity ? bibleOpacity->value() : 100))
        bibleListenerStatus->setText(QString("● Preview staged — %1").arg(stagedBibleVerse.reference.toUpper()));
      else
        bibleListenerStatus->setText(QString("● Staged — preview scene unavailable — %1").arg(stagedBibleVerse.reference.toUpper()));
    });
    connect(biblePush, &QPushButton::clicked, this, [this] {
      if (!stagedBibleVerse.textAvailable) return;
      if (vystrm_apply_bible_to_scene(false, stagedBibleVerse.reference,
                                    stagedBibleVerse.text,
                                    bibleSkin->currentData().toString(),
                                    bibleOpacity ? bibleOpacity->value() : 100))
        bibleListenerStatus->setText(QString("● Pushed to Program — %1").arg(stagedBibleVerse.reference.toUpper()));
      else
        bibleListenerStatus->setText("● Program graphic source unavailable");
    });
    connect(bibleClear, &QPushButton::clicked, this, [this] {
      const bool removed = vystrm_clear_bible_from_scenes();
      stagedBibleVerse = {};
      bibleReference->setText("No reference staged");
      bibleText->setText("Cleared from Preview and Program. Nothing is sent live automatically.");
      bibleHeard->setText("Heard: waiting for speech…");
      biblePush->setEnabled(false);
      bibleListenerStatus->setText(removed
        ? "● Cleared from Preview and Program"
        : "● Cleared — no verse graphic was active");
    });
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
    settingsLayout->addWidget(new QLabel("BIBLE LANGUAGE / TRANSLATION"));
    bibleSettingsTranslation = new QComboBox;
    for (const auto &translation : bible.translations())
      bibleSettingsTranslation->addItem(translation.label, translation.code);
    settingsLayout->addWidget(bibleSettingsTranslation);
    settingsLayout->addWidget(new QLabel("VERSE GRAPHIC SKIN"));
    bibleSettingsSkin = new QComboBox;
    for (const auto &skin : bible.skins())
      bibleSettingsSkin->addItem(skin.label, skin.code);
    settingsLayout->addWidget(bibleSettingsSkin);
    settingsLayout->addWidget(new QLabel("VERSE GRAPHIC OPACITY"));
    bibleOpacity = new QSlider(Qt::Horizontal);
    bibleOpacity->setRange(25, 100);
    bibleOpacity->setValue(100);
    settingsLayout->addWidget(bibleOpacity);
    settingsLayout->addWidget(new QLabel("PAIR & TRANSPORT"));
    qr = new QLabel; qr->setObjectName("card"); qr->setAlignment(Qt::AlignCenter); qr->setWordWrap(true); qr->setText("Waiting for pairing code…"); qr->setMinimumHeight(155); settingsLayout->addWidget(qr);
    settingsLayout->addWidget(new QLabel("Wi-Fi discovery: ACTIVE")); settingsLayout->addStretch(); tabs->addTab(settings, "SETTINGS");
    connect(bibleSettingsTranslation, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
      if (bibleTranslation && bibleTranslation->currentIndex() != index)
        bibleTranslation->setCurrentIndex(index);
    });
    connect(bibleSettingsSkin, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
              if (bibleSkin && bibleSkin->currentIndex() != index)
                bibleSkin->setCurrentIndex(index);
            });
    connect(bibleSkin, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
              if (bibleSettingsSkin && bibleSettingsSkin->currentIndex() != index)
                bibleSettingsSkin->setCurrentIndex(index);
            });
    auto *footer = new QLabel("UDP 45990 discovery  •  46010/46011 talkback"); footer->setAlignment(Qt::AlignCenter); footer->setStyleSheet("color:#687386;font-size:9px;"); root->addWidget(footer);
    root->addStretch();

#ifndef __APPLE__
    stack->addWidget(login);
    stack->addWidget(dashboard);
    stack->setCurrentWidget(login);
    connect(auth, &VystrmAuthManager::authenticatedChanged, this, [this, login, dashboard](bool signedIn) {
      stack->setCurrentWidget(signedIn ? dashboard : login);
      refreshEntitlementLabel();
    });
#else
    // macOS receives the authentication/session backend now, but its login
    // screen is intentionally deferred. Restore any saved session silently.
    stack->addWidget(dashboard);
    stack->setCurrentWidget(dashboard);
    connect(auth, &VystrmAuthManager::authenticatedChanged, this, [this, dashboard](bool) {
      stack->setCurrentWidget(dashboard);
      refreshEntitlementLabel();
    });
#endif
    connect(auth, &VystrmAuthManager::entitlementsChanged, this, &VyStreamDock::refreshEntitlementLabel);
    connect(logout, &QPushButton::clicked, auth, &VystrmAuthManager::signOut);

    auto *timer = new QTimer(this); connect(timer,&QTimer::timeout,this,&VyStreamDock::refresh); timer->start(750);
    refresh();
    // Defer the first refresh until OBS has returned to its event loop. OBS
    // owns the dock lifecycle, but it does not own plugin authentication state.
    QTimer::singleShot(0, auth, &VystrmAuthManager::restoreSession);
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
public:
  void handleBibleTranscript(const QString &transcript) {
    const QString code = bibleTranslation ? bibleTranslation->currentData().toString() : "bsb";
    stagedBibleVerse = bible.lookup(transcript, code);
    if (bibleHeard) bibleHeard->setText(QString("Heard: \"%1\"").arg(transcript));
    renderStagedBible();
    if (stagedBibleVerse.referenceRecognized &&
        !stagedBibleVerse.textAvailable && code != "kjv")
      fetchBibleVerse(code);
  }

  void renderStagedBible() {
    if (!bibleReference || !bibleText) return;
    if (!stagedBibleVerse.referenceRecognized) {
      bibleReference->setText("REFERENCE NOT RECOGNIZED");
      bibleText->setText("Try John 3:16, Johanu 3:16, Romans 8:28, or Orin Dafidi 23:1.");
      if (biblePush) biblePush->setEnabled(false);
      return;
    }
    const bool loading = !stagedBibleVerse.textAvailable &&
                         stagedBibleVerse.text.startsWith("Fetching");
    bibleReference->setText(stagedBibleVerse.reference.toUpper() +
                            (stagedBibleVerse.textAvailable ? "  •  READY"
                             : (loading ? "  •  FETCHING…" : "  •  PACK NEEDED")));
    bibleText->setText(stagedBibleVerse.text);
    if (biblePush) biblePush->setEnabled(stagedBibleVerse.textAvailable);
    if (!bibleRecentReferences.contains(stagedBibleVerse.reference)) {
      bibleRecentReferences.prepend(stagedBibleVerse.reference);
      while (bibleRecentReferences.size() > 5) bibleRecentReferences.removeLast();
    }
    if (bibleRecent) bibleRecent->setText("Recent: " + bibleRecentReferences.join("  •  "));
  }

  void fetchBibleVerse(const QString &translationCode) {
    if (!bibleNetwork || !stagedBibleVerse.referenceRecognized) return;
    const QString requestedReference = stagedBibleVerse.reference;
    const QString url = bible.helloAoChapterUrl(requestedReference, translationCode);
    if (url.isEmpty()) {
      stagedBibleVerse.text = "This reference is not available from the HelloAO book index.";
      renderStagedBible();
      return;
    }
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "VYSTRM Camera/3.0");
    QNetworkReply *reply = bibleNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, requestedReference, translationCode] {
              const QByteArray payload = reply->readAll();
              if (stagedBibleVerse.reference != requestedReference) {
                reply->deleteLater();
                return;
              }
              if (reply->error() != QNetworkReply::NoError) {
                stagedBibleVerse.text = "HelloAO could not be reached. Check the OBS connection and try again.";
                stagedBibleVerse.textAvailable = false;
                renderStagedBible();
                reply->deleteLater();
                return;
              }
              QJsonParseError parseError{};
              const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
              const QJsonObject root = document.object();
              const QJsonArray content =
                  root.value("chapter").toObject().value("content").toArray();
              const QRegularExpression referencePattern(
                  "^.+?\\s+(\\d+):([0-9]+)(?:-([0-9]+))?$");
              const auto match = referencePattern.match(requestedReference);
              const int firstVerse = match.hasMatch() ? match.captured(2).toInt() : 0;
              const int lastVerse = match.hasMatch() && !match.captured(3).isEmpty()
                                        ? match.captured(3).toInt()
                                        : firstVerse;
              QStringList verseText;
              for (const auto &value : content) {
                const QJsonObject item = value.toObject();
                if (item.value("type").toString() != "verse") continue;
                const int number = item.value("number").toInt();
                if (number >= firstVerse && number <= lastVerse)
                  verseText << item.value("text").toString().trimmed();
              }
              if (verseText.isEmpty()) {
                stagedBibleVerse.text = "The selected verse was not found in this translation.";
                stagedBibleVerse.textAvailable = false;
              } else {
                stagedBibleVerse.text = verseText.join(" ");
                stagedBibleVerse.textAvailable = true;
                stagedBibleVerse.translation = translationCode;
              }
              renderStagedBible();
              reply->deleteLater();
            });
  }
private slots:
  void refresh() {
    // Tally reads OBS scene state and therefore must run on the dock/UI thread.
    // Polling here keeps program/preview state live without worker-thread OBS calls.
    vystrm_send_tally_states();
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
  QLabel *qr{},*status{},*health{};
  QComboBox *cameras{},*talkbackCameras{},*bibleTranslation{},*bibleSettingsTranslation{},
            *bibleAudioSource{},*bibleSkin{},*bibleSettingsSkin{};
  QCheckBox *bibleListener{};
  QLineEdit *bibleSearch{};
  QLabel *bibleListenerStatus{},*bibleHeard{},*bibleReference{},*bibleText{},*bibleRecent{};
  QPushButton *biblePreview{},*biblePush{},*bibleClear{};
  QSlider *bibleOpacity{};
  QNetworkAccessManager *bibleNetwork{};
  HoldButton *talk{}; QString pairingPayload; QStringList bibleRecentReferences;
  VystrmBibleAssistant bible; VystrmBibleVerse stagedBibleVerse;
  QSlider *zoom{},*exposure{},*brightness{};
};

static QString bibleHtmlEscape(const QString &value) {
  return value.toHtmlEscaped().replace("\n", "<br/>");
}

static QString bibleSkinCss(const QString &skin) {
  const QString common = R"CSS(
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
    body{font-family:'Segoe UI',Arial,sans-serif;color:#fff}
    .stage{position:relative;width:100vw;height:100vh;display:flex;align-items:flex-end;
      box-sizing:border-box;padding:0 2.2vw 2.4vh;overflow:hidden}
    .panel{position:relative;width:100%;box-sizing:border-box;padding:1.8vh 3vw 2vh;
      border-radius:22px;overflow:hidden;isolation:isolate}
    .badge{display:inline-block;position:relative;z-index:2;padding:.55vh 2.1vw;
      margin-bottom:1.3vh;border-radius:999px;font-size:clamp(18px,2.1vw,44px);
      font-weight:800;letter-spacing:.04em}
    .verse{position:relative;z-index:2;font-size:clamp(20px,2.15vw,46px);line-height:1.25;
      font-weight:600;text-shadow:0 2px 4px rgba(0,0,0,.7)}
    .orb{position:absolute;border-radius:50%;filter:blur(20px);opacity:.8;pointer-events:none;
      animation:drift 12s ease-in-out infinite alternate}
    .orb-a{width:44vw;height:16vw;left:-8vw;bottom:-9vw;background:#ffb41f}
    .orb-b{width:35vw;height:12vw;right:-5vw;bottom:-7vw;background:#1ba8ff;animation-delay:-4s}
    @keyframes drift{from{transform:translate3d(-2%,0,0) rotate(-2deg)}
      to{transform:translate3d(3%,-5%,0) rotate(2deg)}}
  )CSS";
  QString skinCss;
  if (skin == "midnight-glass") {
    skinCss = R"CSS(
      .panel{background:rgba(8,15,29,.82);border:2px solid rgba(70,207,255,.72);
        box-shadow:0 0 28px rgba(0,178,255,.35),inset 0 0 34px rgba(31,93,143,.24)}
      .badge{background:rgba(28,184,255,.18);border:1px solid #42d5ff;color:#d8f8ff}
      .orb-a{background:#164c9c}.orb-b{background:#10d8ff}
    )CSS";
  } else if (skin == "blue-pulse") {
    skinCss = R"CSS(
      .panel{background:linear-gradient(115deg,rgba(3,23,60,.92),rgba(9,73,153,.78));
        border:2px solid rgba(25,185,255,.9);box-shadow:0 0 28px rgba(25,185,255,.38)}
      .panel:after{content:'';position:absolute;left:-10%;right:-10%;bottom:15%;
        height:3px;background:#27d8ff;box-shadow:0 0 16px #27d8ff;
        animation:pulse 3s ease-in-out infinite}
      .badge{background:#0d71db;color:#fff;box-shadow:0 0 12px rgba(15,152,255,.8)}
      @keyframes pulse{50%{transform:translateX(8%);opacity:.55}}
      .orb-a{background:#0655ff}.orb-b{background:#19e5ff}
    )CSS";
  } else if (skin == "royal-burgundy") {
    skinCss = R"CSS(
      .panel{background:linear-gradient(110deg,rgba(54,8,27,.94),rgba(117,25,42,.82));
        border:2px solid rgba(255,204,108,.82);box-shadow:0 0 28px rgba(255,104,24,.38)}
      .badge{background:#d99429;color:#32130b;border:1px solid #ffd276}
      .orb-a{background:#ff8d22}.orb-b{background:#c51e46}
    )CSS";
  } else if (skin == "clean-light") {
    skinCss = R"CSS(
      body{color:#132033}.verse{text-shadow:0 1px 2px rgba(255,255,255,.8)}
      .panel{background:rgba(247,250,255,.88);border:2px solid rgba(34,112,193,.75);
        box-shadow:0 5px 22px rgba(0,20,50,.3)}
      .badge{background:#f4b63d;color:#17243a}.verse{color:#132033}
      .orb-a{background:#ffd16b}.orb-b{background:#6fc8ff}
    )CSS";
  } else {
    skinCss = R"CSS(
      .panel{background:rgba(7,22,38,.76);border:2px solid rgba(255,185,53,.72);
        box-shadow:0 0 34px rgba(255,169,24,.34),inset 0 0 42px rgba(9,107,165,.24)}
      .panel:before{content:'';position:absolute;inset:-30% -5%;z-index:0;
        background:linear-gradient(105deg,transparent 20%,rgba(255,175,41,.7) 35%,
        transparent 47%,rgba(32,172,255,.7) 68%,transparent 81%);
        filter:blur(9px);transform:rotate(-2deg);animation:trails 8s ease-in-out infinite alternate}
      .badge{background:linear-gradient(105deg,#ffc44f,#f4a526);color:#10223d;
        border:1px solid #ffe19a;box-shadow:0 3px 12px rgba(255,172,29,.45)}
      .orb-a{background:#ffb21d}.orb-b{background:#168cff}
      @keyframes trails{from{transform:translateX(-10%) rotate(-2deg)}
        to{transform:translateX(10%) rotate(2deg)}}
    )CSS";
  }
  return common + skinCss;
}

static QString writeBibleGraphicHtml(const QString &reference, const QString &text,
                                     const QString &skin, int opacity) {
  QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (base.isEmpty()) base = QDir::tempPath();
  QDir directory(base + "/bible");
  directory.mkpath(".");
  const QString filePath = directory.filePath("vystream-bible-live.html");
  QString html = R"HTML(<!doctype html><html><head><meta charset="utf-8"><style>
{{CSS}}
</style></head><body><div class="stage"><div class="orb orb-a"></div>
<div class="orb orb-b"></div><div class="panel" style="opacity:{{OPACITY}}">
<div class="badge">{{REFERENCE}}</div><div class="verse">{{TEXT}}</div></div></div></body></html>)HTML";
  html.replace("{{CSS}}", bibleSkinCss(skin));
  html.replace("{{OPACITY}}", QString::number(qBound(25, opacity, 100) / 100.0, 'f', 2));
  html.replace("{{REFERENCE}}", bibleHtmlEscape(reference.toUpper()));
  html.replace("{{TEXT}}", bibleHtmlEscape(text));
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
  file.write(html.toUtf8());
  file.close();
  return QUrl::fromLocalFile(filePath).toString(QUrl::FullyEncoded);
}

extern "C" bool vystrm_clear_bible_from_scenes(void) {
  bool removed = false;
  const char *names[] = {"VYSTRM Bible — Preview", "VYSTRM Bible — Program"};
  obs_source_t *scenes[] = {
    obs_frontend_get_current_preview_scene(),
    obs_frontend_get_current_scene()
  };

  for (obs_source_t *sceneSource : scenes) {
    if (!sceneSource) continue;
    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    if (scene) {
      for (const char *name : names) {
        if (obs_sceneitem_t *item = obs_scene_find_source(scene, name)) {
          obs_sceneitem_remove(item);
          removed = true;
        }
      }
    }
    obs_source_release(sceneSource);
  }
  return removed;
}

extern "C" bool vystrm_apply_bible_to_scene(bool preview, const QString &reference,
                                             const QString &text, const QString &skin,
                                             int opacity) {
  obs_source_t *sceneSource = preview ? obs_frontend_get_current_preview_scene()
                                      : obs_frontend_get_current_scene();
  if (!sceneSource) return false;
  obs_scene_t *scene = obs_scene_from_source(sceneSource);
  if (!scene) { obs_source_release(sceneSource); return false; }

  obs_video_info videoInfo{};
  const bool haveVideoInfo = obs_get_video_info(&videoInfo);
  const int canvasWidth = haveVideoInfo ? int(videoInfo.base_width) : 1920;
  const int canvasHeight = haveVideoInfo ? int(videoInfo.base_height) : 1080;
  const QString sourceName = preview ? "VYSTRM Bible — Preview" : "VYSTRM Bible — Program";
  const QString url = writeBibleGraphicHtml(reference, text, skin, opacity);
  if (url.isEmpty()) { obs_source_release(sceneSource); return false; }

  obs_source_t *source = obs_get_source_by_name(sourceName.toUtf8().constData());
  if (!source) {
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "url", url.toUtf8().constData());
    obs_data_set_int(settings, "width", canvasWidth);
    obs_data_set_int(settings, "height", canvasHeight);
    obs_data_set_int(settings, "fps", 30);
    obs_data_set_bool(settings, "reroute_audio", false);
    source = obs_source_create("browser_source", sourceName.toUtf8().constData(),
                               settings, nullptr);
    obs_data_release(settings);
    if (!source) {
#ifdef _WIN32
      const char *fallbackType = "text_gdiplus";
#else
      const char *fallbackType = "text_ft2_source";
#endif
      obs_data_t *fallback = obs_data_create();
      const QString fallbackText = reference.toUpper() + "\n" + text;
      obs_data_set_string(fallback, "text", fallbackText.toUtf8().constData());
      source = obs_source_create(fallbackType, sourceName.toUtf8().constData(),
                                 fallback, nullptr);
      obs_data_release(fallback);
    }
    if (!source) { obs_source_release(sceneSource); return false; }
    obs_scene_add(scene, source);
  } else {
    obs_data_t *settings = obs_source_get_settings(source);
    const char *sourceId = obs_source_get_id(source);
    if (sourceId && QString::fromUtf8(sourceId) == "browser_source") {
      obs_data_set_string(settings, "url", url.toUtf8().constData());
      obs_data_set_int(settings, "width", canvasWidth);
      obs_data_set_int(settings, "height", canvasHeight);
      obs_data_set_int(settings, "fps", 30);
    } else {
      obs_data_set_string(settings, "text",
                          (reference.toUpper() + "\n" + text).toUtf8().constData());
    }
    obs_source_update(source, settings);
    obs_data_release(settings);
  }
  obs_source_release(source);
  obs_source_release(sceneSource);
  return true;
}

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

extern "C" void vystrm_bible_listener_transcript(const char *transcript) {
  if (!g_vystrm_dock || !transcript) return;
  const QString value = QString::fromUtf8(transcript);
  QMetaObject::invokeMethod(g_vystrm_dock, [value] {
    if (g_vystrm_dock) g_vystrm_dock->handleBibleTranscript(value);
  }, Qt::QueuedConnection);
}

#include "vystrm-dock.moc"
