# QuietBoost collaboration guidance

## Maintainer and attribution

- Maintainer and publisher: minelogy-dev.
- Codex acts as an AI development collaborator. Do not invite an unrelated GitHub account named Codex or grant repository access based on attribution alone.
- Use the repository-local Git identity `minelogy-dev <72928567+minelogy-dev@users.noreply.github.com>` for authorized commits.
- Add `Co-authored-by: Codex <noreply@openai.com>` only to commits actually assisted by Codex.
- Do not change global Git identity settings or publish a repository without the user's requested destination and visibility.

## Product constraints

- Keep the application native C++ with static MSVC runtime linking and one distributable EXE.
- Keep Visual Studio solution builds and product operations usable without a terminal.
- Do not write Windows system volume, application volume or mute settings. Automatic output-device selection and restoration are authorized features.
- Silence recovery defaults to enabled for every process. Disabling it requires the confirmation dialog and must never be persisted.
- Restore only default roles still pointing to the tool's virtual endpoint; preserve a user's later manual device choice.
- Keep official VB-CABLE files byte-for-byte unchanged, preserve licensing and attribution, and verify integrity/signature before launching its installer.
- Run relevant checks for behavior changes. Clearly distinguish simulated endpoint tests, silent WASAPI probes and complete non-silent virtual-device testing.
