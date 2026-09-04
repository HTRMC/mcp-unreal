# Contract fixtures

One JSON file per request/response exchange between the Rust server and the
McpLink plugin. Both halves read these files, so a rename on either side breaks
a test instead of breaking an agent at runtime.

Captured from real exchanges against UE 5.8.2 — do not hand-edit the shapes to
make a test pass; re-capture from a live editor instead.

## Schema

```jsonc
{
  "name":        "unique_slug",
  "description": "what this exchange covers",
  "route":       "/api/<group>/<verb>",      // plugin endpoint
  "tool":        "widget_blueprint_modify",  // MCP tool whose input type owns the request
  "request":     { "operation": "add_widget", "...": "..." },
  "status":      200,                        // HTTP status the plugin returns
  "response":    { "ok": true, "data": { } } // full envelope, verbatim
}
```

`response` is the complete envelope, not just the payload: the envelope shape
(`ok` / `data` / `error.code` / `error.message`) is itself part of the contract.

## Who checks what

**Rust** (`tests/contract_tests.rs`)
- Every fixture's `request` deserializes into the `tool`'s input type and
  re-serializes unchanged — catches a field renamed on the Rust side.
- wiremock serves `response` with `status` at `route`; `EditorClient::post` must
  yield exactly `data` on success, or an `Api` error carrying the fixture's
  status and message on failure — catches envelope-handling drift.

**Plugin** (`McpLink.Core.Contract.Fixtures`)
- Every fixture parses with the plugin's own body parser, and its `route` is
  registered in the live route table — catches a route renamed or dropped.
- `FMcpResponder` re-serialises the fixture's `data` / `error` into an envelope
  byte-identical to `response` — catches envelope drift on the plugin side.

The plugin finds this directory through the `McpLink.ContractFixtures` CVar, or
by walking up from the plugin's own directory (the layout in this repo).
