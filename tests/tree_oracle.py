# SPDX-License-Identifier: GPL-2.0-only
"""Read actual Git blobs independently of the program's libgit2 reader."""

from pathlib import Path
import subprocess

from review_oracle import lines, parse_patch


class Objects:
    def __init__(self, repository):
        self.process = subprocess.Popen(
            ['git', '-C', str(repository), 'cat-file', '--batch'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, expression):
        self.process.stdin.write(expression.encode('utf-8', 'surrogateescape') + b'\n')
        self.process.stdin.flush()
        header = self.process.stdout.readline().split()
        assert header, ('Git object reader stopped', expression)
        if header[-1] == b'missing':
            return None, None, None
        oid, kind, size = header
        body = self.process.stdout.read(int(size))
        assert self.process.stdout.read(1) == b'\n'
        return oid.decode(), kind, body

    def commit(self, revision):
        explicit = revision.split('..') if '..' in revision else None
        oid, kind, body = self.read(explicit[1] if explicit else revision)
        assert kind == b'commit', (revision, kind)
        parents = [line[7:].decode() for line in body.split(b'\n\n', 1)[0].split(b'\n')
                   if line.startswith(b'parent ')]
        parent = parents[0] if parents else None
        if explicit:
            parent, kind, _ = self.read(explicit[0])
            assert kind == b'commit' and parent in parents
        return oid, parent

    def source(self, revision, path):
        if revision is None:
            return {}
        _, kind, body = self.read(revision + ':' + path)
        assert kind in (None, b'blob'), (revision, path, kind)
        return dict(enumerate(lines(body or b'')))

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        assert self.process.wait() == 0


def derive_inputs(repository, revisions, derive, env):
    patches = []
    for revision in revisions:
        operands = revision.split('..')
        operands = operands[::-1] if len(operands) == 2 else operands
        patches.append(subprocess.check_output(
            [str(derive), str(repository), *operands], env=env))
    return patches


def check_quotations(parsed, sources):
    """Every original quote must occupy its declared position in the source."""
    for leg in (0, 1):
        for file in parsed[leg]:
            if not file.rows:
                continue
            candidates = [sources[leg][p] for p in file.aliases if p in sources[leg]]
            assert candidates, ('no original source for', leg, file.path)
            for row in file.rows:
                for stage in (0, 1):
                    if row.sign == ('+' if stage == 0 else '-'):
                        continue
                    assert any(view[stage].get(row.pos[stage]) == row.text
                               for view in candidates), ('original quote differs', leg, file.path, row)


def tree_sources(repository, revisions, patches):
    objects = Objects(repository)
    try:
        parsed = [parse_patch(p) for p in patches]
        paths = {p for files in parsed for file in files for p in file.aliases}
        sources = []
        for revision in revisions:
            tip, parent = objects.commit(revision)
            sources.append({p: [objects.source(parent, p), objects.source(tip, p)]
                            for p in paths})
        check_quotations(parsed, sources)
        return sources
    finally:
        objects.close()
