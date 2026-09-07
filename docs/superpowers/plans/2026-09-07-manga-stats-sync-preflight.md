# Reading-language Nearby sync compatibility preflight

Date: 2026-09-07. Documentation-only source audit. CrossInk CodeGraph was consulted before targeted reads. No build, simulator, radio or hardware test was performed.

## Finding

Current Nearby stats sync has an application-level success ACK, but no explicit rejection packet or capability negotiation. An old receiver cannot falsely acknowledge a v4 summary: it accepts STATS only when `isValidStatsPayload()` recognizes the version/size, converts an invalid payload to the internal-only `INVALID_STATS` event, enters its own version-mismatch error, and never calls `sendAck()` (`src/activities/network/NearbyStatsSyncActivity.cpp:142-145`, `:357-395`, `:451-516`). ACK is sent only after `writeSyncedStatsFile()` has validated, written, flushed, synced, closed and renamed the received file (`:169-203`, `:506-513`). The sender reaches SYNCED only when it has both saved the peer's stats and received that application ACK (`:592-613`).

Transport acceptance is weaker. The activity calls `esp_now_send()` directly and treats `ESP_OK` as “sent” (`NearbyStatsSyncActivity.cpp:531-582`); it registers only a receive callback, not an ESP-NOW send-status callback (`:281-304`). The SDK `EspNowTransport::send()` has the same queue-acceptance semantics—`esp_now_send(...) == ESP_OK`—and exposes received packets only (`freeink-sdk/libs/network/NearbyTransfer/src/NearbyTransfer.cpp:187-203`, `:205-267`). Neither path proves peer delivery or parsing. The Stats activity does not use the SDK transport wrapper, so changing that wrapper alone would not change this protocol.

Therefore the existing design's safety claim is true only in the narrow sense that missing application ACK eventually prevents a success screen. A new sender cannot distinguish an old receiver's format rejection from lost packets, radio loss, queue drops or storage failure: it retries every 750 ms and reports the generic 12-second timeout (`NearbyStatsSyncActivity.cpp:100-103`, `:592-613`). Without a new signal from the receiver or an advertised capability, explicit recognition is impossible.

## Narrow compatibility contract

Use reserved stats-envelope byte 7 as a backward-compatible maximum-summary-version capability. The current sender always writes zero there (`NearbyStatsSyncActivity.cpp:535-543`), and the current receiver neither validates nor interprets it (`:357-395`), so old firmware continues to accept HELLO, NAME and ACK packets of the unchanged 14-byte length.

- Updated HELLO and NAME packets set byte 7 to the highest stats summary version the sender can import. Zero means legacy/unspecified and must be treated conservatively as maximum v3 for peers using protocol version 1.
- Extend `SyncEvent` and peer session state with this single byte. Capture it from every accepted peer packet, reject inconsistent nonzero values during one session, and decide compatibility before sending local STATS in response to HELLO.
- An updated v4 device receiving legacy zero/v3 must not send its v4 summary. Record an incompatible-peer flag but keep processing that peer's already-sent v1-v3 STATS long enough to validate, save and ACK it; only then enter the existing translated version-mismatch terminal state (or time out waiting for the old summary). Entering `State::ERROR` immediately would make `handleEvent()` discard the old STATS at its current early return (`NearbyStatsSyncActivity.cpp:451-453`) and violate the design's old-summary import promise. The overall bidirectional exchange never becomes SYNCED because its own v4 summary was not imported remotely.
- Updated peers advertising v4 exchange the 223-byte summary normally. Application ACK continues to mean the specific STATS payload was validated and durably published. Do not reinterpret ESP-NOW queue acceptance or a send callback as import success.
- Do not silently downgrade v4 to v3: doing so would omit language totals and could let both screens report success for an incomplete exchange. A separate explicit one-way/import-old-only UX would require its own wording and is outside this minimum.

This is capability negotiation, but it uses an already-reserved byte and does not change the envelope protocol version, packet length or add another round trip. If negotiation is declined, the plan must instead promise only “old peers cannot produce false success; incompatibility appears as a generic timeout.” It cannot promise that the sender recognizes rejection.

## Tests required

Add a deterministic two-peer state-machine harness around packet encode/decode and storage publication; `SIMULATOR` currently replaces Nearby stats sync with an unavailable screen, so UI-only simulator coverage cannot prove the protocol (`NearbyStatsSyncActivity.cpp:5-63`). Test:

1. Updated v4 ↔ updated v4 advertises v4, exchanges 223-byte summaries, ACKs only after successful durable publication, and both sides reach SYNCED.
2. Updated v4 sender ↔ legacy receiver fixture with byte 7 zero: updated side identifies maximum v3 before sending STATS, sends no v4 payload, still imports/ACKs the valid old summary, then reports version mismatch rather than success. If the old summary never arrives, a bounded timeout remains valid. The legacy state machine must never reach SYNCED.
3. Legacy sender ↔ updated v4 receiver: v1/v2/v3 import succeeds and attributes legacy time as Unknown, but the updated side does not claim bidirectional completion when the peer cannot accept v4.
4. Missing, malformed and inconsistent capability values, unrelated protocol versions, spoofed ACK device MACs, ACK-before-send, packet loss, event overflow, storage write/sync/close/rename failure and timeout cannot set `localStatsAcked_` or SYNCED.
5. Capture the outgoing v4 STATS bytes and feed them to the exact pre-v4 validation fixture. Assert rejection emits no ACK. Then feed an ACK only after the receiver fixture has accepted and durably published a supported payload.

Hardware acceptance pairs updated↔updated and updated↔old C3/S3 devices, with packet loss and SD write failure injected where practical. Expected evidence separates queued send, received capability, parsed version, durable import ACK and final bidirectional state in logs. No daily appendix is transmitted.
