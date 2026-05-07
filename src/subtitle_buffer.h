#pragma once

#include <deque>
#include <string>
#include <vector>

class SubtitlesBuffer {
public:
	SubtitlesBuffer(size_t max_lines, size_t max_chars_per_line);
	void addText(const std::string &text, bool is_final);
	[[nodiscard]] std::string getBufferContent() const;
	[[nodiscard]] std::string formatAsFinalPreview(const std::string &text) const;
	void changeSize(size_t new_max_lines, size_t new_max_chars_per_line);
	void clear();

private:
	size_t max_lines_;
	size_t max_chars_per_line_;

	std::string interim_suffix_;
	std::deque<std::string> lines_;

	[[nodiscard]] size_t lineCap() const;
	[[nodiscard]] static std::string tailUtf8(const std::string &text, size_t max_chars);
	[[nodiscard]] std::string wrapUtf8ByWords(const std::string &text) const;

	void pushLine(std::deque<std::string> &dst, std::string line) const;
	void appendWords(std::deque<std::string> &dst, const std::vector<std::string> &words) const;
	[[nodiscard]] std::string joinLines(const std::deque<std::string> &lines) const;
};
