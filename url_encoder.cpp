#include "url_encoder.h"
#include <array>

namespace url_encoder
{

constexpr auto unreserved_table = []()
{
    std::array<bool, 256> table{};
    for (int i = 'A'; i <= 'Z'; ++i) table[i] = true;
    for (int i = 'a'; i <= 'z'; ++i) table[i] = true;
    for (int i = '0'; i <= '9'; ++i) table[i] = true;
    table['-'] = true;
    table['.'] = true;
    table['_'] = true;
    table['~'] = true;
    return table;
}();

constexpr auto sub_delim_table = []()
{
    std::array<bool, 256> table{};
    table['!'] = true;
    table['$'] = true;
    table['&'] = true;
    table['\''] = true;
    table['('] = true;
    table[')'] = true;
    table['*'] = true;
    table['+'] = true;
    table[','] = true;
    table[';'] = true;
    table['='] = true;
    return table;
}();

constexpr bool is_unreserved(unsigned char c) noexcept
{
    return unreserved_table[c];
}

constexpr bool is_sub_delim(unsigned char c) noexcept
{
    return sub_delim_table[c];
}

static constexpr bool is_pchar(char c)
{
    return is_unreserved(c) || is_sub_delim(c) || c == ':' || c == '@';
}

static bool is_allowed_for_part(char c, part p)
{
    switch (p)
    {
    case part::host:
        // RFC 3986 host = IP-literal / IPv4address / reg-name
        // reg-name allows unreserved + sub-delims; IP-literal is "[" IPv6addr "]" (and IPvFuture)
        return is_unreserved(c) || is_sub_delim(c) || c == '[' || c == ']' || c == ':';
    case part::path:
        if (c == '/')
            return true;
        [[fallthrough]];
    case part::path_segment:
        if (c == '&' || c == '=')
            return false;
        return is_pchar(c);
    case part::query_key:
        if (c == '&' || c == '=' || c == '/' || c == '?')  // `/` and `?` are encoded to be on a safe side
            return false;
        return is_pchar(c);
    case part::query_value:
        if (c == '&' || c == '=')
            return false;
        [[fallthrough]];
    case part::fragment:
        return is_pchar(c) || c == '/' || c == '?';
    case part::user_info:
        return is_unreserved(c) || is_sub_delim(c);
    case part::form_component:
    default:
        return is_unreserved(c);
    }
}

std::pair<size_t, bool> append(char *dst, size_t dst_size, std::string_view src, mode m)
{
    if (dst_size < src.size())
        return {0, false};
    char *pos = dst;
    char *end = dst + dst_size;
    bool encode_asterisk = (m.m & modifier::encode_asterisk);

    enum class space_mode { percent, plus };
    space_mode mode;
    if ((m.m & modifier::space_as_plus))
        mode = space_mode::plus;
    else if ((m.m & modifier::space_as_percent))
        mode = space_mode::percent;
    else
        mode = (m.p == part::form_component) ? space_mode::plus : space_mode::percent;

    auto should_encode = [&](char &c) -> bool
    {
        if (c == ' ')
        {
            if (mode == space_mode::plus)
                c = '+';  // durty hack
            return mode != space_mode::plus;
        }

        if (c == '*')
            return encode_asterisk;
        return !is_allowed_for_part(c, m.p);
    };

    constexpr char hexmap[] = "0123456789ABCDEF";
    for (char c : src)
    {
        if (should_encode(c))
        {
            if (end - pos < 3)
                return {static_cast<size_t>(pos - dst), false};

            *pos++ = '%';
            *pos++ = hexmap[static_cast<uint8_t>(c) >> 4];
            *pos++ = hexmap[c & 0xF];
        }
        else
        {
            if (pos == end)
                return {static_cast<size_t>(pos - dst), false};
            *pos++ = c;
        }
    }

    return {static_cast<size_t>(pos - dst), true};
}

mode::mode(part p, uint16_t m) : p(p), m(m)
{
}

}
