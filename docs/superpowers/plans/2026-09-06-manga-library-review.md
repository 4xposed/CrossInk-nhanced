# Task 6 review

Reviewed by independent workers, with parent integration checks. No commit or push.

## Resolved findings

- Progress needs page count only: replaced Metadata open with Index mode to avoid a possible 196,615-byte optional metadata allocation in Home/Grid.
- Panel transitions discarded fractional seconds: reader now banks milliseconds and converts at the physical-page/store boundary.
- Final-page completion did not count that page: an explicit forward attempt banks/counts the final page once, guarded against repeat presses.
- Full sleep covers baked in contain bars and bypassed validation on warm lookup: full derivatives now preserve source aspect and manga sleep paths prepare/validate before use.
- Same-size source replacement and malformed generated pixel offsets could reuse stale/corrupt art: version 2 adds a streamed content fingerprint and strict generated-BMP header validation.
- Directory deletion could continue after incomplete metadata scans and used unbounded recursion/storage: a fallible 16 KiB iterative snapshot replaces both scans; no deletion after observable scan failure or overflow.

## Evidence

Independent re-review found no unresolved blocking issues. Final native suite passes 575/575, including 9 cover and 4 deletion-snapshot tests. Full simulator/firmware results are recorded in the validation ledger. Earlier in-flight builds are not final completion evidence.

## Scope limits

Folder move/rename is not exposed by the on-device UI; external path changes do not migrate manga state in this slice. JPEG/PNG simulator converters remain stubs, so native cover tests validate dispatch and generated-output handling rather than the production JPEG/PNG decoder. Hardware cover quality, heap stability and timing remain separate acceptance checks.
