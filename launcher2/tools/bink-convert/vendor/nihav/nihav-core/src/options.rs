//! Options support.
//!
//! This module contains the definitions for options and names for common options.
//! Options are used to set custom parameters in e.g. decoders or muxers.
//!
//! As a rule target for options should provide a list of supported options and ignore unknown options.

use std::sync::Arc;
use std::fmt;

pub use crate::compr::deflate::{DEFLATE_MODE_DESCRIPTION, DEFLATE_OPTION_VALUES, DEFLATE_MODE_NONE, DEFLATE_MODE_FAST, DEFLATE_MODE_BETTER, DEFLATE_MODE_BEST};

/// Common name for keyframe interval option.
pub const KEYFRAME_OPTION: &str = "key_int";
/// Common description for keyframe interval option.
pub const KEYFRAME_OPTION_DESC: &str = "Keyframe interval (0 - automatic)";

/// Common name for frame skipping mode.
pub const FRAME_SKIP_OPTION: &str = "frame_skip";
/// Common description for frame skipping mode.
pub const FRAME_SKIP_OPTION_DESC: &str = "Frame skipping mode";
/// Frame skipping option value for no skipped frames.
pub const FRAME_SKIP_OPTION_VAL_NONE: &str = "none";
/// Frame skipping option value for decoding only keyframes.
pub const FRAME_SKIP_OPTION_VAL_KEYFRAME: &str = "keyframes";
/// Frame skipping option value for decoding only intra frames.
pub const FRAME_SKIP_OPTION_VAL_INTRA: &str = "intra";

/// Common name for forcing seek outside defined seekpoints.
pub const FORCE_SEEK_OPTION: &str = "force_seek";
/// Common description for forcing seek outside defined seekpoints.
pub const FORCE_SEEK_OPTION_DESC: &str = "Allow seeking to arbitrary positions";

/// Common name for forcing stream tag.
pub const FORCE_STREAM_TAG_OPTION: &str = "stream_tag";
/// Common description for forcing stream tag.
pub const FORCE_STREAM_TAG_OPTION_DESC: &str = "Forced stream tag(s) in 'streamNtagHHHH' format";

/// A list specifying option parsing and validating errors.
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum OptionError {
    /// Input is not intended for the current option definition.
    WrongName,
    /// Option value is not in the expected format.
    InvalidFormat,
    /// Option value was not in the range.
    InvalidValue,
    /// Parse error.
    ParseError,
}

/// A specialised `Result` type for option parsing/validation.
pub type OptionResult<T> = Result<T, OptionError>;

/// Option definition type.
#[derive(Debug)]
pub enum NAOptionDefinitionType {
    /// Option may just be present.
    None,
    /// Option is a boolean value.
    Bool,
    /// Option is an integer with optional minimum and maximum value.
    Int(Option<i64>, Option<i64>),
    /// Option is a floating point number with optional minimum and maximum value.
    Float(Option<f64>, Option<f64>),
    /// Option is a string with an optional list of allowed values.
    String(Option<&'static [&'static str]>),
    /// Option is some binary data.
    Data,
}

/// Option definition.
#[derive(Debug)]
pub struct NAOptionDefinition {
    /// Option name.
    pub name:           &'static str,
    /// Option meaning.
    pub description:    &'static str,
    /// Option type.
    pub opt_type:       NAOptionDefinitionType,
}

