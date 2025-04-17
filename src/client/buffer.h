#pragma once

#include "common.h"

/**
 * Buffer class for managing message data
 * Provides thread-safe buffer management for publishers and subscribers
 */
class Buffer {
public:
    /**
     * Constructor for Buffer
     * @param num_buf Number of buffers to manage
     * @param num_threads_per_broker Number of threads per broker
     * @param client_id Client identifier
     * @param message_size Size of messages
     * @param order Order level
     */
    Buffer(size_t num_buf, size_t num_threads_per_broker, int client_id, size_t message_size, int order = 0);
    
    /**
     * Destructor - cleans up allocated buffers
     */
    ~Buffer();
    
    /**
     * Adds buffers to the pool
     * @param buf_size Size of each buffer
     * @return true if successful, false otherwise
     */
    bool AddBuffers(size_t buf_size);
    
#ifdef BATCH_OPTIMIZATION
    /**
     * Writes a message to the buffer with batch optimization
     * @param client_order Client-side message order
     * @param msg Message data
     * @param len Message length
     * @param paddedSize Padded size of the message
     * @return true if successful, false otherwise
     */
    bool Write(size_t client_order, char* msg, size_t len, size_t paddedSize);
    
    /**
     * Reads from the buffer with batch optimization
     * @param bufIdx Buffer index to read from
     * @return Pointer to the read data or nullptr if empty
     */
    void* Read(int bufIdx);
#else
    /**
     * Writes a message to the buffer without batch optimization
     * @param bufIdx Buffer index to write to
     * @param client_order Client-side message order
     * @param msg Message data
     * @param len Message length
     * @param paddedSize Padded size of the message
     * @return true if successful, false otherwise
     */
    bool Write(int bufIdx, size_t client_order, char* msg, size_t len, size_t paddedSize);
    
    /**
     * Reads from the buffer without batch optimization
     * @param bufIdx Buffer index to read from
     * @param len Output parameter for the length of data read
     * @return Pointer to the read data or nullptr if empty
     */
    void* Read(int bufIdx, size_t& len);
#endif
    
    /**
     * Signals that reading is complete
     */
    void ReturnReads();
    
    /**
     * Signals that writing is finished
     */
    void WriteFinished();

		void Flush();
		void ResetFlush() { is_flushing_ = false; }
private:
    /**
     * Buffer structure with cache line alignment
     */
    struct alignas(64) Buf {
        // Static
        void* buffer;
        size_t len;
        // Writer modify
        size_t writer_head;
        size_t tail;
        size_t num_msg;
        // Reader modify
        size_t reader_head;
        
        Buf() : writer_head(0), tail(0), num_msg(0), reader_head(0) {}
    };
    
    std::vector<Buf> bufs_;
    size_t num_threads_per_broker_;
    int order_;
    size_t i_ = 0;
    size_t j_ = 0;
    
    size_t write_buf_id_ = 0;
    std::atomic<size_t> num_buf_{0};
    std::atomic<size_t> batch_seq_{0};
    bool shutdown_{false};
    bool seal_from_read_{false};
    Embarcadero::MessageHeader header_;
    
    /**
     * Advances the write buffer ID
     */
    void AdvanceWriteBufId();

		bool is_flushing_ = false;
};
