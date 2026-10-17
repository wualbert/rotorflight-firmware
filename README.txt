Fireball hardware evidence for parameter-journal draft PR #539

PR: https://github.com/rotorflight/rotorflight-firmware/pull/539
This artifact-only branch preserves seven original bench recordings, reports,
and checksums. It is separate from the firmware feature branch and is not
intended to merge into upstream.

PR branch: blackbox-param-journal-pr
PR head: 430bc337de87a11eccf673d0bf7155c7a2df0663
Installed production code: 85d7049a9377af53b1ce9564248139d9ceb854c1

The ZIP contains two new 4.7 FULL logs, five earlier 4.6 stock/OFF/FULL logs,
verification reports and a SHA256SUMS manifest. Read the README inside for
precise scope and limitations. MCU backups and complete CLI dumps are excluded.

Archive: fireball-hardware-evidence-430bc337d-20261014.zip
SHA-256: 90e23e9fb9f04608ed4ad5b6380f455f16e77ad15c54e11dd01c5d49293a469a

ADDITIONAL EVIDENCE, 2026-10-16: 4.6 BACKPORT, BLADES-OFF SPOOL-UP BENCH
Firmware: 4.6.0-bbp 58a4bd5cc = 874f369d5 (branch
blackbox-param-journal-bench-20261013) with the 4.6.0 spektrum.c restored.
This is the 4.6 backport of the journal, not the PR head.
FULL mode, 368.5 s armed in three logs plus one header-only log: transmitter
Lua edits between arms, PID-profile changes from an adjustment switch, a
re-arm in the grace period. Read the README inside the archive.

Archive: fireball-bench-evidence-58a4bd5cc-20261016.zip
SHA-256: fbd2319bb6989003f5cecabe80746a40de43a33b4c996cf736189c9abd54dce9
