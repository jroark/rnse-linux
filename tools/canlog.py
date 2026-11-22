#!/usr/bin/env python3
"""The 193 kernel's log from a log of the CAN bus (rnse-sdk's
tools/canbench.py or bench193.py): the frames 0x7FA, a counter and up
to seven characters each, put together again.  A gap in the counter (a
frame the bus did not take, or the log missed) shows as [...].

    canlog.py LOG          the text so far
    canlog.py -f LOG       and what comes, as it comes"""
import sys
import time


def main():
    follow = sys.argv[1:2] == ['-f']
    path = sys.argv[2] if follow else sys.argv[1]
    last = None
    with open(path, errors='replace') as f:
        while True:
            line = f.readline()
            if not line:
                if not follow:
                    break
                time.sleep(0.2)
                continue
            w = line.split()
            if len(w) < 5 or w[1] != 'rx' or w[2] != '7fa':
                continue
            n = int(w[3].strip('[]'))
            d = bytes(int(x, 16) for x in w[4:4 + n])
            if not d:
                continue
            if last is not None and d[0] != (last + 1) & 255:
                sys.stdout.write('[...]')
            last = d[0]
            sys.stdout.write(d[1:].decode('latin-1'))
            sys.stdout.flush()


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main()
