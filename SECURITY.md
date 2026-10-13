# Security

## Reporting a vulnerability

Contact the maintainer (Bdexez) privately, not in a public issue. Give the
version (`bdex-framegen --report`, or the Windows launcher's « Signaler un
problème »), the system and the steps.

## What bdex-framegen is, security-wise

Local software, nothing else: a Vulkan layer loaded into games, a capture
application, a control panel and command-line tools. It has **no server,
no account, no database, no network access at run time and no telemetry**:
it collects nothing and sends nothing. The only page it opens is GitHub's
issue form, when the user clicks « Signaler un problème », pre-filled with
a summary of the diagnostic report (the user's home or profile path
replaced by `~` / `%USERPROFILE%`, as in the report file itself).

What it protects, and how:

| Risk | Measure |
|---|---|
| Files it reads that another program could write (configuration, status and session files, recording requests, the injection marker next to a game) | Parsers bounded and fuzzed in CI (`tools/ci/fuzz.sh`: configuration, status, options UI, online_tune cache); the injection removal only deletes the translation DLLs it knows by name |
| Shared directories | State lives in the user's home (`~/.cache/bdex-framegen`, `%TEMP%`, `%LOCALAPPDATA%`); without a home, a `/tmp` directory that must be ours alone (mode 0700, owned by us) or none is used |
| Game, window and GPU names shown in the control panel | Plain text, never markup |
| Third-party code it ships (DXVK, VKD3D-Proton) | Pinned versions, SHA256-checked downloads, license texts shipped, VKD3D-Proton's complete source on each release |
| Supply chain of the build | Read-only CI token by default; third-party actions pinned by commit; Dependabot (monthly) for actions and the Decky plugin; `pnpm audit` of what the Decky plugin ships; secret scanning (gitleaks) of the whole history at every push |
| Tampered downloads | `SHA256SUMS-linux.txt` / `SHA256SUMS-windows.txt` on each release; Authenticode signing of the Windows binaries once a certificate is configured (`windows.yml`, step « Sign ») |
| Anti-cheat systems | The layer withdraws from games protected by one (and from listed competitive games); capture, which injects nothing, is used there instead |

Web-application measures (login rate limiting, sessions and cookies,
security headers, HTTPS redirection, password hashing, SQL parameters,
row-level security, file uploads, bot protection) have no object here:
there is no web service.
