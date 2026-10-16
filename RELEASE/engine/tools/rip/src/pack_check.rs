//! The rules a texture replacement pack must follow, as the game's loader
//! (SDK patch 0016, `replacement.cpp`) checks them: the pack writers here
//! check their own output with them, and `rip cod3-check-pack <pack dir>`
//! checks any pack (one downloaded, or edited by hand) before the game uses
//! it. Everything a pack holds is treated as untrusted input:
//!
//! - `index.tsv` is at most `MAX_INDEX_BYTES`; each line is
//!   `<key>\t<width>\t<height>\t<xenos_format>\t<file>` with a 16-hex-digit
//!   key, sides 1 to 8192, one of the five formats, and a `file` that is a
//!   plain relative path inside the pack (`is_plain_pack_file`). The file,
//!   symbolic links followed, must be inside the pack folder.
//! - Each DDS has a legacy header (no DX10 header, no cube map or volume) in
//!   one of the pack formats, sides 1 to `MAX_IMAGE_SIDE`, at most
//!   `MAX_IMAGE_LEVELS` levels and no more than a full chain, and is exactly
//!   as long as its levels; its format is the index line's and its size has
//!   the original's aspect ratio (`is_replacement_size`). Sizes come from the
//!   DDS header, checked, never from the index.

use std::collections::BTreeSet;
use std::path::Path;
use xenos::Format;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Longest `file` in an index line, in bytes.
pub const MAX_PACK_FILE_LENGTH: usize = 255;
/// Largest index.tsv read (a pack of every texture of a game is a few MB).
pub const MAX_INDEX_BYTES: u64 = 64 << 20;
/// Largest side of a pack image, whatever the GPU can do.
pub const MAX_IMAGE_SIDE: u32 = 16384;
/// Most levels a DDS header may declare (a full chain of `MAX_IMAGE_SIDE`).
pub const MAX_IMAGE_LEVELS: u32 = 15;
/// Largest side of an original texture (the Xenos fetch constant's limit).
const MAX_TEXTURE_SIDE: u32 = 8192;
const DDS_HEADER_BYTES: u64 = 128;
/// Problems printed by name before only counting the rest.
const PROBLEMS_SHOWN: usize = 20;

/// True if `file` may name a pack's DDS: a relative path with `/` between
/// its parts, none of them empty, `.` or `..`, no backslash, no colon (a
/// drive letter or an alternate data stream), no control character, at most
/// `MAX_PACK_FILE_LENGTH` bytes. The same test as the game's
/// `IsPlainPackFile`.
pub fn is_plain_pack_file(file: &str) -> bool {
    if file.is_empty() || file.len() > MAX_PACK_FILE_LENGTH || file.starts_with('/') {
        return false;
    }
    if file
        .bytes()
        .any(|c| c < 0x20 || c == 0x7F || c == b'\\' || c == b':')
    {
        return false;
    }
    file.split('/')
        .all(|part| !part.is_empty() && part != "." && part != "..")
}

/// True if `path` (an existing file), symbolic links followed, lies inside
/// `folder`.
pub fn is_inside_folder(folder: &Path, path: &Path) -> bool {
    match (folder.canonicalize(), path.canonicalize()) {
        (Ok(folder), Ok(path)) => path != folder && path.starts_with(&folder),
        _ => false,
    }
}

/// Whether a `width` x `height` image may stand for an original of
/// `original` size: its aspect ratio to within the pack makers' rounding
/// (3.5 texels a side), `|w * oh - h * ow| <= 4 * (ow + oh)`, as the game
/// checks it.
pub fn is_replacement_size(original: (u32, u32), width: u32, height: u32) -> bool {
    let (ow, oh) = (original.0 as u64, original.1 as u64);
    if ow == 0 || oh == 0 || width == 0 || height == 0 {
        return false;
    }
    (width as u64 * oh).abs_diff(height as u64 * ow) <= 4 * (ow + oh)
}

/// What a DDS header says, checked.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DdsInfo {
    pub format: Format,
    pub width: u32,
    pub height: u32,
    /// Declared levels (1 when the header declares none).
    pub levels: u32,
    /// Bytes of all declared levels: the file must be 128 + this long.
    pub data_bytes: u64,
}

fn le32(header: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([header[at], header[at + 1], header[at + 2], header[at + 3]])
}

