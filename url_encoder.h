#ifndef URL_ENCODER_H
#define URL_ENCODER_H

#include <cstdint>
#include <string_view>

namespace url_encoder
{

enum class part: uint16_t
{
    query_value = 1,
    query_key,
    path_segment,
    path,
    fragment,       // latest part after `#` (client-side processing)
    user_info,      // login or password (encode separately and concat via `:`)
    form_component, // application/x-www-form-urlencoded, space defaults to '+'
    host
};

enum modifier: uint16_t
{
    none = 0,
    space_as_plus    = 1u << 0,
    space_as_percent = 1u << 1,
    encode_asterisk  = 1u << 2
};

struct mode
{
    part p = part::form_component;
    uint16_t m = none;
    mode() = default;
    mode(part p, uint16_t m = none);
};

std::pair<size_t, bool> append(char *dst, size_t dst_size, std::string_view src, mode m = {});

template <typename C>
void append(C &dst, std::string_view src, mode m = {})
{
    size_t prev_size = dst.size();
    dst.resize(dst.size() + src.size() * 3);
    char *end = dst.data() + prev_size;
    auto res = append(end, dst.size() - prev_size, src, m);
    dst.resize(prev_size + res.first);
}

}


#endif // URL_ENCODER_H
