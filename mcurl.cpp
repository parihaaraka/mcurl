#include <ev.h>
#include "mcurl.h"
#include <algorithm>
#include <string.h>
#include <random>
#include "b64/encode.h"

std::atomic<int> mcurl::instances_counter{0};
std::mutex mcurl::counter_guard{};

bool ci_comparator::operator()(const std::string &a, const std::string &b) const
{
    return std::lexicographical_compare(
        a.begin(), a.end(),
        b.begin(), b.end(),
        [](const char &a, const char &b) -> bool {
            return tolower(a) < tolower(b);
        });
};

struct sock_info
{
    curl_socket_t sock;
    CURL *easy;
    ev_io ev;
};

size_t write_body_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
    std::string &dest = *static_cast<std::string*>(data);
    size_t total = size * nmemb;
    dest.append(static_cast<char*>(ptr), total);
    return total;
}

size_t write_headers_cb(void *input, size_t size, size_t nmemb, void *user_ptr)
{
    auto &j = *static_cast<mcurl::job*>(user_ptr);
    auto &headers = j.resp.headers;
    size_t total = size * nmemb;
    if (!total)
        return total;
    auto raw = std::string_view{static_cast<char*>(input), total};
    // folded header continuation
    if (std::isspace(raw.front()))
    {
        if (!j.prev_header.empty())
        {
            if (auto it = headers.find(j.prev_header); it != headers.end())
            {
                raw.remove_prefix(std::min(raw.find_first_not_of(" \t\r\v"), raw.size()));
                it->second.append(raw);
            }
        }
    }
    else if (auto pos = raw.find(':'); pos != std::string::npos)
    {
        auto key = raw.substr(0, pos);
        auto val = raw.substr(std::min(pos + 1, raw.size()), raw.size() - pos - 1);
        val.remove_prefix(std::min(val.find_first_not_of(" \t"), val.size()));
        headers[std::string(key)] = val;
    }

    //dest->append(static_cast<char*>(ptr), total);
    return total;
}

void cmd_cb(struct ev_loop *, ev_async *w, int) noexcept
{
    auto &c = *static_cast<mcurl*>(w->data);
    if (c.cmd == 's')
    {
        //ev_async_stop(c.loop, c.cmd_watcher); // do not stop cmd_watcher to allow terminate() call
        ev_async_stop(c.loop, c.new_job_watcher);
        c.active = false;

        //mcurl::stop(on_finish) call on empty queue should force on_finish() call
        std::unique_lock lk(c.queue_guard);
        bool empty = c.on_the_go.empty();
        lk.unlock();
        if (empty)
        {
            ev_async_stop(c.loop, c.cmd_watcher);
            ev_timer_stop(c.loop, c.timeout_watcher);
            c.call_empty_queue_cb();
        }
    }
    else if (c.cmd == 't')
    {
        ev_async_stop(c.loop, c.cmd_watcher);
        ev_async_stop(c.loop, c.new_job_watcher);
        ev_timer_stop(c.loop, c.timeout_watcher);
        c.active = false;

        decltype(c.on_the_go) tmp;
        std::unique_lock lk(c.queue_guard);
        tmp.swap(c.on_the_go);
        lk.unlock();

        // man: removing an easy handle while being used is perfectly legal and will effectively halt the transfer in progress involving that easy handle
        for (auto &[easy, j]: tmp)
        {
            curl_multi_remove_handle(c.multi, easy);
            curl_easy_cleanup(easy);
            if (j.on_finish)
            {
                try
                {
                    auto &err = j.resp.error;
                    err.resize(strnlen(err.data(), err.capacity()));
                    if (err.empty())
                        j.resp.error = "terminated via user command";
                    j.on_finish(j.req, j.resp);
                }
                catch(...) { }
            }
        }
        tmp.clear();

        // clean up easy handles
        for (CURL *easy: c.free_easy_handles)
            curl_easy_cleanup(easy);  // man: call curl_multi_remove_handle before curl_easy_cleanup
        c.free_easy_handles.clear();

        c.call_empty_queue_cb();
    }
}

mcurl::smtp_state::smtp_state() :
    curl_header(nullptr, curl_slist_free_all),
    curl_recipients(nullptr, curl_slist_free_all)
{
}

