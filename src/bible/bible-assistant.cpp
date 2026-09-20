#include "bible-assistant.h"

#include <QRegularExpression>
#include <QUrl>

namespace {
struct BookAlias {
  const char *canonical;
  const char *aliases;
};

const BookAlias kBookAliases[] = {
    {"genesis", "genesis|gen|jẹnẹsisi"},
    {"exodus", "exodus|exo|eksodu"},
    {"leviticus", "leviticus|lev|lefítíkù"},
    {"numbers", "numbers|num|númérì"},
    {"deuteronomy", "deuteronomy|deu|diutarónómì"},
    {"joshua", "joshua|jos|jóṣua"},
    {"judges", "judges|jdg|àwọn onídàjọ́"},
    {"ruth", "ruth|rut|rutu"},
    {"1 samuel", "1 samuel|first samuel|1sa|1 sam"},
    {"2 samuel", "2 samuel|second samuel|2sa|2 sam"},
    {"1 kings", "1 kings|first kings|1ki|1 ọba"},
    {"2 kings", "2 kings|second kings|2ki|2 ọba"},
    {"1 chronicles", "1 chronicles|first chronicles|1ch"},
    {"2 chronicles", "2 chronicles|second chronicles|2ch"},
    {"ezra", "ezra|ezr|ẹ́sírà"},
    {"nehemiah", "nehemiah|neh|nehemáyà"},
    {"esther", "esther|est|ẹ́sítà"},
    {"job", "job|jóbù"},
    {"psalm", "psalm|psalms|psa|orin dafidi|orin-dafidi|ìwé orin"},
    {"proverbs", "proverbs|pro|owe|òwe"},
    {"ecclesiastes", "ecclesiastes|ecc|oniwaasu|oníwàásù"},
    {"song of solomon", "song of solomon|song|sng|orin solomoni"},
    {"isaiah", "isaiah|isa|isaya|ìsáyà"},
    {"jeremiah", "jeremiah|jer|jeremáyà"},
    {"lamentations", "lamentations|lam|ìkẹ́dùn"},
    {" ezekiel", "ezekiel|ezk|ẹ́síkíẹ́lì"},
    {"daniel", "daniel|dan|dáníẹ́lì"},
    {"hosea", "hosea|hos|hóséà"},
    {"joel", "joel|jol|jówẹ́lì"},
    {"amos", "amos|amo|ámọ́sì"},
    {"obadiah", "obadiah|oba|ọ́bádáyà"},
    {"jonah", "jonah|jon|jónà"},
    {"micah", "micah|mic|míkà"},
    {"nahum", "nahum|nam|náhúmù"},
    {"habakkuk", "habakkuk|hab|hábákúkù"},
    {"zephaniah", "zephaniah|zep|sófóníà"},
    {"haggai", "haggai|hag|hágáì"},
    {"zechariah", "zechariah|zec|sekaráyà"},
    {"malachi", "malachi|mal|málákì"},
    {"matthew", "matthew|mat|mátíù"},
    {"mark", "mark|mrk|mákù"},
    {"luke", "luke|luk|lúùkù"},
    {"john", "john|jhn|johanu|yohanna|jóhánù"},
    {"acts", "acts|act|ìṣe àwọn aposteli"},
    {"romans", "romans|rom|romu|àwọn romu"},
    {"1 corinthians", "1 corinthians|first corinthians|1co|1 cor|1 korinti|korinti kini"},
    {"2 corinthians", "2 corinthians|second corinthians|2co|2 korinti|korinti keji"},
    {"galatians", "galatians|gal|galatia"},
    {"ephesians", "ephesians|eph|efesu"},
    {"philippians", "philippians|php|filipi"},
    {"colossians", "colossians|col|kolosse"},
    {"1 thessalonians", "1 thessalonians|first thessalonians|1th"},
    {"2 thessalonians", "2 thessalonians|second thessalonians|2th"},
    {"1 timothy", "1 timothy|first timothy|1ti"},
    {"2 timothy", "2 timothy|second timothy|2ti"},
    {"titus", "titus|tit|titu"},
    {"philemon", "philemon|phm|filemoni"},
    {"hebrews", "hebrews|heb|hebreu"},
    {"james", "james|jas|jakọbu"},
    {"1 peter", "1 peter|first peter|1pe|1 pet"},
    {"2 peter", "2 peter|second peter|2pe|2 pet"},
    {"1 john", "1 john|first john|1jn|1 johanu"},
    {"2 john", "2 john|second john|2jn|2 johanu"},
    {"3 john", "3 john|third john|3jn|3 johanu"},
    {"jude", "jude|jud|juda"},
    {"revelation", "revelation|rev|ifihan|ìfihàn"},
};

const QHash<QString, QString> kNumberWords = {
    {"twenty eight", "28"}, {"twenty seven", "27"}, {"twenty six", "26"},
    {"twenty five", "25"},  {"twenty four", "24"},  {"twenty three", "23"},
    {"twenty two", "22"},   {"twenty one", "21"},   {"nineteen", "19"},
    {"eighteen", "18"},     {"seventeen", "17"},   {"sixteen", "16"},
    {"fifteen", "15"},      {"fourteen", "14"},    {"thirteen", "13"},
    {"twelve", "12"},       {"eleven", "11"},       {"ten", "10"},
    {"nine", "9"},          {"eight", "8"},         {"seven", "7"},
    {"six", "6"},           {"five", "5"},          {"four", "4"},
    {"three", "3"},         {"two", "2"},           {"one", "1"},
    {"thirty", "30"},
};

const QHash<QString, QString> kBookCodes = {
    {"genesis", "GEN"}, {"exodus", "EXO"}, {"leviticus", "LEV"},
    {"numbers", "NUM"}, {"deuteronomy", "DEU"}, {"joshua", "JOS"},
    {"judges", "JDG"}, {"ruth", "RUT"}, {"1 samuel", "1SA"},
    {"2 samuel", "2SA"}, {"1 kings", "1KI"}, {"2 kings", "2KI"},
    {"1 chronicles", "1CH"}, {"2 chronicles", "2CH"}, {"ezra", "EZR"},
    {"nehemiah", "NEH"}, {"esther", "EST"}, {"job", "JOB"},
    {"psalm", "PSA"}, {"proverbs", "PRO"}, {"ecclesiastes", "ECC"},
    {"song of solomon", "SNG"}, {"isaiah", "ISA"}, {"jeremiah", "JER"},
    {"lamentations", "LAM"}, {"ezekiel", "EZK"}, {"daniel", "DAN"},
    {"hosea", "HOS"}, {"joel", "JOL"}, {"amos", "AMO"}, {"obadiah", "OBA"},
    {"jonah", "JON"}, {"micah", "MIC"}, {"nahum", "NAM"},
    {"habakkuk", "HAB"}, {"zephaniah", "ZEP"}, {"haggai", "HAG"},
    {"zechariah", "ZEC"}, {"malachi", "MAL"}, {"matthew", "MAT"},
    {"mark", "MRK"}, {"luke", "LUK"}, {"john", "JHN"}, {"acts", "ACT"},
    {"romans", "ROM"}, {"1 corinthians", "1CO"}, {"2 corinthians", "2CO"},
    {"galatians", "GAL"}, {"ephesians", "EPH"}, {"philippians", "PHP"},
    {"colossians", "COL"}, {"1 thessalonians", "1TH"},
    {"2 thessalonians", "2TH"}, {"1 timothy", "1TI"}, {"2 timothy", "2TI"},
    {"titus", "TIT"}, {"philemon", "PHM"}, {"hebrews", "HEB"},
    {"james", "JAS"}, {"1 peter", "1PE"}, {"2 peter", "2PE"},
    {"1 john", "1JN"}, {"2 john", "2JN"}, {"3 john", "3JN"},
    {"jude", "JUD"}, {"revelation", "REV"},
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
  // Keep a small public-domain fallback for offline use. All selectable
  // translations below are sourced from HelloAO at lookup time.
  englishVerses_.insert(
      "john 3:16",
      "For God so loved the world, that he gave his only begotten Son, "
      "that whosoever believeth in him should not perish, but have "
      "everlasting life.");
  englishVerses_.insert(
      "romans 8:28",
      "And we know that all things work together for good to them that love "
      "God, to them who are the called according to his purpose.");
  englishVerses_.insert("psalm 23:1",
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
      {"bsb", "BSB — English", "English", "BSB",
       "https://berean.bible/", false},
      {"web", "WEB — English", "English", "ENGWEBP",
       "https://ebible.org/Scriptures/details.php?id=engwebp", false},
      {"yoruba", "Yorùbá — Bíbélì Mímọ́", "Yorùbá", "yor_bib",
       "https://ebible.org/Scriptures/details.php?id=yor", false},
      {"igbo", "Igbo — Baịbụlụ Nsọ", "Igbo", "ibo_bib",
       "https://ebible.org/Scriptures/details.php?id=ibo", false},
      {"hausa", "Hausa — Littafi Mai Tsarki", "Hausa", "hau_bib",
       "https://ebible.org/Scriptures/details.php?id=hausa", false},
      {"french", "Français — Louis Segond", "Français", "fra_lsg",
       "https://ebible.org/Scriptures/details.php?id=fraLSG", false},
      {"spanish", "Español — Reina Valera", "Español", "spa_r09",
       "https://ebible.org/Scriptures/details.php?id=spaRV1909", false},
      {"kjv", "KJV — Offline fallback", "English", "", "", true},
  };
}

QVector<VystrmBibleSkin> VystrmBibleAssistant::skins() const {
  return {
      {"amber-trails", "Amber Trails", "Gold-and-blue animated glass lower third"},
      {"midnight-glass", "Midnight Glass", "Dark glass with cyan edge light"},
      {"blue-pulse", "Blue Pulse", "Deep blue panel with a moving cyan pulse"},
      {"royal-burgundy", "Royal Burgundy", "Burgundy and warm-gold broadcast style"},
      {"clean-light", "Clean Light", "Bright readable card for light footage"},
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
        return QString::fromUtf8(entry.canonical).trimmed();
      }
    }
  }
  return value;
}

