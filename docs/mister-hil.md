# MiSTer FPGA Saturn HIL research spike

Status: **opt-in evidence capture; no Saturn guest test assertion**.

The independent runner \`tools/mister_hil.py\` accepts an already-built CUE/BIN image,
copies it to MiSTer by SSH/SCP, launches it with the Zaparoo Core JSON-RPC API,
optionally injects controller input and saves a PNG and JSON report.

This is a *MiSTer FPGA* integration, not a real Sega Saturn hardware oracle.
MiSTer cannot be treated as a substitute for original hardware or as an
independent proof of hardware timing.

## Prerequisites

- A MiSTer FPGA with a Saturn core, compatible BIOS and sufficient storage.
- Zaparoo Core installed and reachable at port 7497 on a trusted LAN/VPN.
- SSH public-key access for \`root@<mister-host>\` and a pinned SSH host key.
- A homebrew Saturn disc image in one folder, with a UTF-8 \`.cue\` file that
  refers to same-directory files using \`FILE "<name>.bin" BINARY\`.
- A PC with Python 3.10+, SSH and SCP.

Never upload proprietary game assets, BIOS files or disc images into GitHub CI.
A compatible image must be built/provided locally and handled as a private
artifact. The runner does not include a Saturn compiler or a disc packager.

## Usage

From the repository root:

\`\`\`bash
python3 -m unittest discover -s tools/tests -p 'test_mister_hil.py' -v

# Network/HIL exercise: run this on a trusted PC, not on public PR runners.
python3 tools/mister_hil.py \
  --cue /path/to/homebrew/smoke.cue \
  --host mister.local \
  --run-id psyz-smoke-001 \
  --buttons '{start}' \
  --settle 5 \
  --output build/mister-hil
\`\`\`

The runner writes \`build/mister-hil/screenshot.png\` and
\`build/mister-hil/report.json\`. Its success status is
\`"evidence_only"\`, and \`"guest_result": "not_asserted"\`. It **never**
labels the Saturn application as PASS based merely on a launch or screenshot.
An infrastructure failure yields exit code 2 and a JSON error report.

You may set \`--zaparoo-url http://<private-address>:7497\` and supply a
Zaparoo static API key through \`ZAPAROO_API_KEY\` (Bearer header).
Use an isolated network or SSH tunnel, as HTTP is not encrypted.
Some newer Zaparoo configurations use paired/encrypted clients instead:
this minimal HTTP client does not implement the pairing protocol.

## What gets checked

1. CUE file parses to same-directory, existing, non-symlinked track files.
2. Remote host and run directory are restricted to safe identifiers.
3. SSH with strict host-key checking creates a dedicated directory under
   \`/media/fat/games/Saturn/PsyZHIL/<run-id>\`.
4. SCP transfers the CUE and its referenced tracks.
5. Zaparoo \`version\` returns a valid JSON-RPC response.
6. Zaparoo \`run\` accepts \`**launch:<remote-cue-path>\`.
7. Zaparoo \`media.active\` reports a Saturn system ID.
8. Optional \`input.gamepad\` macro reaches Zaparoo (not guaranteed to be mapped
   to controller 1 inside the Saturn core).
9. Zaparoo \`screenshot\` returns a PNG-like image, which is saved and hashed.

For the next phase, a guest-authored **machine-readable test result** is
essential. Prefer a minimal display protocol with fixed geometry and known
pixel regions, or a separately validated debug transport, plus explicit build
ID, test count and timeout. Do not infer functional PASS from successful
transfer, launch or screenshot. For visual tests, compare stable native frames,
record the FPGA core version, resolution, video settings and BIOS configuration,
and allow core-specific tolerances.

## Security and CI integration

The contract workflow \`.github/workflows/mister-hil-contract.yml\` runs
only Python unit tests with no hardware access, no privileged secrets and
read-only repository permissions.

Do **not** run public pull request code on a permanently network-connected
self-hosted runner with access to MiSTer. Keep the MiSTer behind a VPN/LAN.
For actual HIL, use a trusted operator or an independently secured worker
which downloads allowlisted build artifacts after approval. Only one job
may control the MiSTer at a time; serialize work and use timeouts.

The device is stateful: the runner does not restore the previous core,
remove old images, establish gamepad assignment, or reset the Saturn after
each test. Dedicated hardware and a per-run storage retention policy are
required before making this unattended.

## Roadmap / exit criteria

- **R0 (this PR):** isolated deploy/launch/input/screenshot client, contract
  tests, documentation. Tested without physical MiSTer.
- **R1 (physical calibration):** successful launch of an independently verified
  Saturn homebrew image; controller mapping and screenshot verified manually.
  Record Zaparoo/MiSTer/Saturn-core versions and BIOS details.
- **R2 (PsyZ integration):** reproducible Saturn CMake/toolchain target,
  BIN/CUE packaging, deterministic guest test program with assertable results.
- **R3 (CI HIL):** trusted artifact promotion, exclusive scheduling,
  error categorization, timeouts, JUnit output and artifact retention.
- **R4 (hardware parity):** compare representative tests with Ymir, Mednafen
  and original Sega Saturn hardware. Treat agreement as evidence, not proof.

## Prior art

- Zaparoo Core API: https://zaparoo.org/docs/core/api/
- Zaparoo methods (\`run\`, \`media.active\`, \`input.gamepad\`, \`screenshot\`):
  https://zaparoo.org/docs/core/api/methods/
- Zaparoo launch syntax: https://zaparoo.org/docs/zapscript/launch/
- Zaparoo virtual input mapping: https://zaparoo.org/docs/zapscript/input/
- MiSTer Remote: https://github.com/wizzomafizzo/mrext
- Remote API: https://github.com/wizzomafizzo/mrext/blob/main/docs/remote-api.md
- MiSTerClaw MCP: https://github.com/catallo/misterclaw
- Saturn MiSTer core: https://github.com/MiSTer-devel/Saturn_MiSTer