size_t mcurl::smtp_state::read_cb(char *buffer, size_t size, size_t nitems, void *user_ptr)
{
    using stage_t = smtp_state::stage_t;
    auto &j = *reinterpret_cast<job*>(user_ptr);
    auto &s = std::get<smtp_state>(j.state);
    auto &r = std::get<smtp_request>(j.req);
    if (s.data.size() == s.bytes_done)
    {
        // определяем, на каком именно этапе опустошился буфер
        switch (s.stage)
        {
        case stage_t::None:
            // начинаем заголовок
            s.stage = stage_t::Header;
            s.partnum = -1;
            for (std::string &h : s.generated_headers)
                s.data += h + "\r\n";
            for (std::string &h : r.headers)
                s.data += h + "\r\n";
            s.data += "\r\n";
            break;
        case stage_t::Header:
            // Начинаем текстовую часть сообщения
            // (кодируется в base64, чтобы можно было использовать юникод).
            // Если тело пустое, то пофиг - всё равно воткнем заголовок и пустое тело.
            s.stage = stage_t::Body;
            s.data = "--" + s.boundary + "\r\n"
                                             "Content-Type: text/plain; charset=utf-8\r\n"
                                             "Content-Transfer-Encoding: base64\r\n\r\n" +
                       base64::encode(r.body, base64::CRLF) + "\r\n";
            break;
        case stage_t::PartHeader:
        {
            // передаем тело вложения или поле формы
            s.stage = stage_t::PartBody;
            auto &p = r.attachments.at(static_cast<size_t>(s.partnum));

            if (p.source_type == content_part::source_type_t::File)
            {
                // вложение забрать из файла
                s.in_stream = std::unique_ptr<std::ifstream>(new std::ifstream(p.source, std::ios::binary | std::ios_base::in));
                //r._in_stream.open(p.source, std::ios::binary | std::ios_base::in);
                if (!*s.in_stream)
                {
                    s.in_stream->close();
                    return CURL_READFUNC_ABORT;
                }
            }
            else
            {
                // вложение передано в буфере
                s.data = base64::encode(p.source, base64::CRLF);
                if (!s.data.empty())
                    break;
            }
        }
        //[[clang::fallthrough]];
        case stage_t::PartBody:
        {
            auto &p = r.attachments.at(static_cast<size_t>(s.partnum));
            // если источник - файл в хорошем состоянии, то нужно прочитать еще кусок
            if (p.source_type == content_part::source_type_t::File && *s.in_stream)
            {
                std::vector<char> buf(100 * 1024);
                s.in_stream->read(buf.data(), buf.size());
                // если достигнут конец файла, то меняем размер буфера до фактически считанного количества байт
                if (s.in_stream->eof())
                    buf.resize(s.in_stream->gcount());
                bool is_bad = s.in_stream->bad();
                // при любом недоразумении (включая конец файла) закрываем файл
                if (!s.in_stream)
                    s.in_stream->close();

                // если проблема не в EOF, то прерываем задание
                if (is_bad)
                    return CURL_READFUNC_ABORT;

                // формируем закодированный в base64 кусок файла
                s.data.resize(2 * buf.size() + 3);
                long len = 0;
                if (!buf.empty())
                    len += s.b64encoder.encode(buf.data(), buf.size(), &s.data[0]);
                if (!s.in_stream->is_open())
                    len += s.b64encoder.encode_end(&s.data[0] + len);
                s.data.resize(len);

                // если снова получился непустой буфер, то продолжим передачу в рамках текущего этапа
                if (len)
                    break;
            }
        }
        //[[clang::fallthrough]];
        case stage_t::Body:
        {
            // проверяем, не закончилось ли содержимое
            if (s.partnum == static_cast<long>(r.attachments.size()) - 1)
            {
                // завершающий boundary
                s.stage = stage_t::Footer;
                s.data = "--" + s.boundary + "--\r\n";
                break;
            }

            // переход к первому или очередному вложению
            ++s.partnum;
            // начинаем заголовки вложений или полей формы
            s.stage = stage_t::PartHeader;
            auto &p = r.attachments.at(static_cast<size_t>(s.partnum));
            s.data = "--" + s.boundary +
                       "\r\nContent-Type: " + p.content_type +
                       "\r\nContent-Transfer-Encoding: base64\r\n";

            // inline - умолчательное значение (для тела письма)
            if (p.disposition != content_part::disposition_t::Inline)
            {
                // form-data вряд ли используется в протоколе smtp
                // (оставлено для доработки кода к универсальному виду)
                s.data += std::string("Content-Disposition: ") +
                            (p.disposition == content_part::disposition_t::FormData ? "form-data" : "attachment");
                if (!p.name.empty())
                    s.data += ";\r\n name=\"" + encode1522(p.name, true) + '"';
                if (!p.filename.empty())
                    s.data += ";\r\n filename=\"" + encode1522(p.filename, true) + '"';
                s.data += "\r\n";
            }
            s.data += "\r\n";
            break;
        }
        case stage_t::Footer:
            // всё отправлено
            s.data.clear();
            s.stage = stage_t::None;
            break;
        }

        // рестарт отсчета переданной части текущего буфера
        s.bytes_done = 0;
    }

    size_t len = std::min<size_t>(size * nitems, s.data.size() - s.bytes_done);
    if (len)
    {
        // собственно, передача очередного куска данных curl'у
        memcpy(buffer, s.data.data() + s.bytes_done, len);
        s.bytes_done += len;
    }
    return len;
}

