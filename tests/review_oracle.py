#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Independent input and rendered-source accounting for paired reviews."""

import ast
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import re
import unicodedata

HEADER = re.compile(rb'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))?(?=[ \r\n]|$)')


def lines(data):
    """Split only at LF; other control bytes belong to source text."""
    return re.findall(rb'[^\n]*\n|[^\n]+$', data)


def name_bytes(label):
    label = label.rstrip(b'\r\n')
    if label.startswith(b'"'):
        match = re.match(rb'"(?:[^"\\]|\\.)*"', label)
        if match:
            try:
                return ast.literal_eval('b' + match[0].decode('ascii'))
            except (ValueError, SyntaxError, UnicodeError):
                pass
    return label.split(b'\t', 1)[0]


def path_key(name):
    return name.decode('utf-8', 'surrogateescape')


@dataclass
class Row:
    sign: str
    text: bytes
    pos: tuple[int, int]
    hunk: int = 0
    neutral: bool = False


@dataclass
class File:
    path: str
    rows: list[Row] = field(default_factory=list)
    views: list[dict[int, bytes]] = field(default_factory=lambda: [{}, {}])
    ambiguous: bool = False
    aliases: set[str] = field(default_factory=set)
    deletion_id: bytes = b''


def parse_patch(data):
    result = []
    current = None
    old_name = None
    git = False
    crlf = False
    remaining = [0, 0]
    pos = [0, 0]
    hunk = -1
    deletion_id = b''
    for raw in lines(data):
        if not any(remaining):
            if raw.startswith(b'diff --git '):
                git = True
                current = None
                old_name = None
                deletion_id = b''
            elif raw.startswith(b'index '):
                match = re.match(rb'index ([0-9a-fA-F]+)\.\.0+(?:[ \r\n]|$)', raw)
                deletion_id = match[1].lower() if match else b''
            elif raw.startswith(b'--- '):
                old_name = name_bytes(raw[4:])
                crlf = raw.endswith(b'\r\n')
            elif raw.startswith(b'+++ ') and old_name is not None:
                new_name = name_bytes(raw[4:])
                crlf = raw.removesuffix(b'\n').endswith(b'\r')
                prefixed = (old_name.startswith(b'a/') and new_name.startswith(b'b/')) or (git and b'/dev/null' in (old_name, new_name))
                key = old_name if new_name == b'/dev/null' else new_name
                if prefixed and key[:2] in (b'a/', b'b/'):
                    key = key[2:]
                current = File(path_key(key))
                if new_name == b'/dev/null':
                    current.deletion_id = deletion_id
                for name in (old_name, new_name):
                    if name != b'/dev/null':
                        current.aliases.add(path_key(name))
                        if name[:2] in (b'a/', b'b/'):
                            current.aliases.add(path_key(name[2:]))
                current.aliases.add(current.path)
                result.append(current)
                hunk = -1
                old_name = None
            elif (match := HEADER.match(raw)) and current is not None:
                off = [int(match[1]), int(match[3])]
                remaining = [int(match[2] or b'1'), int(match[4] or b'1')]
                pos = [off[s] - bool(off[s] and remaining[s]) for s in (0, 1)]
                hunk += 1
                continue
            elif raw.startswith(b'\\') and current and current.rows:
                current.rows[-1].text = current.rows[-1].text.removesuffix(b'\n')
            if not any(remaining):
                continue
        if raw.startswith(b'\\'):
            if current and current.rows:
                current.rows[-1].text = current.rows[-1].text.removesuffix(b'\n')
            continue
        if crlf:
            raw = raw.removesuffix(b'\n').removesuffix(b'\r') + (b'\n' if raw.endswith(b'\n') else b'')
        if raw[:1] in (b' ', b'-', b'+'):
            sign, text = chr(raw[0]), raw[1:]
        elif raw[:1] in (b'\n', b'\t'):
            sign, text = ' ', raw
        else:
            raise ValueError(f'unrecognized body line: {raw!r}')
        if not text.endswith(b'\n'):
            text += b'\n'
        current.rows.append(Row(sign, text, tuple(pos), hunk))
        for stage in (0, 1):
            if sign != ('+' if stage == 0 else '-'):
                remaining[stage] -= 1
                pos[stage] += 1
    for file in result:
        for row in file.rows:
            for stage in (0, 1):
                if row.sign == ('+' if stage == 0 else '-'):
                    continue
                previous = file.views[stage].get(row.pos[stage])
                file.ambiguous |= previous is not None and previous != row.text
                file.views[stage][row.pos[stage]] = row.text
        first = 0
        while first < len(file.rows):
            mid = first
            while mid < len(file.rows) and file.rows[mid].sign == '-' and file.rows[mid].hunk == file.rows[first].hunk:
                mid += 1
            end = mid
            while end < len(file.rows) and file.rows[end].sign == '+' and file.rows[end].hunk == file.rows[first].hunk:
                end += 1
            if mid > first and [r.text for r in file.rows[first:mid]] == [r.text for r in file.rows[mid:end]]:
                for row in file.rows[first:end]:
                    row.neutral = True
            first = max(end, first + 1)
    return result


