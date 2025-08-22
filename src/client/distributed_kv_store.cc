// NOTE: This only works when num of brokers == NUM_MAX_BROKERS. 
// TODO(Jae) modify this later
#include "distributed_kv_store.h"

#include <sstream>

// OperationId implementation
bool OperationId::operator==(const OperationId& other) const {
	return clientId == other.clientId && requestId == other.requestId;
}

// OperationId hash implementation
size_t std::hash<OperationId>::operator()(const OperationId& id) const {
	return hash<uint64_t>()(id.clientId) ^ hash<uint64_t>()(id.requestId);
}

// Serialize log entry to a byte array
std::vector<char> LogEntry::serialize() const {
	std::ostringstream oss;

	// Write operation ID, Use message header's client_id and order instead
	//oss.write(reinterpret_cast<const char*>(&opId.clientId), sizeof(opId.clientId));
	//oss.write(reinterpret_cast<const char*>(&opId.requestId), sizeof(opId.requestId));

	// Write operation type
	uint8_t opTypeValue = static_cast<uint8_t>(type);
	oss.write(reinterpret_cast<const char*>(&opTypeValue), sizeof(opTypeValue));

	// Write transaction ID
	oss.write(reinterpret_cast<const char*>(&transactionId), sizeof(transactionId));

	// Write KV pairs count
	uint32_t pairCount = static_cast<uint32_t>(kvPairs.size());
	oss.write(reinterpret_cast<const char*>(&pairCount), sizeof(pairCount));

	// Write each KV pair
	for (const auto& kv : kvPairs) {
		// Write key length and key
		uint32_t keyLength = static_cast<uint32_t>(kv.key.length());
		oss.write(reinterpret_cast<const char*>(&keyLength), sizeof(keyLength));
		oss.write(kv.key.c_str(), keyLength);

		// Write value length and value
		uint32_t valueLength = static_cast<uint32_t>(kv.value.length());
		oss.write(reinterpret_cast<const char*>(&valueLength), sizeof(valueLength));
		oss.write(kv.value.c_str(), valueLength);
	}

	// Convert to vector<char>
	std::string serialized = oss.str();
	return std::vector<char>(serialized.begin(), serialized.end());
}

// Deserialize from a byte array
LogEntry LogEntry::deserialize(const void* data, size_t client_id, size_t client_order) {  // size is intentionally unused
	LogEntry entry;
	const char* buffer = static_cast<const char*>(data);
	size_t offset = 0;

	entry.opId.clientId = client_id;
	entry.opId.requestId =client_order;
	// Read operation ID
	//memcpy(&entry.opId.clientId, buffer + offset, sizeof(entry.opId.clientId));
	//offset += sizeof(entry.opId.clientId);
	//memcpy(&entry.opId.requestId, buffer + offset, sizeof(entry.opId.requestId));
	//offset += sizeof(entry.opId.requestId);

	// Read operation type
	uint8_t opTypeValue;
	memcpy(&opTypeValue, buffer + offset, sizeof(opTypeValue));
	entry.type = static_cast<OpType>(opTypeValue);
	offset += sizeof(opTypeValue);

	// Read transaction ID
	memcpy(&entry.transactionId, buffer + offset, sizeof(entry.transactionId));
	offset += sizeof(entry.transactionId);

	// Read KV pairs count
	uint32_t pairCount;
	memcpy(&pairCount, buffer + offset, sizeof(pairCount));
	offset += sizeof(pairCount);

	// Read each KV pair
	for (uint32_t i = 0; i < pairCount; ++i) {
		KeyValue kv;

		// Read key length and key
		uint32_t keyLength;
		memcpy(&keyLength, buffer + offset, sizeof(keyLength));
		offset += sizeof(keyLength);
		kv.key.assign(buffer + offset, keyLength);
		offset += keyLength;

		// Read value length and value
		uint32_t valueLength;
		memcpy(&valueLength, buffer + offset, sizeof(valueLength));
		offset += sizeof(valueLength);
		kv.value.assign(buffer + offset, valueLength);
		offset += valueLength;

		entry.kvPairs.push_back(kv);
	}

	return entry;
}

