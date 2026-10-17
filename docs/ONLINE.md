# Online track library and leaderboards

Decisions: share **maps only** (never audio); players supply their own MP3. Own **server** for search,
uploads, usernames and scores; a **GitHub repo** mirrors the map catalogue. The console talks only to
the server (plain sockets, no DNS, no TLS), never to GitHub.

## What a track is
A track entry is the map file plus metadata, keyed by an **audio fingerprint**, not by a file name:

| Field | Notes |
|---|---|
| `fp` | fingerprint of the audio (below), the primary key |
| title, artist | free text from ID3 tags or the uploader; searchable |
| duration, bpm | from the analysis; used for matching and display |
| analyser version | maps from a newer analyser are not re-derived locally |
| maps | easy / medium / hard `.pulse` text (10-20 KB each) with the uploader's **best seed** and any hand edits |
| uploader, created, plays, rating | server-side |

Several maps can exist per fingerprint (different seeds / uploaders); the library shows them ranked by
plays and rating, and the uploader's chosen seed is just one of them. A map is deterministic from
`(audio, analyser version, seed)`, so an upload without hand edits needs only those three numbers; hand
edits ship the full note list.

## Matching a local MP3 to a map
Different rips of one song differ in encoder delay, bitrate and a second or so of silence, so a file hash
fails. Fingerprint instead from what the analysis already computes:
1. duration rounded to 0.5 s, and BPM rounded to 0.1;
2. a hash of the beat-grid's onset signature: the strongest ~64 low-band onset intervals in beats, quantised
   to 1/4 beat (robust to offset and gain);
3. the **time offset** between the local audio's first beat and the map's, stored in the downloaded map so a
   rip with a different lead-in still lines up (the viewer's offset calibration applies it).
The server returns candidates by (duration, bpm); the client confirms with the onset signature and
rejects mismatches. This is a v1 design to validate on a few rips of one track; if it proves unreliable,
a chroma-based fingerprint can replace step 2 without changing the protocol.

## Server
A small UDP server (token handshake, resend/ack, append-only leaderboard file, TCP status page). It needs
HELLO/WELCOME, reliable messages and a per-level board, and for Pulse:
- **Usernames**: first launch picks a name (default: the PSN user name from `platform::playerName()`);
  the server issues and the client stores a secret token in `/download0` (no passwords). Names are unique,
  3-16 chars, profanity-filtered; a token can rename.
- **Search** (`SEARCH` -> `RESULTS`): text query plus filters (bpm range, duration, newest, most played),
  paged rows of 40 (existing `LIST_ROWS`).
- **Transfer**: datagrams cap at 1200 bytes, so maps move in numbered chunks with the existing
  resend/ack (about 15 chunks per map), or over a TCP port if UDP chunking proves fiddly.
- **Upload** (`UPLOAD_BEGIN/CHUNK/END`): size-limited, validated server-side with the same
  `mapper::validate` the tool uses, plus rate limits per token.
- **Scores**: board key = `(fp, map id, difficulty)`; one row per user (best score), with accuracy, max combo
  and a replay-lite (swing times) later for verification. Scores are submitted with the map's content hash
  so a re-seeded map never mixes boards.
- **Storage**: flat files first (`tracks/<fp>/<mapid>.pulse` + `index.tsv`), scores append-only in
  `leaderboards.tsv`; move to SQLite if search gets slow.
- **Anti-cheat**: scores are client-reported, so plausibility checks only (score <= max possible for the
  map, hit count <= notes, submission time >= duration); a replay check can come later.

## GitHub catalogue
A public repo (e.g. `pulse-maps`) holds `maps/<fp>/<mapid>.pulse` and `index.json`. A server-side job commits
newly accepted maps (batched, one commit per N uploads or per day), giving backup, a browsable catalogue,
history/diffs, and a path for outside PRs (checked by a validation Action, then ingested by the server).
Moderation = deleting a file; the server re-syncs. Scores and tokens stay off GitHub.

## Client (game)
- Library screen: local tracks (scan a songs folder, analyse once, cache the map) and online search.
- Selecting an online track with no local MP3 shows "needs the audio"; with a matching local file it downloads
  the map, aligns offset, and plays.
- After a run: submit score; show the board with the player's rank.
- Seed picker in the post-run / track-edit screen (the viewer's N/B keys); "Publish" uploads the chosen seed.
- Network: plain BSD sockets with resend; the server address comes from `server.txt`.

## Phases
1. Local library + map cache + fingerprint (works offline, desktop first).
2. Server: usernames, search, upload/download chunks, flat-file store; test with `pulse-map`/viewer as the
   uploader on a PC.
3. Score boards + submit; plausibility checks.
4. GitHub mirror job + validation Action.
5. Moderation tools, reports, replay verification.

## Risks
Fingerprint robustness across rips; copyright (mitigated: no audio hosted, but titles/artists are still
user text, so keep a report/takedown path); a public server needs rate limiting and abuse handling;
console has no TLS, so tokens travel in clear over UDP (acceptable for a hobby leaderboard, flagged for
anyone reusing a password-like secret: don't).
