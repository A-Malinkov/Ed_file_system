# Ed_file_system
Architecture: Uses FUSE as an intermediary between the kernel and the ./edfuse program.

Storage: Operates on a virtual disk image rather than a physical hardware device.

Structure: Features a boot block, superblock, bitmap, inode table, and data blocks.

File Limits: Supports file names up to 59 characters and utilizes indirect blocks for files larger than two blocks.