#include "line_editor.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#endif

namespace lais {
namespace {
std::size_t previous(std::string_view s, std::size_t p) {
    if (!p) return 0;
    --p;
    while (p && (static_cast<unsigned char>(s[p]) & 0xc0) == 0x80) --p;
    return p;
}
std::size_t next(std::string_view s, std::size_t p) {
    if (p >= s.size()) return s.size();
    ++p;
    while (p < s.size() && (static_cast<unsigned char>(s[p]) & 0xc0) == 0x80) ++p;
    return p;
}
std::uint32_t codepoint(std::string_view s) {
    if (s.empty()) return 0;
    unsigned char lead = static_cast<unsigned char>(s.front());
    if (lead < 0x80) return lead;
    std::uint32_t cp = lead & (lead < 0xe0 ? 0x1f : lead < 0xf0 ? 0x0f : 0x07);
    for (std::size_t i = 1; i < s.size(); ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3f);
    return cp;
}
std::size_t width(std::string_view s) {
    const auto c = codepoint(s);
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0xfe00 && c <= 0xfe0f) || c == 0x200d) return 0;
    return ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
            (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
            (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff01 && c <= 0xff60) ||
            (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1faff) || c >= 0x20000) ? 2 : 1;
}
bool space(char c) { return c == ' ' || c == '\t'; }
std::size_t terminal_columns() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
#else
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col) return size.ws_col;
#endif
    return 80;
}
#ifdef _WIN32
std::string utf8(std::uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += static_cast<char>(0xc0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { s += static_cast<char>(0xe0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 63)); s += static_cast<char>(0x80 | (cp & 63)); }
    else { s += static_cast<char>(0xf0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 63)); s += static_cast<char>(0x80 | ((cp >> 6) & 63)); s += static_cast<char>(0x80 | (cp & 63)); }
    return s;
}
#else
bool read_byte(char& c, int timeout_ms = -1) {
    if (timeout_ms >= 0) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &timeout) <= 0) return false;
    }
    return read(STDIN_FILENO, &c, 1) == 1;
}
#endif
}

void LineEditor::insert(std::string_view value) { text_.insert(cursor_, value); cursor_ += value.size(); }
void LineEditor::left() { cursor_ = previous(text_, cursor_); }
void LineEditor::right() { cursor_ = next(text_, cursor_); }
void LineEditor::backspace() { const auto p = previous(text_, cursor_); text_.erase(p, cursor_ - p); cursor_ = p; }
void LineEditor::erase_forward() { text_.erase(cursor_, next(text_, cursor_) - cursor_); }
void LineEditor::word_left() {
    while (cursor_ && space(text_[previous(text_, cursor_)])) left();
    while (cursor_ && !space(text_[previous(text_, cursor_)])) left();
}
void LineEditor::word_right() {
    while (cursor_ < text_.size() && !space(text_[cursor_])) right();
    while (cursor_ < text_.size() && space(text_[cursor_])) right();
}
void LineEditor::erase_word() { const auto old = cursor_; word_left(); text_.erase(cursor_, old - cursor_); }
void LineEditor::erase_before() { text_.erase(0, cursor_); cursor_ = 0; }
void LineEditor::erase_after() { text_.erase(cursor_); }
void LineEditor::clear() { text_.clear(); cursor_ = 0; history_index_ = history_.size(); draft_.clear(); draft_cursor_ = 0; }
void LineEditor::previous_history() {
    if (history_index_ == history_.size()) { draft_ = text_; draft_cursor_ = cursor_; }
    if (history_index_) { text_ = history_[--history_index_]; end(); }
}
void LineEditor::next_history() {
    if (history_index_ < history_.size()) {
        ++history_index_;
        text_ = history_index_ == history_.size() ? draft_ : history_[history_index_];
        cursor_ = history_index_ == history_.size() ? draft_cursor_ : text_.size();
    }
}
LineEditor::View LineEditor::view(std::size_t columns) const {
    columns = std::max<std::size_t>(columns, 2);
    std::size_t start = cursor_, used = 0;
    while (start) {
        const auto p = previous(text_, start);
        const auto w = width(std::string_view(text_).substr(p, start - p));
        if (used + w >= columns) break;
        start = p; used += w;
    }
    const auto caret = used;
    std::size_t finish = cursor_;
    while (finish < text_.size()) {
        const auto p = next(text_, finish);
        const auto w = width(std::string_view(text_).substr(finish, p - finish));
        if (used + w > columns) break;
        finish = p; used += w;
    }
    return {text_.substr(start, finish - start), caret};
}

