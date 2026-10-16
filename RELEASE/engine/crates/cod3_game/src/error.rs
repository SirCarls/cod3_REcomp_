//! The game's fatal errors.
//!
//! The original calls `Com_Error` (0x825239A0), which never returns: it
//! jumps back to the frame loop (`ERR_DROP` ends the level) or stops the
//! game (`ERR_FATAL`). A port returns `Err(GameError)` at the same point and
//! with the same message, so the caller decides what to do.

use std::fmt;

/// `Com_Error`'s first argument.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorLevel {
    /// 0: stops the game.
    Fatal,
    /// 1: drops back to the menu.
    Drop,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameError {
    pub level: ErrorLevel,
    /// The message without the game's leading 0x15 (its "localized text" marker).
    pub message: String,
}

impl GameError {
    pub fn drop(message: impl Into<String>) -> Self {
        GameError {
            level: ErrorLevel::Drop,
            message: message.into(),
        }
    }

    pub fn fatal(message: impl Into<String>) -> Self {
        GameError {
            level: ErrorLevel::Fatal,
            message: message.into(),
        }
    }

    /// The name of the original's error exit, for the checker.
    pub fn exit_name(&self) -> &'static str {
        "Com_Error"
    }
}

impl fmt::Display for GameError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.level, self.message)
    }
}

impl std::error::Error for GameError {}
