# System 7.5.3 starter disk repair

The `MicroMac7` volume in the published starter ZIP contains 509 files. Its primary HFS master directory block records 508. The root has four files and its recorded count is three. Two Finder aliases in `System Folder:Apple Menu Items:Recent Applications` also share file ID 864. Both have independent resource forks, neither has a catalog thread or overflow extents, and ID 865 is unused. The older alias is `Prince of Persia`. The newer alias, `Speedometer 4.02`, receives ID 865. Its resource fork and target remain intact.

The firmware checks the first selected writable hard disk during startup. A candidate must be a flat `.dsk` of exactly 1,048,576,000 bytes with the starter's dirty primary header. It hashes bytes `[0, 2106880)` followed by the last 1,024 bytes. This covers the boot blocks, both volume headers, allocation bitmap, and entire extents-overflow and catalog files. The hash authenticates the filesystem structures involved in the repair. File payloads remain outside the startup check. The installer separately verifies the full downloaded disk.

Each changed byte is written, synced and read back before the next edit. A retry accepts the original and corrected values at these five offsets and authenticates the metadata against the original hash. The clean flag is written last. If a check fails, the original guest mount path handles the volume.

| Byte offset | Original | Repaired | Field |
| ---: | ---: | ---: | --- |
| 1137333 | `60` | `61` | Speedometer alias file ID, 864 → 865 |
| 1057 | `61` | `62` | Next catalog ID, 865 → 866 |
| 1037 | `03` | `04` | Files in root, 3 → 4 |
| 1111 | `fc` | `fd` | Files in volume, 508 → 509 |
| 1034 | `00` | `01` | Clean-unmount attribute |

## Reproduction

| Input | SHA-256 |
| --- | --- |
| Published `sdCard.zip` | `1417a88acb433927d6a6ea7dd487ddbf6fe9a6f2afbb879eac49f16dbfb373e8` |
| Original `Macintosh7.dsk` | `37aca18d2ddeadd29851c7fd49b73475da37b3578514066fcf9b745840fdc2f5` |
| Original boot and filesystem metadata, in firmware read order | `46402dbdbd6ddeedabacc30434f57d45c525d55387cbe3c42f6394ece4b53a5b` |
| Repaired disk immediately before the first guest mount | `76eb952c2f1829c411b9f1a715b0a6db28006d6eb8e86b0eaacaa8b4c5bf5305` |

On a copy of the official disk, [Debian's classic-HFS checker](https://packages.debian.org/bookworm/arm64/hfsprogs) (`fsck.hfs -d -f -n`, version 540.1-Linux) identified a volume file-count discrepancy before repair. After the five-byte change it passed the extents-file, catalog-file, catalog-hierarchy, allocation-bitmap and volume-information checks. A separate catalog and extent walk found 509 unique file IDs, 61 directories including the root, matching directory child counts, and exact agreement between referenced blocks and the allocation bitmap. The remaining contents of the disk matched the original byte-for-byte.

Run the native regression tests with the original disk available locally. They read the supplied image and exercise repairs on a temporary copy of its filesystem metadata.

```sh
M5TAB_STARTER7_DISK=/path/to/Macintosh7.dsk python3 -m unittest discover -s test -p test_hfs_starter7.py
```

To recover the original published contents, extract a fresh `Macintosh7.dsk` from the ZIP. Keep any copy containing later guest changes separate.