QString VystrmBibleAssistant::numberWordToDigits(QString value) {
  value = collapseSpaces(value);
  for (auto it = kNumberWords.constBegin(); it != kNumberWords.constEnd(); ++it)
    value.replace(it.key(), it.value());
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
  if (!match.hasMatch())
    return {};

  const QString book = canonicalizeBook(match.captured(1));
  if (book.isEmpty())
    return {};

  QString reference = QString("%1 %2:%3")
                          .arg(book)
                          .arg(match.captured(2))
                          .arg(match.captured(3));
  if (!match.captured(4).isEmpty())
    reference += "-" + match.captured(4);
  return reference;
}

QString VystrmBibleAssistant::helloAoTranslationId(
    const QString &translationCode) const {
  for (const auto &translation : translations()) {
    if (translation.code == translationCode)
      return translation.apiId;
  }
  return {};
}

QString VystrmBibleAssistant::helloAoBookCode(
    const QString &canonicalBook) {
  return kBookCodes.value(canonicalBook);
}

QString VystrmBibleAssistant::helloAoChapterUrl(
    const QString &reference, const QString &translationCode) const {
  const QString apiId = helloAoTranslationId(translationCode);
  if (apiId.isEmpty())
    return {};

  const QRegularExpression pattern(
      "^(.+?)\\s+(\\d+)\\s*:\\s*(\\d+)(?:\\s*-\\s*(\\d+))?$");
  const auto match = pattern.match(reference.toLower().trimmed());
  if (!match.hasMatch())
    return {};

  const QString book = helloAoBookCode(match.captured(1));
  if (book.isEmpty())
    return {};

  return QString("https://bible.helloao.org/api/%1/%2/%3.simple.json")
      .arg(apiId, book, match.captured(2));
}

VystrmBibleVerse VystrmBibleAssistant::lookup(
    const QString &query, const QString &translationCode) const {
  const QString reference = normalizeReference(query);
  if (reference.isEmpty())
    return {};

  VystrmBibleVerse verse;
  verse.reference = reference;
  verse.translation = translationCode;
  verse.referenceRecognized = true;

  if (translationCode == "kjv") {
    const auto it = englishVerses_.constFind(reference);
    if (it != englishVerses_.constEnd()) {
      verse.text = it.value();
      verse.textAvailable = true;
    } else {
      verse.text = "This offline fallback contains only the sample passages. "
                   "Choose an online HelloAO translation for other verses.";
    }
    return verse;
  }

  verse.text = "Fetching this chapter from the HelloAO Bible API…";
  verse.textAvailable = false;
  return verse;
}
