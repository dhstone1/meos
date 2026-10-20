MeOS ext2 disk (mount point /hd)

Contents of this image are packed at build time by
tools/mkext2.py from the src/ext2root/ seed tree. The kernel
itself only trusts the ext2 superblock / group descriptor --
the fixed layout below is a convenience, not a contract.

  hello.txt    smoke test file
  readme.txt   this file
  etc/motd     login banner

Discipline (same as src/ramfs): printable ASCII + newline only,
because the console font covers 0x20..0x7E and nothing else.