@dataclass
class PrintedRow:
    """A parsed cell whose source bytes are filled by check_review().

    Coordinates are zero-based, with -1 for an absent stage. Shared delta
    cells recover their native sign from the populated coordinate. Group and
    pair numbers span the report. Parsing leaves provisional display text;
    source validation replaces it and supplies the raw source for the
    relationship and operand-reversal checks.
    """
    section: str
    paths: tuple[str, str]
    group: int
    leg: int
    sign: str
    text: bytes
    pos: tuple[int, int]
    shared: bool = False
    fragments: list[str] = field(default_factory=list)
    columns: int = 0
    newline: bool = True
    pair: int = 0
    source: bytes = b''
    source_start: int = 0


def character_width(char):
    """Approximate terminal widths using Python's Unicode database.

    This independent model can disagree with the C locale's wcwidth() on
    unassigned or special characters. Such differences reject a report;
    they do not excuse unequal source bytes.
    """
    if unicodedata.category(char) in ('Mn', 'Me', 'Cf'):
        return 0
    return 2 if unicodedata.east_asian_width(char) in ('W', 'F') else 1


def display_width(text, start=0):
    used = 0
    for char in text:
        used += 8 - (start + used) % 8 if char == '\t' else character_width(char)
    return used


def split_columns(text, columns, start=0):
    used = 0
    for i, char in enumerate(text):
        width = 8 - (start + used) % 8 if char == '\t' else character_width(char)
        if used + width > columns:
            return text[:i], text[i:]
        used += width
        if used == columns:
            end = i + 1
            while end < len(text) and not character_width(text[end]):
                end += 1
            return text[:end], text[end:]
    return text, ''


def wrapped(text, columns, start=0):
    result = []
    while text:
        piece, text = split_columns(text, columns, start)
        assert piece, ('column too small for source character', columns, text)
        result.append(piece + ' ' * (columns - display_width(piece, start)))
    return result or [' ' * columns]


def flatten(fragments):
    parts = list(fragments)
    while len(parts) > 1 and not parts[-1].strip():
        parts.pop()
    return ''.join(parts[:-1]) + (parts[-1].rstrip(' ') if parts else '')


