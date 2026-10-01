# Project Agent Channel

Monitor Hub uses one durable Agent conversation channel per monitored project.

The desktop UI does **not** start a new `claude -p` / headless model session for each question. Qt only:

1. reads normalized monitor state;
2. optionally runs the project's existing read-only `live_query`;
3. appends context and the user's message to the project channel;
4. renders replies written back by the project's bound Agent or integration.

## Storage

For project `<project_id>`:

```text
MONITOR_HUB_DATA/
└── agent-channels/
    └── <project_id>/
        ├── binding.json
        └── messages.jsonl
```

`project_id` must use the same stable identity already used by registry, events, commands and recovery policies.

## Binding

`binding.json` records the Agent/session that owns the project conversation.

Example:

```json
{
  "schema_version": 1,
  "provider": "claude_code",
  "agent_id": "origin-agent",
  "session_id": "session-123",
  "session_name": "Rh/CeO2 monitor",
  "workspace": "D:/Research/Rh-CeO2",
  "transcript_path": "C:/Users/.../session.jsonl",
  "request_id": "monitor-request-20300101-010203",
  "bound_at": "2030-01-01T01:02:03"
}
```

New monitor requests capture the currently observed Claude Code session only when its workspace is the same as, a parent of, or a child of the requested project directory. This prevents an unrelated active Claude session from being silently bound to a new monitor.

The bootstrap/setup Agent is not the conversation owner. After it chooses the stable project ID, it binds the project to the origin metadata and stores the original monitor request in the project channel.

## Messages

`messages.jsonl` is append-only. Each line is one JSON object:

```json
{
  "schema_version": 1,
  "message_id": "msg-...",
  "project_id": "alpha",
  "created_at": "2030-01-01T01:03:00",
  "sender": "user",
  "target": "project_agent",
  "kind": "question",
  "body": "现在跑到哪一步？",
  "correlation_id": "qa:alpha:...",
  "reply_to": "",
  "source": "qt"
}
```

Common kinds:

- `monitor_request`: original monitoring request kept as project context;
- `context`: Monitor Hub normalized status/live-query context for a question;
- `question`: user question to the bound project Agent;
- `instruction`: user follow-up instruction for the bound project Agent;
- `answer`: Agent reply;
- `live_status`: manually requested read-only live status.

An Agent answer should set `reply_to` to the message ID it answers. Pending questions are derived from unanswered `question` / `instruction` messages.

## CLI / MCP integration surface

The CLI is the stable non-UI integration surface intended for an MCP gateway or another Agent host.

Read the full project conversation:

```text
monitor_hub_cli --agent-channel <project_id>
```

Read only unanswered messages:

```text
monitor_hub_cli --agent-pending <project_id>
```

Post a question or instruction:

```text
monitor_hub_cli --agent-post <project_id> --message-file <utf8-file>
monitor_hub_cli --agent-post <project_id> --message-file <utf8-file> --agent-kind instruction
```

Write an Agent reply:

```text
monitor_hub_cli --agent-reply <project_id> --reply-to <message_id> --message-file <utf8-file> --agent-source monitor-hub-mcp
```

Bind the project to its originating Agent/session:

```text
monitor_hub_cli --agent-bind <project_id> --binding-file <binding.json>
```

All commands also accept `--hub-data <dir>`.

## Authority

The Agent Channel is a messaging/context surface, not an authority bypass.

- A question does not grant permission to change files, parameters, jobs or scientific method.
- Recovery remains governed by the existing L1/L2/L3 command/policy system.
- User instructions that imply an operational action must still pass through the normal application-service / command-control authority path.
- `agent action finished` still does not mean `issue resolved`; recovery must be verified independently.

## Backward compatibility

Existing registry projects remain valid without a binding. Their project channel is created lazily when a message is stored.

For an unbound legacy project, Qt can keep queued project messages durably. An Agent/MCP integration can bind the project later without rewriting the registry or changing its project ID.