// DistributedKVStore implementation
DistributedKVStore::DistributedKVStore(SequencerType seq_type)
	: last_request_id_(0),
	last_applied_total_order_(0),
	last_transaction_id_(0),
	running_(true) {

		char topic[TOPIC_NAME_SIZE];
		memset(topic, 0, TOPIC_NAME_SIZE);
		memcpy(topic, "KVStoreTopic", 12);

		// Setup Embarcadero
		stub_ = HeartBeat::NewStub(
				grpc::CreateChannel("127.0.0.1:" + std::to_string(BROKER_PORT), 
					grpc::InsecureChannelCredentials()));
		int ack_level = 0;
		int num_threads = 3;
		int order = 0;
		if(SequencerType::EMBARCADERO == seq_type){
			order = 4;
		} else if(SequencerType::SCALOG == seq_type){
			order = 1;
		} else if(SequencerType::CORFU == seq_type){
			order = 4;
		}

		CreateNewTopic(stub_, topic, order, seq_type, 1/*replication_factor*/, false, ack_level);

		subscriber_ = std::make_unique<Subscriber>("127.0.0.1", std::to_string(BROKER_PORT), topic);
		publisher_ = std::make_unique<Publisher>(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
				num_threads, 1024, (1UL<<33), order, seq_type);
		publisher_->Init(ack_level);
		server_id_ = publisher_->GetClientId();

		subscriber_->WaitUntilAllConnected(); // Asuume there exists NUM_MAX_BROKERS
		log_consumer_threads_.emplace_back(&DistributedKVStore::logConsumer, this);
	}

DistributedKVStore::~DistributedKVStore() {
	publisher_->~Publisher();
	subscriber_->~Subscriber();

	// Terminate Embarcadero Cluster
	google::protobuf::Empty request, response;
	grpc::ClientContext context;
	stub_->TerminateCluster(&context, request, &response);
	VLOG(3) <<"DistributedKVStore Destructing";

	running_ = false;

	for (auto &t: log_consumer_threads_){
		if (t.joinable()) {
			t.join();
		}
	}
	VLOG(3) <<"DistributedKVStore Destructed";
}

void DistributedKVStore::processLogEntryFromRawBuffer(const void* data, size_t size,
		uint32_t client_id, size_t client_order,
		size_t total_order) {
	if (!data || size == 0) {
		LOG(ERROR) << "Invalid raw buffer data for processing";
		return;
	}

	const char* buffer = static_cast<const char*>(data);
	size_t offset = 0;

	// Create an OperationId from the message header information
	OperationId opId{client_id, client_order};

	// Read operation type
	uint8_t opTypeValue;
	if (offset + sizeof(opTypeValue) > size) return;
	memcpy(&opTypeValue, buffer + offset, sizeof(opTypeValue));
	OpType type = static_cast<OpType>(opTypeValue);
	offset += sizeof(opTypeValue);

	// Read transaction ID
	uint64_t transactionId;
	if (offset + sizeof(transactionId) > size) return;
	memcpy(&transactionId, buffer + offset, sizeof(transactionId));
	offset += sizeof(transactionId);

	// Read KV pairs count
	uint32_t pairCount;
	if (offset + sizeof(pairCount) > size) return;
	memcpy(&pairCount, buffer + offset, sizeof(pairCount));
	offset += sizeof(pairCount);

	// Process based on operation type
	switch (type) {
		case OpType::PUT: 
			{
				// Process a single PUT operation
				if (pairCount != 1) {
					LOG(ERROR) << "Expected 1 KV pair for PUT, got " << pairCount;
					return;
				}

				// Read the key
				uint32_t keyLength;
				if (offset + sizeof(keyLength) > size) return;
				memcpy(&keyLength, buffer + offset, sizeof(keyLength));
				offset += sizeof(keyLength);

				if (offset + keyLength > size) return;
				std::string key(buffer + offset, keyLength);
				offset += keyLength;

				// Read the value
				uint32_t valueLength;
				if (offset + sizeof(valueLength) > size) return;
				memcpy(&valueLength, buffer + offset, sizeof(valueLength));
				offset += sizeof(valueLength);

				if (offset + valueLength > size) return;
				std::string value(buffer + offset, valueLength);
				offset += valueLength;

				// Apply the operation directly to the ShardedKVStore (no mutex needed!)
				kv_store_.put(key, value);

				// If this is our own request, complete the pending operation
				completeOperation(client_order);
				break;
			}

		case OpType::DELETE: 
			{
				// Process a single DELETE operation
				if (pairCount != 1) {
					LOG(ERROR) << "Expected 1 KV pair for DELETE, got " << pairCount;
					return;
				}

				// Read the key
				uint32_t keyLength;
				if (offset + sizeof(keyLength) > size) return;
				memcpy(&keyLength, buffer + offset, sizeof(keyLength));
				offset += sizeof(keyLength);

				if (offset + keyLength > size) return;
				std::string key(buffer + offset, keyLength);
				offset += keyLength;

				// Skip the value (DELETE only needs the key)
				uint32_t valueLength;
				if (offset + sizeof(valueLength) > size) return;
				memcpy(&valueLength, buffer + offset, sizeof(valueLength));
				offset += sizeof(valueLength) + valueLength; // Skip value content

				// Apply the operation directly to the ShardedKVStore (no mutex needed!)
				kv_store_.remove(key);

				// If this is our own request, complete the pending operation
				completeOperation(client_order);
				break;
			}

		case OpType::MULTI_PUT: 
			{
				// Process multiple PUT operations
				std::vector<std::pair<std::string, std::string>> kvPairs;
				kvPairs.reserve(pairCount);

				for (uint32_t i = 0; i < pairCount; ++i) {
					// Read key
					uint32_t keyLength;
					if (offset + sizeof(keyLength) > size) return;
					memcpy(&keyLength, buffer + offset, sizeof(keyLength));
					offset += sizeof(keyLength);

					if (offset + keyLength > size) return;
					std::string key(buffer + offset, keyLength);
					offset += keyLength;

					// Read value
					uint32_t valueLength;
					if (offset + sizeof(valueLength) > size) return;
					memcpy(&valueLength, buffer + offset, sizeof(valueLength));
					offset += sizeof(valueLength);

					if (offset + valueLength > size) return;
					std::string value(buffer + offset, valueLength);
					offset += valueLength;

					// Collect key-value pairs
					kvPairs.emplace_back(key, value);
				}

				// Apply all key-value pairs in one call (efficient batching)
				kv_store_.multiPut(kvPairs);

				// If this is our own request, complete the pending operation
				completeOperation(client_order);
				break;
			}

		case OpType::BEGIN_TX:
		case OpType::COMMIT_TX:
		case OpType::ABORT_TX:
			// Transaction operations would be processed here
			// This is a simplified implementation without full transaction support
			break;

		default:
			LOG(ERROR) << "Unknown operation type: " << static_cast<int>(type);
			break;
	}

	// Update the last applied index
	{
		absl::MutexLock lock(&apply_mutex_);
		if (last_applied_total_order_ < total_order) {
			last_applied_total_order_ = total_order;
		}
	}
}

