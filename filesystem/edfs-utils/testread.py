#!/usr/bin/env python3
# EdFS -- An educational file system
# Copyright (C) 2017--2026 Leiden University, The Netherlands.

import os
import sys
import hashlib
import argparse

from pathlib import Path

files = (
    # filename        MD5
    ("sonnet18.txt", "d5ae6056523475163b1f8e64776053cf"),
    ("aesop.txt",    "5b37c689c5978a1244498f3598b2d1b4"),
    ("file1.txt",    "6b8d91211b247b4eef395dc8789ed52d"),
    ("file2.txt",    "7320de03916e08e86693ad75ef8800dc"),
    ("file3.txt",    "9fdb85c793cf43095c11acff998b9a2f"),
    ("file4.txt",    "9fdb85c793cf43095c11acff998b9a2f"),
    ("file5.txt",    "93b47be308f91225e4716ac09f839609"),
    ("leiden.png",   "72164757ae461d7539a9e23c265533a3"),
)

partial = (
    # filename   offset  size   MD5
    ("aesop.txt",   400,  256, "e88b0efd7c71466c5970fd90fa927a6a"),
    ("aesop.txt",   437, 2142, "cf88e4c67bd4c182339e9b098108dc98"),
    ("aesop.txt", 16881, 3100, "8b0395b93e1c5d02a55c268f24cab980"),
    ("alice.txt", 31870, 1763, "a2850fc2f447ac13a33ea6775d070023"),
)

def read_file_max(filename):
    """
    Read the entire file @filename.
    Returns the MD5 checksum of the data.
    """
    contents = filename.read_bytes()

    m = hashlib.md5()
    m.update(contents)

    return m.hexdigest()

def read_file_partial(filename, start, count):
    """
    Read @count bytes from a file @filename starting at offset @start.
    Returns the MD5 checksum of the data.
    """
    m = hashlib.md5()

    fd = os.open(filename, os.O_DIRECT)
    os.lseek(fd, start, os.SEEK_SET)

    b = os.read(fd, count)
    m.update(b)

    os.close(fd)

    return m.hexdigest()

def read_file(filename, size):
    """
    Read a file @filename using a buffer size of @size.
    Returns the MD5 checksum.
    """
    fd = os.open(filename, os.O_DIRECT)

    m = hashlib.md5()
    b = os.read(fd, size)
    while b:
        m.update(b)
        b = os.read(fd, size)

    os.close(fd)
    return m.hexdigest()

def read_file_with_sizes(filename, sizes):
    """
    Read a file @filename using varying sizes from the list @sizes.
    Returns the MD5 checksum.
    """
    i = 0
    m = hashlib.md5()

    fd = os.open(filename, os.O_DIRECT)

    b = os.read(fd, sizes[i])
    while b:
        m.update(b)
        i = (i + 1) % len(sizes)
        b = os.read(fd, sizes[i])

    os.close(fd)

    return m.hexdigest()

def run_tests(mp, descr, func, *args):
    for filename, want in files:
        if func(mp / filename, *args) != want:
            print("{}: test {} failed.".format(filename, descr))

def run_tests_partial_reads(mp):
    for filename, off, size, want in partial:
      got = read_file_partial(mp / filename, off, size)
      if got != want:
          print("{}: partial read test failed.".format(filename))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mountpoint", type=str,
                        help="Mountpoint of EdFS file system")
    args = parser.parse_args()

    mp = Path(args.mountpoint)
    if not mp.exists() or not mp.is_dir():
        print("{}: not a mountpount.".format(mp), file=sys.stderr)
        exit(1)

    run_tests(mp, "read with max size", read_file_max)

    run_tests(mp, "read with size 256", read_file, 256)
    run_tests(mp, "read with size 512", read_file, 512)
    run_tests(mp, "read with size 223", read_file, 223)

    run_tests(mp, "read with different sizes",
              read_file_with_sizes, [564, 12, 226, 16, 54, 90])

    run_tests_partial_reads(mp)
