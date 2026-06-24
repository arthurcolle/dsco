

## Operating Rule: Temporal Discipline (added 2026-06-24)

**ALWAYS call `timestamp` at the start of every task before reasoning about anything time-sensitive.**

Applies to: email/meeting replies, deadlines, relative-date language ("tomorrow/today/next week"), invoices, anything labeled "latest/recent."

- Email inbox tools (cloudmail `inbox`) may return STALE snapshots **not** anchored to today. Never assume returned messages are recent.
- Cross-check every relative date ("meeting tomorrow") against BOTH (a) the message's own Date header and (b) the real current date — before drafting or acting.

**Failure that triggered this rule:** 2026-06-24 — drafted a "Confirmed, I'll be on the call tomorrow" reply to Andy's LogiQ invite that was dated May 11 (~6 weeks stale). The "tomorrow" in the invite had long passed. Caught only when the user asked for the current date.
