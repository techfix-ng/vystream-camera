#include "auth-manager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <atomic>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#endif

namespace {
constexpr auto kApiBase = "https://vystream.techfixng.com/api/v1";
constexpr auto kCredentialTarget = L"VYSTRM OBS Plugin Refresh Token";
std::atomic<bool> g_authenticated{false};
std::atomic<int> g_maxCameras{1};
std::atomic<int> g_maxWidth{1280};
std::atomic<int> g_maxHeight{720};
std::atomic<qint64> g_validUntil{0};
std::mutex g_planMutex;
std::string g_plan{"free"};

void publish(const VystrmEntitlements &value, bool authenticated) {
  g_authenticated = authenticated;
  g_maxCameras = qMax(1, value.maxCameras);
  g_maxWidth = qMax(1280, value.maxWidth);
  g_maxHeight = qMax(720, value.maxHeight);
  g_validUntil = value.validUntil;
  std::lock_guard<std::mutex> lock(g_planMutex);
  g_plan = value.plan.toStdString();
}

QString networkErrorMessage(QNetworkReply *reply, const QString &fallback) {
  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray payload = reply->readAll();
  const QJsonDocument document = QJsonDocument::fromJson(payload);
  QString detail;
  if (document.isObject()) {
    const QJsonObject object = document.object();
    detail = object.value("error").toString();
    if (detail.isEmpty())
      detail = object.value("message").toString();
  }

  if (status == 401)
    return QStringLiteral("Incorrect email address or password.");
  if (status == 403)
    return QStringLiteral(
        "The VYSTREAM request was blocked by the server security service "
        "(HTTP 403). Allow the /api/v1 endpoint in Imunify360, then try again.");
  if (status == 409)
    return QStringLiteral("An account already exists for this email.");
  if (status == 422)
    return detail.isEmpty()
               ? QStringLiteral("Check the submitted information and try again.")
               : QStringLiteral("VYSTREAM rejected the request: %1").arg(detail);
  if (status == 429)
    return QStringLiteral(
        "Too many login attempts. Wait 15 minutes before trying again.");
  if (status > 0)
    return detail.isEmpty()
               ? QStringLiteral("VYSTREAM API returned HTTP %1.").arg(status)
               : QStringLiteral("VYSTREAM API returned HTTP %1: %2")
                     .arg(status)
                     .arg(detail.left(160));

  return QStringLiteral("Secure connection failed: %1")
      .arg(reply->errorString().isEmpty() ? fallback : reply->errorString());
}
}

VystrmAuthManager::VystrmAuthManager(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {
  publish(entitlements_, false);
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &VystrmAuthManager::refreshEntitlements);
  timer->start(5 * 60 * 1000);
}

bool VystrmAuthManager::isAuthenticated() const { return authenticated_; }
QString VystrmAuthManager::accountEmail() const { return email_; }
VystrmEntitlements VystrmAuthManager::entitlements() const { return entitlements_; }

void VystrmAuthManager::postJson(
    const QString &path, const QByteArray &body,
    const std::function<void(QNetworkReply *)> &handler) {
  QNetworkRequest request(QUrl(QString::fromLatin1(kApiBase) + path));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    "VYSTREAM-OBS-Plugin/3.0.1 (Windows; Qt)");
  request.setRawHeader("Accept", "application/json");
  request.setRawHeader("X-VYSTRM-Client", "obs-plugin/3.0.1/windows");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(20000);
  if (!accessToken_.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());

  busy_ = true;
  emit busyChanged(true);
  QNetworkReply *reply = network_->post(request, body);
  connect(reply, &QNetworkReply::finished, this, [this, reply, handler] {
    busy_ = false;
    emit busyChanged(false);
    handler(reply);
    reply->deleteLater();
  });
}

void VystrmAuthManager::signIn(const QString &email, const QString &password,
                               bool remember) {
  if (email.trimmed().isEmpty() || password.isEmpty()) {
    emit errorOccurred("Enter your email address and password.");
    return;
  }
  QJsonObject request{{"email", email.trimmed().toLower()},
                      {"password", password},
                      {"client", "obs-windows"},
                      {"remember", remember}};
  postJson("/auth/login", QJsonDocument(request).toJson(QJsonDocument::Compact),
           [this, remember](QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
      emit errorOccurred(
          networkErrorMessage(reply, QStringLiteral("Login request failed.")));
      return;
    }
    acceptSession(reply->readAll(), remember);
  });
}

void VystrmAuthManager::registerAccount(
    const QString &displayName, const QString &email,
    const QString &password, bool remember) {
  if (displayName.trimmed().isEmpty()) {
    emit errorOccurred("Enter your name.");
    return;
  }
  if (email.trimmed().isEmpty() || password.size() < 10) {
    emit errorOccurred("Enter a valid email and a password of at least 10 characters.");
    return;
  }
  QJsonObject request{{"display_name", displayName.trimmed()},
                      {"email", email.trimmed().toLower()},
                      {"password", password},
                      {"client", "obs-windows"}};
  postJson("/auth/register", QJsonDocument(request).toJson(QJsonDocument::Compact),
           [this, remember](QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
      emit errorOccurred(networkErrorMessage(
          reply, QStringLiteral("Account creation request failed.")));
      return;
    }
    acceptSession(reply->readAll(), remember);
  });
}

