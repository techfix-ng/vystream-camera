#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct VystrmBibleTranslation {
  QString code;
  QString label;
  QString language;
  QString apiId;
  QString licenseUrl;
  bool offlineAvailable = false;
};

struct VystrmBibleSkin {
  QString code;
  QString label;
  QString description;
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
  QVector<VystrmBibleSkin> skins() const;
  VystrmBibleVerse lookup(const QString &query,
                          const QString &translationCode) const;
  QString normalizeReference(const QString &query) const;

  // HelloAO publishes chapter JSON at this URL. The plugin requests only the
  // selected chapter and caches nothing until the operator has staged it.
  QString helloAoChapterUrl(const QString &reference,
                            const QString &translationCode) const;
  QString helloAoTranslationId(const QString &translationCode) const;

private:
  static QString canonicalizeBook(QString value);
  static QString numberWordToDigits(QString value);
  static QString cleanBook(QString value);
  static QString helloAoBookCode(const QString &canonicalBook);
  QHash<QString, QString> englishVerses_;
};
