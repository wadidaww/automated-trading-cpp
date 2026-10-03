# Runbook

Audience: whoever is on call for `futu_trader`. Alerts link to the anchors below. The system trades
HK equities through Futu OpenD. Read **Honest limits** once.

## Honest limits

* Everything here was tested against the in-process mock OpenD and a simulator, **never against a real
  OpenD**. The first real session is an experiment: do it in SIMULATE, watch it, expect surprises.
* OpenD is a gateway: tens of ms to the venue, about 15 order writes per 30 s per account.
* A halt cancels resting orders; it does **not** flatten positions. Flattening is a human decision.
* A halt **exits the process** (6, or 7 if cancels failed): there is no live "halted" state to watch.
  The alerts therefore stay latched for 30 minutes (`keep_firing_for`) and `FutuTraderDown` is the one
  that also fires. Read the exit status and the log, not just the alert.
* The OpenD port (11111) is plaintext on loopback. While OpenD is down any local user could bind it and
  impersonate it (the trade-password hash would be sent to them). Keep OpenD running, keep the host
  single-purpose, and never leave 11111 unbound while the trader can start.
* The OpenD link is plaintext (loopback only is enforced). The promotion log is a plain file: it
  guards mistakes and stale state, not an attacker who can write it.
* Startup refuses on anything it does not understand. A refusal is the system working.

## Layout on a host