template <typename R>
void set_common_curl_options(CURL *easy, mcurl::job &j)
{
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &j.resp.body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_headers_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &j);//.resp.headers);
    //curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, true);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, j.resp.error.data());
    curl_easy_setopt(easy, CURLOPT_PRIVATE, &j);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 8L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);

    auto &r = std::get<R>(j.req);

    // протокол (smtp или http) определяется по url
    // e.g.: smtp://mx.mydomain.com:25
    curl_easy_setopt(easy, CURLOPT_URL, r.url.c_str());

    // тип прокси-сервера также определяется по url: https://curl.haxx.se/libcurl/c/CURLOPT_PROXY.html
    // e.g.: socks5://51.15.45.8:1080
    if (!r.proxy.empty())
        curl_easy_setopt(easy, CURLOPT_PROXY, r.proxy.c_str());

    //if(_traceCallback) {
    //    curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, trace_cb);
    //    curl_easy_setopt(easy, CURLOPT_DEBUGDATA, this);
    //    curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
    //}

    if (!r.user.empty())
    {
        curl_easy_setopt(easy, CURLOPT_USERNAME, r.user.c_str());
        curl_easy_setopt(easy, CURLOPT_PASSWORD, r.password.c_str());
    }

    if (!r.verify_peer)
    {
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L); // don't verify the certificate's name against host
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L); // don't verify the peer's SSL certificate
    }
    else if (!r.ca.empty())
    {
        curl_easy_setopt(easy, CURLOPT_CAINFO, r.ca.c_str());
    }

    if (!r.cert.empty() )
    {
        curl_easy_setopt(easy, CURLOPT_SSLCERT, std::string(r.cert + ".pem").c_str());
        curl_easy_setopt(easy, CURLOPT_SSLKEY, std::string(r.cert + ".key").c_str());
    }
    curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1);
}

mcurl::mcurl()
{
    cmd_watcher = new ev_async{};
    new_job_watcher = new ev_async{};
    timeout_watcher = new ev_timer{};
    cmd_watcher->data = this;
    new_job_watcher->data = this;
    timeout_watcher->data = this;
}

mcurl::~mcurl()
{
    cmd = 't';
    cmd_cb(loop, cmd_watcher, 0); // terminate()

    curl_multi_cleanup(multi);   // curl_multi_cleanup should be called when all easy handles are removed

    --instances_counter;
    if (!instances_counter.load())
        curl_global_cleanup();

    delete cmd_watcher;
    delete new_job_watcher;
    delete timeout_watcher;
}

