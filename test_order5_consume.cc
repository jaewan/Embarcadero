#include "src/client/subscriber.h"
#include "src/client/publisher.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>

int main() {
    const char* topic = "OrderTestTopic";
    const size_t num_messages = 100;
    const size_t message_size = 1024;
    
    std::cout << "Testing Sequencer 5 ordered consumption with " << num_messages << " messages..." << std::endl;
    
    // Create publisher and subscriber
    Publisher p(topic, "127.0.0.1", "1214", 1, message_size, 1024*1024, 5, EMBARCADERO);
    Subscriber s("127.0.0.1", "1214", const_cast<char*>(topic), false, 5);
    
    // Initialize
    p.Init(2);
    s.WaitUntilAllConnected();
    
    std::cout << "Initialized publisher and subscriber" << std::endl;
    
    // Publish messages
    char message[message_size];
    for (size_t i = 0; i < message_size; i++) {
        message[i] = 'A' + (i % 26);
    }
    
    std::cout << "Publishing " << num_messages << " messages..." << std::endl;
    for (size_t i = 0; i < num_messages; i++) {
        p.Publish(message, message_size);
    }
    
    p.DEBUG_check_send_finish();
    p.Poll(num_messages);
    
    std::cout << "Published all messages, now testing ordered consumption..." << std::endl;
    
    // Test ordered consumption using Consume()
    std::vector<size_t> received_orders;
    size_t timeout_count = 0;
    const size_t max_timeouts = 10;
    
    for (size_t i = 0; i < num_messages; i++) {
        void* msg = s.Consume(2000); // 2 second timeout
        
        if (msg == nullptr) {
            timeout_count++;
            std::cout << "Timeout waiting for message " << i << " (timeout #" << timeout_count << ")" << std::endl;
            
            if (timeout_count >= max_timeouts) {
                std::cout << "Too many timeouts, aborting test" << std::endl;
                break;
            }
            i--; // Retry same message
            continue;
        }
        
        // Extract total_order from message header
        Embarcadero::MessageHeader* header = static_cast<Embarcadero::MessageHeader*>(msg);
        size_t total_order = header->total_order;
        received_orders.push_back(total_order);
        
        std::cout << "Received message " << i << " with total_order=" << total_order << std::endl;
        
        // Check if orders are sequential
        if (i > 0 && total_order != received_orders[i-1] + 1) {
            std::cout << "ERROR: Order violation! Expected " << (received_orders[i-1] + 1) 
                      << ", got " << total_order << std::endl;
        }
    }
    
    // Analyze results
    std::cout << "\n=== RESULTS ===" << std::endl;
    std::cout << "Messages received: " << received_orders.size() << "/" << num_messages << std::endl;
    
    if (received_orders.size() > 0) {
        std::cout << "Order range: " << received_orders[0] << " to " << received_orders.back() << std::endl;
        
        // Check for gaps
        bool ordered = true;
        for (size_t i = 1; i < received_orders.size(); i++) {
            if (received_orders[i] != received_orders[i-1] + 1) {
                std::cout << "Gap detected: " << received_orders[i-1] << " -> " << received_orders[i] << std::endl;
                ordered = false;
            }
        }
        
        if (ordered) {
            std::cout << "✅ All messages received in correct sequential order!" << std::endl;
        } else {
            std::cout << "❌ Order violations detected!" << std::endl;
        }
    }
    
    return 0;
}
