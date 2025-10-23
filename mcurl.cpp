#include "mcurl.h"
#include <algorithm>
#include <string.h>

std::atomic<int> mcurl_global::instances_counter{0};
bool ci_comparator::operator()(const std::string &a, const std::string &b) const
{
    return std::lexicographical_compare(
        a.begin(), a.end(),
        b.begin(), b.end(),
        [](const char &a, const char &b) -> bool {
            return tolower(a) < tolower(b);
        });
};

std::string encode1522(const std::string &value, bool wrap)
{
    /* man:
     * There are two limits that this specification places on the number of characters in a line.
     * Each line of characters MUST be no more than 998 characters, and SHOULD be
     * no more than 78 characters, excluding the CRLF.
    */
    // если длина строки менее 60 символов и состоит только из печатаемых ASCII-сиволов, то не кодируем
    if (value.length() < 60 &&
            std::find_if(value.begin(), value.end(), [](const char &c) -> bool
    { return c < 20 || c > 0x7E; }
                         ) == value.end())
        return value;

    if (!wrap)
        return "=?utf-8?B?" + base64::encode(value) + "?=";

    // разбивать закодированный в base64 результат НЕЛЬЗЯ,
    // т.к. кодируются БАЙТЫ, и если многобайтовый символ окажется на
    // разных строках, то из одной читаемой буквы получится два нечитаемых символа

    std::string res;
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < value.length(); ++i)
    {
        line += value[i];
        // переносим после 44-го байта (не буквы!)
        if (line.size() == 44)
        {
            // копируем байты до начала следующего UTF-8 символа
            while (i < value.length() - 1 && (value[i + 1] & 0xC0) != 0xC0)
                line += value[++i];
            lines.push_back(std::move(line));
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));

    for (const std::string &l: lines)
    {
        // перенос
        if (!res.empty())
            res += "\r\n ";

        res += "=?utf-8?B?" + base64::encode(l) + "?=";
    }
    return res;
}

mcurl_global::mcurl_global()
{
    if (!instances_counter.load())
    {
        CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (res)
            throw std::runtime_error(curl_easy_strerror(res));
    }
    ++instances_counter;
}

mcurl_global::~mcurl_global()
{
    --instances_counter;
    if (!instances_counter.load())
        curl_global_cleanup();
}

mcurl_content_part mcurl_content_part::file(std::string_view file_path, std::string_view destination_file_name)
{
    mcurl_content_part p;
    p.source = file_path;
    if (destination_file_name.empty())
    {
        size_t pos = file_path.find_last_of("/\\");
        if (pos == std::string::npos)
            p.filename = file_path;
        else
            p.filename = file_path.substr(pos + 1);
    }
    else
    {
        p.filename = destination_file_name;
    }
    return p;
}

mcurl_content_part mcurl_content_part::file_from_buffer(std::string_view buffer, std::string_view destination_file_name)
{
    mcurl_content_part p;
    p.source_type = source_type_t::Buffer;
    p.source = buffer;
    if (!destination_file_name.empty())
        p.filename = destination_file_name;

    return p;
}

mcurl_content_part mcurl_content_part::form_data(std::string_view name, std::string_view buffer)
{
    mcurl_content_part p;
    p.source_type = source_type_t::Buffer;
    p.disposition = disposition_t::FormData;
    p.source = buffer;
    p.name = name;
    return p;
}

smtp_request::proto_state::~proto_state()
{
    // задание для curl'а недоступно пользователю после помещения его в очередь
    // методом enqueue(Job &&), не получится снаружи менять поля задания и
    // косвенно воздействовать на внутренние переменные вроде curl_header, поэтому
    // всё дотерпит до деструктора Job
    if (curl_recipients)
        curl_slist_free_all(curl_recipients);
}

// make sure dst has `src.size()*3` bytes available
size_t append_url_encoded(char *dst, std::string_view src, bool asterisk2hex, bool space2hex)
{
    static char tbl[256] = {0x7f};
    if (tbl[0] == 0x7f)
    {
        for (size_t i = 0; i < 256; i++)
            tbl[i] = isalnum(i) || i == '~' || i == '-' || i == '.' || i == '_' ? i : 0;
    }

    char *end = dst;
    auto put_hex = [&end](char c)
    {
        constexpr char hexmap[] = "0123456789ABCDEF";
        *end++ = '%';
        *end++ = hexmap[static_cast<uint8_t>(c) >> 4];
        *end++ = hexmap[c & 0xF];
    };

    for (auto c: src)
    {
        switch (c)
        {
        case '*':
            if (asterisk2hex)
                put_hex(c);
            else
                *end++ = c;
            break;
        case ' ':
            if (space2hex)
                put_hex(c);
            else
                *end++ = '+';
            break;
        default:
            if (tbl[(size_t)c])
                *end++ = tbl[(size_t)c];
            else
                put_hex(c);
        }
    }
    return end - dst;
}

request_common::proto_state::~proto_state()
{
    if (curl_headers)
        curl_slist_free_all(curl_headers);
    if (mime)
        curl_mime_free(mime);
}

