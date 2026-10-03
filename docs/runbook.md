# Runbook

Audience: whoever is on call for `futu_trader`. Alerts link to the anchors below. The system trades
HK equities through Futu OpenD. Read **Honest limits** once.

## Honest limits

* Everything here was tested against the in-process mock OpenD and a simulator, **never against a real
  OpenD**. The first real session is an experiment: do it in SIMULATE, watch it, expect surprises.
* OpenD is a gateway: tens of ms to the venue, about 15 order writes per 30 s per account.
* A halt cancels resting orders; it does **not** flatten positions. Flattening is a human decision.
* The OpenD link is plaintext (loopback only is enforced). The promotion log is a plain file: it
  guards mistakes and stale state, not an attacker who can write it.
* Startup refuses on anything it does not understand. A refusal is the system working.

## Layout on a host

| Path | What |
|---|---|
| `/opt/futu_trader/bin/futu_trader` | the binary |
| `/etc/futu_trader/trader.yaml` | config (copy from `config/paper.yaml`); read-only to the service |
| `/etc/futu_trader/trade_pwd.md5` | REAL only: MD5 of the trade password, `chmod 600`, owner `futu` |
| `/var/lib/futu_trader/` | state dir, mode 0700: `orders.wal`, `kill_switch.tripped`, `HALT`, `promotion.log` |
| `ops/systemd/futu-trader.service` | hardened unit (`systemd-analyze security`: 1.5 OK) |
| `ops/prometheus/`, `ops/grafana/` | scrape config, alert rules, dashboard |

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

## Starting (SIMULATE)

1. Start OpenD, log in, make sure it listens on the host/port in the config (`scripts/setup_opend.sh`).
2. Put the SIMULATE account id (from OpenD) in `account.id`. The process refuses a SIMULATE config
   pointed at a REAL account and vice versa.
3. `futu_trader --config /etc/futu_trader/trader.yaml` (or `systemctl start futu-trader`).
4. Check: log says `trading started`, `/readyz` is 200, dashboard `futu_trader` is green.

Startup order (each step refuses rather than guesses): state dir private → kill switch clear →
write-ahead log intact (a torn tail is repaired, a corrupt log refused) → OpenD answers → account
exists with the requested environment and HK authorisation → (REAL) full live gate → intents from the
previous run restored → bootstrap positions and live orders from the broker → first reconciliation
clean → metrics → engine.

## Stopping

`SIGTERM` / `systemctl stop`: quote handling stops first, then resting orders are cancelled (without
tripping the kill switch), the log is flushed. Orders of unknown outcome cannot be cancelled and are
reported in the log line `... of unknown outcome`: look at them in the broker app.

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
closed). Remove the file and reset the switch before restarting.

## Halt pending
*(alert `FutuHaltPending`)* A cancel-all was scheduled and has not finished for a minute: the broker
refused or never answered cancels. **Cancel by hand in the broker app now.** Then see the log for
`cancel` errors and treat as OpenD down if the link is bad.

## OpenD down
*(alert `FutuOpenDDisconnected`)* The link dropped; trading was halted (kill switch tripped) so a
reconnect cannot silently resume. Restart OpenD (it may need a fresh login / 2FA), confirm with the
broker app what is open, then follow *Kill switch tripped*.

## Process down
*(alert `FutuTraderDown`)* The process crashed or hung. **Resting orders may still be live.** Check
the broker app first. Then `journalctl -u futu-trader`. On restart the write-ahead log restores every
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

A SIMULATE session that stops cleanly after at least `live.min_session_minutes` (default 240) records
`YYYY-MM-DD clean` in `live.promotion_log`; a halt, reconciliation problem, OMS anomaly, unresolved
order or foreign push records `dirty`; a shorter clean session records nothing. One line per day, and
dirty is sticky for that day. REAL needs **5 recent consecutive clean days**, no gap over 7 days, newest
at most 5 days old.

## Going live
*(only after a human has read `tooling/skills/trading-risk-checklist` and had a second person review)*

1. Five clean SIMULATE days against a real OpenD, reviewed (not just counted).
2. `config/live.example.yaml` copied, limits deliberately small, `account.id` = the REAL account,
   `live.ack_phrase` = `I-ACCEPT-REAL-MONEY-<last 4 digits of the account id>`.
3. Secret file: `chmod 600`, owned by `futu`. The process refuses a file others can read.
4. Drop-in (`systemctl edit futu-trader`) adding `--live` to `ExecStart` and
   `Environment=FUTU_LIVE_TRADING=I_UNDERSTAND_REAL_MONEY`. Never in the base unit or the repo.
5. Start. The gate checks config phrase, environment variable, `--live`, promotion record, trade unlock,
   the account being a REAL account at the broker, then a **read-only** startup reconciliation of the
   real account, and only then issues the approval. Any failure exits 3 without placing an order.
6. Stay at the screen for the first session. Keep the broker app open.

## Releasing

`deploy-prod.yml` builds a commit SHA (never a branch, never `:latest`), runs the full sanitizer test
suite, pushes the image, probes it, produces an SBOM and signs the digest with cosign (keyless). It
starts nothing on a trading host. Requires a protected `production` environment with a reviewer and
Code Owners review on main (repository settings, not enforced by the files). No vulnerability scanner
is wired in: the one I would have used I could not verify the provenance of. Add one you trust.

## Known gaps in operations

* No self-hosted paper-trading job against a real OpenD (needs a logged-in OpenD).
* No log redaction layer: secrets never reach the logger (`Secret` is redacted, errors never contain
  secret values, tested), but there is no generic scrubber.
* Alertmanager routing, paging and dashboards' data source are site-specific and not provided.
* `docker-compose.yml`, the CI release job and the image digests were not run in this environment.
