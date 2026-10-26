---
name: handoff-work
description: Write next-session findings, decisions, task results, and restart instructions using a selected simplified SSC, docs mission, or full RFC workflow. Use when ending a session or requesting durable handoff notes.
---

# Handoff work

## Workflow selection

Accept `workflow: ssc`, `workflow: docs`, or `workflow: rfc` in the request, as well as natural wording such as "simplified SSC style", "docs mission style", or "full RFC workflow". These are prompt choices, not CLI flags.

Use the user's explicit choice first, then the choice recorded for this effort, then established project conventions. If none exists, default to `ssc`. Ask only when conflicting conventions materially affect where work belongs or what is authorized. State the selected workflow briefly. When writing planning or handoff documents, record `Workflow: ssc|docs|rfc` in the existing effort entry point so future sessions inherit it. A new explicit choice changes how subsequent work is organized; it does not authorize moving, deleting, or rewriting existing artifacts.

All modes honor applicable project governance. Inspect relevant instructions, document indexes, templates, and linked governing documents to identify required artifacts, authority, statuses, and approval gates. Do not infer the meaning or precedence of POL, ARC, ADR, or other custom types from their names. A simpler mode does not waive a required RFC or approval. Explain concrete conflicts and follow the applicable requirement; continue independently permitted work.

## Document layout

- **ssc:** Lightweight README entry point, active `plan.md` or phase plan, `findings.md`, and `decisions.md`. Tasks normally stay inline. Keep the next-session checklist in the active plan.
- **docs:** Group an effort under the existing mission directory, or `docs/<effort>/` when creating one. Use `brief.md` for purpose, scope, and constraints; `plan.md` for sequencing, dependencies, status, and the next-session checklist; `tasks/` for detailed tasks; and `decisions.md` for choices and unresolved questions. Reuse task templates, IDs, and formats. If none exist, use Markdown tasks with outcome, scope, references, acceptance, status, and results. Small tasks may stay inline. Add `findings.md` only for useful shared evidence.
- **rfc:** Use the project's RFC/specification, ADR, policy, and architecture conventions, including their status and approval rules. Link implementation plans and tasks to governing documents. Keep evidence, decisions, and handoff in the designated locations rather than creating parallel SSC documents. If the project has no convention, use `docs/rfcs/` for a draft proposal covering problem, goals/non-goals, evidence, alternatives, proposed design, risks, validation, rollout where applicable, and open questions; link a docs-style execution plan when useful. Do not invent mandatory approval bodies or custom document types.

The selected mode determines where the instructions below apply: "findings", "decision log", "plan", and "entry point" mean their equivalents in that workflow. Create only artifacts needed for the requested work.

Make the next session able to continue from the project's documents without replaying this conversation. Use the existing structure; the handoff normally belongs in the active plan's next-session section, linked from the README.

Read current notes and inspect relevant working-tree changes before editing. Reconcile them with the work and evidence available in this session. Do not claim to remember inaccessible conversations or overwrite unrelated changes. A handoff request authorizes recording the work, not starting the next implementation task.

In docs mode, update detailed task results and the mission plan's status and restart checklist; keep the brief focused on purpose and scope. In RFC mode, link the governing RFC/ADR/POL/ARC identifiers and actual statuses, outstanding approvals, and any proposed deviation. Preserve accepted documents according to their amendment rules; record a proposed amendment separately when required. Do not silently rewrite approved decisions, promote drafts, or create an RFC merely to close a session. Store research evidence and unresolved questions in the project's designated locations.

Capture only context that changes how the next session should proceed:

- Add undocumented discoveries to findings, preserving existing identifiers. Include evidence such as source locations, input artifacts, commands, or observed results. Distinguish measured behavior, code inspection, inference, and unresolved hypotheses. Include failed approaches when they prevent repeated effort.
- Record accepted decisions and their rationale in the decision log. Mark superseded decisions with a reference to the replacement. Keep unresolved choices, recommendations, and their implications in the active plan's open-decisions section; do not turn a suggestion into approval.
- Update task status to reflect what was implemented and what was actually verified. Include meaningful validation scope and limitations. Preserve historical results as dated evidence rather than presenting them as a fresh run.
- Refresh the next-session checklist with the first task, relevant finding/spec references, concrete input or command when known, expected stopping condition, remaining dependencies, and pitfalls. Separate work that can proceed from work that requires the user.
- Record repository location, revision, uncommitted work, or environment details when they matter to reproducing or continuing this work, especially across repositories. Do not copy sensitive values into notes.

Keep the README as a navigational entry point. Correct materially stale current-state claims touched by the handoff, but avoid a general documentation rewrite. Preserve discovery history; make corrections and supersession explicit. Do not create a parallel handoff file or tracking hierarchy unless the project needs one or the user asks.

If accessible project memory is already used, update its concise pointers and essential continuation cues. Keep durable technical detail in the repository and avoid contradictory copies. If memory cannot be accessed, say so only when relevant and leave a self-contained repository handoff.

Check that the referenced files and sections exist and that the next task agrees with recorded completion and blockers. Do not rerun expensive checks solely to write notes; label carried-forward results accurately. Finish with the files updated, the restart point, and unresolved user inputs. Do not commit or push without explicit authorization.
