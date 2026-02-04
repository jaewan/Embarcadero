# Investigation: One Broker Not Ready (9/12 Subscriber Connections)

## Symptom

In E2E throughput runs (publisher + subscriber, ALL_INGESTION=1), the subscriber sometimes reports:

```text
Timeout waiting for connections. Got 9/12 after 31 seconds
```

So 12 connections were expected (4 brokers × 3 sub_connections) but only 9 were established—i.e. **one broker never accepted any of its 3 subscriber connections**.

## Root Cause

**The broker was signaling "ready" before its data port was actually listening.**

1. **Broker startup order**
   - `main()` creates `NetworkManager(broker_id, num_network_io_threads, is_sequencer_node)`.
   - The `NetworkManager` constructor starts `MainThread` and the request-handler threads, then **waits for `thread_count_ == 1 + num_reqReceive_threads`**.
   - `thread_count_` is incremented at the **very start** of `MainThread()` (in `network_manager.cc`), **before** `socket()`, `bind()`, or `listen()`.

2. **So when the constructor returns**
   - `MainThread` has **started** but may still be:
     - in `bind()` (which on failure retries up to 6 times with `sleep(5)` each),
     - or not yet at `listen()`.
   - So `listening_` is not necessarily `true` when the constructor returns.

3. **embarlet.cc then**
   - Connects the managers (SetCXLManager, etc.).
   - Immediately calls **`SignalScriptReady()`**, which writes `/tmp/embarlet_<pid>_ready`.

4. **The script**
   - Waits for the ready file, then starts the client (publisher + subscriber).
   - The subscriber’s `SubscribeToClusterStatus()` receives the cluster status and spawns `ManageBrokerConnections()` for each of the 4 brokers.
   - Each of those tries to open 3 TCP connections to that broker’s data port (`PORT + broker_id`).

5. **Race**
   - For one broker (often the **head**, broker 0), `listen()` had not yet been called when the client tried to connect.
   - So that broker’s 3 subscriber connections get **connection refused** (or timeout).
   - `ManageBrokerConnections()` then logs "No successful connections established to broker X" and returns without adding any connection for that broker → **9/12**.

The head broker is the most likely to be late because it does more one-time setup (heartbeat server, etc.) and is started first; if `bind()` fails once (e.g. port still in use), it spends 5 seconds in `sleep(5)` before retrying.

## Fix (Implemented)

**Wait for the data port to be listening before signaling ready.**

In `src/embarlet/embarlet.cc`, after connecting the managers and **before** `SignalScriptReady()`:

- If this broker is a **data broker** (`!is_sequencer_node`), wait up to 30 seconds for `network_manager.IsListening()` to become true (poll every 100 ms).
- Only then call `SignalScriptReady()`.

So the script only sees "broker ready" when the TCP listener is actually accepting connections. Sequencer-only nodes skip networking, so `IsListening()` stays false; we do not wait in that case (they don’t accept data connections anyway).

## Additional Recommendations

1. **Broker `io_threads`**  
   Set `network.io_threads` ≥ `threads_per_broker + sub_connections` (e.g. ≥ 6 for 3+3) so every inbound connection gets a `ReqReceiveThread` immediately. See `docs/PUBLISHER_BROKER_THREAD_AND_CONNECTION_COUNT.md`.

2. **Subscriber connection retry (optional)**  
   In `ManageBrokerConnections()`, if no connections succeed, retry once after a short delay (e.g. 2 s) to tolerate any remaining timing quirks. Optionally loop `epoll_wait` until all sockets are connected or the overall timeout, so edge-triggered epoll doesn’t miss late connections.
