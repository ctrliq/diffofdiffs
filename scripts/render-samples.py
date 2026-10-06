#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the sample reports, or check them without changing files."""
import argparse
import os
import subprocess
import sys
from pathlib import Path


SAMPLES = (('arguments', 'light'), ('indentation', 'dark'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='fail if a sample differs from generated output')
    parser.add_argument('binary', help='diffofdiffs executable to use')
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    root = Path(__file__).resolve().parent.parent
    environment = dict(os.environ, LC_ALL='C.UTF-8')
    stale = False

    try:
        for name, theme in SAMPLES:
            destination = root / 'samples' / (name + '.html')

            # Relative input names keep labels independent of the checkout path
            result = subprocess.run(
                [str(binary), '--backport-labels', '--html', '--theme=' + theme,
                 'backport.patch', 'upstream.patch'],
                cwd=str(root / 'samples' / name), env=environment,
                stdout=subprocess.PIPE, check=True)
            if (destination.is_file() and
                    destination.read_bytes() == result.stdout):
                continue
            if args.check:
                print(f'samples/{name}.html is out of date; '
                      'run make update-samples', file=sys.stderr)
                stale = True
            else:
                destination.write_bytes(result.stdout)
                print(f'Updated samples/{name}.html')
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'sample reports: {error}', file=sys.stderr)
        return 1

    if args.check and not stale:
        print('sample reports: up to date')
    return stale


if __name__ == '__main__':
    sys.exit(main())
