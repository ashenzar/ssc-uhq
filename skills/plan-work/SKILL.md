---
name: plan-work
description: Create or update project plans using a user-selected simplified SSC, docs mission, or full RFC workflow. Use when establishing document-based work or planning a new phase, with project governance and next-session continuity.
---

# Plan work

## Workflow selection

Accept `workflow: ssc`, `workflow: docs`, or `workflow: rfc` in the request, as well as natural wording such as "simplified SSC style", "docs mission style", or "full RFC workflow". These are prompt choices, not CLI flags.

Use the user's explicit choice first, then the choice recorded for this effort, then established project conventions. If none exists, default to `ssc`. Ask only when conflicting conventions materially affect where work belongs or what is authorized. State the selected workflow briefly. When writing planning or handoff documents, record `Workflow: ssc|docs|rfc` in the existing effort entry point so future sessions inherit it. A new explicit choice changes how subsequent work is organized; it does not authorize moving, deleting, or rewriting existing artifacts.

All modes honor applicable project governance. Inspect relevant instructions, document indexes, templates, and linked governing documents to identify required artifacts, authority, statuses, and approval gates. Do not infer the meaning or precedence of POL, ARC, ADR, or other custom types from their names. A simpler mode does not waive a required RFC or approval. Explain concrete conflicts and follow the applicable requirement; continue independently permitted work.

## Document layout

- **ssc:** Lightweight README entry point, active `plan.md` or phase plan, `findings.md`, and `decisions.md`. Tasks normally stay inline. Keep the next-session checklist in the active plan.
- **docs:** Group an effort under the existing mission directory, or `docs/<effort>/` when creating one. Use `brief.md` for purpose, scope, and constraints; `plan.md` for sequencing, dependencies, status, and the next-session checklist; `tasks/` for detailed tasks; and `decisions.md` for choices and unresolved questions. Reuse task templates, IDs, and formats. If none exist, use Markdown tasks with outcome, scope, references, acceptance, status, and results. Small tasks may stay inline. Add `findings.md` only for useful shared evidence.
- **rfc:** Use the project's RFC/specification, ADR, policy, and architecture conventions, including their status and approval rules. Link implementation plans and tasks to governing documents. Keep evidence, decisions, and handoff in the designated locations rather than creating parallel SSC documents. If the project has no convention, use `docs/rfcs/` for a draft proposal covering problem, goals/non-goals, evidence, alternatives, proposed design, risks, validation, rollout where applicable, and open questions; link a docs-style execution plan when useful. Do not invent mandatory approval bodies or custom document types.

The selected mode determines where the instructions below apply: "findings", "decision log", "plan", and "entry point" mean their equivalents in that workflow. Create only artifacts needed for the requested work.

Use the project's existing planning conventions. The goal is enough durable context for another session to continue accurately, with little bookkeeping.

Read the README, applicable project instructions, current plan, and relevant findings and decisions before planning. Inspect the implementation when needed to ground the plan. Preserve existing work and identifiers.

Choose artifacts using the selected workflow. In docs mode, establish the brief and plan before expanding detailed tasks. In RFC mode, locate an applicable existing RFC first. Reuse it when it covers the work; draft a new proposal or amendment when the requested design or governing rules require one. For a new substantive effort explicitly requesting full RFC treatment, draft the proposal even if no local rule requires it. Do not create an RFC per task.

Distinguish investigation from implementation. An investigation may gather evidence for a draft RFC without authorizing its proposed implementation. Draft an investigation RFC when requested or required, using questions, methods, outputs, and boundaries suited to research. Preserve actual approval status; never mark a proposal accepted because a plan was written. An ADR records a decision in the project's required form, not every session note.

A plan should explain the goal, bounded tasks, acceptance criteria, important dependencies, and exclusions. Use the existing status notation, or `[ ]` todo, `[~]` in progress, `[x]` complete, and `[-]` dropped with a reason. Qualify task references by phase where IDs repeat. Research tasks may finish with evidence and an explicit unknown rather than an implementation.

Keep evidence in findings and accepted choices with their rationale in decisions. Put unanswered choices in an open-decisions section of the active plan. Distinguish recommendations from accepted decisions. Link supporting findings instead of copying their contents. Keep specifications in their current home; extract a dedicated spec only when that helps the requested work.

End the active plan with a short next-session checklist: the first useful task, relevant evidence or spec sections, concrete inputs, known pitfalls, and any user-dependent work. Link the active plan from the README. Avoid duplicating changing progress summaries across files.

Planning does not itself authorize implementing the plan. Report the plan and its starting point; do not introduce approval gates between routine tasks the user has already authorized.
