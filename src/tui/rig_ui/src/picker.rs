use crate::model::{Config, DeviceEntry};

pub const BLOCKS: [i32; 8] = [0, 32, 64, 128, 256, 512, 1024, 2048];
pub const RATES: [f64; 4] = [44100.0, 48000.0, 88200.0, 96000.0];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Field {
    Input,
    Output,
    Mode,
    Block,
    Rate,
}

impl Field {
    pub fn name(self) -> &'static str {
        match self {
            Field::Input => "input",
            Field::Output => "output",
            Field::Mode => "mode",
            Field::Block => "block",
            Field::Rate => "rate",
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum PickerStatus {
    Idle,
    Pending,
    Applied,
    RolledBack(String),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Choice {
    pub id: String,
    pub label: String,
}

pub struct Picker {
    pub devices: Vec<DeviceEntry>,
    pub list_error: Option<String>,
    pub edited: Config,
    pub cursor: usize,
    pub status: PickerStatus,
}

pub fn khz(rate: f64) -> String {
    let k = rate / 1000.0;
    if k.fract() == 0.0 {
        format!("{k:.0} kHz")
    } else {
        format!("{k:.1} kHz")
    }
}

fn step_index(i: usize, len: usize, dir: i8) -> usize {
    if dir < 0 {
        i.saturating_sub(1)
    } else {
        (i + 1).min(len.saturating_sub(1))
    }
}

fn step_list<T: Copy + PartialOrd>(list: &[T], current: T, dir: i8) -> T {
    if dir > 0 {
        list.iter()
            .copied()
            .find(|&v| v > current)
            .unwrap_or(current)
    } else {
        list.iter()
            .rev()
            .copied()
            .find(|&v| v < current)
            .unwrap_or(current)
    }
}

impl Picker {
    pub fn new(config: Config, devices: Result<Vec<DeviceEntry>, String>) -> Self {
        let mut p = Picker {
            devices: Vec::new(),
            list_error: None,
            edited: config,
            cursor: 0,
            status: PickerStatus::Idle,
        };
        p.set_devices(devices);
        p
    }

    pub fn set_devices(&mut self, devices: Result<Vec<DeviceEntry>, String>) {
        match devices {
            Ok(d) => {
                self.devices = d;
                self.list_error = None;
            }
            Err(e) => {
                self.devices.clear();
                self.list_error = Some(e);
            }
        }
    }

    pub fn is_wasapi(&self) -> bool {
        self.edited.backend == "wasapi"
    }

    pub fn visible_fields(&self) -> Vec<Field> {
        let mut f = vec![Field::Input, Field::Output];
        if self.is_wasapi() {
            f.push(Field::Mode);
        }
        f.push(Field::Block);
        f.push(Field::Rate);
        f
    }

    pub fn read_only(&self, field: Field) -> bool {
        match field {
            Field::Rate => self.is_wasapi(),
            Field::Block => self.is_wasapi() && self.edited.exclusive,
            _ => false,
        }
    }

    pub fn selected(&self) -> Field {
        let f = self.visible_fields();
        f[self.cursor.min(f.len() - 1)]
    }

    pub fn select(&mut self, field: Field) {
        if let Some(i) = self.visible_fields().iter().position(|&f| f == field) {
            self.cursor = i;
        }
    }

    pub fn move_cursor(&mut self, dir: i8) {
        let fields = self.visible_fields();
        let mut i = self.cursor.min(fields.len() - 1);
        loop {
            let j = step_index(i, fields.len(), dir);
            if j == i {
                return;
            }
            i = j;
            if !self.read_only(fields[i]) {
                self.cursor = i;
                return;
            }
        }
    }

    pub fn choices(&self, field: Field) -> Vec<Choice> {
        let output = match field {
            Field::Input => false,
            Field::Output => true,
            _ => return Vec::new(),
        };
        let current = if output {
            &self.edited.output
        } else {
            &self.edited.input
        };
        let default_name = self
            .devices
            .iter()
            .find(|d| if output { d.default_out } else { d.default_in })
            .map(|d| d.name.as_str());
        let mut list = vec![Choice {
            id: String::new(),
            label: match default_name {
                Some(n) => format!("System default ({n})"),
                None => "System default".into(),
            },
        }];
        let listed: Vec<&DeviceEntry> = self
            .devices
            .iter()
            .filter(|d| if output { d.outputs > 0 } else { d.inputs > 0 })
            .collect();
        if !current.is_empty() && !listed.iter().any(|d| &d.id == current) {
            list.push(Choice {
                id: current.clone(),
                label: format!("(missing) {current}"),
            });
        }
        list.extend(listed.into_iter().map(|d| Choice {
            id: d.id.clone(),
            label: d.name.clone(),
        }));
        list
    }

    pub fn step(&mut self, dir: i8) -> bool {
        let field = self.selected();
        if self.read_only(field) {
            return false;
        }
        match field {
            Field::Input | Field::Output => {
                let output = field == Field::Output;
                let list = self.choices(field);
                let current = if output {
                    &self.edited.output
                } else {
                    &self.edited.input
                };
                let i = list.iter().position(|c| &c.id == current).unwrap_or(0);
                let j = step_index(i, list.len(), dir);
                if j == i {
                    return false;
                }
                let id = list[j].id.clone();
                if output {
                    self.edited.output = id;
                } else {
                    self.edited.input = id;
                }
                true
            }
            Field::Mode => {
                let want = dir > 0;
                if self.edited.exclusive == want {
                    return false;
                }
                self.edited.exclusive = want;
                true
            }
            Field::Block => {
                let next = step_list(&BLOCKS, self.edited.block, dir);
                let changed = next != self.edited.block;
                self.edited.block = next;
                changed
            }
            Field::Rate => {
                let next = step_list(&RATES, self.edited.rate, dir);
                let changed = next != self.edited.rate;
                self.edited.rate = next;
                changed
            }
        }
    }

    pub fn label(&self, field: Field, device_rate: f64, sep: &str) -> String {
        match field {
            Field::Input | Field::Output => {
                let current = if field == Field::Output {
                    &self.edited.output
                } else {
                    &self.edited.input
                };
                self.choices(field)
                    .into_iter()
                    .find(|c| &c.id == current)
                    .map_or_else(String::new, |c| c.label)
            }
            Field::Mode => {
                let mode = if self.edited.exclusive {
                    "exclusive"
                } else {
                    "shared"
                };
                mode.to_owned()
            }
            Field::Block if self.read_only(Field::Block) => "device minimum".into(),
            Field::Block if self.edited.block == 0 => "driver minimum".into(),
            Field::Block => {
                let rate = if self.is_wasapi() {
                    device_rate
                } else {
                    self.edited.rate
                };
                let ms = if rate > 0.0 {
                    1000.0 * f64::from(self.edited.block) / rate
                } else {
                    0.0
                };
                format!("{} fr{sep}{ms:.2} ms", self.edited.block)
            }
            Field::Rate => khz(if self.is_wasapi() {
                device_rate
            } else {
                self.edited.rate
            }),
        }
    }

    pub fn note(&self, field: Field) -> &'static str {
        match field {
            Field::Rate if self.is_wasapi() => "set by the device mix format",
            Field::Block if self.is_wasapi() && !self.read_only(Field::Block) => {
                "the driver can round it"
            }
            _ => "",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dev(id: &str, name: &str, inputs: i32, outputs: i32, default_in: bool) -> DeviceEntry {
        DeviceEntry {
            id: id.into(),
            name: name.into(),
            inputs,
            outputs,
            default_rate: 48000.0,
            default_in,
            default_out: false,
        }
    }

    fn devices() -> Vec<DeviceEntry> {
        vec![
            dev("mic", "Mic", 2, 0, true),
            dev("phones", "Phones", 0, 2, false),
            dev("usb", "USB", 2, 2, false),
        ]
    }

    fn config(backend: &str) -> Config {
        Config {
            backend: backend.into(),
            input: String::new(),
            output: String::new(),
            rate: 48000.0,
            block: 128,
            exclusive: false,
        }
    }

    fn ids(list: &[Choice]) -> Vec<&str> {
        list.iter().map(|c| c.id.as_str()).collect()
    }

    #[test]
    fn device_lists_filter_by_direction_with_default_first() {
        let p = Picker::new(config("wasapi"), Ok(devices()));
        assert_eq!(ids(&p.choices(Field::Input)), ["", "mic", "usb"]);
        assert_eq!(ids(&p.choices(Field::Output)), ["", "phones", "usb"]);
        assert_eq!(p.choices(Field::Input)[0].label, "System default (Mic)");
        assert_eq!(p.choices(Field::Output)[0].label, "System default");
        assert_eq!(
            p.label(Field::Input, 48000.0, " · "),
            "System default (Mic)"
        );
        assert!(p.choices(Field::Block).is_empty());
    }

    #[test]
    fn a_missing_id_is_listed_until_stepped_away() {
        let mut c = config("wasapi");
        c.input = "gone".into();
        let mut p = Picker::new(c, Ok(devices()));
        assert_eq!(ids(&p.choices(Field::Input)), ["", "gone", "mic", "usb"]);
        assert_eq!(p.label(Field::Input, 48000.0, " · "), "(missing) gone");
        assert!(p.step(1));
        assert_eq!(p.edited.input, "mic");
        assert_eq!(ids(&p.choices(Field::Input)), ["", "mic", "usb"]);
    }

    #[test]
    fn device_steps_stop_at_the_ends() {
        let mut p = Picker::new(config("null"), Ok(devices()));
        assert!(!p.step(-1));
        assert!(p.step(1));
        assert!(p.step(1));
        assert_eq!(p.edited.input, "usb");
        assert!(!p.step(1));
    }

    #[test]
    fn wasapi_rules() {
        let mut p = Picker::new(config("wasapi"), Ok(devices()));
        assert_eq!(
            p.visible_fields(),
            [
                Field::Input,
                Field::Output,
                Field::Mode,
                Field::Block,
                Field::Rate
            ]
        );
        assert!(p.read_only(Field::Rate));
        assert!(!p.read_only(Field::Block));
        assert_eq!(p.note(Field::Rate), "set by the device mix format");
        assert_eq!(p.note(Field::Block), "the driver can round it");
        assert_eq!(p.label(Field::Rate, 44100.0, " · "), "44.1 kHz");

        p.select(Field::Mode);
        assert!(p.step(1));
        assert!(p.edited.exclusive);
        assert!(!p.step(1));
        assert!(p.read_only(Field::Block));
        assert_eq!(p.label(Field::Block, 48000.0, " · "), "device minimum");
        assert_eq!(p.note(Field::Block), "");

        p.move_cursor(1);
        assert_eq!(p.selected(), Field::Mode, "block and rate are read-only");
        p.move_cursor(-1);
        assert_eq!(p.selected(), Field::Output);
    }

    #[test]
    fn alsa_hides_mode_and_steps_rate() {
        let mut p = Picker::new(config("alsa"), Ok(devices()));
        assert_eq!(
            p.visible_fields(),
            [Field::Input, Field::Output, Field::Block, Field::Rate]
        );
        assert!(!p.read_only(Field::Rate));
        assert_eq!(p.note(Field::Rate), "");
        p.select(Field::Rate);
        assert!(p.step(-1));
        assert_eq!(p.edited.rate, 44100.0);
        assert!(!p.step(-1));
        assert_eq!(p.label(Field::Rate, 0.0, " · "), "44.1 kHz");
    }

    #[test]
    fn block_steps_snap_off_list_values_and_stop_at_the_ends() {
        let mut p = Picker::new(config("null"), Ok(devices()));
        p.select(Field::Block);
        assert_eq!(p.label(Field::Block, 48000.0, " · "), "128 fr · 2.67 ms");
        p.edited.block = 100;
        assert!(p.step(1));
        assert_eq!(p.edited.block, 128);
        p.edited.block = 100;
        assert!(p.step(-1));
        assert_eq!(p.edited.block, 64);
        p.edited.block = 2048;
        assert!(!p.step(1));
        p.edited.block = 0;
        assert!(!p.step(-1));
        assert_eq!(p.label(Field::Block, 48000.0, " · "), "driver minimum");
        assert!(p.step(1));
        assert_eq!(p.edited.block, 32);
    }

    #[test]
    fn a_list_error_leaves_only_the_default() {
        let mut p = Picker::new(config("null"), Err("boom".into()));
        assert_eq!(p.list_error.as_deref(), Some("boom"));
        assert_eq!(ids(&p.choices(Field::Input)), [""]);
        p.set_devices(Ok(devices()));
        assert!(p.list_error.is_none());
        assert_eq!(p.choices(Field::Input).len(), 3);
    }

    #[test]
    fn khz_formats_whole_and_fractional_rates() {
        assert_eq!(khz(48000.0), "48 kHz");
        assert_eq!(khz(44100.0), "44.1 kHz");
    }
}
