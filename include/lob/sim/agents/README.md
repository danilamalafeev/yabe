# Reference agents

These agents are intentionally small infrastructure examples. They demonstrate
safe `AgentContext` usage, deterministic wakeups, and delayed public market-data
consumption. Production strategies should use `AgentContext` only and must not
depend on exchange diagnostics or live matching-book state.
