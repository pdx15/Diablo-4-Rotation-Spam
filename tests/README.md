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

## Release preparation

```sh
python3 -m unittest discover -s tests -p 'test_release.py' -v
```

Requires Python 3.10+ and only the standard library. Tests cover accepted/rejected
versions (including Windows version limits and unsafe input), the six `version.h`
definitions, the existing tag/title/ZIP naming scheme, bilingual release notes,
and CLI output to `GITHUB_OUTPUT`. CLI tests use an isolated temporary copy and
do not change the real `version.h` or call GitHub.

The manual **Release** workflow runs both suites before building and publishing.
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
