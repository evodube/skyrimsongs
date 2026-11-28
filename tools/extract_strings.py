#!/usr/bin/env python3
# SkyrimSongs - play the lute and flute in Skyrim.
# Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
"""Every line of text a player sees, from the code, into the English translation file.

Each T("Key", "English"), TF(...), TL(...) and Tr(...) in src/ becomes a line
    $SkyrimSongs_Key<tab>English
of assets/translations/SkyrimSongs_ENGLISH.txt, written the way Skyrim's own translation files are (UTF-16 LE with a
byte order mark, CRLF). A translation is that file copied to SkyrimSongs_<LANGUAGE>.txt with the text after each tab
translated.

    python tools/extract_strings.py          writes the file
    python tools/extract_strings.py --check  fails if the file isn't up to date with the code (CI runs this)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "translations" / "SkyrimSongs_ENGLISH.txt"
SOURCES = sorted(list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.h")))

LIT = r'"(?:[^"\\]|\\.)*"'
CALL = re.compile(r'(?<![A-Za-z0-9_])(?:T|TF|TL|Tr)\(\s*"([A-Za-z0-9_]+)"\s*,\s*((?:' + LIT + r'\s*)+)')
# the crowd's lines: { "Key", "English" } in the music code's tables
TABLE = re.compile(r'\{\s*"(Crowd[A-Za-z0-9_]+)"\s*,\s*((?:' + LIT + r'\s*)+)\}')

# lines that aren't in the code as a call: the translation's own settings
EXTRA = [
    ("Font", "", "A font for your language's letters, if Windows' don't suit: a .ttf/.ttc file in Data\\SKSE\\Plugins\\SkyrimSongs\\Fonts "
                 "or Windows\\Fonts, e.g. myfont.ttf. Leave empty for Windows' own."),
]

HEADER = """SkyrimSongs - translation file. Lines starting with $ are text; anything else is a comment.
Each line is  $SkyrimSongs_Key<TAB>Text . Translate only the Text after the tab; keep the key and the tab.
Keep {} {0} {1} {:.0f} and the like exactly: the numbers and names go there. Reorder them freely ({1} before {0}).
\\n is a new line. Lines missing from your file stay in English, so a translation keeps working after an update.
Save as SkyrimSongs_<LANGUAGE>.txt next to this one (RUSSIAN, GERMAN, FRENCH, POLISH, CHINESE, JAPANESE...),
in UTF-16 LE (Skyrim's own translation format) or UTF-8. The language is the game's own (sLanguage in Skyrim.ini),
or [Display] Language in SkyrimSongs.ini.
Names of the game's people, places and items (Npc..., Place..., Item...) must match what the game calls them in
your language, or the mod won't recognise them (Item... and Place... in lower case).
"""


def unescape(lits):
    out = ""
    for lit in re.findall(LIT, lits):
        body = lit[1:-1]
        out += re.sub(r'\\(.)', lambda m: {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(m.group(1), "\\" + m.group(1)), body)
    return out


def escape(text):
    text = re.sub(r'\\(?=[nt\\])', r'\\\\', text)
    return text.replace("\t", "\\t").replace("\n", "\\n")


def collect():
    seen = {}
    order = []
    errors = []
    for path in SOURCES:
        src = path.read_text(encoding="utf-8")
        for m in sorted(list(CALL.finditer(src)) + list(TABLE.finditer(src)), key=lambda m: m.start()):
            if "//" in src[src.rfind("\n", 0, m.start()) + 1:m.start()]:
                continue  # in a comment
            key, english = m.group(1), unescape(m.group(2))
            where = f"{path.relative_to(ROOT)}:{src.count(chr(10), 0, m.start()) + 1}"
            if key in seen:
                if seen[key][0] != english:
                    errors.append(f"{where}: {key} is also {seen[key][1]} with other English:\n  {english!r}\n  {seen[key][0]!r}")
                continue
            seen[key] = (english, where)
            order.append((key, english, path.relative_to(ROOT).as_posix()))
    return order, errors


def render(order):
    lines = [l for l in HEADER.splitlines()]
    lines.append("")
    for key, english, comment in EXTRA:
        lines.append(comment)
        lines.append(f"$SkyrimSongs_{key}\t{escape(english)}")
    current = None
    for key, english, src in order:
        if src != current:
            lines.append("")
            lines.append(f"From {src}")
            current = src
        lines.append(f"$SkyrimSongs_{key}\t{escape(english)}")
    return ("\r\n".join(lines) + "\r\n").encode("utf-16-le")


def main():
    order, errors = collect()
    if errors:
        print("\n".join(errors))
        return 1
    data = b"\xff\xfe" + render(order)
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_bytes() != data:
            print(f"{OUT.relative_to(ROOT)} is out of date: run python tools/extract_strings.py and commit it")
            return 1
        print(f"{len(order)} lines, up to date")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"{len(order)} lines -> {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