// 3. Update the processLogEntry method if you're still using it
void DistributedKVStore::processLogEntry(const LogEntry& entry, size_t total_order) {
	// Only process write operations - reads are handled locally
	switch (entry.type) {
		case OpType::PUT: 
			{
				// Process a single PUT operation
				assert(entry.kvPairs.size() == 1);
				const auto& kv = entry.kvPairs[0];

				// Use the ShardedKVStore directly - no mutex needed here!
				kv_store_.put(kv.key, kv.value);

				// If this is our own request, complete the pending operation
				completeOperation(entry.opId.requestId);
				break;
			}

		case OpType::DELETE: 
			{
				// Process a single DELETE operation
				assert(entry.kvPairs.size() == 1);
				const auto& kv = entry.kvPairs[0];

				VLOG(3) << "DELETE operation: " << kv.key;

				// Use the ShardedKVStore directly - no mutex needed here!
				kv_store_.remove(kv.key);

				// If this is our own request, complete the pending operation
				completeOperation(entry.opId.requestId);
				break;
			}

		case OpType::MULTI_PUT: 
			{
				// Process a multi-key PUT operation
				VLOG(3) << "MULTI_PUT operation with " << entry.kvPairs.size() << " pairs";

				std::vector<std::pair<std::string, std::string>> keyValuePairs;
				keyValuePairs.reserve(entry.kvPairs.size());

				for (const auto& kv : entry.kvPairs) {
					keyValuePairs.emplace_back(kv.key, kv.value);
				}
				// Use the batch operation for efficiency
				kv_store_.multiPut(keyValuePairs);

				// If this is our own request, complete the pending operation
				completeOperation(entry.opId.requestId);
				break;
			}

		case OpType::BEGIN_TX:
		case OpType::COMMIT_TX:
		case OpType::ABORT_TX:
			// Transaction operations would be processed here
			// This is a simplified implementation without full transaction support
			break;

		default:
			LOG(ERROR) << "Unknown operation type: " << static_cast<int>(entry.type);
			break;
	}
	// Update the last applied index
	{
		absl::MutexLock lock(&apply_mutex_);
		if (last_applied_total_order_ < total_order) {
			last_applied_total_order_ = total_order;
		}
	}

}