| Path | What |
|---|---|
| `/opt/futu_trader/bin/futu_trader` | the binary |
| `/etc/futu_trader/trader.yaml` | config (copy from `config/paper.yaml`); read-only to the service |
| `/etc/futu_trader/trade_pwd.md5` | REAL only: MD5 of the trade password, `chmod 600`, owner `futu` |
| `/var/lib/futu_trader/` | state dir, mode 0700 (the unit's `StateDirectory=` creates it): `orders.wal` (locked: **one instance per state dir**, a second one refuses to start), `kill_switch.tripped`, `HALT`, `promotion.log`, `audit.log` |
| `ops/systemd/futu-trader.service` | hardened unit (`systemd-analyze security`: 1.5 OK) |
| `ops/prometheus/`, `ops/grafana/` | scrape config, alert rules, dashboard |

`state.dir` and `live.promotion_log` must be absolute paths under systemd/Docker (the working directory is
`/`, or a read-only `/app`); the promotion log must sit directly inside `state.dir`. The config file must
not be writable by group/others.

Metrics, `GET /metrics`, `/healthz`, `/readyz`, are on `127.0.0.1:<metrics.port>` only.
`/readyz` is 200 only when OpenD is linked, the kill switch is clear, no halt is pending and the
engine and write-ahead log are healthy. Container health check: `futu_trader --probe 9464`.

## Exit codes

| Code | Meaning | Restarted by systemd? |
|---|---|---|
| 0 | stopped on request; resting orders cancelled | no (stopped) |
| 2 | bad command line or config | **no** |
| 3 | a safety condition refused startup (kill switch, live gate, corrupt WAL, wrong account) | **no** |
| 4 | OpenD unreachable or a startup broker call failed | yes, every 10 s (5 per 5 min) |
| 6 | trading halted while running; resting orders cancelled | **no** |
| 7 | stopped or halted but cancels did NOT all succeed within 20 s: **orders may be resting** | **no** |
| killed by a signal / crash | no exit code (SIGKILL, OOM, abort) | **yes, after 10 s**: see *Process down*; for REAL set `Restart=no` in the drop-in |

## Starting (SIMULATE)

0. The system clock must be synced (`timedatectl`): the process refuses to start with a clock before
   2025, and "today" decides which logged order intents are restored after a restart.
1. Start OpenD, log in, make sure it listens on the host/port in the config (`scripts/setup_opend.sh`).
2. Put the SIMULATE account id (from OpenD) in `account.id`. The process refuses a SIMULATE config
   pointed at a REAL account and vice versa.
3. `futu_trader --config /etc/futu_trader/trader.yaml` (or `systemctl start futu-trader`).
4. Check: log says `trading started`, `/readyz` is 200, dashboard `futu_trader` is green.

Startup order (each step refuses rather than guesses): clock sane → state dir private → no `HALT`
file → kill switch clear →
write-ahead log intact (a torn tail is repaired, a corrupt log refused) → OpenD answers → account
exists with the requested environment and HK authorisation → (REAL) full live gate → intents from the
previous run restored → bootstrap positions and live orders from the broker → first reconciliation
clean → metrics → engine.

## Stopping

`SIGTERM` / `systemctl stop`: quote handling stops first (quotes already queued are still handled, and
may still place an order), then **our** resting orders are cancelled without tripping the kill switch (a
human's manual orders are left alone). Cancels are retried, with a reconciliation between attempts to
learn the broker id of orders of unknown outcome, for up to 20 s; if any still fail the process says
`WARNING: orders may still be resting` and exits **7**. `TimeoutStopSec=90` / `stop_grace_period: 90s`
leave room for that. Look at the broker app after any stop that was not clean.

## Kill switch tripped
*(alert `FutuKillSwitchTripped`; exit code 3 on the next start)*

The switch trips on: daily loss limit, reconciliation drift (position/cash/order mismatch), a busted
fill, a late-appearing order we had declared dead, the OpenD link dropping, a strategy or engine
exception, a write-ahead-log failure, the `HALT` file appearing in the state dir. It is persisted: a
restart does not clear it.

1. Read the reason: the log line `trading halted: <reason>` or `refused: the kill switch is tripped (<reason>)`.
2. In the **broker app**, look at open orders and positions. Cancel stragglers, decide about positions.
3. Work out *why*. Do not reset to "see if it works".
4. Only then: `futu_trader --config /etc/futu_trader/trader.yaml --reset-kill-switch --operator <your name>`
   (the name is kept for the audit trail), then start normally.

To halt from outside the process: `touch /var/lib/futu_trader/HALT` (checked every 500 ms; fails
closed). Startup **refuses** while the file exists: remove it and reset the switch before restarting.

Run the reset as the service user (`sudo -u futu futu_trader ... --reset-kill-switch --operator <name>`);
it is appended to `<state dir>/audit.log`. If the log says the trip "could NOT be persisted" (metric
`futu_kill_switch_persist_failed`, alert `FutuKillSwitchNotPersisted`) a restart would not be blocked:
fix the disk first.

## Halt pending
*(alert `FutuHaltPending`, exit code 7)* A cancel-all did not complete: the broker refused or never
answered cancels (the log shows `N failed` per attempt and `WARNING: orders may still be resting`).
**Cancel by hand in the broker app now.** Then treat as OpenD down if the link was bad.

## OpenD down
*(alert `FutuOpenDDisconnected`)* The link dropped; trading was halted (kill switch tripped) and the
process exits (6, or 7 if the cancels could not get through because the link was down). Restart OpenD
(it may need a fresh login / 2FA), confirm with the broker app what is open, then follow *Kill switch
tripped*. A flapping link therefore ends the trading day until a human resets: that is deliberate.

## Process down
*(alert `FutuTraderDown`)* The process crashed or hung. **Resting orders may still be live.** Check
the broker app first. Then `journalctl -u futu-trader` and `systemctl status futu-trader` for the exit status (6/7 = halted,
a signal = crashed). After a crash systemd restarts the service after 10 s unless the drop-in says
`Restart=no` (**do that for REAL**). Orders that were resting before a crash are adopted again at
bootstrap and left resting; the first reconciliation must be clean for trading to start. On restart the write-ahead log restores every
order intent from today as "outcome unknown": each is matched to the broker's orders by its ClOrdId
(carried in the order remark) or, after a grace period and three clean listings, declared dead. Its key
stays blocked either way, so a crash cannot produce a duplicate order. Expect `restored N order
intent(s)` in the log and `futu_oms_unresolved_orders` to fall to 0 within about a minute.

If the log refuses to open (`WAL ... is corrupt`): do **not** delete it. Copy it aside for the
post-mortem, reconcile by hand against the broker, then move it away deliberately.

## Reconciliation problems
*(alerts `FutuReconcileProblems`, `FutuReconcileStalled`)* Our book and the broker's disagree, or the
broker did not answer. Three consecutive failures trip the kill switch. Compare positions in the broker
app with `futu_oms_*` and the journal. A position the broker holds and we do not know about means a
fill was missed: it is applied on the next reconciliation; if it keeps differing, stop and investigate.

## Other alerts

| Alert | First look |
|---|---|
| `FutuUnresolvedOrders` | a submit timed out or a restart restored an intent; wait one reconcile cycle, then check the broker |
| `FutuQuotesDropped`, `FutuQuotesStale` | engine thread stalled, usually a slow broker call; check `futu_engine_handle_seconds` |
| `FutuRateLimitHigh` | strategy is too chatty for OpenD's order limit |
| `FutuForeignPush` | pushes for another account/environment: possible SIMULATE/REAL mix-up or a second client on OpenD |
| `FutuWalSyncSlow`, `FutuWalFailed`, `FutuIntentNotDurable` | disk full, slow or read-only; orders are refused until fixed |
| `FutuNoQuotesDuringHkHours` | subscription lost or symbol suspended; false positive on exchange holidays |

## Promotion (SIMULATE evidence)

A SIMULATE session that stops cleanly after at least `live.min_session_minutes` (default 240, minimum
60) **and processed quotes** records `YYYY-MM-DD clean` in `live.promotion_log`; a halt, failed cancel,
reconciliation problem, OMS anomaly, unresolved order or foreign push records `dirty`; a shorter or idle
session records nothing. A crash records nothing, so a crashed day does not break the streak by itself:
review the days, do not just count them. The evidence is not bound to the strategy or limits used. One line per day, and
dirty is sticky for that day. REAL needs **5 recent consecutive clean days**, no gap over 7 days, newest
at most 5 days old.

## Going live
*(only after a human has read `tooling/skills/trading-risk-checklist` and had a second person review)*

1. Five clean SIMULATE days against a real OpenD, reviewed (not just counted), with the **same strategy and
   comparable limits** you will run live (nothing enforces that).
2. `config/live.example.yaml` copied, limits deliberately small, `account.id` = the REAL account,
   `live.ack_phrase` = `I-ACCEPT-REAL-MONEY-<last 4 digits of the account id>`.
3. Secret file: `chmod 600`, owned by `futu`. The process refuses a file others can read.
   Set `engine.cash_tolerance_hkd` (mandatory for REAL: broker cash must match our accounting within it).
   The account must have **no live orders we did not place**: startup refuses to adopt a stranger's order.
   Fire a **test page** through Alertmanager and confirm the Watchdog reaches the dead-man's-switch.
4. Drop-in (`systemctl edit futu-trader`) adding `--live` to `ExecStart` and
   `Environment=FUTU_LIVE_TRADING=I_UNDERSTAND_REAL_MONEY`. Never in the base unit or the repo.
5. Start. The gate checks config phrase, environment variable, `--live`, promotion record and the
   account being a REAL account at the broker **before** it unlocks trading (so a refusal never leaves
   the account unlocked); then unlocks, runs a **read-only** startup reconciliation of the real account
   and only then issues the approval. Any failure exits 3 without placing an order. The account is
   locked again on every way out of the process. The "clean" check proves the broker lists parse and
   no status is unknown, that cash agrees within tolerance and that no stranger's order rests; it does
   not independently verify positions (they are seeded from the broker).
6. Stay at the screen for the first session. Keep the broker app open.

## Releasing

`deploy-prod.yml` builds a commit SHA (never a branch, never `:latest`), runs the full sanitizer test
suite, pushes the image, probes it, produces an SBOM and signs the digest with cosign (keyless). It
starts nothing on a trading host. Requires a protected `production` environment with a reviewer and
Code Owners review on main (repository settings, not enforced by the files). No vulnerability scanner
is wired in: the one I would have used I could not verify the provenance of. Add one you trust.

## Known gaps in operations

* No self-hosted paper-trading job against a real OpenD (needs a logged-in OpenD).
* No metrics for positions, PnL, cash or exposure; no alert on "loss approaching the daily limit".
* No tool to dump the binary WAL journal; incident reconstruction is by log lines, the broker's own
  history and the (opaque) `orders.wal`. The WAL header carries no build or config hash.
* The WAL and the in-memory order records are never rotated or pruned, and the process locks its
  memory (`mlockall`) with no `MemoryMax`: restart daily.
* Pushes that arrive between subscribing and bootstrap can make the startup fail (fail-closed, exit 4):
  just start again.
* A SIGTERM still handles quotes already queued before cancelling, which can place an order.
* No log redaction layer: secrets never reach the logger (`Secret` is redacted, errors never contain
  secret values, tested), but there is no generic scrubber.
* Alertmanager routing, paging and dashboards' data source are site-specific and not provided.
* `docker-compose.yml`, the CI release job and the image digests were not run in this environment.
