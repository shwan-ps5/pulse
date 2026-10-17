# Contributing

Pulse is a hobby project, not a production line. Bug reports, ideas and pull requests are welcome, and nobody is on a deadline:
things get looked at when there is time, and a PR may take a while to get a reply.

## Reporting a problem

Open an issue and say what you were doing, which level and song, your console firmware and whether it is a standard PS5 or a Pro.
If the game crashed, the log helps: development builds write `/download0/ps5-opengl.log` and can stream it to a PC with
`tools/logsrv.py`. Please leave out anything private (addresses, account names).

## Sending a change

- Keep a change small and about one thing. A bug fix or a tweak is easier to review than a rewrite.
- Run `make test` first; it pins the rules, the ranks and the map generator.
- Read `AGENTS.md`: it explains how the code fits together and the console quirks that have already cost time (folder listing, memory,
  drawing order, no flat frames in VR). Comments in the code follow the same compact style.
- If you change how a level plays or how ranks work, update the tests, the Help screen text and the README to match.
- If you change what the mapper produces, bump `library::kMapperVersion` so old cached maps are not reused.
- Testing on a real console is the only proof for anything that touches VR, the headset or the sandbox. Say what you tried it on.
- By sending a change you agree it is released under the project's licence, GPL-3.0-or-later (see `LICENSE`).

## Other games

The platform, VR and rendering layers are written to be reused. `AGENTS.md` has a short recipe for starting another PS VR2 game from this
base. You are welcome to do that in your own repository.
