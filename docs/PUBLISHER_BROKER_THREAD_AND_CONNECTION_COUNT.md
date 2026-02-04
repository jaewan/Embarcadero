# Publisher / Broker: Network Threads and Connection Counts

This document summarizes how many network threads and TCP connections the **publisher** establishes, and how many **broker** threads are needed to serve them (and the subscriber).

---

## 1. Publisher side

### Threads (per publisher process)

| Thread / role | Count | Notes |
|---------------|--------|--------|
| **PublishThread** | `num_brokers × num_threads_per_broker` | Each thread holds one TCP connection to one broker. |
| **EpollAckThread** | 1 | Listens for broker→client ACKs (one listen socket; brokers connect to it). |
| **SubscribeToCluster (gRPC)** | 1 | Cluster status / broker discovery. |

So total publisher threads = **1 + 1 + (num_brokers × num_threads_per_broker)**.

### Connections opened by the publisher

| Direction | Count | Purpose |
|----------|--------|--------|
| Publisher → Broker (data) | **num_brokers × num_threads_per_broker** | One TCP connection per PublishThread to each broker’s data port (`PORT + broker_id`). |
| Broker → Publisher (ACK) | **num_brokers** | Each broker opens one TCP connection to the publisher’s ACK listener. |

**Per-broker view (one publisher):**

- **Inbound at broker:** `num_threads_per_broker` publish connections.
- **Outbound from broker:** 1 ACK connection to the publisher.

### Where the numbers come from

- **num_brokers:** From gRPC `SubscribeToCluster` (data brokers only; sequencer-only head is excluded). Typical: 4.
- **num_threads_per_broker:** Client config `client.publisher.threads_per_broker` (or CLI `-n`). Default in code: 4; often overridden to 3 (e.g. `config/client.yaml`) or 1 in scaling configs.

**Example (4 brokers, 3 threads/broker):**

- Publisher threads: 1 + 1 + 12 = **14**.
- Publisher→broker data connections: **12** (3 per broker).
- Broker→publisher ACK connections: **4** (1 per broker).

---

## 2. Subscriber side (for broker thread sizing)

One subscriber process opens:

- **Subscriber → Broker (data):** `data_brokers × sub_connections` = 4 × 3 = **12** connections total, i.e. **3 connections per broker**.

So each broker sees **3 subscribe** connections in addition to the publish connections.

---

## 3. Broker side: connections and threads

### Inbound connections per broker (1 publisher + 1 subscriber)

| Source | Connections per broker |
|--------|-------------------------|
| Publisher (data) | `num_threads_per_broker` |
| Subscriber (data) | `sub_connections` |
| **Total inbound** | **num_threads_per_broker + sub_connections** |

Example: 3 + 3 = **6** inbound connections per broker.

### Broker threads that serve these connections

With **use_nonblocking = false** (current default in `config/embarcadero.yaml`):

| Thread type | Count | Role |
|-------------|--------|--------|
| **MainThread** | 1 | Accept loop on `PORT + broker_id`; enqueues each accepted socket to `request_queue_`. |
| **ReqReceiveThread** | `num_reqReceive_threads` | Each thread pulls from `request_queue_`, does handshake, then either `HandlePublishRequest` or `HandleSubscribeRequest` and **stays with that connection** for its lifetime. |
| **AckThread** | **1 per publish connection** | Started from `HandlePublishRequest` (and non-blocking path); one thread per publisher data connection to this broker. |

So:

- **num_reqReceive_threads** = config **`network.io_threads`** (broker is started with `--network_threads` defaulting to `NUM_NETWORK_IO_THREADS` = that config value).
- Each **inbound connection** (publish or subscribe) is handled by **one** ReqReceiveThread for the whole connection lifetime.
- So you need at least **num_reqReceive_threads ≥ num_threads_per_broker + sub_connections** so every connection can be served without waiting in the queue.

**Recommended:**

```text
io_threads >= threads_per_broker + sub_connections
```

Example: 3 + 3 = 6 → **io_threads ≥ 6**. With **io_threads: 4** (current in `embarcadero.yaml`), 6 connections contend for 4 ReqReceive threads, so 2 connections wait in the queue until a thread frees up. This can delay connection setup and contribute to subscriber “9/12” or similar timeouts.

### Additional broker threads (when all connections are up)

- **AckThreads:** one per publish connection to this broker = **num_threads_per_broker** (e.g. 3).

So per broker, total threads (excluding other components) = **1 + io_threads + num_threads_per_broker** (e.g. 1 + 4 + 3 = 8 when all 6 connections are established and all 3 publish connections have started their AckThreads).

---

## 4. Config summary (typical 4-broker, 1 pub + 1 sub)

| Config | Location | Typical value | Purpose |
|--------|----------|----------------|---------|
| **threads_per_broker** | client (e.g. `client.yaml`, config client.publisher) | 3 or 4 | Publisher threads (and data connections) **per broker**. |
| **sub_connections** | network in broker config (e.g. `embarcadero.yaml`) | 3 | Subscriber connections **per broker**. |
| **io_threads** | network in broker config | 4 (current) | Broker **ReqReceiveThread** count. Should be **≥ threads_per_broker + sub_connections** (e.g. ≥ 6). |

**Recommendation:** Set **`network.io_threads`** to at least **`threads_per_broker + sub_connections`** (e.g. 6 for 3+3) so every inbound connection gets a dedicated ReqReceiveThread and connection setup is not delayed.

---

## 5. Quick formula

- **Publisher:**  
  - Data connections to brokers: **num_brokers × num_threads_per_broker**.  
  - Threads: **2 + num_brokers × num_threads_per_broker** (Ack + gRPC + PublishThreads).

- **Subscriber:**  
  - Data connections to brokers: **num_brokers × sub_connections**.

- **Per-broker inbound:**  
  - **threads_per_broker** (publish) + **sub_connections** (subscribe).

- **Broker ReqReceive threads needed:**  
  - **io_threads ≥ threads_per_broker + sub_connections** (e.g. **≥ 6** for 3+3).

- **Broker AckThreads:**  
  - **threads_per_broker** (one per publish connection to this broker).
