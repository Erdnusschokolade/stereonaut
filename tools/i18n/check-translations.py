#!/usr/bin/env python3
"""Checks the translation catalogs of the player (src/player/i18n-*.c).

Collects the English texts passed to tr() and N_() in src/player and
compares them with each catalog: texts without a translation (they show in
English), catalog entries the code no longer uses, duplicates and
translations whose printf/strftime conversions differ from the English text.

  python3 tools/i18n/check-translations.py [--strict] [--list LANG]

--strict also fails on missing translations; --list LANG prints the
missing English texts of one language as C entries to fill in.
"""
import argparse
import pathlib
import re
import sys

PLAYER = pathlib.Path(__file__).resolve().parents[2] / "src" / "player"
CONVERSION = re.compile(r"%(?:%|[-+ #0]*\d*(?:\.\d+)?[hlLqjzt]*[a-zA-Z])")
STRFTIME = {"%" + c for c in "aAbBdeHjmMpSyY"}


def same_conversions(english, text):
    """printf conversions in the same order; strftime ones in any."""
    ours, theirs = CONVERSION.findall(english), CONVERSION.findall(text)
    if ours and all(c in STRFTIME for c in ours):
        return sorted(ours) == sorted(theirs)
    return ours == theirs


def skip_space(text, i):
    """Skips whitespace and comments."""
    while i < len(text):
        if text[i].isspace():
            i += 1
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
        elif text.startswith("//", i):
            i = text.index("\n", i)
        else:
            break
    return i


def literal(text, i):
    """Decodes the C string literal at text[i] ('"'); returns (bytes, end)."""
    data = bytearray()
    i += 1
    while text[i] != '"':
        c = text[i]
        if c != "\\":
            data += c.encode()
            i += 1
            continue
        e = text[i + 1]
        i += 2
        if e == "x":
            j = i
            while j < len(text) and text[j] in "0123456789abcdefABCDEF":
                j += 1
            data.append(int(text[i:j], 16) & 0xFF)
            i = j
        elif e in "01234567":
            j = i - 1
            while j < i + 2 and text[j] in "01234567":
                j += 1
            data.append(int(text[i - 1:j], 8) & 0xFF)
            i = j
        else:
            data += {"n": b"\n", "t": b"\t", "r": b"\r", "0": b"\0"}.get(
                e, e.encode())
    return bytes(data), i + 1


def literals(text, i):
    """Adjacent string literals from text[i]; returns (str or None, end)."""
    i = skip_space(text, i)
    if i >= len(text) or text[i] != '"':
        return None, i
    data = b""
    while i < len(text) and text[i] == '"':
        part, i = literal(text, i)
        data += part
        i = skip_space(text, i)
    return data.decode("utf-8"), i


def used_texts():
    texts = {}
    call = re.compile(r"(?<![A-Za-z0-9_])(tr|N_)\(")
    for path in sorted(PLAYER.glob("*.[ch]")):
        if path.name.startswith("i18n"):
            continue
        source = path.read_text(encoding="utf-8")
        for match in call.finditer(source):
            value, end = literals(source, match.end())
            if value is not None and source[end] == ")":
                line = source.count("\n", 0, match.start()) + 1
                texts.setdefault(value, f"{path.name}:{line}")
    return texts


def catalogs():
    result = {}
    for path in sorted(PLAYER.glob("i18n-*.c")):
        source = path.read_text(encoding="utf-8")
        entries = []
        for match in re.finditer(r"\{", source):
            english, i = literals(source, match.end())
            if english is None or source[i] != ",":
                continue
            text, j = literals(source, i + 1)
            if text is not None and source[j] == "}":
                line = source.count("\n", 0, match.start()) + 1
                entries.append((english, text, line))
        result[path.name[len("i18n-"):-2]] = (path.name, entries)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--list", metavar="LANG")
    arguments = parser.parse_args()
    used = used_texts()
    failed = False
    for code, (name, entries) in catalogs().items():
        known = {}
        for english, text, line in entries:
            if english in known:
                print(f"{name}:{line}: duplicate {english!r}")
                failed = True
            known[english] = text
            if english in used and not same_conversions(english, text):
                print(f"{name}:{line}: conversions differ: {english!r} -> "
                      f"{text!r}")
                failed = True
        missing = [t for t in used if t not in known]
        unused = [t for t in known if t not in used]
        for english in unused:
            print(f"{name}: unused {english!r}")
        for english in missing:
            print(f"{name}: missing {english!r} ({used[english]})")
        print(f"{name}: {len(known) - len(unused)} of {len(used)} texts "
              f"translated, {len(missing)} missing, {len(unused)} unused")
        if arguments.strict and missing:
            failed = True
        if arguments.list == code:
            for english in missing:
                quoted = english.replace("\\", "\\\\").replace('"', '\\"')
                print(f'    {{"{quoted}", ""}},')
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
