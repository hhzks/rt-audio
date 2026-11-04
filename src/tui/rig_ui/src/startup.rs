use crate::model::EngineError;

/// The backends rt_rig tries at start, in order. `None` is the platform default.
pub fn startup_backends(cli: Option<&str>, available: &[String]) -> Vec<Option<String>> {
    match cli {
        Some(b) => vec![Some(b.to_owned())],
        None if available.iter().any(|b| b == "asio") => vec![Some("asio".to_owned()), None],
        None => vec![None],
    }
}

/// The stderr lines when no backend opened, one error per attempt in order.
pub fn failure_lines(errors: &[EngineError]) -> Vec<String> {
    let Some((last, earlier)) = errors.split_last() else {
        return Vec::new();
    };
    let mut lines: Vec<String> = earlier
        .iter()
        .filter(|e| e.message() != last.message())
        .map(|e| format!("rt-rig: ASIO did not open: {}", e.message()))
        .collect();
    lines.push(format!("rt-rig: {last}"));
    lines
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
    fn one_failure_is_one_line() {
        let e = EngineError::Device("no ASIO driver is installed".into());
        assert_eq!(
            failure_lines(&[e]),
            ["rt-rig: device error: no ASIO driver is installed"]
        );
    }

    #[test]
    fn a_failed_fallback_names_asio_first() {
        let errors = [
            EngineError::Device("no ASIO driver is installed".into()),
            EngineError::Device("no output endpoint".into()),
        ];
        assert_eq!(
            failure_lines(&errors),
            [
                "rt-rig: ASIO did not open: no ASIO driver is installed",
                "rt-rig: device error: no output endpoint",
            ]
        );
    }

    #[test]
    fn the_same_error_twice_is_printed_once() {
        let errors = [
            EngineError::Arg("ring margin out of range".into()),
            EngineError::Arg("ring margin out of range".into()),
        ];
        assert_eq!(
            failure_lines(&errors),
            ["rt-rig: invalid argument: ring margin out of range"]
        );
    }

    #[test]
    fn fallback_notice_text() {
        assert_eq!(
            fallback_notice("no ASIO driver is installed"),
            "ASIO did not open: no ASIO driver is installed; using WASAPI"
        );
    }
}