/// Bytes of level `level` of a `width` x `height` image in `format`.
pub fn level_bytes(format: Format, width: u32, height: u32, level: u32) -> u64 {
    let (block, block_bytes) = format.block().unwrap_or((1, 4));
    let w = (width >> level).max(1);
    let h = (height >> level).max(1);
    w.div_ceil(block) as u64 * h.div_ceil(block) as u64 * block_bytes as u64
}

/// Checks a DDS file's first 128 bytes the way the game does (see the module
/// documentation). Nothing here trusts a value before checking it.
pub fn parse_dds_header(header: &[u8]) -> std::result::Result<DdsInfo, String> {
    if header.len() < DDS_HEADER_BYTES as usize {
        return Err("too short for a DDS header".into());
    }
    if &header[..4] != b"DDS " || le32(header, 4) != 124 || le32(header, 76) != 32 {
        return Err("not a DDS file".into());
    }
    let flags = le32(header, 8);
    let height = le32(header, 12);
    let width = le32(header, 16);
    let mip_count = le32(header, 28);
    let pixel_flags = le32(header, 80);
    let caps2 = le32(header, 112);
    const DDSD_MIPMAPCOUNT: u32 = 0x2_0000;
    const DDPF_ALPHAPIXELS: u32 = 0x1;
    const DDPF_FOURCC: u32 = 0x4;
    const DDPF_RGB: u32 = 0x40;
    const CUBEMAP_OR_VOLUME: u32 = 0x200 | 0x20_0000;
    let format = if pixel_flags & DDPF_FOURCC != 0 {
        match &header[84..88] {
            b"DXT1" => Format::Dxt1,
            b"DXT2" | b"DXT3" => Format::Dxt2_3,
            b"DXT4" | b"DXT5" => Format::Dxt4_5,
            b"ATI2" | b"BC5U" => Format::Dxn,
            b"DX10" => return Err("DDS files with a DX10 header aren't supported".into()),
            _ => return Err("unsupported DDS format (FourCC)".into()),
        }
    } else if pixel_flags & DDPF_RGB != 0 && le32(header, 88) == 32 {
        // Each channel one whole byte, all different (the game rearranges
        // any such order).
        let byte_of = |mask: u32| (0..4u32).find(|i| mask == 0xFF << (8 * i));
        let mut masks = vec![le32(header, 92), le32(header, 96), le32(header, 100)];
        if pixel_flags & DDPF_ALPHAPIXELS != 0 {
            masks.push(le32(header, 104));
        }
        let bytes: Vec<Option<u32>> = masks.iter().map(|&mask| byte_of(mask)).collect();
        let distinct: BTreeSet<u32> = bytes.iter().flatten().copied().collect();
        if distinct.len() != bytes.len() {
            return Err("unsupported 32-bit DDS channel masks".into());
        }
        Format::A8R8G8B8
    } else {
        return Err("unsupported DDS pixel format".into());
    };
    if caps2 & CUBEMAP_OR_VOLUME != 0 {
        return Err("cube maps and volume textures aren't supported".into());
    }
    if !(1..=MAX_IMAGE_SIDE).contains(&width) || !(1..=MAX_IMAGE_SIDE).contains(&height) {
        return Err(format!(
            "the image is {width}x{height}, not between 1x1 and {MAX_IMAGE_SIDE}x{MAX_IMAGE_SIDE}"
        ));
    }
    let full_chain = 32 - width.max(height).leading_zeros();
    let levels = if flags & DDSD_MIPMAPCOUNT != 0 && mip_count != 0 {
        mip_count
    } else {
        1
    };
    if levels > MAX_IMAGE_LEVELS || levels > full_chain {
        return Err(format!(
            "the header declares {levels} mip levels, more than a {width}x{height} image can have"
        ));
    }
    let data_bytes = (0..levels)
        .map(|level| level_bytes(format, width, height, level))
        .sum();
    Ok(DdsInfo {
        format,
        width,
        height,
        levels,
        data_bytes,
    })
}

/// One usable index line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IndexLine {
    pub key: u64,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub file: String,
}

