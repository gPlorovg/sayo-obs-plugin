#include "subtitle_buffer.h"

#include <algorithm>
#include <cctype>

namespace {
constexpr size_t kMinCharsPerLine = 1;

static inline bool is_utf8_continuation(unsigned char c)
{
	return (c & 0xC0) == 0x80;
}

static size_t utf8_prev_char_start(const std::string &s, size_t i)
{
	if (i == 0) {
		return 0;
	}
	size_t j = i - 1;
	while (j > 0 && is_utf8_continuation(static_cast<unsigned char>(s[j]))) {
		--j;
	}
	return j;
}

static size_t utf8_count_codepoints(const std::string &s)
{
	size_t count = 0;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		if ((c & 0x80) == 0x00)
			len = 1;
		else if ((c & 0xE0) == 0xC0)
			len = 2;
		else if ((c & 0xF0) == 0xE0)
			len = 3;
		else if ((c & 0xF8) == 0xF0)
			len = 4;
		/* If invalid/truncated, treat byte as a standalone char. */
		if (i + len > s.size())
			len = 1;
		i += len;
		++count;
	}
	return count;
}

static std::string utf8_take_first_codepoints(const std::string &s, size_t n)
{
	if (n == 0 || s.empty()) {
		return {};
	}
	size_t i = 0;
	size_t left = n;
	while (i < s.size() && left > 0) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		if ((c & 0x80) == 0x00)
			len = 1;
		else if ((c & 0xE0) == 0xC0)
			len = 2;
		else if ((c & 0xF0) == 0xE0)
			len = 3;
		else if ((c & 0xF8) == 0xF0)
			len = 4;
		if (i + len > s.size())
			len = 1;
		i += len;
		--left;
	}
	return s.substr(0, i);
}

static std::string utf8_tail_by_codepoints(const std::string &s, size_t max_chars)
{
	if (max_chars == 0 || s.empty()) {
		return {};
	}
	size_t total = utf8_count_codepoints(s);
	if (total <= max_chars) {
		return s;
	}
	size_t skip = total - max_chars;
	size_t i = 0;
	while (i < s.size() && skip > 0) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		if ((c & 0x80) == 0x00)
			len = 1;
		else if ((c & 0xE0) == 0xC0)
			len = 2;
		else if ((c & 0xF0) == 0xE0)
			len = 3;
		else if ((c & 0xF8) == 0xF0)
			len = 4;
		if (i + len > s.size())
			len = 1;
		i += len;
		--skip;
	}
	/* Ensure we start at a character boundary. */
	i = utf8_prev_char_start(s, i);
	return s.substr(i);
}

static std::vector<std::string> split_words_utf8(const std::string &s)
{
	std::vector<std::string> out;
	out.reserve(32);
	std::string cur;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		if ((c & 0x80) == 0x00)
			len = 1;
		else if ((c & 0xE0) == 0xC0)
			len = 2;
		else if ((c & 0xF0) == 0xE0)
			len = 3;
		else if ((c & 0xF8) == 0xF0)
			len = 4;
		if (i + len > s.size())
			len = 1;

		/* Treat ASCII whitespace as delimiter; for non-ASCII, keep byte sequence as part of the word. */
		if (len == 1 && std::isspace(c) != 0) {
			if (!cur.empty()) {
				out.push_back(std::move(cur));
				cur.clear();
			}
			i += 1;
			continue;
		}

		cur.append(s, i, len);
		i += len;
	}
	if (!cur.empty()) {
		out.push_back(std::move(cur));
	}
	return out;
}
}

SubtitlesBuffer::SubtitlesBuffer(size_t max_lines, size_t max_chars_per_line)
	: max_lines_(max_lines), max_chars_per_line_(max_chars_per_line)
{
}

std::string SubtitlesBuffer::tailUtf8(const std::string &text, size_t max_chars)
{
	return utf8_tail_by_codepoints(text, max_chars);
}

size_t SubtitlesBuffer::lineCap() const
{
	return std::max(kMinCharsPerLine, max_chars_per_line_);
}

