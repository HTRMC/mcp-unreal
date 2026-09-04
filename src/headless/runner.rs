//! Subprocess runner for UnrealEditor-Cmd / Build.bat / RunUAT.bat.
//!
//! - No shell interpolation: explicit arg arrays only.
//! - stdout/stderr are drained concurrently so multi-hundred-MB build logs
//!   never deadlock the pipe, and partial output survives a timeout kill.
//! - On Windows the child is placed in a Job Object with kill-on-close so a
//!   timeout kills the whole tree (UBT spawns compiler children).

use std::path::Path;
use std::time::{Duration, Instant};

use tokio::io::AsyncReadExt;
use tokio::process::Command;

#[derive(Debug)]
pub struct CmdResult {
    pub stdout: String,
    pub stderr: String,
    /// Process exit code; -1 when unavailable (killed / signal).
    pub exit_code: i32,
    pub timed_out: bool,
    pub duration_secs: u64,
}

impl CmdResult {
    /// Combined stdout+stderr, the haystack for log parsers.
    pub fn combined(&self) -> String {
        if self.stderr.is_empty() {
            self.stdout.clone()
        } else {
            format!("{}\n{}", self.stdout, self.stderr)
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum RunError {
    #[error("failed to start {program}: {source}")]
    Spawn {
        program: String,
        #[source]
        source: std::io::Error,
    },
}

pub async fn run(
    program: &Path,
    args: &[String],
    timeout: Duration,
) -> Result<CmdResult, RunError> {
    let started = Instant::now();
    let mut cmd = Command::new(program);
    cmd.args(args)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .kill_on_drop(true);

    #[cfg(windows)]
    {
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    tracing::debug!(program = %program.display(), ?args, "spawning");

    let mut child = cmd.spawn().map_err(|source| RunError::Spawn {
        program: program.display().to_string(),
        source,
    })?;

    // Keep the job alive for the child's lifetime; dropping it kills the tree.
    #[cfg(windows)]
    let _job = assign_kill_on_close_job(&child);

    let mut stdout_pipe = child.stdout.take().expect("stdout piped");
    let mut stderr_pipe = child.stderr.take().expect("stderr piped");
    let stdout_task = tokio::spawn(async move {
        let mut buf = Vec::new();
        let _ = stdout_pipe.read_to_end(&mut buf).await;
        buf
    });
    let stderr_task = tokio::spawn(async move {
        let mut buf = Vec::new();
        let _ = stderr_pipe.read_to_end(&mut buf).await;
        buf
    });

    let (timed_out, exit_code) = match tokio::time::timeout(timeout, child.wait()).await {
        Ok(Ok(status)) => (false, status.code().unwrap_or(-1)),
        Ok(Err(e)) => {
            tracing::warn!("wait() failed: {e}");
            (false, -1)
        }
        Err(_) => {
            tracing::warn!(
                program = %program.display(),
                "timed out after {}s — killing process tree",
                timeout.as_secs()
            );
            let _ = child.start_kill();
            #[cfg(windows)]
            drop(_job); // job close kills the whole tree
            let _ = child.wait().await;
            (true, -1)
        }
    };

    let stdout_bytes = stdout_task.await.unwrap_or_default();
    let stderr_bytes = stderr_task.await.unwrap_or_default();

    Ok(CmdResult {
        stdout: decode(stdout_bytes),
        stderr: decode(stderr_bytes),
        exit_code,
        timed_out,
        duration_secs: started.elapsed().as_secs(),
    })
}

fn decode(bytes: Vec<u8>) -> String {
    String::from_utf8_lossy(&bytes).replace("\r\n", "\n")
}

#[cfg(windows)]
fn assign_kill_on_close_job(child: &tokio::process::Child) -> Option<win32job::Job> {
    let mut info = win32job::ExtendedLimitInfo::new();
    info.limit_kill_on_job_close();
    let job = match win32job::Job::create_with_limit_info(&info) {
        Ok(j) => j,
        Err(e) => {
            tracing::warn!("failed to create job object (kill-tree unavailable): {e}");
            return None;
        }
    };
    let handle = child.raw_handle()?;
    if let Err(e) = job.assign_process(handle as isize) {
        tracing::warn!("failed to assign process to job object: {e}");
        return None;
    }
    Some(job)
}