/// Parses an index.tsv like the game: usable lines, and the numbers
/// (1-based) of malformed ones, which the game skips.
pub fn parse_index(text: &str) -> (Vec<IndexLine>, Vec<usize>) {
    let mut lines = Vec::new();
    let mut malformed = Vec::new();
    for (number, line) in text.split('\n').enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let parsed = (|| {
            let mut fields = line.splitn(5, '\t');
            let key = fields.next()?;
            let key = u64::from_str_radix(key, 16)
                .ok()
                .filter(|_| key.len() == 16 && key.bytes().all(|b| b.is_ascii_hexdigit()))?;
            // Digits only (Rust's parse would take a leading '+'; the game doesn't).
            let number = |s: &str| {
                (!s.is_empty() && s.len() <= 9 && s.bytes().all(|b| b.is_ascii_digit()))
                    .then(|| s.parse::<u32>().ok())
                    .flatten()
            };
            let side = |s: &str| number(s).filter(|v| (1..=MAX_TEXTURE_SIDE).contains(v));
            let width = side(fields.next()?)?;
            let height = side(fields.next()?)?;
            let format = number(fields.next()?)?;
            Format::from_xenos(format).block()?;
            let file = fields.next()?;
            is_plain_pack_file(file).then(|| IndexLine {
                key,
                width,
                height,
                format,
                file: file.to_string(),
            })
        })();
        match parsed {
            Some(line) => lines.push(line),
            None => malformed.push(number + 1),
        }
    }
    (lines, malformed)
}

/// Checks one DDS of a pack against its index line. Returns the problem.
pub fn check_dds(pack: &Path, line: &IndexLine) -> std::result::Result<DdsInfo, String> {
    let path = pack.join(&line.file);
    if !is_inside_folder(pack, &path) {
        return Err("missing, or not inside the pack folder".into());
    }
    let meta = std::fs::metadata(&path).map_err(|e| e.to_string())?;
    let mut header = vec![0u8; DDS_HEADER_BYTES as usize];
    {
        use std::io::Read as _;
        let mut file = std::fs::File::open(&path).map_err(|e| e.to_string())?;
        file.read_exact(&mut header)
            .map_err(|_| "too short for a DDS header".to_string())?;
    }
    let info = parse_dds_header(&header)?;
    if info.format.to_xenos() != line.format {
        return Err(format!(
            "the file is format {}, the index says {}",
            info.format.to_xenos(),
            line.format
        ));
    }
    if !is_replacement_size((line.width, line.height), info.width, info.height) {
        return Err(format!(
            "the image is {}x{}, not the original {}x{} scaled (another aspect ratio)",
            info.width, info.height, line.width, line.height
        ));
    }
    if meta.len() != DDS_HEADER_BYTES + info.data_bytes {
        return Err(format!(
            "the file is {} bytes, its header ({}x{}, {} levels) means {}",
            meta.len(),
            info.width,
            info.height,
            info.levels,
            DDS_HEADER_BYTES + info.data_bytes
        ));
    }
    Ok(info)
}

/// What `check_pack` found.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct CheckSummary {
    pub keys: usize,
    pub files: usize,
    pub malformed_lines: usize,
    pub bad_files: usize,
    /// Bytes of the files' image data.
    pub data_bytes: u64,
}

