#include "login-widget.h"
#include "auth-manager.h"

#include <QAction>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
QLabel *fieldLabel(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setObjectName("fieldLabel");
  return label;
}
void decorateEmail(QLineEdit *edit) {
  edit->addAction(QIcon(":/vystrm/mail.svg"), QLineEdit::LeadingPosition);
  edit->setInputMethodHints(Qt::ImhEmailCharactersOnly);
}
void decoratePassword(QLineEdit *edit) {
  edit->setEchoMode(QLineEdit::Password);
  edit->addAction(QIcon(":/vystrm/lock.svg"), QLineEdit::LeadingPosition);
  auto *reveal = edit->addAction(QIcon(":/vystrm/eye-open.svg"),
                                 QLineEdit::TrailingPosition);
  QObject::connect(reveal,&QAction::triggered,edit,[edit,reveal] {
    const bool hidden = edit->echoMode() == QLineEdit::Password;
    edit->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
    reveal->setIcon(QIcon(hidden ? ":/vystrm/eye-closed.svg"
                                 : ":/vystrm/eye-open.svg"));
  });
}
}

VystrmLoginWidget::VystrmLoginWidget(VystrmAuthManager *auth, QWidget *parent)
    : QWidget(parent), auth_(auth) {
  setObjectName("VystrmLogin");
  setMinimumWidth(260);
  setStyleSheet(R"CSS(
    #VystrmLogin, #loginContent, QScrollArea, QScrollArea > QWidget > QWidget,
    QStackedWidget, QWidget#authPage {
      background:qradialgradient(cx:.5,cy:.12,radius:1.1,
        stop:0 #102a48,stop:.38 #07192b,stop:1 #030b15); color:#f6f8ff;
    }
    QScrollBar:vertical { background:#061424; width:5px; }
    QScrollBar::handle:vertical { background:#245681; border-radius:2px; min-height:24px; }
    QLabel#wordmark { font-size:31px; font-weight:900; letter-spacing:2px; }
    QLabel#tagline { color:#a9bad0; font-size:9px; letter-spacing:2px; }
    QLabel#title { color:#fff; font-size:29px; font-weight:800; }
    QLabel#subtitle { color:#aabbd2; font-size:12px; }
    QLabel#fieldLabel { color:#f4f7fc; font-size:11px; font-weight:700; }
    QLabel#error { color:#ff7582; background:rgba(90,15,26,150);
      border:1px solid #963441; border-radius:8px; padding:8px; font-size:11px; }
    QLabel#success { color:#8ff0bd; background:rgba(12,72,48,150);
      border:1px solid #277e59; border-radius:8px; padding:8px; font-size:11px; }
    QLabel#feature { color:#eef5ff; font-size:10px; font-weight:700; }
    QLabel#footerTag { color:#9eafc6; font-size:9px; letter-spacing:2px; }
    QLineEdit { background:rgba(7,27,47,220); color:#fff; border:1px solid #2b67a0;
      border-radius:12px; padding:13px 10px; selection-background-color:#1976ff;
      min-height:22px; }
    QLineEdit:focus { border:1px solid #22adff; }
    QCheckBox { color:#eef4fc; font-size:11px; }
    QPushButton { border-radius:11px; padding:11px; font-weight:700; }
    QPushButton#primary { color:white; border:0; min-height:26px;
      background:qlineargradient(x1:0,y1:0,x2:1,y2:0,
        stop:0 #10c9ee,stop:.55 #075dff,stop:1 #ff7200); }
    QPushButton#provider { color:#899ab0; background:rgba(7,25,43,150);
      border:1px solid #263e58; min-height:25px; }
    QPushButton#link { color:#21a0ff; background:transparent; border:0; padding:4px; }
    QPushButton#language { color:#f5f7fb; background:rgba(7,25,43,190);
      border:1px solid #324c6a; border-radius:16px; padding:7px 12px; }
  )CSS");

  auto *page = new QVBoxLayout(this);
  page->setContentsMargins(0,0,0,0);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *content = new QWidget;
  content->setObjectName("loginContent");
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(28,20,28,24);
  layout->setSpacing(10);

  auto *languageRow = new QHBoxLayout;
  languageRow->addStretch();
  auto *language = new QPushButton("◎  English  ⌄");
  language->setObjectName("language");
  language->setToolTip("Language selection is coming soon");
  languageRow->addWidget(language);
  layout->addLayout(languageRow);

  auto *logo = new QLabel;
  logo->setAlignment(Qt::AlignCenter);
  logo->setPixmap(QPixmap(":/vystrm/vystrm_camera_icon.png")
      .scaled(92,92,Qt::KeepAspectRatio,Qt::SmoothTransformation));
  layout->addWidget(logo);

  auto *wordmark = new QLabel(
      "<span style='color:#19bfff'>VYS</span>"
      "<span style='color:#f7f9ff'>TREA</span>"
      "<span style='color:#ff7800'>M</span>");
  wordmark->setObjectName("wordmark");
  wordmark->setTextFormat(Qt::RichText);
  wordmark->setAlignment(Qt::AlignCenter);
  layout->addWidget(wordmark);

  auto *tagline = new QLabel("TURN ANY CAMERA INTO A BROADCAST CAMERA");
  tagline->setObjectName("tagline");
  tagline->setAlignment(Qt::AlignCenter);
  tagline->setWordWrap(true);
  layout->addWidget(tagline);
  layout->addSpacing(16);

  auto *title = new QLabel("Welcome Back");
  title->setObjectName("title");
  title->setAlignment(Qt::AlignCenter);
  layout->addWidget(title);
  auto *subtitle = new QLabel("Sign in to your VYSTREAM account");
  subtitle->setObjectName("subtitle");
  subtitle->setAlignment(Qt::AlignCenter);
  subtitle->setWordWrap(true);
  layout->addWidget(subtitle);
  layout->addSpacing(12);

  error_ = new QLabel;
  error_->setObjectName("error");
  error_->setWordWrap(true);
  error_->hide();
  layout->addWidget(error_);
  auto *success = new QLabel;
  success->setObjectName("success");
  success->setWordWrap(true);
  success->hide();
  layout->addWidget(success);

  auto *forms = new QStackedWidget;
  forms->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);

  // Sign in
  auto *loginPage = new QWidget;
  loginPage->setObjectName("authPage");
  auto *loginLayout = new QVBoxLayout(loginPage);
  loginLayout->setContentsMargins(0,0,0,0);
  loginLayout->setSpacing(9);
  loginLayout->addWidget(fieldLabel("Email Address",loginPage));
  email_ = new QLineEdit;
  email_->setPlaceholderText("you@example.com");
  decorateEmail(email_);
  // Restore only the last-used email. The password is never persisted.
  email_->setText(auth_->rememberedEmail());
  loginLayout->addWidget(email_);
  loginLayout->addWidget(fieldLabel("Password",loginPage));
  password_ = new QLineEdit;
  password_->setPlaceholderText("Enter your password");
  decoratePassword(password_);
  loginLayout->addWidget(password_);
  auto *options = new QHBoxLayout;
  remember_ = new QCheckBox("Remember email and keep me signed in");
  remember_->setToolTip("Stores your email and a secure refresh session. Your password is never saved.");
  remember_->setChecked(!email_->text().trimmed().isEmpty());
  auto *forgot = new QPushButton("Forgot password?");
  forgot->setObjectName("link");
  options->addWidget(remember_);
  options->addStretch();
  options->addWidget(forgot);
  loginLayout->addLayout(options);
  signIn_ = new QPushButton("Sign In  →");
  signIn_->setObjectName("primary");
  signIn_->setMinimumHeight(48);
  loginLayout->addWidget(signIn_);
  auto *orLabel = new QLabel("────────  OR  ────────");
  orLabel->setAlignment(Qt::AlignCenter);
  orLabel->setObjectName("subtitle");
  loginLayout->addWidget(orLabel);
  auto *providers = new QHBoxLayout;
  providers->setSpacing(10);
  auto *google = new QPushButton(QIcon(":/vystrm/google.svg"),"Google");
  auto *apple = new QPushButton(QIcon(":/vystrm/apple.svg"),"Apple");
  for(auto *button:{google,apple}) {
    button->setObjectName("provider");
    button->setIconSize(QSize(21,21));
    button->setMinimumHeight(46);
    button->setEnabled(false);
    button->setToolTip("Coming soon");
    providers->addWidget(button);
  }
  loginLayout->addLayout(providers);
  auto *create = new QPushButton("Don't have an account?  Create one");
  create->setObjectName("link");
  loginLayout->addWidget(create);
  forms->addWidget(loginPage);

  // Create account
  auto *signupPage = new QWidget;
  signupPage->setObjectName("authPage");
  auto *signupLayout = new QVBoxLayout(signupPage);
  signupLayout->setContentsMargins(0,0,0,0);
  signupLayout->setSpacing(9);
  signupLayout->addWidget(fieldLabel("Full Name",signupPage));
  auto *signupName = new QLineEdit;
  signupName->setPlaceholderText("Your name");
  signupLayout->addWidget(signupName);
  signupLayout->addWidget(fieldLabel("Email Address",signupPage));
  auto *signupEmail = new QLineEdit;
  signupEmail->setPlaceholderText("you@example.com");
  decorateEmail(signupEmail);
  signupLayout->addWidget(signupEmail);
  signupLayout->addWidget(fieldLabel("Password",signupPage));
  auto *signupPassword = new QLineEdit;
  signupPassword->setPlaceholderText("At least 10 characters");
  decoratePassword(signupPassword);
  signupLayout->addWidget(signupPassword);
  signupLayout->addWidget(fieldLabel("Confirm Password",signupPage));
  auto *signupConfirm = new QLineEdit;
  signupConfirm->setPlaceholderText("Repeat your password");
  decoratePassword(signupConfirm);
  signupLayout->addWidget(signupConfirm);
  auto *signupRemember = new QCheckBox("Keep me signed in");
  signupRemember->setChecked(true);
  signupLayout->addWidget(signupRemember);
  auto *signup = new QPushButton("Create Account  →");
  signup->setObjectName("primary");
  signup->setMinimumHeight(48);
  signupLayout->addWidget(signup);
  auto *backFromSignup = new QPushButton("Already have an account?  Sign in");
  backFromSignup->setObjectName("link");
  signupLayout->addWidget(backFromSignup);
  forms->addWidget(signupPage);

  // Request reset
  auto *forgotPage = new QWidget;
  forgotPage->setObjectName("authPage");
  auto *forgotLayout = new QVBoxLayout(forgotPage);
  forgotLayout->setContentsMargins(0,0,0,0);
  forgotLayout->setSpacing(9);
  forgotLayout->addWidget(fieldLabel("Account Email",forgotPage));
  auto *resetEmail = new QLineEdit;
  resetEmail->setPlaceholderText("you@example.com");
  decorateEmail(resetEmail);
  forgotLayout->addWidget(resetEmail);
  auto *requestReset = new QPushButton("Send Reset Token  →");
  requestReset->setObjectName("primary");
  requestReset->setMinimumHeight(48);
  forgotLayout->addWidget(requestReset);
  auto *haveToken = new QPushButton("I already have a reset token");
  haveToken->setObjectName("link");
  forgotLayout->addWidget(haveToken);
  auto *backFromForgot = new QPushButton("Back to sign in");
  backFromForgot->setObjectName("link");
  forgotLayout->addWidget(backFromForgot);
  forms->addWidget(forgotPage);

  // Reset password
  auto *resetPage = new QWidget;
  resetPage->setObjectName("authPage");
  auto *resetLayout = new QVBoxLayout(resetPage);
  resetLayout->setContentsMargins(0,0,0,0);
  resetLayout->setSpacing(9);
  resetLayout->addWidget(fieldLabel("Reset Token",resetPage));
  auto *resetToken = new QLineEdit;
  resetToken->setPlaceholderText("Paste the token from your email");
  resetLayout->addWidget(resetToken);
  resetLayout->addWidget(fieldLabel("New Password",resetPage));
  auto *newPassword = new QLineEdit;
  newPassword->setPlaceholderText("At least 10 characters");
  decoratePassword(newPassword);
  resetLayout->addWidget(newPassword);
  resetLayout->addWidget(fieldLabel("Confirm New Password",resetPage));
  auto *confirmNewPassword = new QLineEdit;
  confirmNewPassword->setPlaceholderText("Repeat your new password");
  decoratePassword(confirmNewPassword);
  resetLayout->addWidget(confirmNewPassword);
  auto *completeReset = new QPushButton("Update Password  →");
  completeReset->setObjectName("primary");
  completeReset->setMinimumHeight(48);
  resetLayout->addWidget(completeReset);
  auto *backFromReset = new QPushButton("Back to sign in");
  backFromReset->setObjectName("link");
  resetLayout->addWidget(backFromReset);
  forms->addWidget(resetPage);

  layout->addWidget(forms);

  auto *waves = new QLabel;
  waves->setPixmap(QPixmap(":/vystrm/waves.svg"));
  waves->setScaledContents(true);
  waves->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
  waves->setFixedHeight(64);
  waves->setMinimumWidth(0);
  layout->addWidget(waves);

  auto addFeature=[](QHBoxLayout *row,const QString &icon,const QString &text) {
    auto *box=new QVBoxLayout;
    auto *image=new QLabel;
    image->setAlignment(Qt::AlignCenter);
    image->setPixmap(QPixmap(icon).scaled(38,38,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    auto *caption=new QLabel(text);
    caption->setObjectName("feature");
    caption->setAlignment(Qt::AlignCenter);
    caption->setWordWrap(true);
    box->addWidget(image);
    box->addWidget(caption);
    row->addLayout(box,1);
  };
  auto *features=new QHBoxLayout;
  addFeature(features,":/vystrm/video.svg","Create\nContent");
  addFeature(features,":/vystrm/broadcast.svg","Stream\nAnywhere");
  addFeature(features,":/vystrm/users.svg","Bigger\nPossibilities");
  layout->addLayout(features);
  auto *footer=new QLabel(
      "SAME CAMERA.  <span style='color:#20baff'>BIGGER</span> "
      "<span style='color:#ff7b25'>POSSIBILITIES.</span>");
  footer->setObjectName("footerTag");
  footer->setTextFormat(Qt::RichText);
  footer->setAlignment(Qt::AlignCenter);
  footer->setWordWrap(true);
  layout->addWidget(footer);

  auto showForm=[=](int index,const QString &heading,const QString &copy) {
    forms->setCurrentIndex(index);
    title->setText(heading);
    subtitle->setText(copy);
    error_->hide();
    success->hide();
    scroll->verticalScrollBar()->setValue(0);
  };
  connect(create,&QPushButton::clicked,this,[=]{showForm(1,"Create Account","Start with one camera at 1280 × 720.");});
  connect(backFromSignup,&QPushButton::clicked,this,[=]{showForm(0,"Welcome Back","Sign in to your VYSTREAM account");});
  connect(forgot,&QPushButton::clicked,this,[=]{resetEmail->setText(email_->text());showForm(2,"Reset Password","Request a secure reset token inside VYSTREAM.");});
  connect(backFromForgot,&QPushButton::clicked,this,[=]{showForm(0,"Welcome Back","Sign in to your VYSTREAM account");});
  connect(haveToken,&QPushButton::clicked,this,[=]{showForm(3,"Enter Reset Token","Choose a new password for your account.");});
  connect(backFromReset,&QPushButton::clicked,this,[=]{showForm(0,"Welcome Back","Sign in to your VYSTREAM account");});

  connect(signIn_,&QPushButton::clicked,this,[this] {
    error_->hide();
    auth_->signIn(email_->text(),password_->text(),remember_->isChecked());
  });
  connect(password_,&QLineEdit::returnPressed,signIn_,&QPushButton::click);
  connect(signup,&QPushButton::clicked,this,[=] {
    error_->hide();
    if(signupPassword->text()!=signupConfirm->text()) {
      error_->setText("The passwords do not match.");
      error_->show();
      return;
    }
    auth_->registerAccount(signupName->text(),signupEmail->text(),
                           signupPassword->text(),signupRemember->isChecked());
  });
  connect(requestReset,&QPushButton::clicked,this,[=] {
    error_->hide();
    auth_->requestPasswordReset(resetEmail->text());
  });
  connect(completeReset,&QPushButton::clicked,this,[=] {
    error_->hide();
    if(newPassword->text()!=confirmNewPassword->text()) {
      error_->setText("The passwords do not match.");
      error_->show();
      return;
    }
    auth_->resetPassword(resetToken->text(),newPassword->text());
  });

  connect(auth_,&VystrmAuthManager::passwordResetRequested,this,[=] {
    showForm(3,"Check Your Email","Enter the reset token sent to your email.");
    success->setText("Reset requested. Check your email for the token.");
    success->show();
  });
  connect(auth_,&VystrmAuthManager::passwordResetCompleted,this,[=] {
    showForm(0,"Password Updated","You can now sign in with your new password.");
    success->setText("Your password was updated successfully.");
    success->show();
  });
  connect(auth_,&VystrmAuthManager::busyChanged,this,[=](bool busy) {
    for(auto *button:{signIn_,signup,requestReset,completeReset})
      button->setEnabled(!busy);
    signIn_->setText(busy ? "Please wait…" : "Sign In  →");
  });
  connect(auth_,&VystrmAuthManager::errorOccurred,this,[this](const QString &message) {
    error_->setText(message);
    error_->show();
  });

  scroll->setWidget(content);
  page->addWidget(scroll);
}
