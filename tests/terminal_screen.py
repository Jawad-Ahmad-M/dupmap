"""Small ANSI screen reader for ncurses regression tests; standard library only."""
import codecs
import re
import unicodedata


class Screen:
    def __init__(self, columns, rows):
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.pending = ""
        self.colored = False
        self.resize(columns, rows)

    def resize(self, columns, rows):
        self.columns, self.rows = columns, rows
        self.cells = [[" "] * columns for _ in range(rows)]
        self.x = self.y = 0
        self.top, self.bottom = 0, rows - 1
        self.saved = (0, 0)
        self.wrap = True
        self.wrap_pending = False
        self.last_printed = " "

    def text(self):
        return "\n".join("".join(row) for row in self.cells)

    def scroll(self, down=False, count=1):
        for _ in range(min(count, self.bottom - self.top + 1)):
            if down:
                del self.cells[self.bottom]
                self.cells.insert(self.top, [" "] * self.columns)
            else:
                del self.cells[self.top]
                self.cells.insert(self.bottom, [" "] * self.columns)

    def newline(self):
        if self.y == self.bottom:
            self.scroll()
        else:
            self.y = min(self.rows - 1, self.y + 1)

    def csi(self, arguments, command):
        private = arguments.startswith("?")
        values = [int(v or 0) for v in arguments.lstrip("?").split(";")]
        n = values[0] or 1
        if command == "m":
            self.colored |= any(30 <= v <= 48 and v not in (39,) or 90 <= v <= 107 for v in values)
            return
        if private:
            if values[0] == 7 and command in "hl":
                self.wrap = command == "h"
            return
        self.wrap_pending = False
        if command in "Hf":
            self.y = max(0, min(self.rows - 1, (values[0] or 1) - 1))
            self.x = max(0, min(self.columns - 1, (values[1] or 1) - 1 if len(values) > 1 else 0))
        elif command == "A": self.y = max(0, self.y - n)
        elif command == "B": self.y = min(self.rows - 1, self.y + n)
        elif command == "C": self.x = min(self.columns - 1, self.x + n)
        elif command == "D": self.x = max(0, self.x - n)
        elif command in "G`": self.x = min(self.columns - 1, n - 1)
        elif command == "d": self.y = min(self.rows - 1, n - 1)
        elif command == "J":
            for y in range(self.rows):
                for x in range(self.columns):
                    if values[0] == 2 or (values[0] == 0 and (y, x) >= (self.y, self.x)) or (values[0] == 1 and (y, x) <= (self.y, self.x)):
                        self.cells[y][x] = " "
        elif command == "K":
            for x in range(self.columns):
                if values[0] == 2 or (values[0] == 0 and x >= self.x) or (values[0] == 1 and x <= self.x):
                    self.cells[self.y][x] = " "
        elif command == "X":
            for x in range(self.x, min(self.columns, self.x + n)):
                self.cells[self.y][x] = " "
        elif command == "P":
            row = self.cells[self.y]
            row[self.x:] = (row[self.x + n:] + [" "] * n)[:self.columns - self.x]
        elif command == "@":
            row = self.cells[self.y]
            row[self.x:] = ([" "] * n + row[self.x:])[:self.columns - self.x]
        elif command == "r":
            self.top = n - 1
            self.bottom = (values[1] or self.rows) - 1 if len(values) > 1 else self.rows - 1
            self.x = self.y = 0
        elif command in "ST": self.scroll(command == "T", n)
        elif command in "LM":
            old_top = self.top
            self.top = self.y
            self.scroll(command == "L", n)
            self.top = old_top
        elif command == "b":
            for _ in range(n): self.put_character(self.last_printed)
        elif command == "s": self.saved = (self.x, self.y)
        elif command == "u": self.x, self.y = self.saved

    def put_character(self, ch):
        if unicodedata.combining(ch):
            if self.x: self.cells[self.y][self.x - 1] += ch
            return
        if self.wrap_pending and self.wrap:
            self.x = 0; self.newline()
        self.wrap_pending = False
        width = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
        self.cells[self.y][self.x] = ch
        self.last_printed = ch
        if width == 2 and self.x + 1 < self.columns:
            self.cells[self.y][self.x + 1] = ""
        if self.x + width >= self.columns:
            self.x = self.columns - 1; self.wrap_pending = True
        else: self.x += width

    def feed(self, data):
        self.pending += self.decoder.decode(data)
        i = 0
        while i < len(self.pending):
            ch = self.pending[i]
            if ch == "\x1b":
                if i + 1 >= len(self.pending): break
                following = self.pending[i + 1]
                if following == "[":
                    match = re.match(r"\x1b\[([0-?]*)([ -/]*)([@-~])", self.pending[i:])
                    if not match: break
                    self.csi(match[1], match[3])
                    i += len(match[0])
                    continue
                if following in "()":
                    if i + 2 >= len(self.pending): break
                    i += 3
                    continue
                if following == "]":
                    end = re.search(r"\x07|\x1b\\", self.pending[i + 2:])
                    if not end: break
                    i += 2 + end.end()
                    continue
                if following == "7": self.saved = (self.x, self.y)
                elif following == "8": self.x, self.y = self.saved
                elif following == "M":
                    if self.y == self.top: self.scroll(True)
                    else: self.y = max(0, self.y - 1)
                elif following in "DE":
                    self.newline()
                    if following == "E": self.x = 0
                self.wrap_pending = False
                i += 2
                continue
            if ch == "\r": self.x = 0; self.wrap_pending = False
            elif ch == "\n": self.newline(); self.wrap_pending = False
            elif ch == "\b": self.x = max(0, self.x - 1); self.wrap_pending = False
            elif ch == "\t": self.x = min(self.columns - 1, (self.x // 8 + 1) * 8)
            elif ch >= " ": self.put_character(ch)
            i += 1
        self.pending = self.pending[i:]