def parse_review(data, known_paths):
    """Read banners, file headers, source cells, notes, and continuations.

    Framing text before each file's number labels is deliberately skipped.
    Once a file header establishes its columns, every source cell must follow
    the numbered report grammar.
    """
    text = re.sub(r'\x1b\[[0-9;]*m', '', data.decode('utf-8', 'surrogateescape'))
    physical = text.split('\n')
    rules = [line for line in physical if re.fullmatch(r'\u2501+|-+', line)]
    if not rules:
        assert not text.strip(), ('report has no file header', text[:100])
        return []
    width = len(rules[0])
    assert width % 2 == 1 and all(len(line) == width for line in rules), 'unequal or changing columns'
    assert all(display_width(line) <= width for line in physical), 'line exceeds report width'
    cell_width = (width - 1) // 2
    unicode = rules[0][0] == '\u2501'
    bar, middle = ('\u2502', '\u2503') if unicode else ('|', '|')
    horizontal, crossing, junction = ('\u2500', '\u2542', '\u253c') if unicode else ('-', '+', '+')

    def cells(line):
        left, rest = split_columns(line, cell_width)
        assert rest.startswith(middle), ('missing middle separator', line)
        right = rest[1:]
        right_width = display_width(right, cell_width + 1)
        assert display_width(left) == cell_width and right_width <= cell_width, ('wrong column width', line)
        return left, right + ' ' * (cell_width - right_width)

    def caption(start, gutter):
        # Input names distinguish a wrapped path from the notes that follow it.
        # Both are unnumbered text, and color is deliberately not required.
        parts = [[], []]
        matches = [{}, {}]
        end = start
        while end < len(physical) and physical[end]:
            column = cells(physical[end])
            if end > start and any(cell[:gutter-1].strip() for cell in column):
                break
            for leg, cell in enumerate(column):
                assert cell[gutter-1] == bar, ('bad caption gutter', physical[end])
                parts[leg].append(cell[gutter:])
                name = path_key(name_bytes(flatten(parts[leg]).encode('utf-8', 'surrogateescape')))
                if name in known_paths:
                    matches[leg].setdefault(name, end + 1)
            end += 1
        if end < len(physical) and physical[end]:
            assert all(matches), ('unknown source file in caption', parts)
        names = [max(found, key=len) if found else '' for found in matches]
        consumed = max((found[name] for found, name in zip(matches, names) if found), default=start+1)
        return tuple(names), consumed

    def finish_notes():
        if notes:
            for parts in notes:
                assert not parts or flatten(parts) == 'No newline at end of file', ('bad newline note', parts)

    result = []
    section = None
    paths = None
    active = [None, None]
    notes = None
    group = 0
    pair = 0
    gutter = None
    fresh = True
    heading_seen = False
    i = 0
    while i < len(physical):
        line = physical[i]
        i += 1
        title = line.lstrip('* ')
        if line.startswith('*') and title.startswith(('DELTA DIFFERENCES', 'CONTEXT DIFFERENCES')):
            section = 'delta' if title.startswith('DELTA') else 'context'
            paths = None
            continue
        if not line:
            finish_notes()
            notes = None
            active = [None, None]
            fresh = True
            continue
        if section is None:
            continue
        if re.fullmatch(r'\u2550+|=+', line):
            continue
        if line in rules:
            paths = None
            continue
        header = re.match(r'^ (?: *old +new| +new)[\u2502|]', line)
        if header:
            gutter = header.end()
            assert gutter >= 9, ('wrong number gutters', line)
            paths, i = caption(i-1, gutter)
            active = [None, None]
            fresh = True
            heading_seen = False
            continue
        if paths is None:
            continue
        group_rule = (' ' * (gutter-1) + bar + horizontal * (cell_width-gutter) + crossing
                      + horizontal * (gutter-1) + junction + horizontal * (cell_width-gutter))
        if line == group_rule:
            finish_notes()
            notes = None
            active = [None, None]
            fresh = True
            heading_seen = False
            continue
        column = cells(line)
        numbers = []
        roles = []
        for cell in column:
            assert cell[gutter-1] == bar, ('wrong source separator', line)
            fields = re.fullmatch(r' *([ +\\-])( *[0-9]*) ( *[0-9]*)[\u2502|]', cell[:gutter])
            assert fields, ('bad gutters', line)
            roles.append(fields[1])
            numbers.append(tuple(field.strip() for field in fields.groups()[1:]))
        if any(old or new for old, new in numbers):
            assert all(not n or n.isdecimal() for nums in numbers for n in nums), ('non-numeric source coordinate', line)
            finish_notes()
            notes = None
            if fresh:
                assert heading_seen, ('source group lacks its heading', line)
                group += 1
            pair += 1
            fresh = False
            active = [None, None]
            for leg, (old, new) in enumerate(numbers):
                if not old and not new:
                    assert roles[leg] == ' ', ('padding has an invented sign', line)
                    assert not column[leg][:gutter-1].strip(), ('padding has an invented role', line)
                    assert not column[leg][gutter:].strip(), ('source has no coordinate', line)
                    continue
                role = roles[leg]
                assert role in (' ', '+', '-'), ('unrecognized source role', line)
                shared = section == 'delta' and role == ' ' and bool(old) != bool(new)
                if section == 'delta':
                    if shared:
                        role = '+' if new else '-'
                    assert bool(old) == (role != '+') and bool(new) == (role != '-'), ('wrong coordinate ownership', line)
                else:
                    assert not old and new and not shared, ('wrong tip coordinates', line)
                    assert role in (' ', '-' if leg == 0 else '+'), ('wrong tip sign', line)
                row = PrintedRow(section, paths, group, leg, role, b'',
                    (int(old)-1 if old else -1, int(new)-1 if new else -1), shared,
                    columns=cell_width-gutter, pair=pair,
                    source_start=leg * (cell_width+1) + gutter)
                row.fragments.append(column[leg][gutter:])
                result.append(row)
                active[leg] = row
            continue
        if fresh:
            assert all(role == ' ' for role in roles), ('invented note signs', line)
            assert all(not cell[:gutter-1].strip() for cell in column), ('invented note coordinates', line)
            heading_seen |= any(cell[gutter:].startswith('@@') for cell in column)
            continue
        if '\\' in roles:
            notes = [[], []]
            for leg, cell in enumerate(column):
                if roles[leg] == '\\':
                    assert active[leg], ('newline note without source', line)
                    active[leg].newline = False
                    notes[leg].append(cell[gutter:])
                else:
                    assert not cell[gutter:].strip(), ('unexpected newline note padding', line)
            continue
        if notes is not None:
            for leg, cell in enumerate(column):
                if notes[leg]:
                    notes[leg].append(cell[gutter:])
                else:
                    assert not cell[gutter:].strip(), ('unexpected note continuation', line)
            continue
        for leg, cell in enumerate(column):
            assert roles[leg] == ' ', ('continuation has an invented sign', line)
            assert not cell[:gutter-1].strip(), ('continuation has an invented role', line)
            if active[leg]:
                active[leg].fragments.append(cell[gutter:])
            else:
                assert not cell[gutter:].strip(), ('continuation without source', line)
    finish_notes()
    for row in result:
        row.text = flatten(row.fragments).encode('utf-8', 'surrogateescape') + (b'\n' if row.newline else b'')
    return result


