# VYSTRM Bible translation packs

The OBS dock uses a translation-pack interface so the UI, reference detector,
and verse renderer do not depend on one hard-coded language.

## HelloAO source

VYSTRM retrieves the selected chapter from the HelloAO Free Use Bible API:

- API documentation: https://bible.helloao.org/docs/guide/making-requests.html
- Download formats: https://bible.helloao.org/docs/guide/downloads.html
- Translation catalogue: https://bible.helloao.org/api/available_translations.json
- License and attribution: https://bible.helloao.org/docs/guide/a-biblical-model-for-licensing-the-bible.html

The plugin requests only the chapter needed for the staged reference, keeps
the text out of the executable, and shows the translation's attribution URL in
the source manifest. The operator still decides when to Preview or Push to
Program. No detected or manually searched verse is sent live automatically.

## Included HelloAO translations

The initial selector uses BSB and WEB English, Yoruba (yor_bib), Igbo
(ibo_bib), Hausa (hau_bib), French (fra_lsg), and Spanish (spa_r09).
Translation IDs and license URLs are kept in helloao-pack-manifest.json so
they can be updated without changing the reference detector.

## Reference parsing

Manual search and the listener accept numeric references and natural forms such
as John 3:16, John chapter three verse sixteen, Johanu 3:16, and
Orin Dafidi 23:1. Localized book aliases are normalized to the canonical
book ID used by HelloAO.

## Verse graphic skins

Five built-in browser-source skins are included:

1. Amber Trails — the gold/blue animated glass style from the VYSTRM reference image.
2. Midnight Glass — dark glass with cyan edge light.
3. Blue Pulse — deep blue with a moving cyan pulse.
4. Royal Burgundy — burgundy and warm gold.
5. Clean Light — bright readable card for light footage.

Skin is also exposed directly in the Overview assistant header so it is always
visible in the compact OBS dock; opacity remains in Bible Settings. Preview
creates or updates the preview graphic source; Push to Program creates or
updates the Program graphic source only after the operator presses the button.
Clear removes both VYSTRM Bible Preview and VYSTRM Bible Program scene items,
then resets the staged reference without automatically sending anything live.
