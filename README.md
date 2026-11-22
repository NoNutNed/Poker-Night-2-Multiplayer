# Poker Night 2 Multiplayer

Play a Poker Night 2 tournament with friends. In Standard mode, the host plays as The Player and up to four friends take cast seats. Four-Player Cast lets the host and up to three guests play as cast characters. AI fills open cast seats in both modes. Everyone stays at the table after elimination and sees the same final standings.

## Getting started

(Note: This entire github page was written by AI, including the play guide, and all of the mod's code.)

Use a Windows copy of Poker Night 2 and the same mod release on every machine. See the [play guide](PLAY_GUIDE.md) for installation, hosting, joining, and removal.

This is an early public test for groups on a trusted private network. It supports Hold ’em, shared Deal prompts, spectating, synchronized cast emotes, and post-match votes for Rematch or Return to Lobby. Treat the table as casual play. The game data synchronized between players includes hidden cards.

## Reporting bugs

Open an issue with the mod version, number of players, host or guest role, and steps to reproduce. For in-match bugs, include the affected hand or action and which copies saw it. Attach the host and affected guest logs if available, after removing personal paths or IP addresses. See the [play guide](PLAY_GUIDE.md) for the log location.

## Building

Install Visual Studio 2022 or Build Tools with **Desktop development with C++** and a Windows SDK, then run this in PowerShell from the repository root.

```powershell
& ./mod/build.ps1
```

The script generates the embedded Lua header and builds the x86 DLL in `mod/build/`. It prints the SHA-256 and leaves game installations untouched.

The native proxy and multiplayer transport live in `mod/src/`. The Lua modules in the same folder handle the lobby, game flow, presentation, and controls. Engine hooks depend on the executable layout checked by the proxy.

## Project status

The project is in public testing. See [GitHub Releases](https://github.com/NoNutNed/Poker-Night-2-Multiplayer/releases) for builds and changes. This is an independent fan project for Poker Night 2.

## License

The original mod code is available under the [MIT License](LICENSE). Poker Night 2 and its assets belong to their respective rights holders.
