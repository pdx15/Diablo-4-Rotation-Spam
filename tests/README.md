# Regression tests

## Macro behavior

Run from the repository root on Linux with GCC (C++20); no Windows SDK or game
is needed:

```sh
mkdir -p out
c++ -std=c++20 -Wall -Wextra -Wpedantic -Wno-missing-field-initializers \
    -pthread -Itests/win32_stubs tests/macro_regression.cpp -o out/macro_regression
./out/macro_regression
```

For an additional AddressSanitizer/UndefinedBehaviorSanitizer pass, add
`-g -fsanitize=address,undefined -fno-omit-frame-pointer` to the compile command.
Do not define `NDEBUG`: these tests use assertions.

The harness compiles the actual `macro.cpp` and `app_state.h`, replacing only
Win32 I/O with test doubles. It captures simulated `SendInput` calls, feeds
health pixels and mouse/key states, and stops the macro loop after a chosen
number of iterations. It never sends real input, uses only a temporary config
directory, and does not modify the user's configuration. Do not use
`tests/win32_stubs` in the application build.

Coverage:

- 384 combinations of game focus, script state, auto-heal enabled/disabled,
  independent mode, health state, combat condition, and held mouse buttons.
  Both heal and combat-key output are checked in every combination.
- Heal cooldown, live setting changes, script shutdown, and unchanged fast loot.
- Profile defaults, snapshot propagation, cloning, switching, deletion, and
  save/load round-trips with different per-profile values.
- Missing/invalid options in older key-value configs, and legacy-format migration.
- English/Russian labels, embedded language loading, and English fallback.

These tests do **not** validate native Win32/DX11 behavior or render the UI.

## Event timers and background service

```sh
mkdir -p out
c++ -std=c++20 -Wall -Wextra -Wpedantic -pthread \
    tests/event_schedule_test.cpp event_schedule.cpp -o out/event_schedule_test
./out/event_schedule_test
c++ -std=c++20 -Wall -Wextra -Wpedantic -Wno-unknown-pragmas -pthread \
    -Itests/win32_stubs tests/event_service_test.cpp event_service.cpp event_schedule.cpp \
    -o out/event_service_test
./out/event_service_test
```

The schedule tests exercise the real parser/calculator/cache with fixed Unix
seconds: structured/nested JSON, sorting, deduplication, stale/malformed/oversized
responses, boss/legion phase rollover across midnight, Helltide's 55/60-minute
boundaries, localized durations, predictions, seven-day expiry and atomic disk
cache replacement. No current timezone or live endpoint is needed.

The service tests compile the real background worker against test-only WinHTTP
stand-ins: HTTPS request path, lazy/idempotent start, nonblocking UI snapshots,
cancellable refresh wait, handle cleanup, cached restart after network/429/read/
JSON failures, missing cache, and unwritable cache. They do not contact the service.
Both binaries can also be built with the sanitizer flags above.

The macro suite additionally checks the Events binding, conflicts, old configs,
profile-independent persistence, key capture and actual hotkey-monitor toggling.

## Release preparation and localization

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

Requires Python 3.10+ and only the standard library. Tests cover accepted/rejected
versions (including Windows version limits and unsafe input), the six `version.h`
definitions, the existing tag/title/ZIP naming scheme, bilingual release notes,
and CLI output to `GITHUB_OUTPUT`. CLI tests use an isolated temporary copy and
do not change the real `version.h` or call GitHub.

Python tests also verify every `LocStrings` field has matching English/Russian
resources and a language-loader mapping, and that the new sources are registered
in both Visual Studio project files.

The manual **Release** workflow runs all suites before building and publishing.
Native MSBuild, executable resource checks, and ZIP packaging run on its Windows
job; the actual release is not created by these local tests.

## Windows smoke test

Build and run the application with Visual Studio, then verify:

1. With **Global Auto-Heal by HP pixel / Глобальный автохил по пикселю ХП** on,
   the **Independent operation / Независимая работа** checkbox is shown and is
   unchecked for a new or older configuration.
2. With low HP, the script ON, and Diablo IV in the foreground:

   | Combat condition | Mouse state | Independent OFF | Independent ON | Combat spam |
   | --- | --- | --- | --- | --- |
   | Always | No button held | Heal | Heal | Run |
   | Hold LMB | No button held | No heal | Heal | Stop |
   | Hold LMB | Only RMB held | No heal | Heal | Stop |
   | Hold LMB | LMB held | Heal | Heal | Run |
   | Hold RMB | No button held | No heal | Heal | Stop |
   | Hold RMB | Only LMB held | No heal | Heal | Stop |
   | Hold RMB | RMB held | Heal | Heal | Run |

3. Even with independent mode ON, healthy HP, the script OFF, auto-heal OFF,
   or switching to another application each prevents healing. The heal timer
   still limits repeated presses.
4. Enable the checkbox in one profile, leave it off in another, switch between
   them, and restart. Each profile must retain its own value in
   `%APPDATA%\d4rt\config.txt` (`profile.N.globalHealthIndependent=0/1`).

## Windows events smoke test

1. Confirm **Events: [F6] / Эвенты: [F6]** is to the right of Options. Press F6:
   a separate panel appears below the HUD; press again to hide it. Holding the
   key should not repeatedly toggle it. The panel must not intercept game clicks.
2. Rebind **Open Events / Открыть эвенты** to the right of Open Options. Verify
   the HUD label, opening/closing, persistence after restart and profile switches.
   Assigning an occupied global hotkey or F9 should show the localized conflict
   hint, not change the binding or exit the app.
3. Compare the displayed remaining time against `/api/schedule`: boss and legion
   count down to a start, Helltide to the end. At minute 55 Helltide shows Break;
   at the next hour it returns to a 55-minute countdown. No dates/timezones/seconds
   should appear. Changing only the OS timezone must not change the countdown.
4. Leave the panel open across a five-minute refresh. A slow/offline connection
   must not freeze the HUD or combat automation. Hide and reopen the panel;
   this must not create another background worker or flood the endpoint.
5. After a successful sync, disconnect and restart: cached timers keep counting
   down from `%APPDATA%\d4rt\events_cache.json`. After the supplied event list
   ends, predictions continue using the 210/25/60-minute cycles. Missing/corrupt/
   older-than-seven-day cache plus no network must show No data rows, not a
   fabricated live schedule. The panel has no sync status line; a failed cache
   write is reported with a localized message.
6. Check both OS UI languages, including the write-error strings.

## HUD layout smoke test

1. With the options window closed, clicks over and around the HUD panels must
   reach the game; panels stay invisible to the cursor.
2. Open Options (F5): the HUD and the events panel (if shown) can be dragged by
   any of their free space and stretched from the bottom-right corner grip, on
   any monitor. They cannot be collapsed and keep a sane minimum size.
3. Close Options: the panels keep the new layout and are again fully
   click-through while gaming.
4. Restart the app (also from a different working directory): the layout is
   restored from `%APPDATA%\d4rt\imgui.ini`; a legacy `imgui.ini` next to the
   executable is migrated on first run.
5. Delete `%APPDATA%\d4rt\imgui.ini`: panels return to the defaults (HUD at the
   top-left, events right below it).
