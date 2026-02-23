# Porting Tools

This directory contains helper tooling for structured upstream porting work.

## `generate_gap_report.py`

Compares the Smartiecoin tree against an upstream Bitcoin Core checkout and
produces:

- A markdown report with high-level deltas.
- A JSON report for automation and backlog tracking.

### Example

```powershell
py -3 contrib/devtools/porting/generate_gap_report.py `
  --smartie-root . `
  --bitcoin-root .upstream/bitcoin-30.2 `
  --output-md doc/porting/bitcoin-v30.2-gap-report.md `
  --output-json doc/porting/bitcoin-v30.2-gap-report.json
```

Run this report again after each batch of port changes to track progress.

