#include "bible-assistant.h"

#include <QRegularExpression>

namespace {
struct BookAlias {
  const char *canonical;
  const char *aliases;
};

const BookAlias kBookAliases[] = {
    {"john", "john|johanu|yohanna"},
    {"romans", "romans|romu"},
    {"psalm", "psalm|psalms|orin dafidi|orin-dafidi"},
    {"1 corinthians", "1 corinthians|first corinthians|1 cor|1 korinti|korinti kini"},
    {"isaiah", "isaiah|isaya"},
};

const QHash<QString, QString> kNumberWords = {
    {"one", "1"},       {"two", "2"},       {"three", "3"},
    {"four", "4"},      {"five", "5"},      {"six", "6"},
    {"seven", "7"},     {"eight", "8"},     {"nine", "9"},
    {"ten", "10"},      {"eleven", "11"},  {"twelve", "12"},
    {"thirteen", "13"}, {"fourteen", "14"}, {"fifteen", "15"},
    {"sixteen", "16"},  {"seventeen", "17"}, {"eighteen", "18"},
    {"nineteen", "19"}, {"twenty", "20"},   {"twenty one", "21"},
    {"twenty two", "22"}, {"twenty three", "23"}, {"twenty four", "24"},
    {"twenty five", "25"}, {"twenty six", "26"}, {"twenty seven", "27"},
    {"twenty eight", "28"}, {"twenty nine", "29"}, {"thirty", "30"},
};

QString collapseSpaces(QString value) {
  value = value.toLower().trimmed();
  value.replace(QRegularExpression("[\\u2013\\u2014]"), "-");
  value.replace(QRegularExpression("[,;.!?]"), " ");
  value.replace(QRegularExpression("\\s+"), " ");
  return value;
}
} // namespace

VystrmBibleAssistant::VystrmBibleAssistant() {
  englishVerses_.insert(
      "john 3:16",
      "For God so loved the world, that he gave his only begotten Son, "
      "that whosoever believeth in him should not perish, but have "
      "everlasting life.");
  englishVerses_.insert(
      "romans 8:28",
      "And we know that all things work together for good to them that love "
      "God, to them who are the called according to his purpose.");
  englishVerses_.insert(
      "psalm 23:1",
      "The LORD is my shepherd; I shall not want.");
  englishVerses_.insert(
      "1 corinthians 13:4-7",
      "Charity suffereth long, and is kind; charity envieth not; charity "
      "vaunteth not itself, is not puffed up, doth not behave itself "
      "unseemly, seeketh not her own, is not easily provoked, thinketh no "
      "evil; rejoiceth not in iniquity, but rejoiceth in the truth; "
      "beareth all things, believeth all things, hopeth all things, "
      "endureth all things.");
  englishVerses_.insert(
      "isaiah 40:31",
      "But they that wait upon the LORD shall renew their strength; they "
      "shall mount up with wings as eagles; they shall run, and not be "
      "weary; and they shall walk, and not faint.");
}

QVector<VystrmBibleTranslation> VystrmBibleAssistant::translations() const {
  return {
      {"kjv", "KJV — English", "English", true},
      {"yoruba", "Yorùbá — Bíbélì Mímọ́", "Yorùbá", false},
      {"igbo", "Igbo — Bible pack", "Igbo", false},
      {"hausa", "Hausa — Bible pack", "Hausa", false},
      {"french", "Français — Bible pack", "Français", false},
      {"spanish", "Español — Bible pack", "Español", false},
  };
}

QString VystrmBibleAssistant::cleanBook(QString value) {
  value = collapseSpaces(value);
  value.replace("chapter", " ");
  value.replace("chap", " ");
  value.replace("verse", " ");
  value.replace("verses", " ");
  value.replace("ẹsẹ", " ");
  return collapseSpaces(value);
}

QString VystrmBibleAssistant::canonicalizeBook(QString value) {
  value = cleanBook(value);
  for (const auto &entry : kBookAliases) {
    const auto aliases = QString::fromUtf8(entry.aliases).split('|');
    for (const auto &alias : aliases) {
      if (value == alias || value.startsWith(alias + " ")) {
        return QString::fromUtf8(entry.canonical);
      }
    }
  }
  return value;
}

QString VystrmBibleAssistant::numberWordToDigits(QString value) {
  value = collapseSpaces(value);
  for (auto it = kNumberWords.constBegin(); it != kNumberWords.constEnd(); ++it) {
    value.replace(it.key(), it.value());
  }
  return collapseSpaces(value);
}

QString VystrmBibleAssistant::normalizeReference(const QString &query) const {
  QString value = numberWordToDigits(query);
  value.replace(QRegularExpression("\\bchapter\\b"), " ");
  value.replace(QRegularExpression("\\bchap\\b"), " ");
  value.replace(QRegularExpression("\\bverse[s]?\\b"), " ");
  value.replace(QRegularExpression("\\bẹsẹ\\b"), " ");
  value.replace(QRegularExpression("\\s+"), " ");
  value = value.trimmed();

  const QRegularExpression pattern(
      "^(.+?)\\s+(\\d+)\\s*[: ]\\s*(\\d+)(?:\\s*[- ]\\s*(\\d+))?$");
  const auto match = pattern.match(value);
  if (!match.hasMatch()) {
    return {};
  }

  const QString book = canonicalizeBook(match.captured(1));
  if (book.isEmpty()) {
    return {};
  }
  QString reference = QString("%1 %2:%3")
                          .arg(book)
                          .arg(match.captured(2))
                          .arg(match.captured(3));
  if (!match.captured(4).isEmpty()) {
    reference += "-" + match.captured(4);
  }
  return reference;
}

VystrmBibleVerse VystrmBibleAssistant::lookup(
    const QString &query, const QString &translationCode) const {
  const QString reference = normalizeReference(query);
  if (reference.isEmpty()) {
    return {};
  }

  VystrmBibleVerse verse;
  verse.reference = reference;
  verse.translation = translationCode;
  verse.referenceRecognized = true;

  if (translationCode == "kjv") {
    const auto it = englishVerses_.constFind(reference);
    if (it != englishVerses_.constEnd()) {
      verse.text = it.value();
      verse.textAvailable = true;
    }
    return verse;
  }

  verse.text = "This translation pack is not installed yet. Add the approved "
               "offline " +
               translationCode + " Bible pack in Bible Settings.";
  verse.textAvailable = false;
  return verse;
}
