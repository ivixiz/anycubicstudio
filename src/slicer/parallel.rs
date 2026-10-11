//! A reusable, bounded pool shared by all slicer calls, including the C ABI.
//! Avoids a fresh CPU-sized pool for each model/job in an embedding GUI.
use std::sync::OnceLock;

pub(super) fn install<T: Send>(work: impl FnOnce() -> T + Send) -> crate::Result<T> {
    static POOL: OnceLock<Result<rayon::ThreadPool, String>> = OnceLock::new();
    let pool = POOL.get_or_init(|| {
        let available = std::thread::available_parallelism().map_or(1, usize::from);
        let default = available.saturating_sub(1).clamp(1, 8);
        let threads = std::env::var("ANYCUBIC_SLICER_THREADS")
            .ok()
            .and_then(|value| value.parse::<usize>().ok())
            .filter(|count| *count > 0)
            .unwrap_or(default)
            .clamp(1, available.min(32));
        rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .thread_name(|index| format!("anycubic-slicer-{index}"))
            .build()
            .map_err(|error| format!("cannot create slicer worker pool: {error}"))
    });
    match pool {
        Ok(pool) => Ok(pool.install(work)),
        Err(message) => Err(std::io::Error::other(message.clone()).into()),
    }
}
