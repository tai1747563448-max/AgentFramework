from __future__ import annotations

import re
import unicodedata


_ASCII_IDENTIFIER = re.compile(r"[A-Za-z0-9_]+")
_CAMEL_BOUNDARY = re.compile(
    r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"
)


def _is_cjk(character: str) -> bool:
    code = ord(character)
    return (
        0x3400 <= code <= 0x4DBF
        or 0x4E00 <= code <= 0x9FFF
        or 0xF900 <= code <= 0xFAFF
        or 0x20000 <= code <= 0x2FA1F
    )


def tokenize(text: str) -> list[str]:
    normalized = unicodedata.normalize("NFKC", text)
    tokens: list[str] = []
    occupied = [False] * len(normalized)
    for match in _ASCII_IDENTIFIER.finditer(normalized):
        for index in range(match.start(), match.end()):
            occupied[index] = True
        original = match.group(0)
        folded = original.casefold()
        tokens.append(folded)
        parts: list[str] = []
        for snake_part in original.split("_"):
            if not snake_part:
                continue
            parts.extend(_CAMEL_BOUNDARY.split(snake_part))
        for part in parts:
            folded_part = part.casefold()
            if folded_part and folded_part != folded:
                tokens.append(folded_part)

    cjk_run: list[str] = []
    other_run: list[str] = []

    def flush_cjk() -> None:
        if not cjk_run:
            return
        tokens.extend(cjk_run)
        tokens.extend(
            cjk_run[index] + cjk_run[index + 1]
            for index in range(len(cjk_run) - 1)
        )
        cjk_run.clear()

    def flush_other() -> None:
        if other_run:
            tokens.append("".join(other_run).casefold())
            other_run.clear()

    for index, character in enumerate(normalized):
        if occupied[index]:
            flush_cjk()
            flush_other()
            continue
        if _is_cjk(character):
            flush_other()
            cjk_run.append(character)
        elif character.isalnum():
            flush_cjk()
            other_run.append(character)
        else:
            flush_cjk()
            flush_other()
    flush_cjk()
    flush_other()
    return tokens
