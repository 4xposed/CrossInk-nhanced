#![allow(clippy::unwrap_used)] // Assertions in the isolated test fixture.

use anyhow::{Result, bail, ensure};
use crossink_manga::{
    inference::{Inputs, Outputs, Runtime, Session},
    native_detector::Detector,
};
use ndarray::{ArrayD, ArrayViewD, IxDyn};
use std::path::Path;

struct PublicRuntime;
struct PublicSession;
struct PublicOutputs([ArrayD<f32>; 3]);
impl Runtime for PublicRuntime {
    fn cache_identity(&self) -> Result<String> {
        Ok("public-detector-test".into())
    }
    fn load(&self, _: &Path) -> Result<Box<dyn Session>> {
        Ok(Box::new(PublicSession))
    }
}
impl Session for PublicSession {
    fn run(&mut self, inputs: Inputs<'_>) -> Result<Box<dyn Outputs + '_>> {
        ensure!(
            inputs.len() == 1 && inputs[0].0 == "images",
            "wrong detector input"
        );
        Ok(Box::new(PublicOutputs([
            ArrayD::zeros(IxDyn(&[1, 0, 7])),
            ArrayD::zeros(IxDyn(&[1, 1, 1024, 1024])),
            ArrayD::zeros(IxDyn(&[1, 2, 1024, 1024])),
        ])))
    }
}
impl Outputs for PublicOutputs {
    fn len(&self) -> usize {
        3
    }
    fn array(&self, index: usize) -> Result<ArrayViewD<'_, f32>> {
        Ok(self.0[index].view())
    }
    fn named_array(&self, name: &str) -> Result<ArrayViewD<'_, f32>> {
        match name {
            "blk" => self.array(0),
            "seg" => self.array(1),
            "det" => self.array(2),
            _ => bail!("unknown output {name}"),
        }
    }
}

#[test]
fn public_detector_configuration_maps_upstream_outputs() {
    let directory = tempfile::tempdir().unwrap();
    std::fs::write(directory.path().join("comictextdetector.onnx"), []).unwrap();
    let config = directory.path().join("detector.json");
    std::fs::write(
        &config,
        r#"{"format_version":1,"graph_interface":"comic-text-detector-upstream-v1"}"#,
    )
    .unwrap();
    let mut detector = Detector::load_with_runtime(directory.path(), &PublicRuntime).unwrap();
    assert!(
        detector
            .detect(&image::RgbImage::new(2, 2))
            .unwrap()
            .is_empty()
    );
    std::fs::write(
        &config,
        r#"{"format_version":1,"graph_interface":"unknown"}"#,
    )
    .unwrap();
    assert!(Detector::load_with_runtime(directory.path(), &PublicRuntime).is_err());
}
