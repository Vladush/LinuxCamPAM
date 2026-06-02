# 1. Record Architecture Decisions

Date: 2026-06-02

## Status

Accepted

## Context

We need to record the architectural decisions made on this project. Historically, decisions were made and implemented without a persistent record of the *why*, making it difficult for new contributors to understand the rationale behind specific tech choices (e.g., IPC mechanisms, hardware acceleration paths, storage formats).

## Decision

We will use Architecture Decision Records, as described by Michael Nygard in [Documenting Architecture Decisions](http://thinkrelevance.com/blog/2011/11/15/documenting-architecture-decisions).

We will keep ADRs in the `docs/adr` directory. Each ADR will follow a simple structure: Title, Context, Decision, and Consequences.

## Consequences

* We have a durable, version-controlled history of our architectural decisions.
* Contributors will be required to write an ADR when proposing significant architectural changes.
* The barrier to understanding the codebase's history is significantly lowered.
