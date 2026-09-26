# Spec-Driven Development (SDD) for AI Agents

## What is Spec-Driven Development?

Spec-Driven Development is a methodology where you write detailed specifications that AI agents can read, understand, and implement. Instead of writing code directly, you write a comprehensive spec that describes **what** needs to be built, and then AI agents can autonomously implement the **how**.

## Why Use SDD?

1. **Clear Requirements**: Forces you to think through requirements before implementation
2. **AI-Friendly**: Provides structured context for AI agents to work with
3. **Documentation Built-In**: Specs serve as living documentation
4. **Iterative Refinement**: Easy to update specs and regenerate code
5. **Team Alignment**: Everyone understands what's being built

## Spec Structure

A well-formed spec should include:

### 1. Overview
Brief description of what this spec is about.

### 2. Objective
Clear statement of the goal.

### 3. Requirements
- **Functional Requirements (FR)**: What the system should do
- **Non-Functional Requirements (NFR)**: Performance, security, usability constraints

### 4. Acceptance Criteria
Given-When-Then scenarios that define success.

### 5. Implementation Details
Suggestions for how to implement (but not prescriptive).

### 6. Test Cases
Specific inputs and expected outputs.

### 7. Constraints checked
An explicit list of the project principles this design was checked against, and the
one-line outcome of each check. The principles live in `CLAUDE.md` and Claude's
memory notes (e.g. *forces add energy, damping resists, redirects turn* — reach for
damping before a new force; *wave-mass forces have four roles* — check all four
before touching a coefficient; *score on ride aggregates, never single-event
maxima*; *never detect turns from roll*). Listing them turns tribal knowledge into
a gate: a plan that never names the principle it might violate has not been
checked against it.

```markdown
## Constraints checked
- Forces are not the default tool — this adds damping, not a force. OK.
- Wave-mass four roles — touches waveMassFlow; drive + redirect re-measured, glide-through block + pitch untouched.
- Aggregates not maxima — acceptance uses whole-ride mean speed, not peak.
```

### 8. Tasks
A numbered, dependency-ordered checklist at the bottom of the spec, checked off as
work lands. Mark tasks that can run independently with `[P]`. This is the resume
point: a handoff note or a fresh session only needs to say "next is T4", not
re-derive the plan from the prose above. Keep it honest — a task that turned out
unnecessary gets struck through with a reason, not silently deleted.

```markdown
## Tasks
- [x] T1 Add `waveNormalDampingCoefficient` to SurfTuningSubsystem (default 0)
- [x] T2 Apply it in ASharedCalculations after the planing redirect
- [ ] T3 [P] Headless run `surf_straight`, compare mean speed vs baseline
- [ ] T4 [P] Headless run `hard_turn`, check turn duration unchanged
- [ ] T5 Device validation, player verdict → bake default or revert
```

### 9. Status Tracking
A `## Status` section at the **top** of the spec: a few lines on where the work is
(branch, what's built, what's validated, what's next) — this is what a fresh
session reads first. Point at the Tasks list for the detail.

## File Naming Convention

```
specs/
├── examples/           # Example specs for learning
├── images/             # Figures referenced from specs
└── <name>.md           # One flat file per feature / bug / investigation

Example: specs/stamina.md, specs/broken-wave-no-consequences.md
```

Branch name matches the spec name where practical (`stamina` → branch `stamina`).

## How to Use with AI Agents

### Step 1: Write the Spec
Create a detailed specification document following the structure above.

### Step 2: Share with AI Agent
Provide the spec to your AI coding assistant (like Claude) with a prompt:

```
Please implement the specification in specs/my-feature.md
```

### Step 3: Review and Iterate
Review the implementation, update the spec if needed, and ask the AI to refine.

### Step 4: Update Status
Tick Tasks as they land and keep the top `## Status` section current — it is the
handoff to the next session. Mark the spec as complete once merged.

### Note on ordering
For bugs and tuning work the investigation comes **before** the spec: the "what"
often can't be written until traces and telemetry say what is actually happening.
Fold the findings into the spec (a `## What the traces show` section) rather than
leaving them in chat.

## Best Practices

### DO:
✅ Be specific about requirements
✅ Include acceptance criteria with examples
✅ Provide context about the codebase
✅ Use clear, unambiguous language
✅ Include test cases
✅ List the project principles the design was checked against
✅ End with a numbered Tasks checklist and keep it ticked
✅ Keep specs version controlled

### DON'T:
❌ Be too prescriptive about implementation details
❌ Write specs for trivial changes
❌ Leave specs stale after implementation
❌ Skip acceptance criteria
❌ Forget to update status

## Example Workflow

1. **Identify Need**: "We need a player inventory system"

2. **Write Spec**: Create `specs/player-inventory.md`

3. **AI Implementation**:
   ```
   Please implement the player inventory system described in
   specs/player-inventory.md following Unreal Engine best practices.
   ```

4. **Review**: Check the generated code against acceptance criteria

5. **Iterate**: Update spec if requirements change, ask AI to adjust

6. **Complete**: Mark spec as done, merge code

## Templates

### Feature Spec Template
See [examples/hello-world.spec.md](examples/hello-world.spec.md) for a complete example.

### Bug Fix Spec Template
```markdown
# Spec: Fix [Bug Name]

## Bug Description
Clear description of the bug

## Current Behavior
What happens now

## Expected Behavior
What should happen

## Root Cause (if known)
Analysis of why the bug occurs

## Acceptance Criteria
How to verify the fix

## Test Cases
Reproduction steps and expected fixes

## Constraints checked
Which project principles the fix was checked against, and the outcome of each

## Tasks
- [ ] T1 ...
```

## Integration with Development Flow

### With Version Control
- Commit specs alongside code
- Reference spec in commit messages: "Implements specs/inventory.md"
- Use specs for code review context

### With Unreal Engine
- Place implementation files in appropriate Source/ directories
- Update .uproject or Build.cs files as needed
- Follow Unreal coding standards
- Use appropriate log categories

### With AI Agents
- Provide full spec in conversation context
- Ask AI to implement incrementally
- Use specs to guide debugging and refactoring

## Getting Started

1. Check out the example: [examples/hello-world.spec.md](examples/hello-world.spec.md)

2. Try implementing it:
   ```
   I want to implement the Hello World spec. Please read
   specs/examples/hello-world.spec.md and create the implementation.
   ```

3. Create your first real spec for a feature you need

4. Iterate and refine your SDD process

## Resources

- [Unreal Engine Documentation](https://docs.unrealengine.com/)
- [Behavior-Driven Development (BDD)](https://cucumber.io/docs/bdd/)
- [Writing Good User Stories](https://www.atlassian.com/agile/project-management/user-stories)

---

**Happy Spec-Driven Development!** 🚀
