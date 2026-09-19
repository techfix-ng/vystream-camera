#include "auth-manager.h"

#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDebug>
#include <QDateTime>
#include <QTextStream>
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
#include <wincrypt.h>
#endif

namespace {
constexpr auto kApiBase = "https://vystream.techfixng.com/api/v1";
constexpr auto kCredentialTarget = L"VYSTRM OBS Plugin Refresh Token";
QString sessionDirectory() {
  // Use OBS's stable plugin configuration directory. QStandardPaths::AppDataLocation
  // depends on the host application's runtime identity and may differ between
  // OBS launches or packaging variants.
  QString root = qEnvironmentVariable("APPDATA");
  if (root.isEmpty())
    root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  const QString directory =
      QDir::cleanPath(root + "/obs-studio/plugin_config/obs-srt-camera");
  QDir().mkpath(directory);
  return directory;
}
QString protectedTokenPath() {
  return sessionDirectory() + "/vystrm-session.bin";
}
void authLog(const QString &message) {
  QFile file(sessionDirectory() + "/auth-status.log");
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    return;
  QTextStream stream(&file);
  stream << QDateTime::currentDateTime().toString(Qt::ISODate)
         << "  " << message << "\n";
}
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
                    "VYSTREAM-OBS-Plugin/3.0.4 (Windows; Qt)");
  request.setRawHeader("Accept", "application/json");
  request.setRawHeader("X-VYSTRM-Client", "obs-plugin/3.0.4/windows");
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
  if (busy_) {
    QTimer::singleShot(1500, this, &VystrmAuthManager::restoreSession);
    return;
  }
  const QString token = storedRefreshToken();
  if (token.isEmpty()) {
    authLog("No saved refresh token was found.");
    return;
  }
  authLog("Saved refresh token found; requesting a new session.");
  QJsonObject request{{"refresh_token", token}, {"client", "obs-windows"}};
  postJson("/auth/refresh", QJsonDocument(request).toJson(QJsonDocument::Compact),
           [this](QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
      const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (status == 401) {
        authLog(QString("Session refresh returned HTTP 401 (attempt %1 of 3).")
                    .arg(restoreAttempts_ + 1));
        // A startup request can reach the server while a previous refresh
        // rotation is still settling. Never erase the only durable token on
        // the first rejection.
        if (++restoreAttempts_ < 3) {
          QTimer::singleShot(3000, this, &VystrmAuthManager::restoreSession);
          return;
        }
        authLog("Saved session was rejected three times; clearing it.");
        clearStoredRefreshToken();
        restoreAttempts_ = 0;
        fallBackToFree();
        return;
      }
      // Startup can briefly race Wi-Fi, DNS, TLS backend initialization or
      // server security. Preserve the valid token and retry instead of
      // turning a transient network problem into a forced login.
      authLog(QString("Session refresh failed (HTTP %1, network error %2).")
                  .arg(status).arg(static_cast<int>(reply->error())));
      if (++restoreAttempts_ <= 6)
        QTimer::singleShot(5000, this, &VystrmAuthManager::restoreSession);
      return;
    }
    restoreAttempts_ = 0;
    authLog("Session refresh succeeded.");
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
  // The plugin is expected to restore the account after OBS restarts. The
  // checkbox remains part of the UI, but a successful session always persists
  // the returned refresh token; only explicit Sign Out clears it.
  if (!refreshToken.isEmpty()) storeRefreshToken(refreshToken);
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
  if (CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) {
    const QString value = QString::fromUtf8(
        reinterpret_cast<const char *>(credential->CredentialBlob),
        static_cast<int>(credential->CredentialBlobSize));
    CredFree(credential);
    if (!value.isEmpty()) {
      authLog("Loaded refresh token from Windows Credential Manager.");
      return value;
    }
  }

  QFile file(protectedTokenPath());
  if (!file.open(QIODevice::ReadOnly)) {
    authLog("Encrypted session backup was not present.");
    return {};
  }
  const QByteArray encrypted = file.readAll();
  if (encrypted.isEmpty()) return {};
  DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                  reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()))};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    authLog(QString("Could not decrypt session backup (Windows error %1).")
                .arg(GetLastError()));
    return {};
  }
  const QString value = QString::fromUtf8(
      reinterpret_cast<const char *>(output.pbData),
      static_cast<int>(output.cbData));
  LocalFree(output.pbData);
  if (!value.isEmpty())
    authLog("Loaded refresh token from encrypted OBS plugin backup.");
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
  const bool credentialSaved = CredWriteW(&credential, 0);
  if (!credentialSaved) {
    const DWORD error = GetLastError();
    qWarning() << "VYSTREAM could not write Windows Credential Manager entry:" << error;
    authLog(QString("Credential Manager write failed (Windows error %1).").arg(error));
  } else {
    authLog("Refresh token saved to Windows Credential Manager.");
  }

  DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                  reinterpret_cast<BYTE *>(const_cast<char *>(bytes.constData()))};
  DATA_BLOB output{};
  if (CryptProtectData(&input, L"VYSTREAM OBS refresh token", nullptr, nullptr,
                       nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    QSaveFile file(protectedTokenPath());
    if (file.open(QIODevice::WriteOnly)) {
      file.write(reinterpret_cast<const char *>(output.pbData),
                 static_cast<qint64>(output.cbData));
      if (file.commit())
        authLog("Encrypted refresh-token backup saved to the OBS plugin directory.");
      else
        authLog("Encrypted refresh-token backup could not be committed.");
    } else {
      authLog("Encrypted refresh-token backup file could not be opened for writing.");
    }
    LocalFree(output.pbData);
  } else {
    const DWORD error = GetLastError();
    qWarning() << "VYSTREAM could not create DPAPI session backup:" << error;
    authLog(QString("DPAPI encryption failed (Windows error %1).").arg(error));
  }
#endif
}

void VystrmAuthManager::clearStoredRefreshToken() {
#ifdef _WIN32
  CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0);
  QFile::remove(protectedTokenPath());
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
