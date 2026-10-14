# How this was developed — AI agents under evidence discipline

**Short version:** a qualified engineer organized autonomous AI agents around verifiable results. The agents scaled the work; the human owned architecture, safety requirements, completion criteria — and the definition of proof.

Topic tag for this repository: `ai-assisted`.

## The method

- **Human owns:** architecture, safety model, what counts as done, and the answer to "what does this test actually certify?".
- **Agents own scale:** source research (XNU, AMD APM, VirtualBox/QEMU as architectural references), subsystem implementation, test suites, offline mocks, documentation and evidence analysis.
- **Everything is evidence-gated:**
  - a build is not proof; a passing unit test certifies only the named property;
  - hardware candidates are staged with recorded UUID/SHA-256 identity, follow the host approval/reboot requirements and are verified after load;
  - completed reference runs record SVM MSRs across all host CPUs and quarantine counters against a baseline; these are named checks, not a full host-register snapshot;
  - unsupported conditions fail closed; checks are never disabled to obtain a green run.
- **Raw evidence and rollback bundles are retained.** A single "current state" document is the source of truth; closed audits are not repeated without a new failing case.

## Examples of what this discipline caught

- **A one-field VMCB control offset bug** (writing `0x090` instead of `0x08C`) that silently cleared `NP_ENABLE` before every `VMRUN`, disabling NPT and crashing the guest with `#UD`. Found by comparing the final staged VMCB state immediately before entry; fixed and verified on hardware.
- **A host MTRR model gap:** the decoder did not read AMD `SYSCFG.TOM2-FORCE-WB` / `TOP_MEM2`, so legitimate memory above 4 GiB was misclassified as uncached. "Code correct with respect to an incomplete model" — caught by instrumented hardware diagnostics, not by unit tests.
- **A userspace mock of the IOKit boundary** that runs the real QEMU control flow caught a cleanup bug in a failure path before it could reach hardware.

## Failure modes seen with agents (and how they were handled)

- Loops of repeated audits without new input;
- unnecessary complexity;
- over-optimistic interpretation of results;
- new fixes breaking earlier smoke tests.

The recurring remedy: stop, restate the invariant, and demand a measurement — not a narrative. Autonomy raises the importance of the operator's knowledge, it does not replace it.

## Long story

The full narrative — from the first single-instruction `VMRUN` to a booting Alpine Linux, including the kernel panic and the inter-CPU rendezvous investigation:

- English: [docs/en/story.md](story.md)
- Russian original: [docs/ru/story.md](../ru/story.md)
