// skynat-engine — tiny JSON writer for events (commands are parsed with
// libobs's obs_data_create_from_json).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace skynat {

inline std::string json_escape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 2);
	out.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char tmp[8];
				snprintf(tmp, sizeof(tmp), "\\u%04x", c);
				out += tmp;
			} else {
				out.push_back((char)c);
			}
		}
	}
	out.push_back('"');
	return out;
}

class JsonObj {
public:
	JsonObj &str(const char *key, const std::string &v)
	{
		sep(key);
		body += json_escape(v);
		return *this;
	}
	JsonObj &integer(const char *key, int64_t v)
	{
		sep(key);
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "%" PRId64, v);
		body += tmp;
		return *this;
	}
	JsonObj &number(const char *key, double v)
	{
		sep(key);
		if (!std::isfinite(v)) {
			body += "null";
			return *this;
		}
		char tmp[48];
		snprintf(tmp, sizeof(tmp), "%.4f", v);
		body += tmp;
		return *this;
	}
	JsonObj &boolean(const char *key, bool v)
	{
		sep(key);
		body += v ? "true" : "false";
		return *this;
	}
	JsonObj &null(const char *key)
	{
		sep(key);
		body += "null";
		return *this;
	}
	// `json` must already be valid JSON (an object, array, string, ...).
	JsonObj &raw(const char *key, const std::string &json)
	{
		sep(key);
		body += json;
		return *this;
	}
	std::string dump() const { return "{" + body + "}"; }

private:
	void sep(const char *key)
	{
		if (!body.empty())
			body.push_back(',');
		body += json_escape(key);
		body.push_back(':');
	}
	std::string body;
};

inline std::string json_array(const std::vector<std::string> &items)
{
	std::string out = "[";
	for (size_t i = 0; i < items.size(); i++) {
		if (i)
			out.push_back(',');
		out += items[i];
	}
	out.push_back(']');
	return out;
}

} // namespace skynat
