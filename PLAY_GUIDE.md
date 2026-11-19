# Poker Night 2 Co-op play guide

## Requirements

- A compatible Windows installation of Poker Night 2 for each player.
- The same co-op release on every machine.
- A trusted private LAN or private VPN. Keep the host ports off the public internet.

## Install or update

1. Close Poker Night 2. Open the game folder containing `PokerNight2.exe`.
2. If the folder already has a `dinput8.dll`, back it up somewhere outside the game folder before replacing it. Only one `dinput8.dll` can occupy that location.
3. Copy the release's `dinput8.dll` into the game folder and launch Poker Night 2 as usual. The main menu should have a **Multiplayer** entry.

The mod detects the game layout at launch. If Multiplayer is missing, check that the DLL is beside the executable and look for an unsupported-layout message in `coop_log.txt`. A fresh installation uses the in-game lobby defaults. If you previously tested development builds, set aside your old `coop_config.txt` before checking the release defaults.

## Host and join

1. The host chooses **Multiplayer → Host a lobby**, selects the expected number of guests, and creates the lobby.
2. Guests choose **Multiplayer → Find a lobby** and select the host. If discovery is unavailable, use **Enter host IP** with the host's private IPv4 address. The default game port is TCP 47103. LAN discovery uses UDP 47104.
3. Guests choose characters and everyone presses **Ready**. Unclaimed cast seats are controlled by AI.
4. Between hands, everyone presses **Deal**, including spectators. At the results screen, agree on **Rematch** or **Return to Lobby** and all select the same option.

If Windows Firewall prompts on the host, allow the game on your private network for the session. Use the same release and restart the lobby together if the connection ends mid-match. The protocol is intended for trusted groups and does not provide private-card secrecy or mid-match rejoin.

## Remove or roll back

Close the game, remove this release's `dinput8.dll`, and restore the previous DLL if you backed one up. For an earlier co-op release, replace the DLL with that version on every peer.

## Collect a bug report

Look for `coop_log.txt` beside the mod DLL. Copy the log before relaunching the game because startup creates a fresh log. Capture the host and affected guest if possible. Include the release version, number of players, connection type, and steps that led to the issue. Review logs for private addresses, account names, and personal paths before sharing.
