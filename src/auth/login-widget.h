#pragma once
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class VystrmAuthManager;

class VystrmLoginWidget final : public QWidget {
  Q_OBJECT
public:
  explicit VystrmLoginWidget(VystrmAuthManager *auth, QWidget *parent = nullptr);

private:
  VystrmAuthManager *auth_;
  QLineEdit *email_;
  QLineEdit *password_;
  QCheckBox *remember_;
  QPushButton *signIn_;
  QLabel *error_;
};
