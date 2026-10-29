#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lais {
// Positions are UTF-8 byte boundaries; rendering uses terminal display columns.
class LineEditor {
public:
    explicit LineEditor(const std::vector<std::string>& history) : history_(history), history_index_(history.size()) {}
    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    void insert(std::string_view value);
    void left();
    void right();
    void home() { cursor_ = 0; }
    void end() { cursor_ = text_.size(); }
    void backspace();
    void erase_forward();
    void word_left();
    void word_right();
    void erase_word();
    void erase_before();
    void erase_after();
    void previous_history();
    void next_history();
    void clear();
    struct View { std::string text; std::size_t cursor_columns; };
    View view(std::size_t columns) const;
private:
    std::string text_, draft_;
    std::size_t cursor_ = 0, draft_cursor_ = 0;
    const std::vector<std::string>& history_;
    std::size_t history_index_;
};
bool read_interactive_line(std::string& line, const std::vector<std::string>& history);
}
