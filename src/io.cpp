// skynat-engine — pipe threads.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "io.hpp"
#include "platform.hpp"
#include "protocol.hpp"

#include <chrono>

namespace skynat {

Writer::Writer(intptr_t handle) : handle_(handle) {}

Writer::~Writer()
{
	if (thread_.joinable())
		thread_.detach();
}

void Writer::start()
{
	thread_ = std::thread([this] { run(); });
}

bool Writer::send(std::vector<uint8_t> &&frame, Lane lane)
{
	if (broken_.load())
		return false;
	const uint64_t size = frame.size();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			return false;
		const uint64_t queued = queued_bytes_.load();
		if (lane == BULK && queued > kBulkLimit) {
			bulk_dropped_++;
			return false;
		}
		if (lane == MEDIA && queued > kMediaLimit) {
			media_dropped_++;
			return false;
		}
		queued_bytes_ += size;
		queue_.push_back(std::move(frame));
	}
	cv_.notify_one();
	return true;
}

bool Writer::event(const std::string &json)
{
	return send(Frame(MSG_EVENT, json.size()).text(json).finish(), CONTROL);
}

void Writer::run()
{
	platform::set_thread_name("skynat: stdout writer");
	std::deque<std::vector<uint8_t>> batch;
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (queue_.empty() && stopping_)
				break;
			batch.swap(queue_);
		}
		for (auto &frame : batch) {
			if (!broken_.load()) {
				if (platform::write_all(handle_, frame.data(), frame.size())) {
					bytes_written_ += frame.size();
				} else {
					broken_ = true;
					if (on_broken_)
						on_broken_();
				}
			}
			queued_bytes_ -= frame.size();
		}
		batch.clear();
	}
	std::lock_guard<std::mutex> lock(mutex_);
	finished_ = true;
	done_cv_.notify_all();
}

void Writer::flush_and_stop(int timeout_ms)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	cv_.notify_one();
	std::unique_lock<std::mutex> lock(mutex_);
	bool done = done_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return finished_; });
	lock.unlock();
	if (thread_.joinable()) {
		if (done)
			thread_.join();
		else
			thread_.detach();
	}
}

Reader::Reader(intptr_t handle, Handler on_message, std::function<void(const std::string &)> on_eof)
	: handle_(handle),
	  on_message_(std::move(on_message)),
	  on_eof_(std::move(on_eof))
{
}

void Reader::start()
{
	thread_ = std::thread([this] { run(); });
	thread_.detach();
}

bool Reader::read_exact(void *dst, size_t size)
{
	uint8_t *p = (uint8_t *)dst;
	while (size > 0) {
		long n = platform::read_some(handle_, p, size);
		if (n <= 0)
			return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

void Reader::run()
{
	platform::set_thread_name("skynat: stdin reader");
	uint8_t header[kHeaderSize];
	for (;;) {
		if (!read_exact(header, kHeaderSize)) {
			on_eof_("stdin closed");
			return;
		}
		uint32_t length = get_u32(header);
		uint8_t type = header[4];
		if (length > kMaxPayload) {
			on_eof_("protocol error: message too large");
			return;
		}
		std::vector<uint8_t> payload(length);
		if (length && !read_exact(payload.data(), length)) {
			on_eof_("stdin closed mid-message");
			return;
		}
		on_message_(type, std::move(payload));
	}
}

} // namespace skynat
