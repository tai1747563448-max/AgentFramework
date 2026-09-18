"""MCP fixture server used by the T14 stdio client tests.

Speaks JSON-RPC 2.0 over stdin/stdout with Content-Length framing.
Implements the minimum surface the runtime needs:
  - initialize   -> reply with capabilities + server info
  - tools/list   -> reply with one synthetic tool called "echo"
  - tools/call   -> reply with the args echoed back as a content array
The fixture also surfaces a synthetic error on a call to a tool
named "boom" so tests can exercise the error path.
"""

import json
import sys


def reply(id_, payload):
    body = {"jsonrpc": "2.0", "id": id_}
    body.update(payload)
    encoded = json.dumps(body).encode("utf-8")
    sys.stdout.buffer.write(
        ("Content-Length: %d\r\n\r\n" % len(encoded)).encode("ascii")
    )
    sys.stdout.buffer.write(encoded)
    sys.stdout.buffer.flush()


def read_envelope(stream):
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = stream.read(1)
        if not chunk:
            return None
        header += chunk
    headers_blob, body = header.split(b"\r\n\r\n", 1)
    content_length = 0
    for line in headers_blob.split(b"\r\n"):
        key, _, value = line.partition(b":")
        if key.strip().lower() == b"content-length":
            content_length = int(value.strip())
    while len(body) < content_length:
        body += stream.read(content_length - len(body))
    return json.loads(body.decode("utf-8"))


def main():
    stream = sys.stdin.buffer
    while True:
        envelope = read_envelope(stream)
        if envelope is None:
            return
        method = envelope.get("method")
        id_ = envelope.get("id")
        if method == "initialize":
            reply(id_, {
                "result": {
                    "protocolVersion": "2024-11-05",
                    "serverInfo": {"name": "mcp-fixture", "version": "0.1.0"},
                    "capabilities": {"tools": {"listChanged": False}},
                }
            })
        elif method == "tools/list":
            reply(id_, {"result": {"tools": [
                {"name": "echo", "description": "echo arguments back",
                 "inputSchema": {"type": "object"}},
                {"name": "boom", "description": "always errors",
                 "inputSchema": {"type": "object"}},
            ]}})
        elif method == "tools/call":
            params = envelope.get("params", {})
            name = params.get("name")
            arguments = params.get("arguments", {})
            if name == "boom":
                reply(id_, {"result": {
                    "isError": True,
                    "content": [
                        {"type": "text", "text": "synthetic boom"},
                    ],
                }})
            else:
                reply(id_, {"result": {
                    "isError": False,
                    "content": [
                        {"type": "text", "text": json.dumps(arguments)},
                    ],
                }})
        elif method == "shutdown":
            reply(id_, {"result": {}})
        else:
            reply(id_, {"error": {
                "code": -32601,
                "message": "method not found: " + str(method),
            }})


if __name__ == "__main__":
    main()
