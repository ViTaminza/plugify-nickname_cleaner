# nickname_cleaner (Plugify)

Nickname cleaner, ported from the Metamod plugin nickname_cleaner (ABKAM) and rewritten on pure s2sdk (no pisexcompat). Reads <configs>/nickname_cleaner/settings.ini (DefaultName) plus whitelist.txt/blocklist.txt (with a <configs>/ fallback) and rewrites the name of every human player whose nickname contains a blocked word or an advertisement (blocklist entries '*.xx' match the end of a token, other entries a substring; the whitelist wins; http://, https:// and www. are always dropped) on round_prestart, one second after player_spawn and on client authorization. A name whose tokens are all filtered becomes DefaultName. No exported methods.

Plugify port (fork) of **nickname_cleaner** by ABKAM (Metamod (C++)).
Original: https://github.com/ABKAM2023/cs2-nickname_cleaner
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `nickname_cleaner-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/nickname_cleaner`.

## License
GPL-3.0, see LICENSE.