impl NAOptionDefinition {
    /// Tries to parse input string(s) as an option and returns new option and number of arguments used (1 or 2) on success.
    pub fn parse(&self, name: &str, value: Option<&String>) -> OptionResult<(NAOption, usize)> {
        let no_name = "no".to_owned() + self.name;
        let opt_no_name = "--no".to_owned() + self.name;
        if name == no_name || name == opt_no_name {
            match self.opt_type {
                NAOptionDefinitionType::Bool => return Ok((NAOption { name: self.name, value: NAValue::Bool(false) }, 1)),
                _ => return Err(OptionError::InvalidFormat),
            };
        }
        let opt_name = "--".to_owned() + self.name;
        if self.name != name && opt_name != name {
            return Err(OptionError::WrongName);
        }
        match self.opt_type {
            NAOptionDefinitionType::None => Ok((NAOption { name: self.name, value: NAValue::None }, 1)),
            NAOptionDefinitionType::Bool => Ok((NAOption { name: self.name, value: NAValue::Bool(true) }, 1)),
            NAOptionDefinitionType::Int(_, _) => {
                if let Some(strval) = value {
                    let ret = strval.parse::<i64>();
                    if let Ok(val) = ret {
                        let opt = NAOption { name: self.name, value: NAValue::Int(val) };
                        self.check(&opt)?;
                        Ok((opt, 2))
                    } else {
                        Err(OptionError::ParseError)
                    }
                } else {
                    Err(OptionError::ParseError)
                }
            },
            NAOptionDefinitionType::Float(_, _) => {
                if let Some(strval) = value {
                    let ret = strval.parse::<f64>();
                    if let Ok(val) = ret {
                        let opt = NAOption { name: self.name, value: NAValue::Float(val) };
                        self.check(&opt)?;
                        Ok((opt, 2))
                    } else {
                        Err(OptionError::ParseError)
                    }
                } else {
                    Err(OptionError::ParseError)
                }
            },
            NAOptionDefinitionType::String(_) => {
                if let Some(strval) = value {
                    let opt = NAOption { name: self.name, value: NAValue::String(strval.to_string()) };
                    self.check(&opt)?;
                    Ok((opt, 2))
                } else {
                    Err(OptionError::ParseError)
                }
            },
            _ => unimplemented!(),
        }
    }
    /// Checks whether input option conforms to the definition i.e. whether it has proper format and it lies in range.
    pub fn check(&self, option: &NAOption) -> OptionResult<()> {
        if option.name != self.name {
            return Err(OptionError::WrongName);
        }
        match option.value {
            NAValue::None => Ok(()),
            NAValue::Bool(_) => Ok(()),
            NAValue::Int(intval) => {
                match self.opt_type {
                    NAOptionDefinitionType::Int(minval, maxval) => {
                        if let Some(minval) = minval {
                            if intval < minval { return Err(OptionError::InvalidValue); }
                        }
                        if let Some(maxval) = maxval {
                            if intval > maxval { return Err(OptionError::InvalidValue); }
                        }
                    },
                    NAOptionDefinitionType::Float(minval, maxval) => {
                        let fval = intval as f64;
                        if let Some(minval) = minval {
                            if fval < minval { return Err(OptionError::InvalidValue); }
                        }
                        if let Some(maxval) = maxval {
                            if fval > maxval { return Err(OptionError::InvalidValue); }
                        }
                    },
                    _ => return Err(OptionError::InvalidFormat),
                };
                Ok(())
            },
            NAValue::Float(fval) => {
                match self.opt_type {
                    NAOptionDefinitionType::Int(minval, maxval) => {
                        let intval = fval as i64;
                        if let Some(minval) = minval {
                            if intval < minval { return Err(OptionError::InvalidValue); }
                        }
                        if let Some(maxval) = maxval {
                            if intval > maxval { return Err(OptionError::InvalidValue); }
                        }
                    },
                    NAOptionDefinitionType::Float(minval, maxval) => {
                        if let Some(minval) = minval {
                            if fval < minval { return Err(OptionError::InvalidValue); }
                        }
                        if let Some(maxval) = maxval {
                            if fval > maxval { return Err(OptionError::InvalidValue); }
                        }
                    },
                    _ => return Err(OptionError::InvalidFormat),
                };
                Ok(())
            },
            NAValue::String(ref cur_str) => {
                if let NAOptionDefinitionType::String(Some(strings)) = self.opt_type {
                    for string in strings.iter() {
                        if cur_str == string {
                            return Ok(());
                        }
                    }
                    Err(OptionError::InvalidValue)
                } else {
                    Ok(())
                }
            },
            NAValue::Data(_) => Ok(()),
        }
    }
}

