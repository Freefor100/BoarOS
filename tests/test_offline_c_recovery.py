"""The offline compiler probe may repair only crash-time ext4 orphans."""

import runpy
import unittest
from pathlib import Path


PROBE = runpy.run_path(str(Path(__file__).with_name("offline-c-riscv.py")))


class OrphanRecoveryTest(unittest.TestCase):
    def test_accepts_only_orphan_cleanup_and_related_counts(self):
        checker = PROBE.get("orphan_cleanup_only")
        self.assertIsNotNone(checker)
        audit = """e2fsck 1.47.4 (6-Mar-2025)
Pass 1: Checking inodes, blocks, and sizes
Inodes that were part of a corrupted orphan linked list found.  Fix? no

Inode 1335 was part of the orphaned inode list.  IGNORED.
Pass 2: Checking directory structure
Pass 3: Checking directory connectivity
Pass 4: Checking reference counts
Pass 5: Checking group summary information
Free blocks count wrong (68993, counted=68948).
Fix? no
Free inodes count wrong (32671, counted=32665).
Fix? no
/disk.img: ********** WARNING: Filesystem still has errors **********
/disk.img: 1329/34000 files (0.2% non-contiguous), 66895/135888 blocks
"""
        self.assertTrue(checker(audit, repaired=False))
        self.assertFalse(checker(audit + "Block bitmap differences: +(100--102)\n",
                                 repaired=False))
        self.assertFalse(checker(audit.replace("Inode 1335 was part of the orphaned inode list.  IGNORED.\n", ""),
                                 repaired=False))
        repair = audit.replace("Fix? no", "Fix? yes").replace("IGNORED.", "FIXED.")
        repair = repair.replace("********** WARNING: Filesystem still has errors **********",
                                "***** FILE SYSTEM WAS MODIFIED *****")
        self.assertTrue(checker(repair, repaired=True))
        self.assertFalse(checker(repair + "Unattached inode 22\n", repaired=True))


if __name__ == "__main__":
    unittest.main()