def display_text(text):
    """Preserve source whitespace and spell nonprinting bytes independently."""
    result = []
    for char in text.decode('utf-8', 'surrogateescape'):
        if char in ('\t', '\n'):
            result.append(char)
        elif unicodedata.category(char) in ('Cc', 'Cs'):
            raw = char.encode('utf-8', 'surrogateescape')
            result.append(''.join(f'\\x{byte:02x}' for byte in raw))
        else:
            result.append(char)
    return ''.join(result).encode('utf-8', 'surrogateescape')


def matches_source(row, text):
    """Compare wrapped bytes while allowing the other column to keep wrapping.

    Cell padding obscures trailing source spaces in this audit. Separate
    byte-output checks verify that rendering preserves those spaces.
    """
    if row.newline != text.endswith(b'\n'):
        return False
    rendered = display_text(text).removesuffix(b'\n').decode('utf-8', 'surrogateescape')
    expected = wrapped(rendered, row.columns, row.source_start)
    return (row.fragments[:len(expected)] == expected and
            all(not part.strip() for part in row.fragments[len(expected):]))


def opaque_deletions(data):
    """Whole-file deletion declarations can omit all deleted source rows."""
    result = {}
    for block in re.split(rb'(?m)^diff --git ', data)[1:]:
        header, _, rest = block.partition(b'\n')
        if not re.search(rb'^deleted file mode ', rest, re.M):
            continue
        if re.search(rb'^@@ ', rest, re.M):
            continue
        match = re.search(rb'^index ([0-9a-fA-F]+)\.\.0+(?:[ \r\n]|$)', rest, re.M)
        if not match or not match[1].strip(b'0'):
            continue
        # Git repeats the deletion path in the two opener names, with a/ and
        # b/ prefixes. Trying separators also handles literal spaces/quotes.
        for i, byte in enumerate(header):
            if byte != 32:
                continue
            left, right = name_bytes(header[:i]), name_bytes(header[i + 1:])
            if left.startswith(b'a/') and right.startswith(b'b/') and left[2:] == right[2:]:
                result[path_key(left[2:])] = match[1].lower()
                break
    return result


