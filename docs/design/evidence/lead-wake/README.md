# Waking the lead only when it sleeps (design §3.10; phase 1 step 2 slice 3, C2)

`lead_wake_model.py` checks the two-word protocol that replaced a locked add and
a futex_wake syscall on every remote operation: writers set a dirty bit per
connection and wake the lead only if its `lead_sleeping` flag is raised - and
only the writer whose CAS takes the flag down. Two and three writers, one and
two operations each, one and two connections, SC and x86-TSO. `engine.py` is the
copy it was run with; `lead_wake_model.out` is the run.

The design and the unclaimed baseline (`every_writer`) both pass with no trapped
state. Each broken variant loses a wake: a `release` store of the flag by the
lead, or a plain store of the mark by the writer, only under TSO; the lead
sleeping without rechecking the bits, or the writer waking without bumping the
wake word, under SC as well.

What it does not cover: the lead's private "busy" predicate - which connections
it visits without a writer's bit. That half was wrong in the first build (the
pre-open drain left bytes unsealed), and `shmem.tree.*` found it, not the model.
