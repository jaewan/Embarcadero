#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <future>
#include <condition_variable>
#include <functional>
#include <signal.h>
#include <errno.h>

constexpr int MAX_EVENTS = 32;
constexpr int LISTEN_PORT = 1214;
constexpr int BUFFER_SIZE = 4096;
constexpr int NUM_THREADS = 16;

std::mutex queue_mutex;
std::condition_variable condition;
std::queue<std::function<void()>> tasks;
bool stop_server = false;

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty())
                            return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto task = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker: workers)
            worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::queue< std::function<void()> > tasks;
    bool stop;
};

int make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }
    return 0;
}

void signal_handler(int sig) {
    stop_server = true;
}

int main() {
    signal(SIGINT, signal_handler);
    ThreadPool pool(NUM_THREADS);
    int server_fd, efd;
    struct epoll_event event, events[MAX_EVENTS];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket creation failed");
        return -1;
    }

    make_socket_non_blocking(server_fd);

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(LISTEN_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

		if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create1 failed");
        close(server_fd);
        return -1;
    }

    event.data.fd = server_fd;
    event.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl");
        close(server_fd);
        close(efd);
        return -1;
    }

    // Main loop
    while (!stop_server) {
        int n = epoll_wait(efd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno != EINTR) {
                perror("epoll_wait");
                break;
            }
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_addr_len = sizeof(client_addr);
                    int new_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
                    if (new_socket == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("accept");
                        }
                        break;
                    }

                    make_socket_non_blocking(new_socket);

                    event.data.fd = new_socket;
                    event.events = EPOLLIN | EPOLLET;
                    if (epoll_ctl(efd, EPOLL_CTL_ADD, new_socket, &event) == -1) {
                        perror("epoll_ctl");
                        close(new_socket);
                        continue;
                    }
                }
            } else {
                pool.enqueue([fd = events[i].data.fd] {
                    char buffer[BUFFER_SIZE];
                    int count = read(fd, buffer, BUFFER_SIZE);
                    if (count > 0) {
                        static std::string response = "1";
                        write(fd, response.c_str(), response.size());
                    } else if (count == 0) {
                        close(fd);
                    } else {
                        if (errno != EAGAIN) {
                            close(fd);
                        }
                    }
                });
            }
        }
    }

    // Cleanup
    close(server_fd);
    close(efd);
    return 0;
}
