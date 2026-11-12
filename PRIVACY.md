# Privacy

Ombra records to your own disk. Clips and screenshots never leave your PC, and there is no account.
This page lists everything that can leave it, and when.

## You choose the level

During setup, and later in Settings > Advanced > Diagnostics, you pick one of three levels. Nothing
is sent before setup is finished, and a lower level takes effect at once: nothing waiting is sent at
a level you've left, and a crash or error that happened while sharing was Off is never sent later.

| Level | What is sent |
| --- | --- |
| **Off** | Nothing. |
| **Error reports** | Crash reports, error reports and a daily crash-free count, described below. No ID. |
| **+ Usage** | The same, plus daily performance numbers and settings, and the games you recorded. With a random ID for this install. |

Reports go to [Sentry](https://sentry.io) (EU data region), which Ombra's developer uses to read
them. Sentry sees the IP address a report comes from and may derive an approximate location
(country, city) from it; Ombra sends no location itself.

Builds you compile yourself have no Sentry key and never send anything.

## Removed before sending

Every text Ombra sends is first cleaned: folder paths become `<path>\` plus the file name, your
Windows user name becomes `<user>`, the PC's name becomes `<computer>`, and window titles become
`«redacted»`. Program names stay, such as a game's `.exe` name in a log line: they're needed to
understand what went wrong.

## Error reports

- **Crash reports**, when Ombra or its overlay crashes: the error, Ombra's version, your Windows
  build, your GPUs and their drivers, the settings logged at the last start, and up to 300 log lines
  before the crash. Also the *minidump*, a snapshot of the crashed process's memory. It can't be
  cleaned like the text, so it may hold a path or a window title. At most 5 crash reports a day.
- **Error reports**, when something fails without crashing (a save, the encoder, capture, audio, an
  update, or Ombra ending without a clean exit): what failed as a fixed code, the GPU vendor
  involved, your Windows build and GPUs, and the log lines before it. At most 3 of the same error
  and 25 in all per day.
- **A daily count**: one line per day and version saying whether Ombra crashed, ended uncleanly,
  hit an error or ran fine, so the developer can see how many installs run crash-free.

A crash before setup is finished isn't sent. The next time you open the overlay, Ombra asks whether
to send it; nothing goes unless you press Send, and then it goes whatever level you're at.

## + Usage

Everything above, each with a random install ID (made on your PC, tied to nothing else), plus:

- **One summary a day**: how long Ombra ran and recorded, frame and encode timings, how many saves,
  rebuilds and dropped audio frames, peak memory, how many monitors and audio sources, and your
  settings. Settings you type or pick yourself (the file name pattern, hotkeys) are sent only as
  "changed or not"; folders, devices, apps, windows, monitors and your Games list are never sent.
- **The games you recorded**, up to 20 a day: the game's exe name (never its path), how long, and
  how it was recorded (encoder, codec, resolution, frame rate, number of saves).

## Sending logs yourself

Settings > Advanced, under Logs for a bug report, has **Send to developer**. It sends a zip of the last three
days' logs and performance files, Ombra's crash files (minidumps included) and your whole settings
file, cleaned as above apart from the minidumps. Unlike the usage summary, the settings file names
your audio devices, the apps and window you record and your Games list. It works at any level, since pressing it is the consent, and
gives you a reference to paste into a GitHub issue. At most 5 a day. **Save zip** makes the same
zip without sending it, so you can look inside first.

## What was sent

Before anything is sent, Ombra writes a copy to the `sent_reports` folder in its data folder
(`%LOCALAPPDATA%\Ombra`) and keeps the newest 20. Settings > Advanced > Diagnostics lists them with
the result of each, and opens any of them.

## Updates

Twice a day Ombra asks GitHub whether a new version is out. Nothing about you or your PC goes with
it beyond what any web request carries (your IP address). Settings > Advanced turns it off.

## Local files

Ombra keeps logs, performance samples (Settings > Advanced turns these off) and crash files on your
PC, in `%LOCALAPPDATA%\Ombra`. They're never sent on their own; only the reports above and a zip you
send yourself leave the PC.