void mcurl::enqueue(http_request &r, std::function<void(mcurl::http_request &req, mcurl::response &resp)> on_finish)
{
    mcurl::request_t rvar{std::move(r)};
    std::unique_lock lk(queue_guard);
    in_queue.emplace(
        rvar,
        [](CURL *easy, job &j)
        {
            auto &r = std::get<mcurl::http_request>(j.req);
            auto &s = j.state.emplace<mcurl::http_state>();

            //bool content_type_found = false;
            for (const std::string &h : r.headers)
            {
                s.curl_header = mcurl::slist_t(curl_slist_append(s.curl_header.release(), h.c_str()), curl_slist_free_all);
                //if (h.substr(0, 12) == "Content-Type")
                //    content_type_found = true;
            }

            // По идее, curl должен сам делать заголовок Content-Type: multipart/..
            // при использовании функций, формирующих эти самые кусочки (curl_formadd и т.п.).
            // Если что - раскомментировать соотв. кусочки.
            //if (const auto* parts = std::get_if<std::vector<content_part>>(&r.body); parts != nullptr && !parts->empty())
            //    j.curl_header= mcurl::slist_t(curl_slist_append(j.curl_header.release(), "Content-Type: multipart/form-data"), curl_slist_free_all);

            if (s.curl_header)
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, s.curl_header.get());

            if (const auto *parts = std::get_if<std::vector<content_part>>(&r.body); parts != nullptr && !parts->empty())
            {
                s.mime = mcurl::mime_t(curl_mime_init(easy), curl_mime_free);
                for (auto &p: *parts)
                {
                    auto part = curl_mime_addpart(s.mime.get());
                    curl_mime_name(part, p.name.c_str());
                    curl_mime_data(part, p.source.data(), p.source.size());
                }
                curl_easy_setopt(easy, CURLOPT_MIMEPOST, s.mime.get());
            }
            else if (const auto *body = std::get_if<std::string>(&r.body); body != nullptr && !body->empty())
            {
                curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body->data());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, body->size());
                curl_easy_setopt(easy, CURLOPT_POST, true);
            }
            else
            {
                curl_easy_setopt(easy, CURLOPT_HTTPGET, true);
            }

            if(!r.method.empty()) {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, r.method.c_str());
            }

            set_common_curl_options<http_request>(easy, j);
        },
        [on_finish](mcurl::request_t &r, mcurl::response &resp)
        {
            auto &req = std::get<mcurl::http_request>(r);
            on_finish(req, resp);
        }
    );
    lk.unlock();
    if (active)
        ev_async_send(loop, new_job_watcher);
}

void mcurl::enqueue(smtp_request &r, std::function<void(mcurl::smtp_request &req, mcurl::response &resp)> on_finish)
{
    mcurl::request_t rvar{std::move(r)};
    std::unique_lock lk(queue_guard);
    in_queue.emplace(
        rvar,
        [](CURL *easy, job &j)
        {
            auto &r = std::get<mcurl::smtp_request>(j.req);
            if (r.url.compare(0, 4, "smtp"))
                throw std::runtime_error("protocol must be specified within url");

            if (r.recipients.empty())
                throw std::runtime_error("recipient is not specified");

            if (r.sender.empty())
                throw std::runtime_error("sender is not specified");

            auto &s = j.state.emplace<mcurl::smtp_state>();

            // TODO test custom smtp headers
            for (const std::string &h : r.headers)
                s.curl_header = mcurl::slist_t(curl_slist_append(s.curl_header.release(), h.c_str()), curl_slist_free_all);

            if (s.curl_header)
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, s.curl_header.get());

            // https://curl.haxx.se/mail/tracker-2013-06/0202.html
            // якобы, использовать один и тот же разделитель небезопасно...
            s.boundary = generate_boundary();

            s.generated_headers.push_back("Date: " + timestamp());
            s.generated_headers.push_back("From: " + r.sender);
            s.generated_headers.push_back("To: " + r.recipients.at(0));
            std::string cc;
            for (size_t i = 1; i < r.recipients.size(); ++i)
                cc += (i > 1 ? ",\r\n " : "") + r.recipients.at(i);
            if (!cc.empty())
                s.generated_headers.push_back("Cc: " + cc);

            if (!r.subject.empty())
                s.generated_headers.push_back("Subject: " + encode1522(r.subject, base64::encoder::im_line_length));

            if (!r.body.empty() || !r.attachments.empty())
                s.generated_headers.push_back("Content-Type: multipart/mixed; boundary=\"" + s.boundary + "\"");

            curl_easy_setopt(easy, CURLOPT_MAIL_FROM, r.sender.c_str());

            for (auto &rec: r.recipients)
                s.curl_recipients = mcurl::slist_t(curl_slist_append(s.curl_recipients.release(), rec.c_str()), curl_slist_free_all);

            curl_easy_setopt(easy, CURLOPT_MAIL_RCPT, s.curl_recipients.get());
            curl_easy_setopt(easy, CURLOPT_MAIL_FROM, r.sender.c_str());

            curl_easy_setopt(easy, CURLOPT_READFUNCTION, &smtp_state::read_cb);
            curl_easy_setopt(easy, CURLOPT_READDATA, &j);
            // без CURLOPT_UPLOAD вообще не вызывается функция read_cb
            curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);

            set_common_curl_options<smtp_request>(easy, j);
        },
        [on_finish](mcurl::request_t &r, mcurl::response &resp)
        {
            auto &req = std::get<mcurl::smtp_request>(r);
            on_finish(req, resp);
        }
    );
    lk.unlock();
    if (active)
        ev_async_send(loop, new_job_watcher);
}