/// `rip cod3-check-pack`: checks a pack folder as the game would read it.
/// Prints each problem (the first few) and a summary.
pub fn check_pack(pack: &Path) -> Result<CheckSummary> {
    let index_path = pack.join("index.tsv");
    let size = std::fs::metadata(&index_path)
        .map_err(|e| format!("{}: {e}", index_path.display()))?
        .len();
    if size > MAX_INDEX_BYTES {
        return Err(format!(
            "{} is {} MB, more than the game reads ({} MB)",
            index_path.display(),
            size >> 20,
            MAX_INDEX_BYTES >> 20
        )
        .into());
    }
    let bytes = std::fs::read(&index_path)?;
    let text = String::from_utf8_lossy(&bytes);
    let (lines, malformed) = parse_index(&text);
    let mut summary = CheckSummary {
        keys: lines.len(),
        malformed_lines: malformed.len(),
        ..Default::default()
    };
    let mut problems = Vec::new();
    for number in &malformed {
        problems.push(format!(
            "index.tsv line {number}: malformed (the game skips it)"
        ));
    }
    let mut checked = BTreeSet::new();
    for line in &lines {
        if !checked.insert(line.file.clone()) {
            continue;
        }
        summary.files += 1;
        match check_dds(pack, line) {
            Ok(info) => summary.data_bytes += info.data_bytes,
            Err(why) => {
                summary.bad_files += 1;
                problems.push(format!("{}: {why}", line.file));
            }
        }
    }
    for problem in problems.iter().take(PROBLEMS_SHOWN) {
        println!("problem: {problem}");
    }
    if problems.len() > PROBLEMS_SHOWN {
        println!("problem: ... and {} more", problems.len() - PROBLEMS_SHOWN);
    }
    println!(
        "checked {}: {} keys, {} files ({} MB of images), {} malformed lines, {} unusable files",
        pack.display(),
        summary.keys,
        summary.files,
        summary.data_bytes >> 20,
        summary.malformed_lines,
        summary.bad_files
    );
    Ok(summary)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_pack_files_only() {
        for good in [
            "a.dds",
            "cloth$burlap_n.dds",
            "sub/a.dds",
            "x__0123456789abcdef.dds",
        ] {
            assert!(is_plain_pack_file(good), "{good}");
        }
        let long = "a".repeat(MAX_PACK_FILE_LENGTH + 1);
        for bad in [
            "",
            "/etc/passwd",
            "../a.dds",
            "a/../../b.dds",
            "./a.dds",
            "a//b.dds",
            "a/",
            "..",
            "C:/a.dds",
            "C:a.dds",
            "a\\b.dds",
            "\\\\server\\share\\a.dds",
            "a.dds:stream",
            "a\nb.dds",
            "a\tb.dds",
            "a\u{7f}.dds",
            long.as_str(),
        ] {
            assert!(!is_plain_pack_file(bad), "{bad:?}");
        }
    }

    #[test]
    fn replacement_sizes_keep_the_aspect_ratio() {
        assert!(is_replacement_size((256, 512), 512, 1024));
        assert!(is_replacement_size((256, 512), 256, 512));
        assert!(is_replacement_size((90, 45), 92, 44)); // rounded to multiples of 4
        assert!(!is_replacement_size((256, 512), 512, 512));
        assert!(!is_replacement_size((256, 512), 0, 0));
        assert!(!is_replacement_size((0, 512), 512, 1024));
    }

    /// A header as the pack writers make it.
    fn header(format: Format, width: u32, height: u32, levels: usize) -> Vec<u8> {
        let top = level_bytes(format, width, height, 0) as usize;
        xenos::dds_mipmapped_header(format, width, height, top, levels).unwrap()
    }

    fn put(header: &mut [u8], at: usize, value: u32) {
        header[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    #[test]
    fn dds_headers_are_checked_strictly() {
        let good = header(Format::Dxt1, 512, 256, 10);
        let info = parse_dds_header(&good).unwrap();
        assert_eq!(
            (info.format, info.width, info.height, info.levels),
            (Format::Dxt1, 512, 256, 10)
        );
        // 512x256 DXT1: 128x64 blocks of 8 bytes, then the smaller levels.
        assert_eq!(
            info.data_bytes,
            (0..10)
                .map(|l| level_bytes(Format::Dxt1, 512, 256, l))
                .sum()
        );
        assert_eq!(level_bytes(Format::Dxt1, 512, 256, 0), 128 * 64 * 8);
        assert_eq!(level_bytes(Format::Dxt1, 512, 256, 9), 8); // 1x1: one block
        let rgba = parse_dds_header(&header(Format::A8R8G8B8, 6, 3, 3)).unwrap();
        assert_eq!(rgba.data_bytes, (6 * 3 + 3 + 1) * 4);

        let broken = |change: &dyn Fn(&mut Vec<u8>)| {
            let mut h = good.clone();
            change(&mut h);
            parse_dds_header(&h)
        };
        assert!(parse_dds_header(&good[..100]).is_err());
        assert!(broken(&|h| h[0] = b'X').is_err());
        assert!(broken(&|h| put(h, 4, 123)).is_err()); // header size
        assert!(broken(&|h| put(h, 76, 0)).is_err()); // pixel format size
        assert!(broken(&|h| h[84..88].copy_from_slice(b"DX10")).is_err());
        assert!(broken(&|h| h[84..88].copy_from_slice(b"BC7U")).is_err());
        assert!(broken(&|h| put(h, 16, 0)).is_err()); // width 0
        assert!(broken(&|h| put(h, 16, MAX_IMAGE_SIDE + 1)).is_err());
        assert!(broken(&|h| put(h, 12, u32::MAX)).is_err());
        assert!(broken(&|h| put(h, 28, 11)).is_err()); // more than a full chain
        assert!(broken(&|h| put(h, 28, 1000)).is_err());
        assert!(broken(&|h| put(h, 112, 0x200)).is_err()); // a cube map
                                                           // The largest image allowed: the sizes can't overflow.
        let mut big = header(Format::A8R8G8B8, 16384, 16384, 15);
        let info = parse_dds_header(&big).unwrap();
        assert!(info.data_bytes > 1 << 30);
        // 32-bit masks must be whole, distinct bytes.
        put(&mut big, 96, 0xFF00_0000); // green = alpha's byte
        assert!(parse_dds_header(&big).is_err());
        put(&mut big, 96, 0x0000_0F00);
        assert!(parse_dds_header(&big).is_err());
    }

    #[test]
    fn index_lines_like_the_game() {
        let text = "# key\twidth\theight\txenos_format\tfile\r\n\
                    0123456789abcdef\t256\t512\t18\ta.dds\r\n\
                    0123456789abcdeg\t256\t512\t18\tb.dds\n\
                    0123456789abcdef\t256\t512\t18\t../b.dds\n\
                    0123456789abcdef\t0\t512\t18\tb.dds\n\
                    0123456789abcdef\t256\t512\t7\tb.dds\n\
                    0123456789abcdef\t256\t512\t18\n\
                    123\t256\t512\t18\tb.dds\n\
                    0123456789abcdef\t+256\t512\t18\tb.dds\n\
                    fedcba9876543210\t8192\t4\t6\tsub/c.dds\n";
        let (lines, malformed) = parse_index(text);
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[1].file, "sub/c.dds");
        assert_eq!(malformed, vec![3, 4, 5, 6, 7, 8, 9]);
    }

    #[test]
    fn packs_are_checked_file_by_file() {
        let root = std::env::temp_dir().join(format!("rip-check-pack-{}", std::process::id()));
        let pack = root.join("pack");
        std::fs::create_dir_all(&pack).unwrap();
        let dds = |w: u32, h: u32| {
            let mut file = header(Format::Dxt1, w, h, 1);
            file.resize(128 + level_bytes(Format::Dxt1, w, h, 0) as usize, 0);
            file
        };
        std::fs::write(pack.join("good.dds"), dds(512, 1024)).unwrap();
        std::fs::write(pack.join("square.dds"), dds(512, 512)).unwrap();
        let mut long = dds(256, 512);
        long.push(0);
        std::fs::write(pack.join("long.dds"), long).unwrap();
        std::fs::write(root.join("outside.dds"), dds(256, 512)).unwrap();
        let index = "0000000000000001\t256\t512\t18\tgood.dds\n\
                     0000000000000002\t256\t512\t18\tsquare.dds\n\
                     0000000000000003\t256\t512\t18\tlong.dds\n\
                     0000000000000004\t256\t512\t20\tgood.dds\n\
                     0000000000000005\t256\t512\t18\tmissing.dds\n\
                     0000000000000006\t256\t512\t18\t../outside.dds\n";
        std::fs::write(pack.join("index.tsv"), index).unwrap();
        #[cfg(unix)]
        {
            // A link that leads out of the pack is refused like a path would be.
            std::os::unix::fs::symlink(root.join("outside.dds"), pack.join("link.dds")).unwrap();
            let mut text = std::fs::read_to_string(pack.join("index.tsv")).unwrap();
            text.push_str("0000000000000007\t256\t512\t18\tlink.dds\n");
            std::fs::write(pack.join("index.tsv"), text).unwrap();
        }
        let summary = check_pack(&pack).unwrap();
        assert_eq!(summary.malformed_lines, 1); // ../outside.dds
                                                // good.dds is checked once (its first line); square, long, missing
                                                // (and the link) are unusable.
        let links = if cfg!(unix) { 1 } else { 0 };
        assert_eq!(summary.files, 4 + links);
        assert_eq!(summary.bad_files, 3 + links);
        std::fs::remove_dir_all(&root).unwrap();
    }
}
