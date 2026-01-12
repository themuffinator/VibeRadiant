# Language Packs

VibeRadiant loads UI translations from JSON language packs in the `i18n/`
folder next to the executable. At runtime it looks for:

`<install>/i18n/<code>.json`

If the file is missing or invalid, VibeRadiant falls back to English.

## How it works

- Language selection is stored in the `Language` preference.
- "Auto (System)" uses the OS locale (Qt `uiLanguages`) and falls back to `en`
  if no supported code matches.
- The language code is normalized (case-insensitive, `_` becomes `-`).
- Regional codes like `en-US` map to `en` when only a base language is
  available.
- `no` maps to `nb` (Norwegian Bokmal) when `nb` is supported.

## File format

Each language pack is a flat JSON object of string-to-string entries:

```json
{
  "Menu Item": "Translated Menu Item",
  "Another String": "Another Translation",
  "_meta": {
    "code": "en",
    "name": "English"
  }
}
```

Notes:

- Keys starting with `_` are ignored (reserved for metadata).
- Missing keys fall back to the original English string.

## Supported languages

The current supported language codes (from `radiant/localization.cpp`) are:

- `en` English
- `fr` French
- `de` German
- `pl` Polish
- `es` Spanish
- `it` Italian
- `pt` Portuguese
- `ru` Russian
- `uk` Ukrainian
- `cs` Czech
- `sk` Slovak
- `hu` Hungarian
- `tr` Turkish
- `nl` Dutch
- `sv` Swedish
- `nb` Norwegian Bokmal
- `da` Danish
- `fi` Finnish
- `ja` Japanese
- `zh` Chinese (Simplified)

## Adding or updating a language

1. Create or edit `<install>/i18n/<code>.json`.
2. Restart VibeRadiant (or reopen Preferences if you want to reapply the
   Language setting).
3. Select the language in Preferences.
