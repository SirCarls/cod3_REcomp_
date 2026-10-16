//! Checks that a `.pk3` / `.iwd` archive is a whole, readable ZIP file
//! without unpacking it.
//!
//! Both games' archives are ordinary ZIP files (PKWARE APPNOTE.TXT, see
//! docs/SOURCES.md). The check reads only the end of the file and the
//! central directory: it finds the end-of-central-directory record (also
//! when a comment follows it), walks every central directory entry and
//! checks that each one points inside the file. A file cut short by a failed
//! download or copy loses its end record, so truncation is caught.
//!
//! The archives come from the player's disk, so they are untrusted input:
//! sizes and counts are checked before anything is allocated.

use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

/// Largest archive accepted. The biggest official archive is about 170 MB;
/// anything near the 4 GiB ZIP limit is not one of the games' files.
pub const MAX_ARCHIVE_BYTES: u64 = 2 << 30;
/// Largest central directory read into memory. A full CoD2 archive's
/// directory is well under a megabyte.
pub const MAX_CENTRAL_DIRECTORY_BYTES: u32 = 64 << 20;

const END_RECORD_SIGNATURE: [u8; 4] = *b"PK\x05\x06";
const END_RECORD_BYTES: usize = 22;
const MAX_COMMENT_BYTES: usize = 0xFFFF;
const ZIP64_LOCATOR_SIGNATURE: [u8; 4] = *b"PK\x06\x07";
const ZIP64_LOCATOR_BYTES: u64 = 20;
const CENTRAL_ENTRY_SIGNATURE: [u8; 4] = *b"PK\x01\x02";
const CENTRAL_ENTRY_BYTES: usize = 46;
const LOCAL_HEADER_SIGNATURE: [u8; 4] = *b"PK\x03\x04";
const LOCAL_HEADER_BYTES: u64 = 30;
/// General purpose flag bit 0: the entry is encrypted.
const FLAG_ENCRYPTED: u16 = 1;
const METHOD_STORED: u16 = 0;
const METHOD_DEFLATED: u16 = 8;

/// What the check learned about a good archive.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ZipSummary {
    /// Number of files and folders in the archive.
    pub entries: u32,
    pub stored: u32,
    pub deflated: u32,
    /// Entries with another compression method (the games use only stored
    /// and deflated, so this should be 0).
    pub other_methods: u32,
    /// Sum of the entries' uncompressed sizes.
    pub unpacked_bytes: u64,
    /// Length of the comment after the end record.
    pub comment_bytes: u16,
}

#[derive(Debug)]
pub enum ZipError {
    Io(io::Error),
    /// Smaller than an empty ZIP file.
    TooSmall(u64),
    TooLarge(u64),
    /// No end-of-central-directory record: not a ZIP file, or cut short.
    NoEndRecord,
    /// Split over several disks (never the case for the games' archives).
    MultiDisk,
    /// Needs ZIP64, which the games' archives never use.
    Zip64,
    /// The central directory claims to lie outside the file.
    DirectoryOutOfBounds {
        offset: u64,
        size: u64,
        end_record: u64,
    },
    DirectoryTooLarge(u32),
    /// An entry of the central directory is damaged.
    BadEntry {
        index: u32,
        reason: &'static str,
    },
    /// The directory holds a different number of entries than the end record says.
    CountMismatch {
        expected: u32,
        found: u32,
    },
    /// The first file's local header is not where the directory says.
    BadLocalHeader,
    Encrypted {
        index: u32,
    },
}

