# Repository guidance

## Code Review Rules

### Untrusted binary and configuration inputs

- Treat KN5, KSANIM, KNH, ACD, CSP, and mod files as untrusted input.
- Flag parsers that use counts or offsets without bounds checks.
- Require a malformed or truncated input test for each parser change.

### Rendering fidelity

- Flag changes that replace recovered native or CSP behavior with an unlabeled approximation.
- Require source or disassembly evidence for claims of exact behavior.
- Require a production WebGL check for changes to visible rendering behavior.

### Cross-platform desktop boundary

- Keep the renderer sandboxed from Node.js.
- Keep the application server on the loopback interface.
- Keep runtime paths portable across Windows, macOS, and Linux.
- Flag exposed permissions, external navigation, and target-specific runtime assumptions.
