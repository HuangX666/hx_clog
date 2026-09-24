# Maintenance Policy

## Supported versions

hx_clog is pre-1.0 in spirit; only the latest `master` and the most recent
tagged release receive defect fixes.

## Reporting a defect

Please report problems **privately**, not in public issues:

- Preferred: open a private advisory via GitHub
  ("Security" tab → "Report a vulnerability") on the repository, or
- contact the maintainer through the address on the repository profile.

Include enough to reproduce (affected version/commit, platform, build options,
and a minimal repro if possible). You can expect an initial acknowledgement
within a few days. Please give a reasonable window to release a fix before any
public disclosure.

## Scope notes

hx_clog is a logging library, not a parser of external content:

- The POSIX crash signal handler is best-effort and not fully
  async-signal-safe; this is documented behavior, not a defect.
- The library does not interpret untrusted log *input* by design — formatting
  is driven by caller-supplied `printf`-style format strings. As with
  `printf`, do not pass external data as the format string itself.
- The network sink transmits log lines in clear text over TCP/UDP; put it
  behind a trusted network or a TLS-terminating relay if confidentiality is
  required.