def check_relationships(rows, views=None, local_order=()):
    """Check source order, context equality, padding, and excerpt continuity.

    A padded unchanged cell needs a corresponding source row between its
    neighbors. Every context excerpt must contain a difference and have no
    interior holes. These checks require validated source bytes; they do
    not choose the correct counterpart among repeated lines or judge delta
    pairing beyond coordinate order.
    """
    pairs = defaultdict(dict)
    groups = defaultdict(lambda: [[], []])
    previous = {}
    by_file = defaultdict(lambda: [[], []])
    for row in rows:
        pairs[row.section, row.paths, row.pair][row.leg] = row
        groups[row.section, row.paths, row.group][row.leg].append(row)
        if row.section == 'context':
            by_file[row.paths][row.leg].append(row)
        for stage, position in enumerate(row.pos):
            if position < 0:
                continue
            key = row.section, row.paths, row.leg, stage
            if (row.leg, row.paths[row.leg]) in local_order:
                key += (row.group,)
            assert position > previous.get(key, -1), ('source order reversed or repeated', row)
            previous[key] = position
    omitted = defaultdict(lambda: -1)
    for (section, paths, pair), cells in pairs.items():
        if section != 'context':
            continue
        equal = [row for row in cells.values() if row.sign == ' ']
        if equal and len(cells) == 2:
            assert len(equal) == 2 and equal[0].source == equal[1].source, (
                'false context equality', paths, pair, cells)
        elif not equal and len(cells) == 2:
            assert cells[0].source != cells[1].source, (
                'equal context marked different', paths, pair, cells)
        elif equal and views is not None:
            row = equal[0]
            other = 1 - row.leg
            source = views[other][paths[other]][1]
            neighbors = by_file[paths][other]
            lower = max((r.pos[1] + 1 for r in neighbors if r.pair < pair), default=0)
            upper = min((r.pos[1] for r in neighbors if r.pair > pair), default=max(source, default=-1) + 1)
            lower = max(lower, omitted[paths, other] + 1)
            candidates = [i for i, text in source.items()
                          if lower <= i < upper and text == row.source]
            assert candidates, ('unpaired context has no corresponding source', row, lower, upper)
            omitted[paths, other] = min(candidates)
    for (section, paths, group), sides in groups.items():
        if section != 'context':
            continue
        assert any(row.sign in '+-' for side in sides for row in side), (
            'context group without a difference', paths, group)
        for leg, side in enumerate(sides):
            assert all(b.pos[1] == a.pos[1] + 1 for a, b in zip(side, side[1:])), (
                'hole inside context group', paths, group, leg)


def moved_hunks(parsed):
    """Identify unique complete edits transferred between different filenames."""
    signatures = defaultdict(lambda: [[], []])
    for leg, files in enumerate(parsed):
        for f, file in enumerate(files):
            hunks = defaultdict(list)
            for row in file.rows:
                if row.sign in '+-' and not row.neutral:
                    hunks[row.hunk].append((row.sign, row.text))
            for h, changes in hunks.items():
                signatures[tuple(changes)][leg].append((file.path.rsplit('/', 1)[-1], f, h))
    moved = [{}, {}]
    for sides in signatures.values():
        for basename in {item[0] for side in sides for item in side}:
            count = min(sum(item[0] == basename for item in side) for side in sides)
            for side in sides:
                for item in [item for item in side if item[0] == basename][:count]:
                    side.remove(item)
        if all(len(side) == 1 for side in sides):
            for leg, side in enumerate(sides):
                moved[leg][side[0][1:]] = sides[1 - leg][0][1:]
    return moved


