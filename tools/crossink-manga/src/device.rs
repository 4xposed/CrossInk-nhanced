use clap::ValueEnum;
use serde::{Deserialize, Serialize};

/// Portrait export bounds from the SDK BoardConfig profiles.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, ValueEnum, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Device {
    X3,
    #[default]
    X4,
    X4Pro,
}

impl Device {
    pub const fn dimensions(self) -> (u32, u32) {
        match self {
            Self::X3 => (528, 792),
            Self::X4 | Self::X4Pro => (480, 800),
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::X3 => "x3",
            Self::X4 => "x4",
            Self::X4Pro => "x4-pro",
        }
    }
}
