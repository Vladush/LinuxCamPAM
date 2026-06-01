# 4. JSON for Biometric Storage

Date: 2026-06-02

## Status

Accepted

## Context

When a user successfully enrolls their face, the SFace recognition model generates a 128-dimensional floating-point vector (an embedding). This vector, along with metadata (camera type, timestamp, label), must be saved to disk so it can be matched against during future logins.

Alternative solutions considered:
1.  **Binary Format (Protobuf / FlatBuffers)**: Extremely fast parsing and small file size, but requires additional build dependencies and makes debugging difficult.
2.  **SQLite Database**: Excellent for querying, but overkill for a system that typically stores fewer than 5 embeddings per user.
3.  **JSON**: Human-readable, widely supported, but parsing large arrays of floats can be slightly slower.

## Decision

We chose **JSON** using the `nlohmann/json` header-only library. Each user gets their own file (`/etc/linuxcampam/users/<username>.json`).

## Consequences

*   **Positive:** Zero dynamic dependencies required for parsing (header-only library).
*   **Positive:** Extremely easy to debug, inspect, and manually edit biometric metadata.
*   **Positive:** The file-per-user approach allows us to use standard Linux filesystem permissions (`0600`) to strictly isolate data.
*   **Negative:** Parsing JSON floats is theoretically slower than loading binary structs, but given the small scale (5 embeddings = ~640 floats), the I/O latency is negligible.
