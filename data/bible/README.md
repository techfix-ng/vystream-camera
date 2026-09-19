# VYSTRM Bible translation packs

The OBS dock uses a translation-pack interface so the UI, reference detector,
and verse renderer do not depend on one hard-coded language.

## Pack requirements

A pack should provide:

- a stable translation code (`yoruba`, `igbo`, `hausa`, etc.);
- UTF-8 verse text with full diacritics preserved;
- canonical book IDs (`john`, `romans`, `psalm`, ...);
- localized book-name aliases for manual search and speech detection;
- a license/attribution file.

The first built-in offline sample is KJV because it is public-domain. The
Yorùbá, Igbo, Hausa, French, and Spanish entries are intentionally marked as
pack-required until an approved text file is supplied or licensed. The plugin
must not scrape or redistribute copyrighted translations.

## Future pack format

The runtime loader will accept UTF-8 JSON Lines records:

```json
{"book":"john","chapter":3,"verse":16,"reference":"Johanu 3:16","text":"..."}
```

A separate metadata record identifies the translation, language, version, and
license. Canonical references remain language-neutral, so `John 3:16`,
`Johanu 3:16`, and the localized equivalent all stage the same verse.

## Manual search

The Overview tab includes a manual reference search field. It accepts numeric
references and natural forms such as `John 3:16`, `John chapter three verse
sixteen`, `Johanu 3:16`, and `Orin Dafidi 23:1`. Search stages the verse
only; it never changes OBS Program until the operator presses **PUSH TO
PROGRAM**.