void VystrmAuthManager::requestPasswordReset(const QString &email) {
  if (email.trimmed().isEmpty()) {
    emit errorOccurred("Enter your account email address.");
    return;
  }
  QJsonObject request{{"email", email.trimmed().toLower()}};
  postJson("/auth/forgot-password",
           QJsonDocument(request).toJson(QJsonDocument::Compact),
           [this](QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
      emit errorOccurred("Unable to request a password reset.");
      return;
    }
    emit passwordResetRequested();
  });
}

void VystrmAuthManager::resetPassword(const QString &token,
                                      const QString &password) {
  if (token.trimmed().isEmpty() || password.size() < 10) {
    emit errorOccurred("Enter the reset token and a password of at least 10 characters.");
    return;
  }
  QJsonObject request{{"token", token.trimmed()}, {"password", password}};
  postJson("/auth/reset-password",
           QJsonDocument(request).toJson(QJsonDocument::Compact),
           [this](QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
      emit errorOccurred("That reset token is invalid or has expired.");
      return;
    }
    emit passwordResetCompleted();
  });
}

void VystrmAuthManager::restoreSession() {
  const QString token = storedRefreshToken();
  if (token.isEmpty()) return;
  QJsonObject request{{"refresh_token", token}, {"client", "obs-windows"}};
  postJson("/auth/refresh", QJsonDocument(request).toJson(QJsonDocument::Compact),
           [this](QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
      clearStoredRefreshToken();
      fallBackToFree();
      return;
    }
    acceptSession(reply->readAll(), true);
  });
}

void VystrmAuthManager::acceptSession(const QByteArray &payload, bool remember) {
  const auto document = QJsonDocument::fromJson(payload);
  if (!document.isObject()) {
    emit errorOccurred("The VYSTREAM service returned an invalid response.");
    return;
  }
  const QJsonObject object = document.object();
  accessToken_ = object.value("access_token").toString();
  const QString refreshToken = object.value("refresh_token").toString();
  email_ = object.value("user").toObject().value("email").toString();
  if (accessToken_.isEmpty()) {
    emit errorOccurred("The VYSTREAM service did not return a session.");
    return;
  }
  if (remember && !refreshToken.isEmpty()) storeRefreshToken(refreshToken);
  applyEntitlements(object.value("entitlements").toObject());
  authenticated_ = true;
  publish(entitlements_, true);
  emit authenticatedChanged(true);
}

void VystrmAuthManager::refreshEntitlements() {
  if (!authenticated_ || accessToken_.isEmpty()) return;
  postJson("/account/entitlements", "{}",
           [this](QNetworkReply *reply) {
    if (reply->error() == QNetworkReply::AuthenticationRequiredError) {
      restoreSession();
      return;
    }
    if (reply->error() != QNetworkReply::NoError) return;
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isObject()) return;
    applyEntitlements(document.object().value("entitlements").toObject());
    publish(entitlements_, true);
    emit entitlementsChanged();
  });
}

void VystrmAuthManager::applyEntitlements(const QJsonObject &object) {
  if (object.isEmpty()) {
    entitlements_ = {};
    return;
  }
  entitlements_.plan = object.value("plan").toString("free");
  entitlements_.maxCameras = qMax(1, object.value("max_cameras").toInt(1));
  entitlements_.maxWidth = qMax(1280, object.value("max_width").toInt(1280));
  entitlements_.maxHeight = qMax(720, object.value("max_height").toInt(720));
  entitlements_.validUntil = object.value("valid_until").toVariant().toLongLong();
}

void VystrmAuthManager::signOut() {
  if (!accessToken_.isEmpty()) postJson("/auth/logout", "{}", [](QNetworkReply *) {});
  clearStoredRefreshToken();
  accessToken_.clear();
  email_.clear();
  authenticated_ = false;
  fallBackToFree();
  emit authenticatedChanged(false);
}

void VystrmAuthManager::fallBackToFree() {
  entitlements_ = {};
  publish(entitlements_, false);
  emit entitlementsChanged();
}

QString VystrmAuthManager::storedRefreshToken() const {
#ifdef _WIN32
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) return {};
  const QString value = QString::fromUtf8(
      reinterpret_cast<const char *>(credential->CredentialBlob),
      static_cast<int>(credential->CredentialBlobSize));
  CredFree(credential);
  return value;
#else
  return {};
#endif
}

void VystrmAuthManager::storeRefreshToken(const QString &token) {
#ifdef _WIN32
  const QByteArray bytes = token.toUtf8();
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<LPWSTR>(kCredentialTarget);
  credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.data()));
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.UserName = const_cast<LPWSTR>(L"VYSTRM");
  CredWriteW(&credential, 0);
#endif
}

void VystrmAuthManager::clearStoredRefreshToken() {
#ifdef _WIN32
  CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0);
#endif
}

extern "C" bool vystrm_auth_is_authenticated() { return g_authenticated.load(); }
extern "C" int vystrm_auth_max_cameras() { return g_maxCameras.load(); }
extern "C" int vystrm_auth_max_width() { return g_maxWidth.load(); }
extern "C" int vystrm_auth_max_height() { return g_maxHeight.load(); }
extern "C" qint64 vystrm_auth_valid_until() { return g_validUntil.load(); }
extern "C" const char *vystrm_auth_plan() {
  thread_local std::string copy;
  std::lock_guard<std::mutex> lock(g_planMutex);
  copy = g_plan;
  return copy.c_str();
}