def moved_rows(parsed, printed):
    """Locate suppressed native spans for checking transferred source windows."""
    whole = moved_hunks(parsed)
    moved = [{}, {}]
    signatures = defaultdict(lambda: [[], []])
    shown = {(row.leg, row.paths[row.leg], row.sign, row.pos[row.sign == '+'])
             for row in printed if row.section == 'delta' and row.sign in '+-' and not row.shared}
    for leg, files in enumerate(parsed):
        for f, file in enumerate(files):
            for i, row in enumerate(file.rows):
                if (f, row.hunk) in whole[leg]:
                    moved[leg][f, i] = whole[leg][f, row.hunk][0]
                elif row.sign in '+-' and not row.neutral and not any(
                        (leg, path, row.sign, row.pos[row.sign == '+']) in shown for path in file.aliases):
                    signatures[row.sign, row.text][leg].append((f, i))
    available = [set(item for sides in signatures.values() for item in sides[leg]) for leg in (0, 1)]
    for sides in signatures.values():
        if any(len(side) != 1 for side in sides):
            continue
        (f, i), (g, j) = (side[0] for side in sides)
        files = [parsed[0][f], parsed[1][g]]
        if files[0].path.rsplit('/', 1)[-1] == files[1].path.rsplit('/', 1)[-1]:
            continue
        sign = files[0].rows[i].sign
        for direction in (-1, 1):
            at = [i, j]
            while (f, at[0]) in available[0] and (g, at[1]) in available[1]:
                pair = [file.rows[n] for file, n in zip(files, at)]
                if pair[0].sign != sign or pair[1].sign != sign or pair[0].text != pair[1].text:
                    break
                moved[0][f, at[0]] = g
                moved[1][g, at[1]] = f
                at = [n + direction for n in at]
    return moved


def check_context_windows(parsed, rows, views, context):
    """Check definite differences, their fixed margins, and the outer bounds.

    `near` contains result positions in a patch's quoted source or edit
    margins. A leg's own additions are exempt from required differences:
    they belong to delta unless another source difference selects them.
    Cross-file pairings use transferred edits and their quoted context;
    native pairings exclude those transferred rows.
    """
    moved = moved_rows(parsed, rows)
    scopes = [defaultdict(set), defaultdict(set)]
    for leg in (0, 1):
        for (f, i), partner in moved[leg].items():
            scopes[leg][f, parsed[leg][f].rows[i].hunk].add(partner)
    grouped = defaultdict(lambda: [[], []])
    for row in rows:
        sides = grouped[row.paths]
        if row.section == 'context':
            sides[row.leg].append(row)
    # An entirely omitted file has no printed pairing to inspect. A common
    # path still establishes a comparison without guessing renamed partners.
    common = [{file.path for file in files} for files in parsed]
    for path in common[0] & common[1]:
        if not any(path in pair for pair in grouped):
            grouped[path, path]
    for paths, sides in grouped.items():
        near, added = [set(), set()], [set(), set()]
        cross_file = any(paths[1 - leg] in parsed[1 - leg][partner].aliases
                         for leg in (0, 1) for item, partner in moved[leg].items()
                         if paths[leg] in parsed[leg][item[0]].aliases)
        for leg, files in enumerate(parsed):
            count = len(views[leg].get(paths[leg], [{}, {}])[1])
            for f, file in enumerate(files):
                if paths[leg] not in file.aliases:
                    continue
                for i, row in enumerate(file.rows):
                    if row.sign == '+' and not row.neutral:
                        added[leg].add(row.pos[1])
                    partner = moved[leg].get((f, i))
                    if cross_file:
                        destinations = scopes[leg][f, row.hunk] if row.sign == ' ' else {partner}
                        if not any(p is not None and paths[1 - leg] in parsed[1 - leg][p].aliases
                                   for p in destinations):
                            continue
                    elif partner is not None:
                        continue
                    if row.sign != '-':
                        near[leg].add(row.pos[1])
                    if row.sign in '+-' and not row.neutral:
                        near[leg].update(range(max(0, row.pos[1] - context),
                                               min(count, row.pos[1] + context + (row.sign == '+'))))
            source = views[leg].get(paths[leg], [{}, {}])[1]
            other = set(views[1 - leg].get(paths[1 - leg], [{}, {}])[1].values())
            printed = {r.pos[1]: r for r in sides[leg]}
            for i in near[leg] - added[leg]:
                if i in source and source[i] not in other:
                    assert i in printed and printed[i].sign in '+-', (
                        'missing definite context difference', paths, leg, i + 1)
            for row in sides[leg]:
                if row.sign == ' ' or row.pos[1] not in near[leg] - added[leg]:
                    continue
                required = set(range(max(0, row.pos[1] - context),
                                     min(count, row.pos[1] + context + 1)))
                assert required <= printed.keys(), (
                    'missing source margin', paths, leg, row.pos[1] + 1, sorted(required - printed.keys()))
        # Margins may contain further differences, but cannot expand again
        allowed = [set(), set()]
        for leg in (0, 1):
            for i in near[leg]:
                allowed[leg].update(range(max(0, i - context), i + context + 1))
            # Adjacent matching source lines enclose an empty gap on this
            # side. A selected difference between them on the other side
            # requires a fixed margin here, even outside this patch's scope.
            count = len(views[leg].get(paths[leg], [{}, {}])[1])
            last_pair = max((r.pair for side in sides for r in side), default=-1)
            anchors = [(-1, -1)]
            anchors += [(r.pos[1], r.pair) for r in sides[leg] if r.sign == ' ']
            anchors.append((count, last_pair + 1))
            seeds = [r.pair for r in sides[1 - leg]
                     if r.sign in '+-' and r.pos[1] in near[1 - leg] - added[1 - leg]]
            for (before, first), (after, last) in zip(anchors, anchors[1:]):
                if after == before + 1 and any(first < p < last for p in seeds):
                    allowed[leg].update(range(max(0, after - context),
                                              min(count, after + context)))
            for row in sides[leg]:
                if row.sign in '+-':
                    assert row.pos[1] in allowed[leg], (
                        'context expanded beyond the source window', row)