void SubtitlesBuffer::pushLine(std::deque<std::string> &dst, std::string line) const
{
	if (line.empty())
		return;
	/* Reserve one slot for the current in-progress line. */
	const size_t max_lines = std::max<size_t>(1, max_lines_);
	const size_t max_committed = max_lines > 0 ? (max_lines - 1) : 0;
	dst.push_back(std::move(line));
	while (dst.size() > max_committed) {
		dst.pop_front();
	}
}

void SubtitlesBuffer::appendWords(std::deque<std::string> &dst, const std::vector<std::string> &words) const
{
	const size_t cap = lineCap();

	if (dst.empty()) {
		dst.emplace_back();
	}

	for (const auto &w : words) {
		if (w.empty())
			continue;
		const size_t w_chars = utf8_count_codepoints(w);
		if (w_chars == 0)
			continue;

		/* Split long words by characters; don't reflow older lines. */
		if (w_chars > cap) {
			/* Finish current line if it has content */
			if (!dst.back().empty()) {
				std::string completed = std::move(dst.back());
				dst.pop_back();
				pushLine(dst, std::move(completed));
				dst.emplace_back();
			}

			std::string rest = w;
			while (utf8_count_codepoints(rest) > cap) {
				std::string head = utf8_take_first_codepoints(rest, cap);
				pushLine(dst, std::move(head));
				if (dst.empty() || !dst.back().empty()) {
					dst.emplace_back();
				}
				const size_t rest_chars = utf8_count_codepoints(rest);
				rest = utf8_tail_by_codepoints(rest, rest_chars - cap);
			}
			if (!rest.empty()) {
				dst.back() = std::move(rest);
			}
			continue;
		}

		const size_t cur_chars = utf8_count_codepoints(dst.back());
		const size_t extra = dst.back().empty() ? 0 : 1;

		if (cur_chars + extra + w_chars <= cap) {
			if (!dst.back().empty())
				dst.back().push_back(' ');
			dst.back() += w;
		} else {
			/* Commit current line and start a new one */
			std::string completed = std::move(dst.back());
			dst.pop_back();
			pushLine(dst, std::move(completed));
			dst.emplace_back();
			dst.back() = w;
		}
	}
}

std::string SubtitlesBuffer::joinLines(const std::deque<std::string> &lines) const
{
	std::string out;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i > 0)
			out.push_back('\n');
		out += lines[i];
	}
	return out;
}

std::string SubtitlesBuffer::wrapUtf8ByWords(const std::string &text) const
{
	if (text.empty()) {
		return {};
	}
	std::deque<std::string> tmp;
	appendWords(tmp, split_words_utf8(text));
	/* drop the in-progress last line if empty */
	if (!tmp.empty() && tmp.back().empty()) {
		tmp.pop_back();
	}
	return joinLines(tmp);
}

void SubtitlesBuffer::addText(const std::string &text, bool is_final)
{
	if (is_final) {
		interim_suffix_.clear();
		appendWords(lines_, split_words_utf8(text));
		return;
	}

	interim_suffix_ = text;
}

std::string SubtitlesBuffer::getBufferContent() const
{
	std::deque<std::string> tmp = lines_;
	if (!interim_suffix_.empty()) {
		appendWords(tmp, split_words_utf8(interim_suffix_));
	}
	if (!tmp.empty() && tmp.back().empty()) {
		tmp.pop_back();
	}
	return joinLines(tmp);
}

std::string SubtitlesBuffer::formatAsFinalPreview(const std::string &text) const
{
	SubtitlesBuffer tmp(max_lines_, max_chars_per_line_);
	tmp.addText(text, true);
	return tmp.getBufferContent();
}

void SubtitlesBuffer::clear()
{
	interim_suffix_.clear();
	lines_.clear();
}

void SubtitlesBuffer::changeSize(size_t new_max_lines, size_t new_max_chars_per_line)
{
	max_lines_ = new_max_lines;
	max_chars_per_line_ = new_max_chars_per_line;
	/* Rewrap by replaying existing text. */
	const std::string snapshot = joinLines(lines_);
	lines_.clear();
	appendWords(lines_, split_words_utf8(snapshot));
}
