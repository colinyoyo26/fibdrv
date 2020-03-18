#!/usr/bin/env python3

expect = [0, 1]
N = 100

for i in range(2, N + 1):
    expect.append(expect[i - 1] + expect[i - 2])
with open('expected_hex.txt', 'w') as f:
    f.truncate()
    for i in range(0, N + 1):
        f.write('Writing to /dev/fibonacci, returned the sequence 1\n')
    for i in range(0, N + 1):
        f.write('Reading from /dev/fibonacci at offset %d, returned the sequence %X.\n' % (i, expect[i]))
    for i in range(N, -1, -1):
        f.write('Reading from /dev/fibonacci at offset %d, returned the sequence %X.\n' % (i, expect[i]))