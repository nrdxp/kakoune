#include "ncurses_ui.hh"

#include "display_buffer.hh"
#include "event_manager.hh"
#include "keys.hh"
#include "ranges.hh"
#include "string_utils.hh"

#include <algorithm>

#define NCURSES_OPAQUE 0
#define NCURSES_INTERNALS

#include <ncurses.h>

#include <fcntl.h>
#include <csignal>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

constexpr char control(char c) { return c & 037; }

namespace Kakoune
{

using std::min;
using std::max;

struct NCursesWin : WINDOW {};

void NCursesUI::Window::create(const DisplayCoord& p, const DisplayCoord& s)
{
    pos = p;
    size = s;
    win = (NCursesWin*)newpad((int)size.line, (int)size.column);
}

void NCursesUI::Window::destroy()
{
    delwin(win);
    invalidate();
}

void NCursesUI::Window::invalidate()
{
    win = nullptr;
    pos = DisplayCoord{};
    size = DisplayCoord{};
}

void NCursesUI::Window::refresh(bool force)
{
    if (not win)
        return;

    if (force)
        redrawwin(win);

    DisplayCoord max_pos = pos + size - DisplayCoord{1,1};
    pnoutrefresh(win, 0, 0, (int)pos.line, (int)pos.column,
                 (int)max_pos.line, (int)max_pos.column);
}

void NCursesUI::Window::move_cursor(DisplayCoord coord)
{
    wmove(win, (int)coord.line, (int)coord.column);
}

void NCursesUI::Window::add_str(StringView str)
{
    waddnstr(win, str.begin(), (int)str.length());
}

void NCursesUI::Window::clear_to_end_of_line()
{
    wclrtoeol(win);
}

void NCursesUI::Window::draw_line(Palette& palette,
                                  const DisplayLine& line,
                                  ColumnCount col_index,
                                  ColumnCount max_column,
                                  const Face& default_face)
{
    for (const DisplayAtom& atom : line)
    {
        set_face(palette, atom.face, default_face);

        StringView content = atom.content();
        if (content.empty())
            continue;

        const auto remaining_columns = max_column - col_index;
        if (content.back() == '\n' and
            content.column_length() - 1 < remaining_columns)
        {
            add_str(content.substr(0, content.length()-1));
            waddch(win, ' ');
        }
        else
        {
            content = content.substr(0_col, remaining_columns);
            add_str(content);
            col_index += content.column_length();
        }
    }
}

void NCursesUI::Window::set_face(Palette& palette, Face face, const Face& default_face)
{
    if (m_active_pair != -1)
        wattroff(win, COLOR_PAIR(m_active_pair));

    face = merge_faces(default_face, face);

    if (face.fg != Color::Default or face.bg != Color::Default)
    {
        m_active_pair = palette.get_color_pair(face);
        wattron(win, COLOR_PAIR(m_active_pair));
    }

    auto set_attribute = [&](Attribute attr, int nc_attr) {
        (face.attributes & attr) ?  wattron(win, nc_attr) : wattroff(win, nc_attr);
    };

    set_attribute(Attribute::Underline, A_UNDERLINE);
    set_attribute(Attribute::Reverse, A_REVERSE);
    set_attribute(Attribute::Blink, A_BLINK);
    set_attribute(Attribute::Bold, A_BOLD);
    set_attribute(Attribute::Dim, A_DIM);
    #if defined(A_ITALIC)
    set_attribute(Attribute::Italic, A_ITALIC);
    #endif
}

void NCursesUI::Window::mark_dirty(LineCount pos, LineCount count)
{
    wredrawln(win, (int)pos, (int)count);
}

void NCursesUI::Window::set_background_color(Palette& palette, Face face)
{
    wbkgdset(win, COLOR_PAIR(palette.get_color_pair(face)));
}

int NCursesUI::Window::get_char()
{
    return wgetch(win);
}

void NCursesUI::Window::set_blocking(bool blocking)
{
    wtimeout(win, blocking ? -1 : 0);
}

constexpr int NCursesUI::default_shift_function_key;

static constexpr StringView assistant_cat[] =
    { R"(  ___            )",
      R"( (__ \           )",
      R"(   / /          ╭)",
      R"(  .' '·.        │)",
      R"( '      ”       │)",
      R"( ╰       /\_/|  │)",
      R"(  | .         \ │)",
      R"(  ╰_J`    | | | ╯)",
      R"(      ' \__- _/  )",
      R"(      \_\   \_\  )",
      R"(                 )"};

static constexpr StringView assistant_clippy[] =
    { " ╭──╮   ",
      " │  │   ",
      " @  @  ╭",
      " ││ ││ │",
      " ││ ││ ╯",
      " │╰─╯│  ",
      " ╰───╯  ",
      "        " };

static constexpr StringView assistant_dilbert[] =
    { R"(  დოოოოოდ   )",
      R"(  |     |   )",
      R"(  |     |  ╭)",
      R"(  |-ᱛ ᱛ-|  │)",
      R"( Ͼ   ∪   Ͽ │)",
      R"(  |     |  ╯)",
      R"( ˏ`-.ŏ.-´ˎ  )",
      R"(     @      )",
      R"(      @     )",
      R"(            )"};

template<typename T> T sq(T x) { return x * x; }

constexpr struct { unsigned char r, g, b; } builtin_colors[] = {
    {0x00,0x00,0x00}, {0x80,0x00,0x00}, {0x00,0x80,0x00}, {0x80,0x80,0x00},
    {0x00,0x00,0x80}, {0x80,0x00,0x80}, {0x00,0x80,0x80}, {0xc0,0xc0,0xc0},
    {0x80,0x80,0x80}, {0xff,0x00,0x00}, {0x00,0xff,0x00}, {0xff,0xff,0x00},
    {0x00,0x00,0xff}, {0xff,0x00,0xff}, {0x00,0xff,0xff}, {0xff,0xff,0xff},
    {0x00,0x00,0x00}, {0x00,0x00,0x5f}, {0x00,0x00,0x87}, {0x00,0x00,0xaf},
    {0x00,0x00,0xd7}, {0x00,0x00,0xff}, {0x00,0x5f,0x00}, {0x00,0x5f,0x5f},
    {0x00,0x5f,0x87}, {0x00,0x5f,0xaf}, {0x00,0x5f,0xd7}, {0x00,0x5f,0xff},
    {0x00,0x87,0x00}, {0x00,0x87,0x5f}, {0x00,0x87,0x87}, {0x00,0x87,0xaf},
    {0x00,0x87,0xd7}, {0x00,0x87,0xff}, {0x00,0xaf,0x00}, {0x00,0xaf,0x5f},
    {0x00,0xaf,0x87}, {0x00,0xaf,0xaf}, {0x00,0xaf,0xd7}, {0x00,0xaf,0xff},
    {0x00,0xd7,0x00}, {0x00,0xd7,0x5f}, {0x00,0xd7,0x87}, {0x00,0xd7,0xaf},
    {0x00,0xd7,0xd7}, {0x00,0xd7,0xff}, {0x00,0xff,0x00}, {0x00,0xff,0x5f},
    {0x00,0xff,0x87}, {0x00,0xff,0xaf}, {0x00,0xff,0xd7}, {0x00,0xff,0xff},
    {0x5f,0x00,0x00}, {0x5f,0x00,0x5f}, {0x5f,0x00,0x87}, {0x5f,0x00,0xaf},
    {0x5f,0x00,0xd7}, {0x5f,0x00,0xff}, {0x5f,0x5f,0x00}, {0x5f,0x5f,0x5f},
    {0x5f,0x5f,0x87}, {0x5f,0x5f,0xaf}, {0x5f,0x5f,0xd7}, {0x5f,0x5f,0xff},
    {0x5f,0x87,0x00}, {0x5f,0x87,0x5f}, {0x5f,0x87,0x87}, {0x5f,0x87,0xaf},
    {0x5f,0x87,0xd7}, {0x5f,0x87,0xff}, {0x5f,0xaf,0x00}, {0x5f,0xaf,0x5f},
    {0x5f,0xaf,0x87}, {0x5f,0xaf,0xaf}, {0x5f,0xaf,0xd7}, {0x5f,0xaf,0xff},
    {0x5f,0xd7,0x00}, {0x5f,0xd7,0x5f}, {0x5f,0xd7,0x87}, {0x5f,0xd7,0xaf},
    {0x5f,0xd7,0xd7}, {0x5f,0xd7,0xff}, {0x5f,0xff,0x00}, {0x5f,0xff,0x5f},
    {0x5f,0xff,0x87}, {0x5f,0xff,0xaf}, {0x5f,0xff,0xd7}, {0x5f,0xff,0xff},
    {0x87,0x00,0x00}, {0x87,0x00,0x5f}, {0x87,0x00,0x87}, {0x87,0x00,0xaf},
    {0x87,0x00,0xd7}, {0x87,0x00,0xff}, {0x87,0x5f,0x00}, {0x87,0x5f,0x5f},
    {0x87,0x5f,0x87}, {0x87,0x5f,0xaf}, {0x87,0x5f,0xd7}, {0x87,0x5f,0xff},
    {0x87,0x87,0x00}, {0x87,0x87,0x5f}, {0x87,0x87,0x87}, {0x87,0x87,0xaf},
    {0x87,0x87,0xd7}, {0x87,0x87,0xff}, {0x87,0xaf,0x00}, {0x87,0xaf,0x5f},
    {0x87,0xaf,0x87}, {0x87,0xaf,0xaf}, {0x87,0xaf,0xd7}, {0x87,0xaf,0xff},
    {0x87,0xd7,0x00}, {0x87,0xd7,0x5f}, {0x87,0xd7,0x87}, {0x87,0xd7,0xaf},
    {0x87,0xd7,0xd7}, {0x87,0xd7,0xff}, {0x87,0xff,0x00}, {0x87,0xff,0x5f},
    {0x87,0xff,0x87}, {0x87,0xff,0xaf}, {0x87,0xff,0xd7}, {0x87,0xff,0xff},
    {0xaf,0x00,0x00}, {0xaf,0x00,0x5f}, {0xaf,0x00,0x87}, {0xaf,0x00,0xaf},
    {0xaf,0x00,0xd7}, {0xaf,0x00,0xff}, {0xaf,0x5f,0x00}, {0xaf,0x5f,0x5f},
    {0xaf,0x5f,0x87}, {0xaf,0x5f,0xaf}, {0xaf,0x5f,0xd7}, {0xaf,0x5f,0xff},
    {0xaf,0x87,0x00}, {0xaf,0x87,0x5f}, {0xaf,0x87,0x87}, {0xaf,0x87,0xaf},
    {0xaf,0x87,0xd7}, {0xaf,0x87,0xff}, {0xaf,0xaf,0x00}, {0xaf,0xaf,0x5f},
    {0xaf,0xaf,0x87}, {0xaf,0xaf,0xaf}, {0xaf,0xaf,0xd7}, {0xaf,0xaf,0xff},
    {0xaf,0xd7,0x00}, {0xaf,0xd7,0x5f}, {0xaf,0xd7,0x87}, {0xaf,0xd7,0xaf},
    {0xaf,0xd7,0xd7}, {0xaf,0xd7,0xff}, {0xaf,0xff,0x00}, {0xaf,0xff,0x5f},
    {0xaf,0xff,0x87}, {0xaf,0xff,0xaf}, {0xaf,0xff,0xd7}, {0xaf,0xff,0xff},
    {0xd7,0x00,0x00}, {0xd7,0x00,0x5f}, {0xd7,0x00,0x87}, {0xd7,0x00,0xaf},
    {0xd7,0x00,0xd7}, {0xd7,0x00,0xff}, {0xd7,0x5f,0x00}, {0xd7,0x5f,0x5f},
    {0xd7,0x5f,0x87}, {0xd7,0x5f,0xaf}, {0xd7,0x5f,0xd7}, {0xd7,0x5f,0xff},
    {0xd7,0x87,0x00}, {0xd7,0x87,0x5f}, {0xd7,0x87,0x87}, {0xd7,0x87,0xaf},
    {0xd7,0x87,0xd7}, {0xd7,0x87,0xff}, {0xd7,0xaf,0x00}, {0xd7,0xaf,0x5f},
    {0xd7,0xaf,0x87}, {0xd7,0xaf,0xaf}, {0xd7,0xaf,0xd7}, {0xd7,0xaf,0xff},
    {0xd7,0xd7,0x00}, {0xd7,0xd7,0x5f}, {0xd7,0xd7,0x87}, {0xd7,0xd7,0xaf},
    {0xd7,0xd7,0xd7}, {0xd7,0xd7,0xff}, {0xd7,0xff,0x00}, {0xd7,0xff,0x5f},
    {0xd7,0xff,0x87}, {0xd7,0xff,0xaf}, {0xd7,0xff,0xd7}, {0xd7,0xff,0xff},
    {0xff,0x00,0x00}, {0xff,0x00,0x5f}, {0xff,0x00,0x87}, {0xff,0x00,0xaf},
    {0xff,0x00,0xd7}, {0xff,0x00,0xff}, {0xff,0x5f,0x00}, {0xff,0x5f,0x5f},
    {0xff,0x5f,0x87}, {0xff,0x5f,0xaf}, {0xff,0x5f,0xd7}, {0xff,0x5f,0xff},
    {0xff,0x87,0x00}, {0xff,0x87,0x5f}, {0xff,0x87,0x87}, {0xff,0x87,0xaf},
    {0xff,0x87,0xd7}, {0xff,0x87,0xff}, {0xff,0xaf,0x00}, {0xff,0xaf,0x5f},
    {0xff,0xaf,0x87}, {0xff,0xaf,0xaf}, {0xff,0xaf,0xd7}, {0xff,0xaf,0xff},
    {0xff,0xd7,0x00}, {0xff,0xd7,0x5f}, {0xff,0xd7,0x87}, {0xff,0xd7,0xaf},
    {0xff,0xd7,0xd7}, {0xff,0xd7,0xff}, {0xff,0xff,0x00}, {0xff,0xff,0x5f},
    {0xff,0xff,0x87}, {0xff,0xff,0xaf}, {0xff,0xff,0xd7}, {0xff,0xff,0xff},
    {0x08,0x08,0x08}, {0x12,0x12,0x12}, {0x1c,0x1c,0x1c}, {0x26,0x26,0x26},
    {0x30,0x30,0x30}, {0x3a,0x3a,0x3a}, {0x44,0x44,0x44}, {0x4e,0x4e,0x4e},
    {0x58,0x58,0x58}, {0x60,0x60,0x60}, {0x66,0x66,0x66}, {0x76,0x76,0x76},
    {0x80,0x80,0x80}, {0x8a,0x8a,0x8a}, {0x94,0x94,0x94}, {0x9e,0x9e,0x9e},
    {0xa8,0xa8,0xa8}, {0xb2,0xb2,0xb2}, {0xbc,0xbc,0xbc}, {0xc6,0xc6,0xc6},
    {0xd0,0xd0,0xd0}, {0xda,0xda,0xda}, {0xe4,0xe4,0xe4}, {0xee,0xee,0xee},
};

const std::initializer_list<HashMap<Kakoune::Color, int>::Item>
NCursesUI::Palette::default_colors = {
    { Color::Default,       -1 },
    { Color::Black,          0 },
    { Color::Red,            1 },
    { Color::Green,          2 },
    { Color::Yellow,         3 },
    { Color::Blue,           4 },
    { Color::Magenta,        5 },
    { Color::Cyan,           6 },
    { Color::White,          7 },
    { Color::BrightBlack,    8 },
    { Color::BrightRed,      9 },
    { Color::BrightGreen,   10 },
    { Color::BrightYellow,  11 },
    { Color::BrightBlue,    12 },
    { Color::BrightMagenta, 13 },
    { Color::BrightCyan,    14 },
    { Color::BrightWhite,   15 },
};

int NCursesUI::Palette::get_color(Color color)
{
    auto it = m_colors.find(color);
    if (it != m_colors.end())
        return it->value;
    else if (m_change_colors and can_change_color() and COLORS > 16)
    {
        kak_assert(color.color == Color::RGB);
        if (m_next_color > COLORS)
            m_next_color = 16;
        init_color(m_next_color,
                   color.r * 1000 / 255,
                   color.g * 1000 / 255,
                   color.b * 1000 / 255);
        m_colors[color] = m_next_color;
        return m_next_color++;
    }
    else
    {
        kak_assert(color.color == Color::RGB);
        int lowestDist = INT_MAX;
        int closestCol = -1;
        for (int i = 0; i < std::min(256, COLORS); ++i)
        {
            auto& col = builtin_colors[i];
            int dist = sq(color.r - col.r)
                     + sq(color.g - col.g)
                     + sq(color.b - col.b);
            if (dist < lowestDist)
            {
                lowestDist = dist;
                closestCol = i;
            }
        }
        return closestCol;
    }
}

int NCursesUI::Palette::get_color_pair(const Face& face)
{
    ColorPair colors{face.fg, face.bg};
    auto it = m_colorpairs.find(colors);
    if (it != m_colorpairs.end())
        return it->value;
    else
    {
        init_pair(m_next_pair, get_color(face.fg), get_color(face.bg));
        m_colorpairs[colors] = m_next_pair;
        return m_next_pair++;
    }
}

bool NCursesUI::Palette::set_change_colors(bool change_colors)
{
    bool reset = false;
    if (can_change_color() and m_change_colors != change_colors)
    {
        fputs("\033]104\007", stdout); // try to reset palette
        fflush(stdout);
        m_colorpairs.clear();
        m_colors = default_colors;
        m_next_color = 16;
        m_next_pair = 1;
        reset = true;
    }
    m_change_colors = change_colors;
    return reset;
}

static sig_atomic_t resize_pending = 0;
static sig_atomic_t sighup_raised = 0;

template<sig_atomic_t* signal_flag>
static void signal_handler(int)
{
    *signal_flag = 1;
    EventManager::instance().force_signal(0);
}

NCursesUI::NCursesUI()
    : m_cursor{CursorMode::Buffer, {}},
      m_stdin_watcher{0, FdEvents::Read,
                      [this](FDWatcher&, FdEvents, EventMode) {
        if (not m_on_key)
            return;

        while (auto key = get_next_key())
            m_on_key(*key);
      }},
      m_assistant(assistant_clippy)
{
    initscr();
    raw();
    noecho();
    nonl();
    curs_set(0);
    start_color();
    use_default_colors();
    set_escdelay(25);
    intrflush(nullptr, false);
    meta(nullptr, true);

    enable_mouse(true);

    set_signal_handler(SIGWINCH, &signal_handler<&resize_pending>);
    set_signal_handler(SIGHUP, &signal_handler<&sighup_raised>);

    check_resize(true);

    redraw(false);
}

NCursesUI::~NCursesUI()
{
    enable_mouse(false);
    if (can_change_color()) // try to reset palette
    {
        fputs("\033]104\007", stdout);
        fflush(stdout);
    }
    endwin();
    set_signal_handler(SIGWINCH, SIG_DFL);
    set_signal_handler(SIGCONT, SIG_DFL);
}

void NCursesUI::redraw(bool force)
{
    m_window.refresh(force);

    if (m_menu.columns != 0 or m_menu.pos.column > m_status_len)
        m_menu.refresh(false);

    m_info.refresh(false);

    Window screen{{}, static_cast<NCursesWin*>(newscr)};
    if (m_cursor.mode == CursorMode::Prompt)
        screen.move_cursor({m_status_on_top ? 0 : m_dimensions.line, m_cursor.coord.column});
    else
        screen.move_cursor(m_cursor.coord + content_line_offset());

    doupdate();
}

void NCursesUI::set_cursor(CursorMode mode, DisplayCoord coord)
{
    m_cursor = Cursor{mode, coord};
}

void NCursesUI::refresh(bool force)
{
    if (m_dirty or force)
        redraw(force);
    m_dirty = false;
}

static const DisplayLine empty_line = { String(" "), {} };

void NCursesUI::draw(const DisplayBuffer& display_buffer,
                     const Face& default_face,
                     const Face& padding_face)
{
    m_window.set_background_color(m_palette, default_face);

    check_resize();

    const DisplayCoord dim = dimensions();
    const LineCount line_offset = content_line_offset();
    LineCount line_index = line_offset;
    for (const DisplayLine& line : display_buffer.lines())
    {
        m_window.move_cursor(line_index);
        m_window.clear_to_end_of_line();
        m_window.draw_line(m_palette, line, 0, dim.column, default_face);
        ++line_index;
    }

    m_window.set_background_color(m_palette, padding_face);
    m_window.set_face(m_palette, padding_face, default_face);

    while (line_index < dim.line + line_offset)
    {
        m_window.move_cursor(line_index++);
        m_window.clear_to_end_of_line();
        m_window.add_str('~');
    }

    m_dirty = true;
}

void NCursesUI::draw_status(const DisplayLine& status_line,
                            const DisplayLine& mode_line,
                            const Face& default_face)
{
    const LineCount status_line_pos = m_status_on_top ? 0 : m_dimensions.line;
    m_window.move_cursor(status_line_pos);

    m_window.set_background_color(m_palette, default_face);
    m_window.clear_to_end_of_line();

    m_window.draw_line(m_palette, status_line, 0, m_dimensions.column, default_face);

    const auto mode_len = mode_line.length();
    m_status_len = status_line.length();
    const auto remaining = m_dimensions.column - m_status_len;
    if (mode_len < remaining)
    {
        ColumnCount col = m_dimensions.column - mode_len;
        m_window.move_cursor({status_line_pos, col});
        m_window.draw_line(m_palette, mode_line, col, m_dimensions.column, default_face);
    }
    else if (remaining > 2)
    {
        DisplayLine trimmed_mode_line = mode_line;
        trimmed_mode_line.trim(mode_len + 2 - remaining, remaining - 2);
        trimmed_mode_line.insert(trimmed_mode_line.begin(), { "…", {} });
        kak_assert(trimmed_mode_line.length() == remaining - 1);

        ColumnCount col = m_dimensions.column - remaining + 1;
        m_window.move_cursor({status_line_pos, col});
        m_window.draw_line(m_palette, trimmed_mode_line, col, m_dimensions.column, default_face);
    }

    if (m_set_title)
    {
        constexpr char suffix[] = " - Kakoune\007";
        char buf[4 + 511 + 2] = "\033]2;";
        // Fill title escape sequence buffer, removing non ascii characters
        auto buf_it = &buf[4], buf_end = &buf[4 + 511 - (sizeof(suffix) - 2)];
        for (auto& atom : mode_line)
        {
            const auto str = atom.content();
            for (auto it = str.begin(), end = str.end();
                 it != end and buf_it != buf_end; utf8::to_next(it, end))
                *buf_it++ = (*it >= 0x20 and *it <= 0x7e) ? *it : '?';
        }
        for (auto c : suffix)
            *buf_it++ = c;

        fputs(buf, stdout);
        fflush(stdout);
    }

    m_dirty = true;
}

void NCursesUI::check_resize(bool force)
{
    if (not force and not resize_pending)
        return;

    resize_pending = 0;

    const int fd = open("/dev/tty", O_RDWR);
    if (fd < 0)
        return;
    auto close_fd = on_scope_end([fd]{ ::close(fd); });

    winsize ws;
    if (::ioctl(fd, TIOCGWINSZ, &ws) != 0)
        return;

    const bool info = (bool)m_info;
    const bool menu = (bool)m_menu;
    if (m_window) m_window.destroy();
    if (info) m_info.destroy();
    if (menu) m_menu.destroy();

    resize_term(ws.ws_row, ws.ws_col);

    m_window.create({0, 0}, {ws.ws_row, ws.ws_col});
    kak_assert(m_window);
    keypad(m_window.win, not m_builtin_key_parser);

    m_dimensions = DisplayCoord{ws.ws_row-1, ws.ws_col};

    if (char* csr = tigetstr((char*)"csr"))
        putp(tparm(csr, 0, ws.ws_row));

    if (menu)
    {
        auto items = std::move(m_menu.items);
        menu_show(items, m_menu.anchor, m_menu.fg, m_menu.bg, m_menu.style);
    }
    if (info)
        info_show(m_info.title, m_info.content, m_info.anchor, m_info.face, m_info.style);

    set_resize_pending();
    clearok(curscr, true);
    werase(curscr);
}

Optional<Key> NCursesUI::get_next_key()
{
    if (sighup_raised)
    {
        set_signal_handler(SIGWINCH, SIG_DFL);
        set_signal_handler(SIGCONT, SIG_DFL);
        m_window.invalidate();
        m_stdin_watcher.disable();
        return {};
    }

    check_resize();

    if (m_resize_pending)
    {
        m_resize_pending = false;
        return resize(dimensions());
    }

    m_window.set_blocking(false);
    const int c = m_window.get_char();
    m_window.set_blocking(true);

    if (c == ERR)
        return {};

    if (c == KEY_MOUSE)
    {
        MEVENT ev;
        if (getmouse(&ev) == OK)
        {
            const auto mask = ev.bstate;
            auto coord = encode_coord({ ev.y - content_line_offset(), ev.x });

            Key::Modifiers mod{};
            if (mask & BUTTON_CTRL)
                mod |= Key::Modifiers::Control;
            if (mask & BUTTON_ALT)
                mod |= Key::Modifiers::Alt;

            if (BUTTON_PRESS(mask, 1))
                return Key{mod | Key::Modifiers::MousePressLeft, coord};
            if (BUTTON_PRESS(mask, 3))
                return Key{mod | Key::Modifiers::MousePressRight, coord};
            if (BUTTON_RELEASE(mask, 1))
                return Key{mod | Key::Modifiers::MouseReleaseLeft, coord};
            if (BUTTON_RELEASE(mask, 3))
                return Key{mod | Key::Modifiers::MouseReleaseRight, coord};
            if (BUTTON_PRESS(mask, m_wheel_down_button))
                return Key{mod | Key::Modifiers::Scroll, static_cast<Codepoint>(m_wheel_scroll_amount)};
            if (BUTTON_PRESS(mask, m_wheel_up_button))
                return Key{mod | Key::Modifiers::Scroll, static_cast<Codepoint>(-m_wheel_scroll_amount)};
            return Key{mod | Key::Modifiers::MousePos, coord};
        }
    }

    auto parse_key = [this](int c) -> Optional<Key> {
        switch (c)
        {
        case KEY_BACKSPACE: case 127: return {Key::Backspace};
        case KEY_DC: return {Key::Delete};
        case KEY_SDC: return shift(Key::Delete);
        case KEY_UP: return {Key::Up};
        case KEY_SR: return shift(Key::Up);
        case KEY_DOWN: return {Key::Down};
        case KEY_SF: return shift(Key::Down);
        case KEY_LEFT: return {Key::Left};
        case KEY_SLEFT: return shift(Key::Left);
        case KEY_RIGHT: return {Key::Right};
        case KEY_SRIGHT: return shift(Key::Right);
        case KEY_PPAGE: return {Key::PageUp};
        case KEY_SPREVIOUS: return shift(Key::PageUp);
        case KEY_NPAGE: return {Key::PageDown};
        case KEY_SNEXT: return shift(Key::PageDown);
        case KEY_HOME: return {Key::Home};
        case KEY_SHOME: return shift(Key::Home);
        case KEY_END: return {Key::End};
        case KEY_SEND: return shift(Key::End);
        case KEY_IC: return {Key::Insert};
        case KEY_SIC: return shift(Key::Insert);
        case KEY_BTAB: return shift(Key::Tab);
        case KEY_RESIZE: return resize(dimensions());
        }

        if (c > 0 and c < 27)
        {
            if (c == control('m') or c == control('j'))
                return {Key::Return};
            if (c == control('i'))
                return {Key::Tab};
            if (c == control('h'))
                return {Key::Backspace};
            if (c == control('z'))
            {
                bool mouse_enabled = m_mouse_enabled;
                enable_mouse(false);

                kill(0, SIGTSTP); // We suspend at this line

                check_resize(true);
                enable_mouse(mouse_enabled);
                return {};
            }
            return ctrl(Codepoint(c) - 1 + 'a');
        }

        for (int i = 0; i < 12; ++i)
        {
            if (c == KEY_F(i + 1))
                return {Key::F1 + i};
            if (c == KEY_F(m_shift_function_key + i + 1))
                return shift(Key::F1 + i);
        }

        if (c >= 0 and c < 256)
        {
           ungetch(c);
           struct getch_iterator
           {
               getch_iterator(Window& win) : window(win) {}
               int operator*() { return window.get_char(); }
               getch_iterator& operator++() { return *this; }
               getch_iterator& operator++(int) { return *this; }
               bool operator== (const getch_iterator&) const { return false; }

               Window& window;
           };
           return Key{utf8::codepoint(getch_iterator{m_window},
                                      getch_iterator{m_window})};
        }
        return {};
    };

    constexpr auto direction = make_array({Key::Up, Key::Down, Key::Right, Key::Left, Key::Home, Key::End});
    constexpr auto special = make_array({Key::Insert, Key::Delete, Key::NamedKey{}, Key::PageUp, Key::PageDown, Key::Home, Key::End, Key::NamedKey{}, Key::NamedKey{},
                                         Key::F1, Key::F2, Key::F3, Key::F4, Key::NamedKey{}, Key::F5, Key::F6, Key::F7, Key::F8, Key::F9, Key::F10, Key::NamedKey{}, Key::F11, Key::F12});
    auto parse_csi = [this, &direction, &special]() -> Optional<Key> {
        int params[16] = {};
        int c = m_window.get_char();
        char private_mode = 0;
        if (c == '?' or c == '<' or c == '=' or c == '>')
        {
            private_mode = c;
            c = m_window.get_char();
        }
        for (int count = 0; count < 16 and c >= 0x30 && c <= 0x3f; c = m_window.get_char())
        {
            if ('0' <= 'c' and c <= '9')
                params[count] = params[count] * 10 + c - '0';
            else if (c == ';')
                ++count;
            else
                return {};
        }
        if (c < 0x40 or c > 0x7e)
            return {};

        auto parse_mask = [](int mask) {
            mask = std::max(0, mask - 1);
            Key::Modifiers mod = Key::Modifiers::None;
            if (mask & 1)
                mod |= Key::Modifiers::Shift;
            if (mask & 2)
                mod |= Key::Modifiers::Alt;
            if (mask & 4)
                mod |= Key::Modifiers::Control;
            return mod;
        };

        auto mouse_button = [this](Key::Modifiers mod, Codepoint coord, bool left, bool release) {
            auto mask = left ? 0x1 : 0x2;
            if (not release)
            {
                mod |= (m_mouse_state & mask) ? Key::Modifiers::MousePos : (left ? Key::Modifiers::MousePressLeft : Key::Modifiers::MousePressRight);
                m_mouse_state |= mask;
            }
            else
            {
                mod |= left ? Key::Modifiers::MouseReleaseLeft : Key::Modifiers::MouseReleaseRight;
                m_mouse_state &= ~mask;
            }
            return Key{mod, coord};
        };

        if (c >= 'A' and c <= 'F')
            return Key{parse_mask(params[1]), direction[c - 'A']};
        if (c == '~' and 2 <= params[0] and params[0] <= 24)
            return Key{parse_mask(params[1]), special[params[0] - 2]};
        if (c == 'Z')
            return shift(Key::Tab);
        if (c == 'I')
            return {Key::FocusIn};
        if (c == 'O')
            return {Key::FocusOut};
        if ((c == 'M' or c == 'm') and private_mode == '<')
        {
            auto coord = encode_coord({params[2] - content_line_offset() - 1, params[1] - 1});
            Key::Modifiers mod = parse_mask(1 + ((params[0] >> 2) & 0x7));
            switch (params[0] & 0x43)
            {
            case 0:
                return mouse_button(mod, coord, true, c == 'm');
            case 2:
                return mouse_button(mod, coord, false, c == 'm');
            case 64:
                return Key{mod | Key::Modifiers::Scroll,
                           static_cast<Codepoint>(-m_wheel_scroll_amount)};
            case 65:
                return Key{mod | Key::Modifiers::Scroll,
                           static_cast<Codepoint>(m_wheel_scroll_amount)};
            }
        }
        if (c == 'M')
        {
            const Codepoint b = m_window.get_char() - 32;
            const int x = m_window.get_char() - 32 - 1;
            const int y = m_window.get_char() - 32 - 1;
            auto coord = encode_coord({y - content_line_offset(), x});
            Key::Modifiers mod = parse_mask(1 + ((b >> 2) & 0x7));
            switch (b & 0x43)
            {
            case 0:
                return mouse_button(mod, coord, true, false);
            case 2:
                return mouse_button(mod, coord, false, false);
            case 3:
                if (m_mouse_state & 0x1)
                    return mouse_button(mod, coord, true, true);
                else if (m_mouse_state & 0x2)
                    return mouse_button(mod, coord, false, true);
                break;
            case 64:
                return Key{mod | Key::Modifiers::Scroll,
                           static_cast<Codepoint>(-m_wheel_scroll_amount)};
            case 65:
                return Key{mod | Key::Modifiers::Scroll,
                           static_cast<Codepoint>(m_wheel_scroll_amount)};
            }
            return Key{Key::Modifiers::MousePos, coord};
        }
        return {};
    };

    if (c == 27)
    {
        m_window.set_blocking(false);
        const int new_c = m_window.get_char();
        if (new_c == '[') // potential CSI
        {
            if (auto key = parse_csi())
                return key;
        }
        m_window.set_blocking(true);

        if (auto key = parse_key(new_c))
            return alt(*key);
        else
            return {Key::Escape};
    }

    return parse_key(c);
}

template<typename T>
T div_round_up(T a, T b)
{
    return (a - T(1)) / b + T(1);
}

void NCursesUI::draw_menu()
{
    // menu show may have not created the window if it did not fit.
    // so be tolerant.
    if (not m_menu)
        return;

    m_menu.set_face(m_palette, m_menu.bg, {});
    m_menu.set_background_color(m_palette, m_menu.bg);

    const int item_count = (int)m_menu.items.size();
    if (m_menu.columns == 0)
    {
        const auto win_width = m_menu.size.column - 4;
        kak_assert(m_menu.size.line == 1);
        ColumnCount pos = 0;

        m_menu.move_cursor({0, 0});
        m_menu.add_str(m_menu.first_item > 0 ? "< " : "  ");

        int i = m_menu.first_item;
        for (; i < item_count and pos < win_width; ++i)
        {
            const DisplayLine& item = m_menu.items[i];
            const ColumnCount item_width = item.length();
            m_menu.draw_line(m_palette, item, 0, win_width - pos,
                             i == m_menu.selected_item ? m_menu.fg : m_menu.bg);

            if (item_width > win_width - pos)
                m_menu.add_str("…");
            else
            {
                m_menu.set_face(m_palette, m_menu.bg, {});
                m_menu.add_str(String{" "});
            }
            pos += item_width + 1;
        }

        m_menu.set_face(m_palette, m_menu.bg, {});
        if (pos <= win_width)
            m_menu.add_str(String{' ', win_width - pos + 1});
        m_menu.add_str(i == item_count ? " " : ">");
        m_dirty = true;
        return;
    }

    const LineCount menu_lines = div_round_up(item_count, m_menu.columns);
    const LineCount win_height = m_menu.size.line;
    kak_assert(win_height <= menu_lines);

    const ColumnCount column_width = (m_menu.size.column - 1) / m_menu.columns;

    const LineCount mark_height = min(div_round_up(sq(win_height), menu_lines),
                                      win_height);

    const int menu_cols = div_round_up(item_count, (int)m_menu.size.line);
    const int first_col = m_menu.first_item / (int)m_menu.size.line;

    const LineCount mark_line = (win_height - mark_height) * first_col / max(1, menu_cols - m_menu.columns);

    for (auto line = 0_line; line < win_height; ++line)
    {
        m_menu.move_cursor(line);
        for (int col = 0; col < m_menu.columns; ++col)
        {
            int item_idx = (first_col + col) * (int)m_menu.size.line + (int)line;
            if (item_idx >= item_count)
                continue;

            const DisplayLine& item = m_menu.items[item_idx];
            m_menu.draw_line(m_palette, item, 0, column_width,
                             item_idx == m_menu.selected_item ? m_menu.fg : m_menu.bg);
            const ColumnCount pad = column_width - item.length();
            m_menu.add_str(String{' ', pad});
        }
        const bool is_mark = line >= mark_line and
                             line < mark_line + mark_height;
        m_menu.clear_to_end_of_line();
        m_menu.move_cursor({line, m_menu.size.column - 1});
        m_menu.set_face(m_palette, m_menu.bg, {});
        m_menu.add_str(is_mark ? "█" : "░");
    }
    m_dirty = true;
}

static LineCount height_limit(MenuStyle style)
{
    switch (style)
    {
        case MenuStyle::Inline: return 10_line;
        case MenuStyle::Prompt: return 10_line;
        case MenuStyle::Search: return 3_line;
    }
    kak_assert(false);
    return 0_line;
}

void NCursesUI::menu_show(ConstArrayView<DisplayLine> items,
                          DisplayCoord anchor, Face fg, Face bg,
                          MenuStyle style)
{
    if (m_menu)
    {
        m_window.mark_dirty(m_menu.pos.line, m_menu.size.line);
        m_menu.destroy();
        m_dirty = true;
    }

    m_menu.fg = fg;
    m_menu.bg = bg;
    m_menu.style = style;
    m_menu.anchor = anchor;

    if (m_dimensions.column <= 2)
        return;

    const int item_count = items.size();
    m_menu.items.clear(); // make sure it is empty
    m_menu.items.reserve(item_count);
    const auto longest = accumulate(items | transform(&DisplayLine::length),
                                    1_col, [](auto&& lhs, auto&& rhs) { return std::max(lhs, rhs); });

    const ColumnCount max_width = m_dimensions.column - 1;
    const bool is_inline = style == MenuStyle::Inline;
    const bool is_search = style == MenuStyle::Search;
    m_menu.columns = is_search ? 0 : (is_inline ? 1 : max((int)(max_width / (longest+1)), 1));

    const LineCount max_height = min(height_limit(style), max(anchor.line, m_dimensions.line - anchor.line - 1));
    const LineCount height = is_search ?
        1 : (min<LineCount>(max_height, div_round_up(item_count, m_menu.columns)));

    const ColumnCount maxlen = (m_menu.columns > 1 and item_count > 1) ?
        max_width / m_menu.columns - 1 : max_width;

    for (auto& item : items)
    {
        m_menu.items.push_back(item);
        m_menu.items.back().trim(0, maxlen);
        kak_assert(m_menu.items.back().length() <= maxlen);
    }

    if (is_inline)
        anchor.line += content_line_offset();

    LineCount line = anchor.line + 1;
    ColumnCount column = std::max(0_col, std::min(anchor.column, m_dimensions.column - longest - 1));
    if (is_search)
    {
        line = m_status_on_top ? 0_line : m_dimensions.line;
        column = m_dimensions.column / 2;
    }
    else if (not is_inline)
        line = m_status_on_top ? 1_line : m_dimensions.line - height;
    else if (line + height > m_dimensions.line)
        line = anchor.line - height;

    const auto width = is_search ? m_dimensions.column - m_dimensions.column / 2
                                 : (is_inline ? min(longest+1, m_dimensions.column)
                                              : m_dimensions.column);
    m_menu.create({line, column}, {height, width});
    m_menu.selected_item = item_count;
    m_menu.first_item = 0;

    draw_menu();

    if (m_info)
        info_show(m_info.title, m_info.content,
                  m_info.anchor, m_info.face, m_info.style);
}

void NCursesUI::menu_select(int selected)
{
    const int item_count = m_menu.items.size();
    if (selected < 0 or selected >= item_count)
    {
        m_menu.selected_item = -1;
        m_menu.first_item = 0;
    }
    else if (m_menu.columns == 0) // Do not columnize
    {
        m_menu.selected_item = selected;
        const ColumnCount width = m_menu.size.column - 3;
        int first = 0;
        ColumnCount item_col = 0;
        for (int i = 0; i <= selected; ++i)
        {
            const ColumnCount item_width = m_menu.items[i].length() + 1;
            if (item_col + item_width > width)
            {
                first = i;
                item_col = item_width;
            }
            else
                item_col += item_width;
        }
        m_menu.first_item = first;
    }
    else
    {
        m_menu.selected_item = selected;
        const int menu_cols = div_round_up(item_count, (int)m_menu.size.line);
        const int first_col = m_menu.first_item / (int)m_menu.size.line;
        const int selected_col = m_menu.selected_item / (int)m_menu.size.line;
        if (selected_col < first_col)
            m_menu.first_item = selected_col * (int)m_menu.size.line;
        if (selected_col >= first_col + m_menu.columns)
            m_menu.first_item = min(selected_col, menu_cols - m_menu.columns) * (int)m_menu.size.line;
    }
    draw_menu();
}

void NCursesUI::menu_hide()
{
    if (not m_menu)
        return;

    m_menu.items.clear();
    m_window.mark_dirty(m_menu.pos.line, m_menu.size.line);
    m_menu.destroy();
    m_dirty = true;

    // Recompute info as it does not have to avoid the menu anymore
    if (m_info)
        info_show(m_info.title, m_info.content, m_info.anchor, m_info.face, m_info.style);
}

static DisplayCoord compute_pos(DisplayCoord anchor, DisplayCoord size,
                                NCursesUI::Rect rect, NCursesUI::Rect to_avoid,
                                bool prefer_above)
{
    DisplayCoord pos;
    if (prefer_above)
    {
        pos = anchor - DisplayCoord{size.line};
        if (pos.line < 0)
            prefer_above = false;
    }
    auto rect_end = rect.pos + rect.size;
    if (not prefer_above)
    {
        pos = anchor + DisplayCoord{1_line};
        if (pos.line + size.line > rect_end.line)
            pos.line = max(rect.pos.line, anchor.line - size.line);
    }
    if (pos.column + size.column > rect_end.column)
        pos.column = max(rect.pos.column, rect_end.column - size.column);

    if (to_avoid.size != DisplayCoord{})
    {
        DisplayCoord to_avoid_end = to_avoid.pos + to_avoid.size;

        DisplayCoord end = pos + size;

        // check intersection
        if (not (end.line < to_avoid.pos.line or end.column < to_avoid.pos.column or
                 pos.line > to_avoid_end.line or pos.column > to_avoid_end.column))
        {
            pos.line = min(to_avoid.pos.line, anchor.line) - size.line;
            // if above does not work, try below
            if (pos.line < 0)
                pos.line = max(to_avoid_end.line, anchor.line);
        }
    }

    return pos;
}

struct InfoBox
{
    DisplayCoord size;
    Vector<String> contents;
};

InfoBox make_info_box(StringView title, StringView message, ColumnCount max_width,
                      ConstArrayView<StringView> assistant)
{
    DisplayCoord assistant_size;
    if (not assistant.empty())
        assistant_size = { (int)assistant.size(), assistant[0].column_length() };

    InfoBox result{};

    const ColumnCount max_bubble_width = max_width - assistant_size.column - 6;
    if (max_bubble_width < 4)
        return result;

    Vector<StringView> lines = wrap_lines(message, max_bubble_width);

    ColumnCount bubble_width = title.column_length() + 2;
    for (auto& line : lines)
        bubble_width = max(bubble_width, line.column_length());

    auto line_count = max(assistant_size.line-1, LineCount{(int)lines.size()} + 2);
    result.size = DisplayCoord{line_count, bubble_width + assistant_size.column + 4};
    const auto assistant_top_margin = (line_count - assistant_size.line+1) / 2;
    for (LineCount i = 0; i < line_count; ++i)
    {
        String line;
        constexpr Codepoint dash{L'─'};
        if (not assistant.empty())
        {
            if (i >= assistant_top_margin)
                line += assistant[(int)min(i - assistant_top_margin, assistant_size.line-1)];
            else
                line += assistant[(int)assistant_size.line-1];
        }
        if (i == 0)
        {
            if (title.empty())
                line += "╭─" + String{dash, bubble_width} + "─╮";
            else
            {
                auto dash_count = bubble_width - title.column_length() - 2;
                String left{dash, dash_count / 2};
                String right{dash, dash_count - dash_count / 2};
                line += "╭─" + left + "┤" + title +"├" + right +"─╮";
            }
        }
        else if (i < lines.size() + 1)
        {
            auto& info_line = lines[(int)i - 1];
            const ColumnCount padding = bubble_width - info_line.column_length();
            line += "│ " + info_line + String{' ', padding} + " │";
        }
        else if (i == lines.size() + 1)
            line += "╰─" + String(dash, bubble_width) + "─╯";

        result.contents.push_back(std::move(line));
    }
    return result;
}

InfoBox make_simple_info_box(StringView contents, ColumnCount max_width)
{
    InfoBox info_box{};
    for (auto& line : wrap_lines(contents, max_width))
    {
        ++info_box.size.line;
        info_box.size.column = std::max(line.column_length(), info_box.size.column);
        info_box.contents.push_back(line.str());
    }
    return info_box;
}

void NCursesUI::info_show(StringView title, StringView content,
                          DisplayCoord anchor, Face face, InfoStyle style)
{
    info_hide();

    m_info.title = title.str();
    m_info.content = content.str();
    m_info.anchor = anchor;
    m_info.face = face;
    m_info.style = style;

    const Rect rect = {content_line_offset(), m_dimensions};
    InfoBox info_box;
    if (style == InfoStyle::Prompt)
    {
        info_box = make_info_box(m_info.title, m_info.content, m_dimensions.column, m_assistant);
        anchor = DisplayCoord{m_status_on_top ? 0 : m_dimensions.line, m_dimensions.column-1};
        anchor = compute_pos(anchor, info_box.size, rect, m_menu, style == InfoStyle::InlineAbove);
    }
    else if (style == InfoStyle::Modal)
    {
        info_box = make_info_box(m_info.title, m_info.content, m_dimensions.column, {});
        auto half = [](const DisplayCoord& c) { return DisplayCoord{c.line / 2, c.column / 2}; };
        anchor = rect.pos + half(rect.size) - half(info_box.size);
    }
    else if (style == InfoStyle::MenuDoc)
    {
        if (not m_menu)
            return;

        const auto right_max_width = m_dimensions.column - (m_menu.pos.column + m_menu.size.column);
        const auto left_max_width = m_menu.pos.column;
        const auto max_width = std::max(right_max_width, left_max_width);
        if (max_width < 4)
            return;

        info_box = make_simple_info_box(m_info.content, max_width);
        anchor.line = m_menu.pos.line;
        if (info_box.size.column <= right_max_width or right_max_width >= left_max_width)
            anchor.column = m_menu.pos.column + m_menu.size.column;
        else
            anchor.column = m_menu.pos.column - info_box.size.column;
    }
    else
    {
        const ColumnCount max_width = m_dimensions.column - anchor.column;
        if (max_width < 4)
            return;

        info_box = make_simple_info_box(m_info.content, max_width);
        anchor = compute_pos(anchor, info_box.size, rect, m_menu, style == InfoStyle::InlineAbove);

        anchor.line += content_line_offset();
    }

    // The info box does not fit
    if (anchor < rect.pos or anchor + info_box.size > rect.pos + rect.size)
        return;

    m_info.create(anchor, info_box.size);

    m_info.set_background_color(m_palette, face);
    for (auto line = 0_line; line < info_box.size.line; ++line)
    {
        m_info.move_cursor(line);
        m_info.clear_to_end_of_line();
        m_info.add_str(info_box.contents[(int)line]);
    }
    m_dirty = true;
}

void NCursesUI::info_hide()
{
    if (not m_info)
        return;
    m_window.mark_dirty(m_info.pos.line, m_info.size.line);
    m_info.destroy();
    m_dirty = true;
}

void NCursesUI::set_on_key(OnKeyCallback callback)
{
    m_on_key = std::move(callback);
    EventManager::instance().force_signal(0);
}

DisplayCoord NCursesUI::dimensions()
{
    return m_dimensions;
}

LineCount NCursesUI::content_line_offset() const
{
    return m_status_on_top ? 1 : 0;
}

void NCursesUI::set_resize_pending()
{
    m_resize_pending = true;
    EventManager::instance().force_signal(0);
}

void NCursesUI::abort()
{
    endwin();
}

void NCursesUI::enable_mouse(bool enabled)
{
    if (enabled == m_mouse_enabled)
        return;

    m_mouse_enabled = enabled;
    if (enabled)
    {
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
        mouseinterval(0);
        // force SGR mode
        if (m_builtin_key_parser)
            fputs("\033[?1006h", stdout);
        // force enable report focus events
        fputs("\033[?1004h", stdout);
        // enable mouse
        fputs("\033[?1000h", stdout);
        // force enable report mouse position
        fputs("\033[?1002h", stdout);
    }
    else
    {
        mousemask(0, nullptr);
        fputs("\033[?1002l", stdout);
        fputs("\033[?1000l", stdout);
        fputs("\033[?1004l", stdout);
        fputs("\033[?1006l", stdout);
    }
    fflush(stdout);
}

void NCursesUI::set_ui_options(const Options& options)
{
    {
        auto it = options.find("ncurses_assistant"_sv);
        if (it == options.end() or it->value == "clippy")
            m_assistant = assistant_clippy;
        else if (it->value == "cat")
            m_assistant = assistant_cat;
        else if (it->value == "dilbert")
            m_assistant = assistant_dilbert;
        else if (it->value == "none" or it->value == "off")
            m_assistant = ConstArrayView<StringView>{};
    }

    {
        auto it = options.find("ncurses_status_on_top"_sv);
        m_status_on_top = it != options.end() and
            (it->value == "yes" or it->value == "true");
    }

    {
        auto it = options.find("ncurses_set_title"_sv);
        m_set_title = it == options.end() or
            (it->value == "yes" or it->value == "true");
    }

    {
        auto it = options.find("ncurses_shift_function_key"_sv);
        m_shift_function_key = it != options.end() ?
            str_to_int_ifp(it->value).value_or(default_shift_function_key)
          : default_shift_function_key;
    }

    {
        auto it = options.find("ncurses_change_colors"_sv);
        if (m_palette.set_change_colors(it == options.end() or
                                        (it->value == "yes" or it->value == "true")))
        {
            m_window.m_active_pair = -1;
            m_menu.m_active_pair = -1;
            m_info.m_active_pair = -1;
        }
    }

    {
        auto enable_mouse_it = options.find("ncurses_enable_mouse"_sv);
        enable_mouse(enable_mouse_it == options.end() or
                     enable_mouse_it->value == "yes" or
                     enable_mouse_it->value == "true");

        auto wheel_up_it = options.find("ncurses_wheel_up_button"_sv);
        m_wheel_up_button = wheel_up_it != options.end() ?
            str_to_int_ifp(wheel_up_it->value).value_or(4) : 4;

        auto wheel_down_it = options.find("ncurses_wheel_down_button"_sv);
        m_wheel_down_button = wheel_down_it != options.end() ?
            str_to_int_ifp(wheel_down_it->value).value_or(5) : 5;

        auto wheel_scroll_amount_it = options.find("ncurses_wheel_scroll_amount"_sv);
        m_wheel_scroll_amount = wheel_scroll_amount_it != options.end() ?
            str_to_int_ifp(wheel_scroll_amount_it->value).value_or(3) : 3;
    }

    {
        auto builtin_key_parser_it = options.find("ncurses_builtin_key_parser"_sv);
        bool mouse_enabled = m_mouse_enabled;
        enable_mouse(false);
        m_builtin_key_parser = builtin_key_parser_it != options.end() and
                               (builtin_key_parser_it->value == "yes" or
                                builtin_key_parser_it->value == "true");
        keypad(m_window.win, not m_builtin_key_parser);
        enable_mouse(mouse_enabled);
    }
}

}