void DistributedKVStore::completeOperation(OPID opId){
	absl::MutexLock lock(&pending_ops_mutex_);
	pending_ops_.erase(opId);
	/*
		 auto it = pending_ops_.find(opId);
		 if (it != pending_ops_.end()) {
		 pending_ops_.erase(it);
		 }else {
		 LOG(ERROR) << "This operation does not belong to us";
		 }
		 */
}

// Process messages in without caring total_order
void DistributedKVStore::logConsumer() {
	while (running_) {
		Embarcadero::MessageHeader *header =	(Embarcadero::MessageHeader*)subscriber_->Consume();
		if(header == nullptr){
			std::this_thread::yield();
			continue;
		}

		// Extract the message payload (skip the header)
		void* payload = (void*)((uint8_t*)header + sizeof(Embarcadero::MessageHeader));
		size_t payload_size = header->size;
		processLogEntryFromRawBuffer(
				payload,
				payload_size,
				header->client_id,
				header->client_order,
				header->total_order
		);
		/*
		// Deserialize the message into a LogEntry
		LogEntry entry = LogEntry::deserialize(payload, header->client_id, header->client_order);

		// Set the operation ID from the message header
		entry.opId.clientId = header->client_id;
		entry.opId.requestId = header->client_order;

		// Process the log entry with the total ordering from the message
		processLogEntry(entry, header->total_order);

		VLOG(3) << "Processed log entry with total order " << header->total_order
		<< " from client " << header->client_id;
		*/
	}
}

size_t DistributedKVStore::put(const std::string& key, const std::string& value) {
	// Create operation ID
	size_t client_order = last_request_id_++;
	OperationId opId{server_id_, client_order};

	// Create log entry
	LogEntry entry;
	entry.opId = opId;
	entry.type = OpType::PUT;
	entry.kvPairs.push_back({key, value});
	entry.transactionId = 0;  // Not part of a transaction

	OPID opid = client_order; // This must be same as MesageHeader's client_order

	// Register pending operation
	{
		absl::MutexLock lock(&pending_ops_mutex_);
		pending_ops_.insert(opid);
	}

	// Publish to log
	auto serialized = entry.serialize();
	publisher_->Publish(serialized.data(), serialized.size());

	return client_order;
}

size_t DistributedKVStore::multiPut(const std::vector<KeyValue>& kvPairs) {
	// Create operation ID
	size_t client_order = last_request_id_++;
	OperationId opId{server_id_, client_order};

	// Create log entry
	LogEntry entry;
	entry.opId = opId;
	entry.type = OpType::MULTI_PUT;
	entry.kvPairs = kvPairs;
	entry.transactionId = 0;  // Not part of a transaction

	OPID opid = client_order; // This must be same as MesageHeader's client_order

	// Register pending operation
	{
		absl::MutexLock lock(&pending_ops_mutex_);
		pending_ops_.insert(opid);
	}

	// Publish to log
	auto serialized = entry.serialize();
	publisher_->Publish(serialized.data(), serialized.size());

	return client_order;
}

void DistributedKVStore::waitForSyncWithLog(){
	// Get last total_order from the shared log and wait until local KV store is up-to-date
	return;
}

// Used by Client to wait for their submitted request
// Usually client submits multiple requests, and call this on the last order
// This only works when there's single client.
// Change following function to track pending_ops_ if there's multi client
void DistributedKVStore::waitUntilApplied(size_t total_order){
	publisher_->WriteFinishedOrPuased();
	while(total_order > last_applied_total_order_){
		std::this_thread::yield();
	}
}

std::string DistributedKVStore::get(const std::string& key) {
	// Wait for all operations up to the desired point to be applied
	waitForSyncWithLog(/* consistency requirement */);

	// No need for kv_store_mutex_! The ShardedKVStore handles locking internally
	return kv_store_.get(key);
}

// Multi-get operation
std::vector<std::pair<std::string, std::string>> DistributedKVStore::multiGet(
		const std::vector<std::string>& keys) {
	// Wait for all operations up to the desired point to be applied
	waitForSyncWithLog(/* consistency requirement */);

	// Use ShardedKVStore's multiGet for better performance
	return kv_store_.multiGet(keys);
}