impl fmt::Display for NAOptionDefinition {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self.opt_type {
            NAOptionDefinitionType::None => write!(f, "{}: {}", self.name, self.description),
            NAOptionDefinitionType::Bool => write!(f, "[no]{}: {}", self.name, self.description),
            NAOptionDefinitionType::String(ref str_list) => {
                if let Some(opts) = str_list {
                    write!(f, "{} {}: {}", self.name, opts.join("|"), self.description)
                } else {
                    write!(f, "{} <string>: {}", self.name, self.description)
                }
            },
            NAOptionDefinitionType::Int(minval, maxval) => {
                let range = match (&minval, &maxval) {
                        (Some(minval), None) => format!("{}-..", minval),
                        (None, Some(maxval)) => format!("..-{}", maxval),
                        (Some(minval), Some(maxval)) => format!("{}-{}", minval, maxval),
                        _ => "<integer>".to_string(),
                    };
                write!(f, "{} {}: {}", self.name, range, self.description)
            },
            NAOptionDefinitionType::Float(minval, maxval) => {
                let range = match (&minval, &maxval) {
                        (Some(minval), None) => format!("{}-..", minval),
                        (None, Some(maxval)) => format!("..-{}", maxval),
                        (Some(minval), Some(maxval)) => format!("{}-{}", minval, maxval),
                        _ => "<float>".to_string(),
                    };
                write!(f, "{} {}: {}", self.name, range, self.description)
            },
            NAOptionDefinitionType::Data => write!(f, "{} <binary data>: {}", self.name, self.description),
        }
    }
}

/// Option.
#[derive(Clone,Debug,PartialEq)]
pub struct NAOption {
    /// Option name.
    pub name:   &'static str,
    /// Option value.
    pub value:  NAValue,
}

/// A list of accepted option values.
#[derive(Debug,Clone,PartialEq)]
pub enum NAValue {
    /// Empty value.
    None,
    /// Boolean value.
    Bool(bool),
    /// Integer value.
    Int(i64),
    /// Floating point value.
    Float(f64),
    /// String value.
    String(String),
    /// Binary data value.
    Data(Arc<Vec<u8>>),
}

/// Trait for all objects that handle `NAOption`.
pub trait NAOptionHandler {
    /// Returns the options recognised by current object.
    fn get_supported_options(&self) -> &[NAOptionDefinition];
    /// Passes options for the object to set (or ignore).
    fn set_options(&mut self, options: &[NAOption]);
    /// Queries the current option value in the object (if present).
    fn query_option_value(&self, name: &str) -> Option<NAValue>;
}

#[cfg(test)]
mod test {
    use super::*;
    #[test]
    fn test_option_validation() {
        let option = NAOption {name: "option", value: NAValue::Int(42) };
        let mut def = NAOptionDefinition { name: "option", description: "", opt_type: NAOptionDefinitionType::Int(None, None) };
        assert!(def.check(&option).is_ok());
        def.opt_type = NAOptionDefinitionType::Int(None, Some(30));
        assert_eq!(def.check(&option), Err(OptionError::InvalidValue));
        def.opt_type = NAOptionDefinitionType::Int(Some(40), None);
        assert!(def.check(&option).is_ok());
        def.name = "option2";
        assert_eq!(def.check(&option), Err(OptionError::WrongName));
        let option = NAOption {name: "option", value: NAValue::String("test".to_string()) };
        let mut def = NAOptionDefinition { name: "option", description: "", opt_type: NAOptionDefinitionType::String(None) };
        assert!(def.check(&option).is_ok());
        def.opt_type = NAOptionDefinitionType::String(Some(&["a string", "test string"]));
        assert_eq!(def.check(&option), Err(OptionError::InvalidValue));
        def.opt_type = NAOptionDefinitionType::String(Some(&["a string", "test"]));
        assert!(def.check(&option).is_ok());
    }
    #[test]
    fn test_option_parsing() {
        let mut def = NAOptionDefinition { name: "option", description: "", opt_type: NAOptionDefinitionType::Float(None, None) };
        assert_eq!(def.parse("--option", None), Err(OptionError::ParseError));
        assert_eq!(def.parse("--nooption", None), Err(OptionError::InvalidFormat));
        assert_eq!(def.parse("--option", Some(&"42".to_string())),
                   Ok((NAOption{name:"option",value: NAValue::Float(42.0)}, 2)));
        def.opt_type = NAOptionDefinitionType::Float(None, Some(40.0));
        assert_eq!(def.parse("--option", Some(&"42".to_string())),
                   Err(OptionError::InvalidValue));
        let def = NAOptionDefinition { name: "option", description: "", opt_type: NAOptionDefinitionType::Bool };
        assert_eq!(def.parse("option", None),
                   Ok((NAOption{name: "option", value: NAValue::Bool(true) }, 1)));
        assert_eq!(def.parse("nooption", None),
                   Ok((NAOption{name: "option", value: NAValue::Bool(false) }, 1)));
    }
}
