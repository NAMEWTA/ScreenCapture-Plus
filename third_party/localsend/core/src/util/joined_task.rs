// huhucapture modification; see docs/localsend-core.md and upstream.json.
//! One owned supervisor join, including a persistent result for repeat callers.

use tokio::sync::Mutex;
use tokio::task::JoinHandle;

pub(crate) struct JoinedTask {
    state: Mutex<State>,
}

struct State {
    task: Option<JoinHandle<()>>,
    failure: Option<&'static str>,
}

impl JoinedTask {
    pub(crate) fn new(task: JoinHandle<()>) -> Self {
        Self { state: Mutex::new(State { task: Some(task), failure: None }) }
    }

    pub(crate) async fn wait(&self) -> anyhow::Result<()> {
        let mut state = self.state.lock().await;
        if let Some(task) = state.task.as_mut() {
            // Await by reference: cancellation of this waiter must not detach
            // the supervisor or make the next waiter report a false success.
            let result = task.await;
            state.task = None;
            if let Err(error) = result {
                state.failure = Some(if error.is_panic() { "supervisor panicked" }
                    else { "supervisor was cancelled before completing shutdown" });
            }
        }
        match state.failure {
            Some(message) => Err(anyhow::anyhow!(message)),
            None => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, atomic::{AtomicBool, Ordering}};
    use std::time::Duration;
    use tokio::sync::{oneshot, Semaphore};

    struct Dropped(Arc<AtomicBool>);
    impl Drop for Dropped {
        fn drop(&mut self) { self.0.store(true, Ordering::Release); }
    }

    #[tokio::test]
    async fn actual_task_panic_is_joined_and_remains_an_error() {
        let dropped = Arc::new(AtomicBool::new(false));
        let owner = Dropped(dropped.clone());
        let supervisor = Arc::new(JoinedTask::new(tokio::spawn(async move {
            let _owner = owner;
            panic!("exercise a real supervisor unwind");
        })));
        let (first, concurrent) = tokio::join!(supervisor.wait(), supervisor.wait());
        assert!(first.unwrap_err().to_string().contains("panicked"));
        assert!(concurrent.is_err());
        assert!(dropped.load(Ordering::Acquire));
        assert!(supervisor.wait().await.is_err());
    }

    #[tokio::test]
    async fn cancelled_waiter_retains_the_task_until_it_really_exits() {
        let release = Arc::new(Semaphore::new(0));
        let done = Arc::new(AtomicBool::new(false));
        let (started, started_rx) = oneshot::channel();
        let supervisor = JoinedTask::new(tokio::spawn({
            let release = release.clone();
            let done = done.clone();
            async move {
                started.send(()).unwrap();
                let _permit = release.acquire().await.unwrap();
                done.store(true, Ordering::Release);
            }
        }));
        started_rx.await.unwrap();
        assert!(tokio::time::timeout(Duration::from_millis(10), supervisor.wait()).await.is_err());
        assert!(!done.load(Ordering::Acquire));
        release.add_permits(1);
        supervisor.wait().await.unwrap();
        assert!(done.load(Ordering::Acquire));
        supervisor.wait().await.unwrap();
    }

    #[tokio::test]
    async fn aborted_supervisor_is_not_reported_as_success() {
        let task = tokio::spawn(std::future::pending::<()>());
        task.abort();
        let supervisor = JoinedTask::new(task);
        assert!(supervisor.wait().await.unwrap_err().to_string().contains("cancelled"));
        assert!(supervisor.wait().await.is_err());
    }
}
