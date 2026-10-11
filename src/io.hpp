// skynat-engine — the two pipe threads: one stdout writer, one stdin reader.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skynat {

// Writes complete frames to the protocol stdout in FIFO order from its own
// thread, so libobs's encoder/output threads never block on the pipe.
class Writer {
public:
	enum Lane {
		CONTROL, // events and codec configs: never dropped
		MEDIA,   // program packets: dropped only if the app stops reading for a long time
		BULK,    // monitor feeds and thumbnails: dropped first when the pipe backs up
	};

	explicit Writer(intptr_t handle);
	~Writer();

	void start();
	// Returns false if the frame was dropped (backed-up pipe or broken pipe).
	bool send(std::vector<uint8_t> &&frame, Lane lane);
	bool event(const std::string &json);
	// Drains what is queued (up to timeout_ms), then stops the thread.
	void flush_and_stop(int timeout_ms);

	void set_on_broken(std::function<void()> fn) { on_broken_ = std::move(fn); }
	bool broken() const { return broken_.load(); }
	bool congested() const { return queued_bytes_.load() > kBulkLimit; }

	uint64_t queued_bytes() const { return queued_bytes_.load(); }
	uint64_t bulk_dropped() const { return bulk_dropped_.load(); }
	uint64_t media_dropped() const { return media_dropped_.load(); }
	uint64_t bytes_written() const { return bytes_written_.load(); }

	static constexpr uint64_t kBulkLimit = 4ull * 1024 * 1024;
	static constexpr uint64_t kMediaLimit = 128ull * 1024 * 1024;

private:
	void run();

	intptr_t handle_;
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::condition_variable done_cv_;
	std::deque<std::vector<uint8_t>> queue_;
	bool stopping_ = false;
	bool finished_ = false;
	std::atomic<bool> broken_{false};
	std::atomic<uint64_t> queued_bytes_{0};
	std::atomic<uint64_t> bulk_dropped_{0};
	std::atomic<uint64_t> media_dropped_{0};
	std::atomic<uint64_t> bytes_written_{0};
	std::function<void()> on_broken_;
};

// Reads frames from the protocol stdin on its own thread and hands each one
// to `on_message`; `on_eof` runs once when the parent closes the pipe (or the
// stream is corrupt).
class Reader {
public:
	using Handler = std::function<void(uint8_t type, std::vector<uint8_t> &&payload)>;

	Reader(intptr_t handle, Handler on_message, std::function<void(const std::string &why)> on_eof);
	void start();

private:
	void run();
	bool read_exact(void *dst, size_t size);

	intptr_t handle_;
	Handler on_message_;
	std::function<void(const std::string &)> on_eof_;
	std::thread thread_;
};

} // namespace skynat
