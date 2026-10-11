use std::time::Instant;

/// Opt-in wall-clock stage timings; no per-triangle clocks or logging.
pub(super) struct Timings {
    stage: &'static str,
    last: Option<Instant>,
}

impl Timings {
    pub(super) fn new(stage: &'static str) -> Self {
        Self {
            stage,
            last: std::env::var_os("ANYCUBIC_SLICER_TIMINGS").map(|_| Instant::now()),
        }
    }

    pub(super) fn lap(&mut self, operation: &str) {
        if let Some(last) = &mut self.last {
            let now = Instant::now();
            eprintln!(
                "Slicer timing: {} / {}: {:.3}s",
                self.stage,
                operation,
                now.duration_since(*last).as_secs_f64()
            );
            *last = now;
        }
    }
}