bool read_interactive_line(std::string& line, const std::vector<std::string>& history) {
#ifdef _WIN32
    const bool terminal = _isatty(_fileno(stdin)) != 0;
#else
    const bool terminal = isatty(STDIN_FILENO) != 0;
#endif
    if (!terminal) { std::cout << "lais> " << std::flush; return static_cast<bool>(std::getline(std::cin, line)); }
#ifndef _WIN32
    termios original{};
    if (tcgetattr(STDIN_FILENO, &original) != 0) return static_cast<bool>(std::getline(std::cin, line));
    struct Restore { termios value; ~Restore() { tcsetattr(STDIN_FILENO, TCSADRAIN, &value); } } restore{original};
    termios raw = original;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
#endif
    LineEditor editor(history);
    auto redraw = [&] {
        const auto columns = terminal_columns();
        const auto view = editor.view(columns > 8 ? columns - 8 : 2);
        std::cout << "\r\033[2K\033[1;32mlais>\033[0m " << view.text << "\033[K\r\033[" << (6 + view.cursor_columns) << "C" << std::flush;
    };
    redraw();
    while (true) {
        int key = 0;
        std::string value;
#ifdef _WIN32
        key = _getwch();
        if (key == WEOF) return false;
        if (key == 0 || key == 0xe0) {
            switch (_getwch()) {
                case 75: editor.left(); break; case 77: editor.right(); break;
                case 71: editor.home(); break; case 79: editor.end(); break;
                case 83: editor.erase_forward(); break;
                case 72: editor.previous_history(); break; case 80: editor.next_history(); break;
                case 115: editor.word_left(); break; case 116: editor.word_right(); break;
            }
            redraw(); continue;
        }
        if (key >= 32 && key != 127) {
            std::uint32_t cp = static_cast<std::uint32_t>(key);
            if (cp >= 0xd800 && cp <= 0xdbff) {
                const auto low = static_cast<std::uint32_t>(_getwch());
                cp = low >= 0xdc00 && low <= 0xdfff ? 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00 : 0xfffd;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) cp = 0xfffd;
            value = utf8(cp);
        }
#else
        char c;
        if (!read_byte(c)) return false;
        key = static_cast<unsigned char>(c);
        if (key == 27) {
            std::string sequence;
            char part;
            if (read_byte(part, 35)) {
                sequence += part;
                if (part == '[' || part == 'O') {
                    while (sequence.size() < 16 && read_byte(part, 35)) {
                        sequence += part;
                        if ((part >= 'A' && part <= 'Z') || part == '~') break;
                    }
                }
            }
            if (sequence == "[A" || sequence == "OA") editor.previous_history();
            else if (sequence == "[B" || sequence == "OB") editor.next_history();
            else if (sequence == "[C" || sequence == "OC") editor.right();
            else if (sequence == "[D" || sequence == "OD") editor.left();
            else if (sequence == "[H" || sequence == "OH" || sequence == "[1~" || sequence == "[7~") editor.home();
            else if (sequence == "[F" || sequence == "OF" || sequence == "[4~" || sequence == "[8~") editor.end();
            else if (sequence == "[3~") editor.erase_forward();
            else if (sequence == "[1;5D" || sequence == "b") editor.word_left();
            else if (sequence == "[1;5C" || sequence == "f") editor.word_right();
            redraw(); continue;
        }
        if (key >= 32 && key != 127) {
            value += c;
            const int bytes = key >= 0xf0 ? 4 : key >= 0xe0 ? 3 : key >= 0xc0 ? 2 : 1;
            for (int i = 1; i < bytes; ++i) { if (!read_byte(c, 100)) break; value += c; }
        }
#endif
        if (key == '\r' || key == '\n') { line = editor.text(); std::cout << "\r\n" << std::flush; return true; }
        if ((key == 4 || key == 26) && editor.text().empty()) { std::cout << "\r\n" << std::flush; return false; }
        switch (key) {
            case 1: editor.home(); break; case 5: editor.end(); break;
            case 2: editor.left(); break; case 6: editor.right(); break;
            case 3: editor.clear(); std::cout << "^C\r\n"; break;
            case 4: editor.erase_forward(); break;
            case 8: case 127: editor.backspace(); break;
            case 11: editor.erase_after(); break; case 21: editor.erase_before(); break;
            case 23: editor.erase_word(); break;
            case 12: std::cout << "\033[2J\033[H"; break;
            default: if (!value.empty()) editor.insert(value); break;
        }
        redraw();
    }
}
}
