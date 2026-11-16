# Agent workflow

Shared tooling and skill source: https://gitlab.com/auvire/ci-wait . Install once per machine; never commit tokens.

This repository uses GitHub Actions. The shared watcher currently supports GitLab only. Apply the follow-through and efficient-validation rules below, but use bounded manual GitHub status checks (one initial check and one follow-up after useful work per turn). Do not register GitHub runs with ci-wait or promise an automatic callback. Report the run link and pending checks if it remains active.

## Agent and CI-provider capability

Automatic delivery requires a compatible bridge for the current agent/session and CI provider. The shared ci-wait installation currently supports Codex conversation delivery and GitLab pipelines only. A Codex executable on the machine does not make Claude or another agent session compatible. Never borrow a Codex thread ID, create another agent session, or claim a callback is registered without confirming delivery to this session.

In Codex, use the installed ci-wait skill when available. In Claude Code or another agent, use an already configured and verified native asynchronous completion mechanism if available. Otherwise skip the Codex callback commands and use manual fallback: one initial status check and one follow-up after useful independent work per turn; then provide the run link, revision, pending checks and next authorized step. Use the bordered waiting format but state **Action needed: Automatic follow-up is unavailable in this session; resume here to check the run.** Do not poll with model turns, repeatedly prompt for installation, or block useful work because a Codex skill is absent. Existing approval remains valid when the user resumes.

Claude Code has native asyncRewake hooks, but this repository does not configure a Claude bridge. Do not assume ordinary async hooks wake an idle session or that a closed Claude session will receive a result. GitHub Actions likewise uses fallback until a compatible provider adapter is configured.

## Efficient work and callback follow-through

- Carry the authorized task through to completion, including necessary fixes, validation and already-approved release steps. Do not ask for renewed permission after a callback or impose automatic active-work, retry, pipeline or callback-count limits. Respect explicit limits, pauses and cancellations from the user.
- Keep repairs focused: inspect the exact failure and retained diagnostics, make the smallest justified correction, and validate affected tests first. Avoid speculative refactors, unrelated repairs and repeated full suites. Preserve meaningful assertions and required release gates.
- Waiting must not consume model turns. For pending authorized CI/deployment in a compatible session, use the installed `ci-wait` skill for the exact pipeline/revision, save the next steps, and end the turn. No model polling, sleep loops or heartbeat messages. The watcher waits without model calls.
- After a callback, inspect the saved result once and continue the same authorized task automatically. Related questions and status requests preserve approval. Defer only for a clear task switch, pause or cancellation; ask only for genuinely missing information or authorization beyond the original scope.
- If watcher credentials are unavailable, offer setup once and use the skill's bounded manual status-check fallback. Do not repeatedly prompt or build another watcher.
- A warning about credits means reduce wasted work and waiting; it does not cancel a simultaneous instruction to keep going. Report concrete blockers. Do not claim visibility into weekly credit consumption.
- Pipeline success is not deployment proof: verify the exact live revision and applicable health checks. When all authorized work is done, explicitly say no next step remains.

- An early failed-job callback leaves the final pipeline wait active. Inspect current state before fixing or canceling; if letting it run, yield for the existing final callback.

## Recognisable waiting handoff

After registering a wait, end the turn with this bordered blockquote. Use the actual
stage, pipeline link, short revision and authorized next action. Do not imply that
a deployment has finished. Say "None" for action needed only when automatic delivery
is registered and no user action is required. For manual fallback, state that automatic
follow-up is unavailable and what action is needed.

> **⏳ WAITING FOR CI — Dev deployment**
>
> **Pipeline:** [pipeline link](URL) · `revision`
> **Next:** Automatically verify dev, then promote to prod if checks pass.
> **Action needed:** None. The callback will resume this task.
