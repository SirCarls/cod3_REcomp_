//! The kernel's "CPU list" text form, as in sysfs and `/proc/<pid>/status`:
//! numbers and ranges separated by commas, such as `0-3,8,10-11`.

/// `"0-3,8,10-11"` -> `[0, 1, 2, 3, 8, 10, 11]`. Stops at the first thing it
/// can't read and returns what came before; a range covers at most 4,096
/// CPUs so a damaged file can't ask for a huge list.
pub fn parse_cpu_list(text: &str) -> Vec<usize> {
    const MAX_RANGE: usize = 4096;
    let mut cpus = Vec::new();
    for part in text.trim().split(',') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        let (first, last) = match part.split_once('-') {
            Some((a, b)) => (a.trim().parse::<usize>(), b.trim().parse::<usize>()),
            None => (part.parse::<usize>(), part.parse::<usize>()),
        };
        let (Ok(first), Ok(last)) = (first, last) else {
            break;
        };
        if last < first {
            break;
        }
        cpus.extend(first..=last.min(first + MAX_RANGE - 1));
    }
    cpus
}

/// Back to the compact form: `[0, 1, 2, 3, 8]` -> `"0-3,8"`. Expects a
/// sorted list.
pub fn join_cpu_list(cpus: &[usize]) -> String {
    let mut text = String::new();
    let mut i = 0;
    while i < cpus.len() {
        let mut j = i;
        while j + 1 < cpus.len() && cpus[j + 1] == cpus[j] + 1 {
            j += 1;
        }
        if !text.is_empty() {
            text.push(',');
        }
        text.push_str(&cpus[i].to_string());
        if j > i {
            text.push('-');
            text.push_str(&cpus[j].to_string());
        }
        i = j + 1;
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_ranges_and_singles() {
        assert_eq!(parse_cpu_list("0-3,8,10-11\n"), vec![0, 1, 2, 3, 8, 10, 11]);
        assert_eq!(parse_cpu_list("5"), vec![5]);
        assert_eq!(parse_cpu_list(""), Vec::<usize>::new());
        assert_eq!(parse_cpu_list(" 0 - 1 , 4 "), vec![0, 1, 4]);
    }

    #[test]
    fn stops_at_bad_input() {
        assert_eq!(parse_cpu_list("0-1,x,4"), vec![0, 1]);
        assert_eq!(parse_cpu_list("3-1"), Vec::<usize>::new());
        assert_eq!(parse_cpu_list("0-99999999").len(), 4096);
    }

    #[test]
    fn joins_back() {
        assert_eq!(join_cpu_list(&[0, 1, 2, 3, 8, 10, 11]), "0-3,8,10-11");
        assert_eq!(join_cpu_list(&[]), "");
        assert_eq!(join_cpu_list(&[7]), "7");
        for text in ["0-3,8,10-11", "1,3,5", "0-127"] {
            assert_eq!(join_cpu_list(&parse_cpu_list(text)), text);
        }
    }
}