impl fmt::Display for ZipError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ZipError::Io(e) => write!(f, "could not read it: {e}"),
            ZipError::TooSmall(n) => write!(f, "only {n} bytes, too small to be a ZIP archive"),
            ZipError::TooLarge(n) => write!(
                f,
                "{n} bytes, larger than any game archive ({MAX_ARCHIVE_BYTES} at most)"
            ),
            ZipError::NoEndRecord => write!(
                f,
                "not a ZIP archive, or cut short (no end-of-central-directory record)"
            ),
            ZipError::MultiDisk => write!(f, "a multi-disk ZIP archive, which the games never use"),
            ZipError::Zip64 => write!(f, "a ZIP64 archive, which the games never use"),
            ZipError::DirectoryOutOfBounds {
                offset,
                size,
                end_record,
            } => write!(
                f,
                "damaged: its central directory ({size} bytes at {offset}) runs past the \
                 end record at {end_record}"
            ),
            ZipError::DirectoryTooLarge(n) => write!(
                f,
                "its central directory is {n} bytes, more than {MAX_CENTRAL_DIRECTORY_BYTES}"
            ),
            ZipError::BadEntry { index, reason } => {
                write!(f, "damaged: central directory entry {index} {reason}")
            }
            ZipError::CountMismatch { expected, found } => write!(
                f,
                "damaged: the end record lists {expected} entries, the directory holds {found}"
            ),
            ZipError::BadLocalHeader => {
                write!(
                    f,
                    "damaged: the first file's header is not where the directory says"
                )
            }
            ZipError::Encrypted { index } => {
                write!(f, "entry {index} is encrypted, which the games never do")
            }
        }
    }
}

impl std::error::Error for ZipError {}

impl From<io::Error> for ZipError {
    fn from(e: io::Error) -> Self {
        ZipError::Io(e)
    }
}

fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// Checks the ZIP file at `path`.
pub fn check_file(path: &Path) -> Result<ZipSummary, ZipError> {
    let mut file = File::open(path)?;
    let len = file.metadata()?.len();
    check(&mut file, len)
}