def paired_cells(rows, reverse=False):
    """Normalize actual pairings, including padding, for operand reversal."""
    pairs = defaultdict(lambda: [None, None])
    for row in rows:
        paths = row.paths[::-1] if reverse else row.paths
        key = row.section, paths, row.pair
        pairs[key][1 - row.leg if reverse else row.leg] = (
            row.sign if row.section == 'delta' else row.sign != ' ',
            row.shared, row.pos, row.source)
    return Counter((section, paths, tuple(cells))
                   for (section, paths, _), cells in pairs.items())


def check_review(patches, output, full_sources=None, context=None):
    """Check source bytes, coordinates, ownership, uniqueness, and accounting.

    The remaining signed-row multisets must agree after displayed and exact
    no-effect changes are removed. This is a coverage check, not a claim that
    every repeated line was paired at the correct semantic location.
    """
    parsed = [parse_patch(patch) for patch in patches]
    original = [defaultdict(list), defaultdict(list)]
    for leg in (0, 1):
        for f, file in enumerate(parsed[leg]):
            for i, row in enumerate(file.rows):
                if row.sign in '+-':
                    stage = row.sign == '+'
                    for path in file.aliases:
                        original[leg][path, row.sign, row.pos[stage]].append(((f, i), row))
    known_paths = {name for side in parsed for file in side for name in file.aliases}
    if full_sources:
        known_paths.update(name for side in full_sources for name in side)
    printed = parse_review(output, known_paths)
    shown = [set(), set()]
    shared_shown = [set(), set()]
    for row in printed:
        if row.section == 'delta':
            candidates = []
            path = row.paths[row.leg]
            if row.sign in '+-':
                stage = row.sign == '+'
                candidates = original[row.leg].get((path, row.sign, row.pos[stage]), [])
                assert candidates, ('invented edit or wrong coordinate', path, row)
                candidates = [(i, orig) for i, orig in candidates
                              if i not in shown[row.leg] and i not in shared_shown[row.leg]]
                assert candidates, ('duplicate displayed edit', path, row)
            else:
                candidates = []
                if full_sources and path in full_sources[row.leg]:
                    views = full_sources[row.leg][path]
                    before = views[0].get(row.pos[0])
                    after = views[1].get(row.pos[1])
                    if before is not None and before == after:
                        candidates.append((None, Row(' ', before, row.pos)))
                else:
                    for file in parsed[row.leg]:
                        if path not in file.aliases:
                            continue
                        before = file.views[0].get(row.pos[0])
                        after = file.views[1].get(row.pos[1])
                        if before is not None and before == after:
                            candidates.append((None, Row(' ', before, row.pos)))
                        for original_row in file.rows:
                            if original_row.sign == ' ' and original_row.pos == row.pos:
                                candidates.append((None, original_row))
                assert candidates, ('invented retained row', row)
            matching = []
            for index, original_row in candidates:
                if matches_source(row, original_row.text):
                    matching.append((index, original_row.text))
            assert matching, ('changed source text or final newline', row, candidates)
            index, original_text = matching[0]
            row.source = original_text
            row.text = display_text(original_text)
            if index is not None:
                (shared_shown if row.shared else shown)[row.leg].add(index)
        else:
            leg = row.leg
            path = row.paths[leg]
            expected = []
            if full_sources and path in full_sources[leg]:
                text = full_sources[leg][path][1].get(row.pos[1])
                if text is not None:
                    expected.append(text)
            else:
                for file in parsed[leg]:
                    if path in file.aliases:
                        expected.extend(r.text for r in file.rows
                                        if r.sign != '-' and r.pos[1] == row.pos[1])
            assert expected, ('unquoted context source', row, leg)
            matching = [text for text in expected if matches_source(row, text)]
            assert matching, ('changed context source', row, expected, leg)
            row.source = matching[0]
            row.text = display_text(matching[0])
    if not any(file.ambiguous for side in parsed for file in side):
        views = full_sources or [dict((name, file.views) for file in files for name in file.aliases)
                                 for files in parsed]
        local_order = set()
        for leg, files in enumerate(parsed):
            names = Counter(name for file in files for name in file.aliases)
            for file in files:
                positions = [[r.pos[stage] for r in file.rows
                              if r.sign != ('+' if stage == 0 else '-')]
                             for stage in (0, 1)]
                # Overlapping quoted hunks and successive patches for the
                # same file can repeat coordinates across output groups.
                overlap = any(b <= a for side in positions for a, b in zip(side, side[1:]))
                if overlap or any(names[name] > 1 for name in file.aliases):
                    local_order.update((leg, name) for name in file.aliases)
        check_relationships(printed, views, local_order)
        if full_sources and context is not None:
            check_context_windows(parsed, printed, full_sources, context)
    # Edits omitted as shared must balance across both reports. This final
    # multiset check permits moves between files; row checks above establish
    # each displayed edit's source and coordinates independently.
    remaining = [Counter(), Counter()]
    opaque = [opaque_deletions(patch) for patch in patches]
    for leg in (0, 1):
        for f, file in enumerate(parsed[leg]):
            other = opaque[1 - leg].get(file.path, b'')
            shared_deletion = (file.deletion_id and other and
                (len(file.deletion_id) <= 40) == (len(other) <= 40) and
                file.deletion_id[:len(other)] == other[:len(file.deletion_id)])
            for i, row in enumerate(file.rows):
                if row.sign in '+-' and not row.neutral and (f, i) not in shown[leg]:
                    if row.sign == '-' and shared_deletion:
                        continue
                    remaining[leg][row.sign, row.text] += 1
    assert remaining[0] == remaining[1], ('unaccounted original changes',
        remaining[0] - remaining[1], remaining[1] - remaining[0])
    return {'shown': [len(side) for side in shown],
            'edits': [[parsed[leg][f].rows[i] for f, i in sorted(shown[leg])]
                      for leg in (0, 1)],
            'shared': sum(remaining[0].values()), 'rows': printed}
