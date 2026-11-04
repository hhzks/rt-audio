/// The backends rt_rig tries at start, in order. `None` is the platform default.
pub fn startup_backends(cli: Option<&str>, available: &[String]) -> Vec<Option<String>> {
    match cli {
        Some(b) => vec![Some(b.to_owned())],
        None if available.iter().any(|b| b == "asio") => vec![Some("asio".to_owned()), None],
        None => vec![None],
    }
}

pub fn fallback_notice(reason: &str) -> String {
    format!("ASIO did not open: {reason}; using WASAPI")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn list(keys: &[&str]) -> Vec<String> {
        keys.iter().map(|k| (*k).to_owned()).collect()
    }

    #[test]
    fn explicit_backend_has_no_fallback() {
        let all = list(&["wasapi", "asio", "null"]);
        assert_eq!(
            startup_backends(Some("asio"), &all),
            [Some("asio".to_owned())]
        );
        assert_eq!(
            startup_backends(Some("null"), &all),
            [Some("null".to_owned())]
        );
    }

    #[test]
    fn asio_build_tries_asio_then_default() {
        let all = list(&["wasapi", "asio", "null"]);
        assert_eq!(
            startup_backends(None, &all),
            [Some("asio".to_owned()), None]
        );
    }

    #[test]
    fn build_without_asio_uses_default() {
        assert_eq!(startup_backends(None, &list(&["wasapi", "null"])), [None]);
        assert_eq!(startup_backends(None, &list(&["alsa", "null"])), [None]);
    }

    #[test]
    fn fallback_notice_text() {
        assert_eq!(
            fallback_notice("no ASIO driver is installed"),
            "ASIO did not open: no ASIO driver is installed; using WASAPI"
        );
    }
}