/// Checks a ZIP archive of `len` bytes read through `reader`.
pub fn check<R: Read + Seek>(reader: &mut R, len: u64) -> Result<ZipSummary, ZipError> {
    if len < END_RECORD_BYTES as u64 {
        return Err(ZipError::TooSmall(len));
    }
    if len > MAX_ARCHIVE_BYTES {
        return Err(ZipError::TooLarge(len));
    }

    // The end record is the last 22 bytes plus an optional comment of up to
    // 65535 bytes, so it starts somewhere in the last 65557 bytes.
    let tail_len = len.min((END_RECORD_BYTES + MAX_COMMENT_BYTES) as u64) as usize;
    let tail_start = len - tail_len as u64;
    let mut tail = vec![0u8; tail_len];
    reader.seek(SeekFrom::Start(tail_start))?;
    reader.read_exact(&mut tail)?;

    // Search backwards; a signature only counts when its comment length
    // reaches exactly to the end of the file (a comment may itself contain
    // the signature bytes).
    let mut found = None;
    let mut at = tail_len - END_RECORD_BYTES;
    loop {
        if tail[at..at + 4] == END_RECORD_SIGNATURE {
            let comment = usize::from(u16_at(&tail, at + 20));
            if at + END_RECORD_BYTES + comment == tail_len {
                found = Some(at);
                break;
            }
        }
        if at == 0 {
            break;
        }
        at -= 1;
    }
    let at = found.ok_or(ZipError::NoEndRecord)?;
    let record = &tail[at..at + END_RECORD_BYTES];
    let end_record_offset = tail_start + at as u64;

    let this_disk = u16_at(record, 4);
    let directory_disk = u16_at(record, 6);
    let entries_this_disk = u16_at(record, 8);
    let entries_total = u16_at(record, 10);
    let directory_size = u32_at(record, 12);
    let directory_offset = u32_at(record, 16);
    let comment_bytes = u16_at(record, 20);

    let zip64_locator = end_record_offset >= ZIP64_LOCATOR_BYTES && {
        let locator = end_record_offset - ZIP64_LOCATOR_BYTES;
        locator >= tail_start
            && tail[(locator - tail_start) as usize..][..4] == ZIP64_LOCATOR_SIGNATURE
    };
    if zip64_locator
        || entries_total == 0xFFFF
        || directory_size == 0xFFFF_FFFF
        || directory_offset == 0xFFFF_FFFF
    {
        return Err(ZipError::Zip64);
    }
    if this_disk != 0 || directory_disk != 0 || entries_this_disk != entries_total {
        return Err(ZipError::MultiDisk);
    }
    let directory_end = u64::from(directory_offset) + u64::from(directory_size);
    if directory_end > end_record_offset {
        return Err(ZipError::DirectoryOutOfBounds {
            offset: u64::from(directory_offset),
            size: u64::from(directory_size),
            end_record: end_record_offset,
        });
    }
    if directory_size > MAX_CENTRAL_DIRECTORY_BYTES {
        return Err(ZipError::DirectoryTooLarge(directory_size));
    }

    let mut directory = vec![0u8; directory_size as usize];
    reader.seek(SeekFrom::Start(u64::from(directory_offset)))?;
    reader.read_exact(&mut directory)?;

    let mut summary = ZipSummary {
        entries: 0,
        stored: 0,
        deflated: 0,
        other_methods: 0,
        unpacked_bytes: 0,
        comment_bytes,
    };
    let mut first_local_header = None;
    let mut pos = 0usize;
    while pos < directory.len() {
        let index = summary.entries;
        if directory.len() - pos < CENTRAL_ENTRY_BYTES {
            return Err(ZipError::BadEntry {
                index,
                reason: "is cut short",
            });
        }
        let entry = &directory[pos..];
        if entry[..4] != CENTRAL_ENTRY_SIGNATURE {
            return Err(ZipError::BadEntry {
                index,
                reason: "has no entry signature",
            });
        }
        let flags = u16_at(entry, 8);
        let method = u16_at(entry, 10);
        let packed_size = u32_at(entry, 20);
        let unpacked_size = u32_at(entry, 24);
        let name_len = usize::from(u16_at(entry, 28));
        let extra_len = usize::from(u16_at(entry, 30));
        let comment_len = usize::from(u16_at(entry, 32));
        let start_disk = u16_at(entry, 34);
        let local_offset = u32_at(entry, 42);
        let entry_len = CENTRAL_ENTRY_BYTES + name_len + extra_len + comment_len;
        if entry_len > directory.len() - pos {
            return Err(ZipError::BadEntry {
                index,
                reason: "runs past the end of the directory",
            });
        }
        if name_len == 0 {
            return Err(ZipError::BadEntry {
                index,
                reason: "has no name",
            });
        }
        if start_disk != 0 {
            return Err(ZipError::MultiDisk);
        }
        if packed_size == 0xFFFF_FFFF || unpacked_size == 0xFFFF_FFFF || local_offset == 0xFFFF_FFFF
        {
            return Err(ZipError::Zip64);
        }
        if flags & FLAG_ENCRYPTED != 0 {
            return Err(ZipError::Encrypted { index });
        }
        // The file's data (local header, then its packed bytes) must end
        // before the central directory starts.
        let data_end =
            u64::from(local_offset) + LOCAL_HEADER_BYTES + name_len as u64 + u64::from(packed_size);
        if data_end > u64::from(directory_offset) {
            return Err(ZipError::BadEntry {
                index,
                reason: "points past the start of the central directory",
            });
        }
        match method {
            METHOD_STORED => summary.stored += 1,
            METHOD_DEFLATED => summary.deflated += 1,
            _ => summary.other_methods += 1,
        }
        summary.unpacked_bytes += u64::from(unpacked_size);
        if first_local_header.is_none() {
            first_local_header = Some(local_offset);
        }
        summary.entries += 1;
        if summary.entries > u32::from(entries_total) {
            break;
        }
        pos += entry_len;
    }
    if summary.entries != u32::from(entries_total) {
        return Err(ZipError::CountMismatch {
            expected: u32::from(entries_total),
            found: summary.entries,
        });
    }
    if let Some(offset) = first_local_header {
        let mut signature = [0u8; 4];
        reader.seek(SeekFrom::Start(u64::from(offset)))?;
        reader.read_exact(&mut signature)?;
        if signature != LOCAL_HEADER_SIGNATURE {
            return Err(ZipError::BadLocalHeader);
        }
    }
    Ok(summary)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testkit::{stored_zip, stored_zip_with_comment};
    use std::io::Cursor;

    fn check_bytes(bytes: &[u8]) -> Result<ZipSummary, ZipError> {
        check(&mut Cursor::new(bytes), bytes.len() as u64)
    }

    #[test]
    fn good_archive() {
        let zip = stored_zip(&[("maps/mp/test.bsp", b"hello"), ("readme.txt", b"")]);
        let summary = check_bytes(&zip).unwrap();
        assert_eq!(summary.entries, 2);
        assert_eq!(summary.stored, 2);
        assert_eq!(summary.unpacked_bytes, 5);
        assert_eq!(summary.comment_bytes, 0);
    }

    #[test]
    fn empty_archive() {
        let zip = stored_zip(&[]);
        assert_eq!(zip.len(), END_RECORD_BYTES);
        assert_eq!(check_bytes(&zip).unwrap().entries, 0);
    }

    #[test]
    fn archive_with_comment() {
        // A comment holding the end signature itself must not confuse the search.
        let mut comment = b"made by a test PK".to_vec();
        comment.extend_from_slice(&END_RECORD_SIGNATURE);
        comment.extend_from_slice(&[0; 30]);
        let zip = stored_zip_with_comment(&[("a.txt", b"abc")], &comment);
        let summary = check_bytes(&zip).unwrap();
        assert_eq!(summary.entries, 1);
        assert_eq!(usize::from(summary.comment_bytes), comment.len());
    }

    #[test]
    fn truncated_archive_is_refused() {
        let zip = stored_zip(&[("a.txt", b"abcdef"), ("b.txt", b"ghijkl")]);
        for cut in [1, 5, 21, 30, zip.len() - 30] {
            let short = &zip[..zip.len() - cut];
            assert!(check_bytes(short).is_err(), "cut {cut} bytes");
        }
        assert!(matches!(
            check_bytes(&zip[..zip.len() - 1]),
            Err(ZipError::NoEndRecord)
        ));
    }

    #[test]
    fn garbage_is_refused() {
        assert!(matches!(check_bytes(b"MZ"), Err(ZipError::TooSmall(2))));
        assert!(matches!(
            check_bytes(&[0x4D; 4096]),
            Err(ZipError::NoEndRecord)
        ));
        // A lone end record that claims a directory before the file start.
        let mut record = vec![0u8; END_RECORD_BYTES];
        record[..4].copy_from_slice(&END_RECORD_SIGNATURE);
        record[10] = 3; // 3 entries
        record[8] = 3;
        record[12] = 200; // directory size
        assert!(matches!(
            check_bytes(&record),
            Err(ZipError::DirectoryOutOfBounds { .. })
        ));
    }

    #[test]
    fn damaged_directory_is_refused() {
        let mut zip = stored_zip(&[("a.txt", b"abc")]);
        // Find the central entry and break its signature.
        let at = zip
            .windows(4)
            .position(|w| w == CENTRAL_ENTRY_SIGNATURE)
            .unwrap();
        zip[at] = b'X';
        assert!(matches!(check_bytes(&zip), Err(ZipError::BadEntry { .. })));

        // An entry whose data would overlap the directory.
        let mut zip = stored_zip(&[("a.txt", b"abc")]);
        let at = zip
            .windows(4)
            .position(|w| w == CENTRAL_ENTRY_SIGNATURE)
            .unwrap();
        zip[at + 20..at + 24].copy_from_slice(&1000u32.to_le_bytes());
        assert!(matches!(check_bytes(&zip), Err(ZipError::BadEntry { .. })));

        // An end record that counts more entries than the directory has.
        let mut zip = stored_zip(&[("a.txt", b"abc")]);
        let end = zip.len() - END_RECORD_BYTES;
        zip[end + 8] = 2;
        zip[end + 10] = 2;
        assert!(check_bytes(&zip).is_err());
    }

    #[test]
    fn absurd_size_is_refused_before_reading() {
        let mut cursor = Cursor::new(Vec::new());
        assert!(matches!(
            check(&mut cursor, MAX_ARCHIVE_BYTES + 1),
            Err(ZipError::TooLarge(_))
        ));
    }
}
