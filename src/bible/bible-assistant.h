#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct VystrmBibleTranslation {
  QString code;
  QString label;
  QString language;
  bool offlineAvailable = false;
};

struct VystrmBibleVerse {
  QString reference;
  QString text;
  QString translation;
  bool referenceRecognized = false;
  bool textAvailable = false;
};

class VystrmBibleAssistant final {
public:
  VystrmBibleAssistant();

  QVector<VystrmBibleTranslation> translations() const;
  VystrmBibleVerse lookup(const QString &query,
                          const QString &translationCode) const;
  QString normalizeReference(const QString &query) const;

private:
  static QString canonicalizeBook(QString value);
  static QString numberWordToDigits(QString value);
  static QString cleanBook(QString value);
  QHash<QString, QString> englishVerses_;
};
