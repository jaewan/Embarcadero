// RAII wrapper for file descriptors (sockets, epoll fd, etc.).
// Ensures fd is closed on scope exit; prevents leaks on early return or exception.
#ifndef EMBARCADERO_SRC_COMMON_SCOPED_FD_H_
#define EMBARCADERO_SRC_COMMON_SCOPED_FD_H_

#include <unistd.h>

struct ScopedFd {
	int fd = -1;

	ScopedFd() = default;
	explicit ScopedFd(int f) : fd(f) {}

	~ScopedFd() {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}

	ScopedFd(const ScopedFd&) = delete;
	ScopedFd& operator=(const ScopedFd&) = delete;

	ScopedFd(ScopedFd&& o) noexcept : fd(o.fd) { o.fd = -1; }
	ScopedFd& operator=(ScopedFd&& o) noexcept {
		if (this != &o) {
			if (fd >= 0) ::close(fd);
			fd = o.fd;
			o.fd = -1;
		}
		return *this;
	}

	int get() const { return fd; }

	// Release ownership; caller must close.
	int release() {
		int f = fd;
		fd = -1;
		return f;
	}
};

#endif  // EMBARCADERO_SRC_COMMON_SCOPED_FD_H_