void mcurl::set_max_simultaneous_transfers(size_t num)
{
    max_simultanous_transfers = num;
}

size_t mcurl::queue_size() const
{
    std::lock_guard lk(queue_guard);
    return in_queue.size();
}

bool mcurl::is_active() const
{
    return active;
}

size_t mcurl::running_tasks_count() const
{
    std::lock_guard lk(queue_guard);
    return on_the_go.size();
}

std::string mcurl::timestamp()
{
    time_t t = time(nullptr);
    tm tmp;
    gmtime_r(&t, &tmp);
    std::array<char, 64> buf;
    size_t len = strftime(buf.data(), buf.size(), "%a, %d %b %y %T GMT", &tmp);
    return std::string(buf.data(), buf.data() + len);
}

std::string mcurl::encode1522(const std::string &value, bool wrap)
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

std::string mcurl::generate_boundary(size_t length)
{
    if (!length)
        return "";

    static const std::string allowed_chars { "ABCDEFGHIJKLMNOPQRSTUVWXYZ_-=abcdefghijklmnopqrstuvwxyz0123456789" };
    static thread_local std::default_random_engine randomEngine(std::random_device{}());
    static thread_local std::uniform_int_distribution<size_t> randomDistribution(0, allowed_chars.length() - 1);

    std::string boundary(length, '-');
    for (auto &c : boundary)
        c = allowed_chars[randomDistribution(randomEngine)];

    return boundary;
}

void mcurl::call_empty_queue_cb()
{
    auto lk2 = std::lock_guard(on_empty_queue_guard);
    if (on_empty_queue)
    {
        try
        {
            on_empty_queue();
        }
        catch (...) {}
        on_empty_queue = nullptr;
    }
}

void mcode_or_die(const std::string &where, CURLMcode code)
{
    if (code != CURLM_OK && code != CURLM_BAD_SOCKET)
    {
        throw std::runtime_error(where + " error: " + curl_multi_strerror(code));
    }
}

void check_multi_info(mcurl &c) noexcept
{
    CURLMsg *msg;
    int msgs_left;
    bool job_done = false;
    bool on_the_go_empty = false;
    while ((msg = curl_multi_info_read(c.multi, &msgs_left)))
    {
        if (msg->msg == CURLMSG_DONE)
        {
            CURL *easy = msg->easy_handle;
            //mcurl::job *j;
            //curl_easy_getinfo(easy, CURLINFO_PRIVATE, &j);

            std::unique_lock lk(c.queue_guard);
            auto it = c.on_the_go.find(easy);
            if (it != c.on_the_go.end())
            {
                mcurl::job j{std::move(it->second)};
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &j.resp.status);
                c.on_the_go.erase(easy);
                on_the_go_empty = c.on_the_go.empty();
                lk.unlock();
                j.call_on_finish();
                c.free_easy_handles.push_front(easy);
            }
            else
            {
                // ?
                // timeout and terminate() consilience may lead to strange result (looks like ok for now)
                on_the_go_empty = c.on_the_go.empty();
                lk.unlock();
            }

            // man: "If you want to re-use an easy handle that was added to the multi handle for transfer,
            // you must first remove it from the multi stack and then re-add it again
            // (possibly after having altered some options at your own choice)."
            curl_multi_remove_handle(c.multi, easy);
            curl_easy_reset(easy);
            job_done = true;
        }
    }

    if (!c.active)
    {
        if (on_the_go_empty)
        {
            ev_async_stop(c.loop, c.cmd_watcher);
            ev_timer_stop(c.loop, c.timeout_watcher);
            c.call_empty_queue_cb();
        }
    }
    else if (job_done)
    {
        ev_async_send(c.loop, c.new_job_watcher);
    }
}

