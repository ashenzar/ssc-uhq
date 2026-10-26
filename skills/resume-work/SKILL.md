---
name: resume-work
description: Resume implementation or investigation from project handoff notes using a selected simplified SSC, docs mission, or full RFC workflow. Use for continuing a phase or task while respecting its governing documents and recorded decisions.
---

# Resume work

## Workflow selection

Accept `workflow: ssc`, `workflow: docs`, or `workflow: rfc` in the request, as well as natural wording such as "simplified SSC style", "docs mission style", or "full RFC workflow". These are prompt choices, not CLI flags.

Use the user's explicit choice first, then the choice recorded for this effort, then established project conventions. If none exists, default to `ssc`. Ask only when conflicting conventions materially affect where work belongs or what is authorized. State the selected workflow briefly. When writing planning or handoff documents, record `Workflow: ssc|docs|rfc` in the existing effort entry point so future sessions inherit it. A new explicit choice changes how subsequent work is organized; it does not authorize moving, deleting, or rewriting existing artifacts.

All modes honor applicable project governance. Inspect relevant instructions, document indexes, templates, and linked governing documents to identify required artifacts, authority, statuses, and approval gates. Do not infer the meaning or precedence of POL, ARC, ADR, or other custom types from their names. A simpler mode does not waive a required RFC or approval. Explain concrete conflicts and follow the applicable requirement; continue independently permitted work.

## Document layout

- **ssc:** Lightweight README entry point, active `plan.md` or phase plan, `findings.md`, and `decisions.md`. Tasks normally stay inline. Keep the next-session checklist in the active plan.
- **docs:** Group an effort under the existing mission directory, or `docs/<effort>/` when creating one. Use `brief.md` for purpose, scope, and constraints; `plan.md` for sequencing, dependencies, status, and the next-session checklist; `tasks/` for detailed tasks; and `decisions.md` for choices and unresolved questions. Reuse task templates, IDs, and formats. If none exist, use Markdown tasks with outcome, scope, references, acceptance, status, and results. Small tasks may stay inline. Add `findings.md` only for useful shared evidence.
- **rfc:** Use the project's RFC/specification, ADR, policy, and architecture conventions, including their status and approval rules. Link implementation plans and tasks to governing documents. Keep evidence, decisions, and handoff in the designated locations rather than creating parallel SSC documents. If the project has no convention, use `docs/rfcs/` for a draft proposal covering problem, goals/non-goals, evidence, alternatives, proposed design, risks, validation, rollout where applicable, and open questions; link a docs-style execution plan when useful. Do not invent mandatory approval bodies or custom document types.

The selected mode determines where the instructions below apply: "findings", "decision log", "plan", and "entry point" mean their equivalents in that workflow. Create only artifacts needed for the requested work.

Recover enough context to continue the requested work without making the user reconstruct the previous session.

Read applicable project instructions and the user's named handoff sources. Follow the README to the active plan and next-session checklist, then read the findings, decisions, and specifications relevant to the next task. Use accessible project memory as an index or supplement; do not assume a memory tool or file exists. If memory is unavailable, proceed from repository evidence unless essential information is missing.

Check working-tree changes in the repositories involved. Preserve unfinished and user-owned work. Treat recorded test results as historical evidence, not proof of the current checkout. Resolve material disagreements between notes and implementation through targeted inspection; distinguish historical findings from current decisions.

In docs mode, read the mission brief, plan, selected task, and relevant decisions together; put task-specific results in the task and shared discoveries in the mission's evidence location. In RFC mode, check the governing proposal's current status and permitted scope before implementation. Existing approval covers only its actual scope. If a required proposal or amendment is missing, draft it using the project convention and identify any real approval gate; proceed with permitted investigation or independent work. Do not generate new RFCs for routine tasks already covered by an existing proposal, or treat a draft as implementation authorization.

Briefly state the recovered starting point and the next concrete action, then execute within the user's requested scope. For an open-ended continuation, use the checklist's first actionable item and its acceptance criteria as the initial work unit. Continue authorized dependent work when appropriate; do not expand a bounded request into completing an entire roadmap.

Keep investigation flexible. Record useful discoveries, failed approaches that would otherwise be repeated, and consequential choices in the existing documents. Mark tasks complete only to the extent their acceptance is supported; distinguish implementation, validation, and user-dependent listening or manual checks. Run appropriate existing checks and add tests when the change warrants them, without imposing a test-first workflow.

If a task needs user input, identify the specific missing input and continue independent authorized work where possible. Do not manufacture decisions, results, or completion percentages.

At the end, report what was accomplished, validation performed, remaining uncertainty, and the next useful action. When the user asks for a session handoff, update the durable notes rather than leaving essential context only in chat. No separate task/run lifecycle is required.
