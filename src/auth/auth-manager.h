#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QtGlobal>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

struct VystrmEntitlements {
  QString plan = "free";
  int maxCameras = 1;
  int maxWidth = 1280;
  int maxHeight = 720;
  qint64 validUntil = 0;
};

class VystrmAuthManager final : public QObject {
  Q_OBJECT
public:
  explicit VystrmAuthManager(QObject *parent = nullptr);
  bool isAuthenticated() const;
  QString accountEmail() const;
  VystrmEntitlements entitlements() const;

public slots:
  void signIn(const QString &email, const QString &password, bool remember);
  void registerAccount(const QString &displayName, const QString &email,
                       const QString &password, bool remember);
  void requestPasswordReset(const QString &email);
  void resetPassword(const QString &token, const QString &password);
  void restoreSession();
  void refreshEntitlements();
  void signOut();

signals:
  void busyChanged(bool busy);
  void authenticatedChanged(bool authenticated);
  void entitlementsChanged();
  void errorOccurred(const QString &message);
  void passwordResetRequested();
  void passwordResetCompleted();

private:
  void postJson(const QString &path, const QByteArray &body,
                const std::function<void(QNetworkReply *)> &handler);
  void acceptSession(const QByteArray &payload, bool remember);
  void applyEntitlements(const QJsonObject &object);
  QString storedRefreshToken() const;
  void storeRefreshToken(const QString &token);
  void clearStoredRefreshToken();
  void fallBackToFree();

  QNetworkAccessManager *network_;
  QString accessToken_;
  QString email_;
  VystrmEntitlements entitlements_;
  bool authenticated_ = false;
  bool busy_ = false;
  int restoreAttempts_ = 0;
};

extern "C" {
bool vystrm_auth_is_authenticated();
int vystrm_auth_max_cameras();
int vystrm_auth_max_width();
int vystrm_auth_max_height();
const char *vystrm_auth_plan();
qint64 vystrm_auth_valid_until();
}