void timer_cb(struct ev_loop *, ev_timer *w, int)
{
    auto &c = *static_cast<mcurl*>(w->data);
    int running_handles;
    CURLMcode rc = curl_multi_socket_action(c.multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
    mcode_or_die("timer_cb: curl_multi_socket_action", rc);
    check_multi_info(c);
}

int multi_timer_cb(CURLM *, long timeout_ms, void *user_ptr)
{
    auto &c = *static_cast<mcurl*>(user_ptr);
    // man: A timeout_ms value of -1 means you should delete your timer.
    ev_timer_stop(c.loop, c.timeout_watcher);
    if (timeout_ms > 0)
    {
        ev_timer_set(c.timeout_watcher, timeout_ms / 1000.0, 0);
        ev_timer_start(c.loop, c.timeout_watcher);
    }
    else if (!timeout_ms)
    {
        timer_cb(c.loop, c.timeout_watcher, 0);
    }
    return 0;
}

void event_cb(struct ev_loop *, ev_io *w, int revents)
{
    auto &c = *static_cast<mcurl*>(w->data);
    CURLMcode rc;
    int action = (revents & EV_READ ? CURL_POLL_IN : 0) | (revents & EV_WRITE ? CURL_POLL_OUT : 0);

    int running_handles;
    rc = curl_multi_socket_action(c.multi, w->fd, action, &running_handles);
    mcode_or_die("event_cb: curl_multi_socket_action", rc);
    check_multi_info(c);
    if (running_handles <= 0)
    {
        // last transfer done, kill timeout
        ev_timer_stop(c.loop, c.timeout_watcher);
    }
}

int socket_cb(CURL *easy, curl_socket_t s, int what, void *data, sock_info *si)
{
    auto &c = *static_cast<mcurl*>(data);
    if (what == CURL_POLL_REMOVE)
    {
        if (si)
        {
            // отцепляем libev от сокета и удаляем контейнер со всеми запчастями
            ev_io_stop(c.loop, &si->ev);
            delete si;
            curl_multi_assign(c.multi, s, nullptr);
        }
    }
    else
    {
        int kind = (what & CURL_POLL_IN ? EV_READ : 0) | (what & CURL_POLL_OUT ? EV_WRITE : 0);
        if (!si) // цепляем libev к сокету
        {
            si = new sock_info;
            si->sock = s;
            si->easy = easy;
            ev_io_init(&si->ev, event_cb, si->sock, kind);
            si->ev.data = &c;
            ev_io_start(c.loop, &si->ev);
            curl_multi_assign(c.multi, s, si);
        }
        else    // меняем тип ожидаемых на сокете событий
        {
            ev_io_stop(c.loop, &si->ev);
            ev_io_set(&si->ev, si->sock, kind);
            ev_io_start(c.loop, &si->ev);
        }
    }

    return 0;
}

void mcurl::stop(std::function<void()> on_empty_queue) noexcept
{
    {
        std::lock_guard lk(on_empty_queue_guard);
        this->on_empty_queue = on_empty_queue;
    }
    if (cmd == 's')
        return;
    cmd = 's';
    ev_async_send(loop, cmd_watcher);
}

void mcurl::terminate(std::function<void()> on_empty_queue) noexcept
{
    {
        std::lock_guard lk(on_empty_queue_guard);
        this->on_empty_queue = on_empty_queue;
    }
    if (cmd == 't')
        return;
    cmd = 't';
    ev_async_send(loop, cmd_watcher);
}

void new_job_cb(struct ev_loop *, ev_async *w, int)
{
    auto &c = *static_cast<mcurl*>(w->data);

    if (!c.active)
        return;

    std::unique_lock lk(c.queue_guard);
    // respect tasks limit
    if (c.on_the_go.size() >= c.max_simultanous_transfers)
        return;

    // очередь заданий пуста
    if (c.in_queue.empty())
    {
        if (c.on_the_go.empty())
        {
            lk.unlock();
            c.call_empty_queue_cb();
        }
        return;
    }
    lk.unlock();

    CURL *easy;
    // Если в кеше нет хэндлов, создаем новый
    if (c.free_easy_handles.empty())
    {
        easy = curl_easy_init();
    }
    else // Забираем из кеша
    {
        easy = std::move(c.free_easy_handles.front());
        c.free_easy_handles.pop_front();
    }

    lk.lock();
    // достаем новое задание
    auto j = std::move(c.in_queue.front());
    c.in_queue.pop();
    lk.unlock();
    mcurl::job *cur_job_ptr = &j;

    // функция для завершения задания при ошибке его инициализации и запуска
    auto finalize_job = [&cur_job_ptr, &easy, &c]()
    {
        cur_job_ptr->call_on_finish();

        if (easy)
        {
            std::unique_lock lk(c.queue_guard);
            c.on_the_go.erase(easy);
            lk.unlock();

            curl_easy_reset(easy);
            c.free_easy_handles.push_front(easy);
        }
    };

    try
    {
        std::unique_lock lk(c.queue_guard);
        // перекладываем задание в выполняемые
        auto [it, success] = c.on_the_go.insert(std::make_pair(easy, std::move(j)));
        mcurl::job &j = it->second;
        lk.unlock();

        cur_job_ptr = &j;
        if (!easy)
            throw std::runtime_error("unable to acquire easy handle");
        j.feed(easy, j);

        // добавление easy handle инициирует работу (запуск таймера и т.п.)
        CURLMcode rc = curl_multi_add_handle(c.multi, easy);
        mcode_or_die("new_job_cb: curl_multi_add_handle", rc);
    }
    catch (const std::runtime_error &e)
    {
        strncpy(cur_job_ptr->resp.error.data(), e.what(), CURL_ERROR_SIZE);
        finalize_job();
    }
    catch(...)
    {
        strncpy(cur_job_ptr->resp.error.data(), "error initiating new curl job", CURL_ERROR_SIZE);
        finalize_job();
    }

    // try to acquire one more job
    ev_async_send(c.loop, c.new_job_watcher);
}

void mcurl::start(struct ev_loop *l)
{
    on_empty_queue = nullptr;
    if (!multi)
    {
        const std::lock_guard<std::mutex> lock(counter_guard);
        if (!instances_counter)
        {
            CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (res)
                throw std::runtime_error(curl_easy_strerror(res));
        }

        multi = curl_multi_init();
        if (!multi)
        {
            if (!instances_counter)
                curl_global_cleanup();
            throw std::runtime_error("curl_multi_init() failed");
        }
        ++instances_counter;

        curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
        curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
        curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);

        ev_init(timeout_watcher, timer_cb);
        ev_init(new_job_watcher, new_job_cb);
        ev_init(cmd_watcher, cmd_cb);
    }

    if (loop && l && loop != l)
    {
        ev_timer_stop(loop, timeout_watcher);
        ev_async_stop(loop, new_job_watcher);
        ev_async_stop(loop, cmd_watcher);
    }

    if (l)
    {
        loop = l;
    }

    if (active || !loop)
        return;

    cmd = 'a';  // active
    ev_async_start(loop, cmd_watcher);
    ev_async_start(loop, new_job_watcher);
    active = true;

    // there may be jobs added before launch
    new_job_cb(loop, new_job_watcher, 0);
}

mcurl::http_state::http_state() :
    mime(nullptr, curl_mime_free),
    curl_header(nullptr, curl_slist_free_all)
{
}

mcurl::job::job(request_t &r, feed_t f1, on_finish_t f2) :
    req(std::move(r)),
    feed(f1),
    on_finish(f2)
{
}

void mcurl::job::call_on_finish()
{
    if (on_finish)
    {
        auto &err = resp.error;
        err.resize(strnlen(err.data(), err.capacity()));
        try
        {
            on_finish(req, resp);
        }
        catch(...) { }
    }
}
