# Security

This is an experimental desktop application, not a service intended for exposure
to the Internet. The HTTP host binds to `127.0.0.1` and requires its generated
bearer token. Do not publish that token or forward its port to a public interface.

Scene actions reject credential fields, stale revisions and arbitrary file paths.
Native chat, MCP and HTTP share the same validation boundary. These protections
do not make it safe to run untrusted executable code or to share provider logins.

Report suspected vulnerabilities through GitHub's **Report a vulnerability**
feature when enabled. If it is unavailable, open an issue asking for a private
reporting channel without including exploit details or secrets. Do not attach
tokens, chats, private scenes or crash dumps to public issues.

There is no stable release or guaranteed response time at this stage.
