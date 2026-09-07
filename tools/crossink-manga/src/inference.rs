//! Runtime-independent native graph execution boundary.
//!
//! Model algorithms own names, shapes, preprocessing and decoding. Adapters own
//! sessions and output storage. This is deliberately a synchronous CPU contract;
//! an asynchronous browser adapter must add awaitable execution/materialization.
use anyhow::Result;
use ndarray::{ArrayD, ArrayViewD, IxDyn};
use std::path::Path;

pub mod ort;

/// Borrowed model input. No runtime-specific tensor escapes the adapter.
pub enum Input<'a> {
    F32(ArrayViewD<'a, f32>),
    I64(ArrayViewD<'a, i64>),
}

pub type Inputs<'a> = Vec<(&'a str, Input<'a>)>;

/// Host-owned input buffer, validated against its shape before execution.
pub struct Tensor<T>(ArrayD<T>);

impl<T> Tensor<T> {
    pub fn from_array((shape, values): (impl AsRef<[usize]>, Vec<T>)) -> Result<Self> {
        Ok(Self(ArrayD::from_shape_vec(IxDyn(shape.as_ref()), values)?))
    }
}

impl Tensor<f32> {
    pub fn input(&self) -> Input<'_> {
        Input::F32(self.0.view())
    }
}

impl Tensor<i64> {
    pub fn input(&self) -> Input<'_> {
        Input::I64(self.0.view())
    }
}

/// Views borrow adapter-owned output storage, avoiding mandatory tensor copies.
/// Indexed outputs must preserve the ONNX graph's declared output order.
pub trait Outputs {
    fn len(&self) -> usize;
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
    fn array(&self, index: usize) -> Result<ArrayViewD<'_, f32>>;
    fn named_array(&self, name: &str) -> Result<ArrayViewD<'_, f32>>;
}

/// Reusable graph session. Output ownership remains inside the adapter.
pub trait Session {
    fn run(&mut self, inputs: Inputs<'_>) -> Result<Box<dyn Outputs + '_>>;
}

/// Native model-session factory; a future runtime implements this and Session.
pub trait Runtime {
    /// Include runtime version, binary identity and execution settings in OCR caches.
    fn cache_identity(&self) -> Result<String>;
    fn load(&self, path: &Path) -> Result<Box<dyn Session>>;
}
