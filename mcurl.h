#ifndef MCURL_H
#define MCURL_H

#include <curl/curl.h>
#include <functional>
#include <queue>
#include <ev++.h>
#include <map>
#include <random>
#include <variant>
#include <vector>
#include <array>
#include <mutex>
#include <memory>
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include "b64/encode.h"

/*
 * curl multi interface details: https://curl.haxx.se/libcurl/c/evhiperfifo.html
 * libev nice man: http://pod.tst.eu/http://cvs.schmorp.de/libev/ev.pod
 *
 * GET sample image: http://dummyimage.com/100x100&text=test1
 * http test server: https://httpbin.org
 *
 * TODO: custom event loop
 */

struct ci_comparator { bool operator()(const std::string &a, const std::string &b) const; };
std::string encode1522(const std::string &value, bool wrap);

struct mcurl_success
{
    std::string body;
    std::map<std::string, std::string, ci_comparator> headers{ci_comparator()};
    long status = 0;
};
struct mcurl_fail
{
    char error[CURL_ERROR_SIZE] = {0};
    long status = 0;
};
struct mcurl_trace
{
    curl_infotype infotype;
    unsigned char *value;
    size_t value_size;
};
using mcurl_event = std::variant<mcurl_success, mcurl_fail, mcurl_trace>;

struct mcurl_content_part
{
    // for smtp always `Content-Transfer-Encoding: base64`
    // https://developer.mozilla.org/ru/docs/Web/HTTP/Reference/Headers/Content-Disposition
    enum class disposition_t { Inline, FormData, Attachment };
    enum class source_type_t  { Buffer, File };

    source_type_t source_type = source_type_t::File;
    disposition_t disposition = disposition_t::Attachment;
    std::string source;         ///< content of the attachment or form field
    std::string name;           ///< `name` of `Content-Disposition` header
    std::string filename;       ///< `filename` of `Content-Disposition` header
    std::string content_type = "application/octet-stream";   ///< mime-type, encoding* (`Content-Type` header)

    static mcurl_content_part file(std::string_view file_path, std::string_view destination_file_name = {});
    static mcurl_content_part file_from_buffer(std::string_view buffer, std::string_view destination_file_name = {});
    static mcurl_content_part form_data(std::string_view name, std::string_view buffer);
};

struct request_common
{
    struct proto_state
    {
        curl_slist *curl_headers = nullptr;
        curl_mime *mime = nullptr;
        // helper to deal with folded headers (https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)
        std::string prev_header;
    protected:
        proto_state() = default;
        ~proto_state();
    };

    std::string uri;
    std::string user;
    std::string password;
    std::vector<std::string> headers;
    std::string cert;
    std::string ca;
    std::string proxy;
    bool verify_peer = true;
    long timeout = 0;

protected:
    request_common() = default;
};

struct http_request : request_common
{
    struct proto_state : request_common::proto_state
    {
    };

    std::string method; /// GET by default
    std::variant<std::string, std::vector<mcurl_content_part>> body;
};

struct smtp_request : request_common
{
    struct proto_state : request_common::proto_state
    {
        enum class smtp_stage { None, Header, Body, PartHeader, PartBody, Footer };
        curl_slist *curl_recipients = nullptr;
        std::string boundary;

        smtp_stage stage = smtp_stage::None;
        // размер переданной в curl части буфера, обрабатываемого на текущем шаге (SMTP)
        size_t bytes_done = 0;
        // индекс обрабатываемого вложения
        long partnum = 0;
        // буфер данных, отправляемых в curl (SMTP)
        // (перезаполняется по мере передачи различных частей тела сообщения)
        std::string data;
        // поток для считывания файлов вложений (gcc < 5 не умеет перемещать поток, поэтому указатель)
        std::unique_ptr<std::ifstream> in_stream;
        base64::encoder b64encoder = base64::CRLF;

        proto_state() = default;
        ~proto_state();
        proto_state(const proto_state &) = delete;
        proto_state& operator=(const proto_state&) = delete;
        proto_state(proto_state&& other)
        {
            *this = std::move(other);
        }
        proto_state& operator=(proto_state&& other)
        {
            if (this != &other)
            {
                std::swap(mime, other.mime);
                std::swap(curl_headers, other.curl_headers);
                prev_header.swap(other.prev_header);
                std::swap(curl_recipients, other.curl_recipients);
                boundary.swap(other.boundary);
                stage = other.stage;
                bytes_done = other.bytes_done;
                partnum = other.partnum;
                data.swap(other.data);
                in_stream.reset(other.in_stream.release());
                b64encoder = other.b64encoder;
            }
            return *this;
        }
    };

    std::string body;
    std::vector<mcurl_content_part> attachments;
    std::string sender;
    // поддерживаются только готовые для заголовка smtp адреса
    // без подписи (имя владельца) и в угловых скобках
    // (не проверяется правильность, не кодируются возможные подписи / TODO ?)
    std::vector<std::string> recipients; ///< адреса получателей письма в угловых скобках
    std::string subject;
};

#define MCURL_CALLABLE(fn_t) fn_t<void(mcurl_request&, mcurl_event)>

class mcurl_global
{
protected:
    static std::atomic<int> instances_counter;
    mcurl_global();
    ~mcurl_global();
};

using mcurl_request = std::variant<http_request, smtp_request>;
using mcurl_request_state = std::variant<http_request::proto_state, smtp_request::proto_state>;

template <typename EventCallbackType = MCURL_CALLABLE(std::function)>
class mcurl : mcurl_global
{
private:
    static_assert(std::is_invocable_v<EventCallbackType, mcurl_request&, mcurl_event>,
                  "EventCallbackType parameter must be invocable with `(mcurl_request&, mcurl_event)` args.");

    struct job
    {
        mcurl_request req;
        EventCallbackType on_event;
        bool trace = false;
    };

    struct job_on_the_go
    {
        CURL *easy = nullptr;
        job j;
        mcurl_success success;
        mcurl_fail fail;
        mcurl_request_state proto_state;
        job_on_the_go(CURL *easy, job &&job) : easy(easy), j(std::move(job))
        {
            if (std::holds_alternative<http_request>(j.req))
                proto_state.emplace<http_request::proto_state>();
            else
                proto_state.emplace<smtp_request::proto_state>();
        }
        request_common::proto_state& state()
        {
            if (std::holds_alternative<http_request>(j.req))
                return std::get<http_request::proto_state>(proto_state);
            else
                return std::get<smtp_request::proto_state>(proto_state);
        }
    };

    struct sock_info
    {
        curl_socket_t sock;
        //CURL *easy;
        ev::io ev;
    };

    struct ev_loop *_loop;

    CURLM *_multi = nullptr;
    ev::timer _timeout_timer;
    ev::async _new_job_watcher;

    enum class mode_wanted
    {
        active,
        ignore_new_jobs,           // don't acquire new jobs
        finalize_started_and_stop, // ignore jobs, finalize started jobs and stop all pollers after that
        stop_the_globe             // stop all pollers, clear all jobs as is
    };

    std::atomic<mode_wanted> _mode_wanted;
    ev::async _stop_signal_watcher;

    // man: "After each single curl_easy_perform operation, libcurl will keep the connection alive and open.
    // A subsequent request using the same easy handle to the same host might just be able to use the already
    // open connection! This reduces network impact a lot.
    // ... Each easy handle will attempt to keep the last few connections (default:5)
    // alive for a while in case they are to be used again.
    std::deque<CURL*> _free_easy_handles;

    // лимит количества одновременных задач
    std::atomic_size_t _max_simultanous_transfers;

    // флаг завершения работы mcurl после выполнения всех заданий из входной очереди
    bool _terminate_on_finish;

public:
    mcurl(struct ev_loop *loop = EV_DEFAULT)
        : _loop(loop), _max_simultanous_transfers(10)
    {
        _multi = curl_multi_init();
        if (!_multi)
            throw std::runtime_error("curl_multi_init() failed");

        curl_multi_setopt(_multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
        curl_multi_setopt(_multi, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(_multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
        curl_multi_setopt(_multi, CURLMOPT_TIMERDATA, this);

        if (loop)
        {
            _timeout_timer.set(loop);
            _new_job_watcher.set(loop);
            _stop_signal_watcher.set(loop);
        }
    }
    ~mcurl()
    {
        if (_multi)
            curl_multi_cleanup(_multi);
    }

    bool is_active() const { return _new_job_watcher.is_active(); }
    size_t running_jobs_count() const { return _on_the_go.size(); }
    void set_max_transfers(size_t num) { _max_simultanous_transfers = num; }

    std::string state() const
    {
        std::string s;
        s += std::string("max_simultaneous_transfers: ") + std::to_string(_max_simultanous_transfers.load()) + "\n";
        s += std::string("active: ") + (is_active() ? "true" : "false") + "\n";
        s += std::string("running_jobs: ") + std::to_string(_on_the_go.size()) + "\n";
        s += std::string("in_queue: ") + std::to_string(_in_queue.size()) + "\n";
        return s;
    }

    void timer_cb(ev::timer &, int)
    {
        int running_handles;
        CURLMcode rc = curl_multi_socket_action(_multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
        mcode_or_die("timer_cb: curl_multi_socket_action", rc);
        check_multi_info();
    }

    void expected_mode_cb(ev::async &, int) noexcept
    {
        if (_mode_wanted == mode_wanted::ignore_new_jobs)
        {
            _terminate_on_finish = false;
            _new_job_watcher.stop();
        }
        else if (_mode_wanted == mode_wanted::finalize_started_and_stop)
        {
            _terminate_on_finish = true;
            _new_job_watcher.stop();
            if (_on_the_go.empty())
                terminate();
        }
        else if (_mode_wanted == mode_wanted::stop_the_globe)
        {
            _stop_signal_watcher.stop();

            // останавливаем прием новых заданий
            _new_job_watcher.stop();

            // man: removing an easy handle while being used is perfectly legal and will effectively halt the transfer in progress involving that easy handle
            for (auto &job: _on_the_go)
            {
                curl_multi_remove_handle(_multi, job.first);
                curl_easy_cleanup(job.first);
            }
            _on_the_go.clear();

            // clean up easy handles
            for (CURL *easy:_free_easy_handles)
                curl_easy_cleanup(easy);  // man: call curl_multi_remove_handle before curl_easy_cleanup
            _free_easy_handles.clear();

            // останавливаем таймер, если он еще активен
            _timeout_timer.stop();
        }
    }

    void start(bool terminate_on_finish = false, struct ev_loop *loop = nullptr)
    {
        _terminate_on_finish = terminate_on_finish;
        if (is_active())
            return;

        if (loop)
        {
            _timeout_timer.set(loop);
            _new_job_watcher.set(loop);
            _stop_signal_watcher.set(loop);
            _loop = loop;
        }

        _timeout_timer.set<mcurl, &mcurl::timer_cb>(this);

        _mode_wanted = mode_wanted::active;
        _stop_signal_watcher.set<mcurl, &mcurl::expected_mode_cb>(this);
        _stop_signal_watcher.start();

        _new_job_watcher.set<mcurl, &mcurl::new_job_cb>(this);
        _new_job_watcher.start();

        // если задания добавляли до запуска, то заберем их
        _new_job_watcher.send();
    }

    void enqueue(mcurl_request req, EventCallbackType on_event, bool trace = false)
    {
        std::lock_guard<std::mutex> lk(_locker);
        _in_queue.push({std::move(req), std::move(on_event), trace});
        if (_new_job_watcher.is_active())
            _new_job_watcher.send();
    }

    /// ignore_new_jobs acquiring new jobs
    void ignore_new_jobs()
    {
        if (_mode_wanted == mode_wanted::ignore_new_jobs)
            return;
        _mode_wanted = mode_wanted::ignore_new_jobs;
        _stop_signal_watcher.send();
    }

    void finalize_started_and_stop()
    {
        if (_mode_wanted == mode_wanted::finalize_started_and_stop)
            return;
        _mode_wanted = mode_wanted::stop_the_globe;
        _stop_signal_watcher.send();
    }

    void terminate()
    {
        if (_mode_wanted == mode_wanted::stop_the_globe)
            return;
        _mode_wanted = mode_wanted::stop_the_globe;
        _stop_signal_watcher.send();
    }

    static std::string timestamp()
    {
        time_t t = time(nullptr);
        tm tmp;
        gmtime_r(&t, &tmp);
        std::array<char, 64> buf;
        size_t len = strftime(buf.data(), buf.size(), "%a, %d %b %y %T GMT", &tmp);
        return std::string(buf.data(), buf.data() + len);
    }

private:
    // очередь задач
    std::queue<mcurl::job> _in_queue;
    // выполняющиеся задания (curl умеет прикапывать только указатель)
    std::map<CURL*, job_on_the_go> _on_the_go;

    // мьютекс для защиты очереди задач
    mutable std::mutex _locker;
    // выполняющиеся задания

    static std::string generateBoundary(size_t length = 48)
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

    static void mcode_or_die(const std::string &where, CURLMcode code)
    {
        if (code != CURLM_OK && code != CURLM_BAD_SOCKET)
        {
            throw std::runtime_error(where + " error: " + curl_multi_strerror(code));
        }
    }

    static int trace_cb(CURL *easy, curl_infotype type, unsigned char *data, size_t size, void*)
    {
        try
        {
            job_on_the_go *job;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &job);
            job->j.on_event(job->j.req, mcurl_event{mcurl_trace{type, data, size}});
        }
        catch(...) {}
        return 0;
    }

    static size_t write_body_cb(void *ptr, size_t size, size_t nmemb, void *data)
    {
        std::string &dest = *static_cast<std::string*>(data);
        size_t total = size * nmemb;
        dest.append(static_cast<char*>(ptr), total);
        return total;
    }

    static size_t write_headers_cb(void *input, size_t size, size_t nmemb, void *user_ptr)
    {
        job_on_the_go &j = *static_cast<job_on_the_go*>(user_ptr);
        auto &headers = j.success.headers;
        size_t total = size * nmemb;
        if (!total)
            return total;
        auto raw = std::string_view{static_cast<char*>(input), total};
        // folded header continuation
        if (std::isspace(raw.front()) && raw.front() != '\r')
        {
            auto &s = j.state();
            if (!s.prev_header.empty())
            {
                if (auto it = headers.find(s.prev_header); it != headers.end())
                {
                    raw.remove_prefix(std::min(raw.find_first_not_of(" \t\r\v"), raw.size()));
                    while (raw.size() && std::isspace(raw.back()))
                        raw.remove_suffix(1);
                    it->second.append(raw);
                }
            }
        }
        else if (auto pos = raw.find(':'); pos != std::string::npos)
        {
            auto key = raw.substr(0, pos);
            auto val = raw.substr(std::min(pos + 1, raw.size()), raw.size() - pos - 1);
            val.remove_prefix(std::min(val.find_first_not_of(" \t"), val.size()));
            while (val.size() && std::isspace(val.back()))
                val.remove_suffix(1);
            auto &s = j.state();
            s.prev_header = key;
            headers[s.prev_header] = val;
        }

        //dest->append(static_cast<char*>(ptr), total);
        return total;
    }

    // works for smtp only
    static size_t read_cb(char *buffer, size_t size, size_t nitems, void *user_ptr)
    {
        job_on_the_go *jg = reinterpret_cast<job_on_the_go*>(user_ptr);
        auto &j = jg->j;
        auto req = std::get<smtp_request>(j.req);
        smtp_request::proto_state &s = std::get<smtp_request::proto_state>(jg->proto_state);
        if (s.data.size() == s.bytes_done)
        {
            // определяем, на каком именно этапе опустошился буфер
            switch (s.stage)
            {
            case smtp_request::proto_state::smtp_stage::None:
                // начинаем заголовок
                s.stage = smtp_request::proto_state::smtp_stage::Header;
                s.partnum = -1;
                for (std::string &h : req.headers)
                    s.data += h + "\r\n";
                s.data += "\r\n";
                break;
            case smtp_request::proto_state::smtp_stage::Header:
                // Начинаем текстовую часть сообщения
                // (кодируется в base64, чтобы можно было использовать юникод).
                // Если тело пустое, то пофиг - всё равно воткнем заголовок и пустое тело.
                s.stage = smtp_request::proto_state::smtp_stage::Body;
                s.data = "--" + s.boundary + "\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Transfer-Encoding: base64\r\n\r\n" +
                        base64::encode(req.body, base64::CRLF) + "\r\n";
                break;
            case smtp_request::proto_state::smtp_stage::PartHeader:
            {
                // передаем тело вложения или поле формы
                s.stage = smtp_request::proto_state::smtp_stage::PartBody;
                mcurl_content_part &p = req.attachments.at(static_cast<size_t>(s.partnum));

                if (p.source_type == mcurl_content_part::source_type_t::File)
                {
                    // вложение забрать из файла
                    s.in_stream = std::unique_ptr<std::ifstream>(new std::ifstream(p.source, std::ios::binary | std::ios_base::in));
                    //s._in_stream.open(p.source, std::ios::binary | std::ios_base::in);
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
            case smtp_request::proto_state::smtp_stage::PartBody:
            {
                mcurl_content_part &p = req.attachments.at(static_cast<size_t>(s.partnum));
                // если источник - файл в хорошем состоянии, то нужно прочитать еще кусок
                if (p.source_type == mcurl_content_part::source_type_t::File && *s.in_stream)
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
            case smtp_request::proto_state::smtp_stage::Body:
            {
                // проверяем, не закончилось ли содержимое
                if (s.partnum == static_cast<long>(req.attachments.size()) - 1)
                {
                    // завершающий boundary
                    s.stage = smtp_request::proto_state::smtp_stage::Footer;
                    s.data = "--" + s.boundary + "--\r\n";
                    break;
                }

                // переход к первому или очередному вложению
                ++s.partnum;
                // начинаем заголовки вложений или полей формы
                s.stage = smtp_request::proto_state::smtp_stage::PartHeader;
                mcurl_content_part &p = req.attachments.at(static_cast<size_t>(s.partnum));
                s.data = "--" + s.boundary +
                        "\r\nContent-Type: " + p.content_type +
                        "\r\nContent-Transfer-Encoding: base64\r\n";

                // inline - умолчательное значение (для тела письма)
                if (p.disposition != mcurl_content_part::disposition_t::Inline)
                {
                    // form-data вряд ли используется в протоколе smtp
                    // (оставлено для доработки кода к универсальному виду)
                    s.data += std::string("Content-Disposition: ") +
                            (p.disposition == mcurl_content_part::disposition_t::FormData ? "form-data" : "attachment");
                    if (!p.name.empty())
                        s.data += ";\r\n name=\"" + encode1522(p.name, true) + '"';
                    if (!p.filename.empty())
                        s.data += ";\r\n filename=\"" + encode1522(p.filename, true) + '"';
                    s.data += "\r\n";
                }
                s.data += "\r\n";
                break;
            }
            case smtp_request::proto_state::smtp_stage::Footer:
                // всё отправлено
                s.data.clear();
                s.stage = smtp_request::proto_state::smtp_stage::None;
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

    void new_job_cb(ev::async &, int) noexcept
    {
        // не забираем заданий больше лимита
        if (_on_the_go.size() >= _max_simultanous_transfers)
            return;

        // очередь заданий пуста
        std::unique_lock<std::mutex> lk(_locker);
        if (_in_queue.empty())
        {
            if (_on_the_go.empty() && _terminate_on_finish)
                terminate();
            return;
        }
        lk.unlock();

        CURL *easy;
        // Если в кеше нет хэндлов, создаем новый
        if (_free_easy_handles.empty())
        {
            easy = curl_easy_init();
        }
        else // Забираем из кеша
        {
            easy = std::move(_free_easy_handles.front());
            _free_easy_handles.pop_front();
        }

        // достаем новое задание и перекладываем его в контейнер выполняемых
        lk.lock();
        mcurl::job tmpj = std::move(_in_queue.front());
        _in_queue.pop();
        lk.unlock();

        auto [it, ok] = _on_the_go.insert(std::pair<CURL*, job_on_the_go>(easy, job_on_the_go{easy, std::move(tmpj)}));
        job_on_the_go &jg = it->second;
        jg.easy = easy;

        // функция для завершения задания при ошибке его инициализации и запуска
        auto finalize_job = [this, &jg]()
        {
            jg.fail.status = 1000;
            // если длина ошибки превышает размер буфера, то это избавит от проблем :)
            jg.fail.error[CURL_ERROR_SIZE - 1] = 0;

            if (jg.easy)
            {
                // очищаем данные текущего задания
                _on_the_go.erase(jg.easy);

                curl_easy_reset(jg.easy);
                _free_easy_handles.push_front(jg.easy);
            }

            try
            {
                jg.j.on_event(jg.j.req, mcurl_event{jg.fail});
            }
            catch(...) { }
        };

        auto set_common = [easy](auto &req)
        {
            if (!req.user.empty())
            {
                curl_easy_setopt(easy, CURLOPT_USERNAME, req.user.c_str());
                curl_easy_setopt(easy, CURLOPT_PASSWORD, req.password.c_str());
            }

            if (!req.verify_peer)
            {
                curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L); // don't verify the certificate's name against host
                curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L); // don't verify the peer's SSL certificate
            }
            else if (!req.ca.empty())
                curl_easy_setopt(easy, CURLOPT_CAINFO, std::string(req.ca).c_str());

            if (!req.cert.empty())
            {
                curl_easy_setopt(easy, CURLOPT_SSLCERT, std::string(req.cert + ".pem").c_str());
                curl_easy_setopt(easy, CURLOPT_SSLKEY, std::string(req.cert + ".key").c_str());
            }

            if (req.timeout > 0)
                curl_easy_setopt(easy, CURLOPT_TIMEOUT, req.timeout);
        };

        try
        {
            if (!easy)
                // почему-то не создался хендл
                throw std::runtime_error("unable to acquire easy handle");

            auto &j = jg.j;
            if (j.trace)
            {
                curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, trace_cb);
                //curl_easy_setopt(easy, CURLOPT_DEBUGDATA, &j);
                curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
            }

            //curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);

            if (std::holds_alternative<smtp_request>(j.req))
            {
                smtp_request &req = std::get<smtp_request>(j.req);
                if (req.recipients.empty())
                    throw std::runtime_error("recipient is not specified");

                if (req.sender.empty())
                    throw std::runtime_error("sender is not specified");

                curl_easy_setopt(easy, CURLOPT_URL, req.uri.c_str());
                // тип прокси-сервера определяется по url: https://curl.haxx.se/libcurl/c/CURLOPT_PROXY.html
                // e.g.: socks5://51.15.45.8:1080
                if (!req.proxy.empty())
                    curl_easy_setopt(easy, CURLOPT_PROXY, req.proxy.c_str());

                auto &state = std::get<smtp_request::proto_state>(jg.proto_state);
                // https://curl.haxx.se/mail/tracker-2013-06/0202.html
                // якобы, использовать один и тот же разделитель небезопасно...
                state.boundary = generateBoundary();

                req.headers.push_back("Date: " + timestamp());
                req.headers.push_back("From: " + req.sender);
                req.headers.push_back("To: " + req.recipients.at(0));
                std::string cc;
                for (size_t i = 1; i < req.recipients.size(); ++i)
                    cc += (i > 1 ? ",\r\n " : "") + req.recipients.at(i);
                if (!cc.empty())
                    req.headers.push_back("Cc: " + cc);

                if (!req.subject.empty())
                    req.headers.push_back("Subject: " + encode1522(req.subject, base64::encoder::im_line_length));

                if (!req.body.empty() || !req.attachments.empty())
                    req.headers.push_back("Content-Type: multipart/mixed; boundary=\"" + state.boundary + "\"");

                curl_easy_setopt(easy, CURLOPT_MAIL_FROM, req.sender.c_str());

                for (auto &r: req.recipients)
                    state.curl_recipients = curl_slist_append(state.curl_recipients, r.c_str());
                curl_easy_setopt(easy, CURLOPT_MAIL_RCPT, state.curl_recipients);
                curl_easy_setopt(easy, CURLOPT_MAIL_FROM, req.sender.c_str() );

                curl_easy_setopt(easy, CURLOPT_READFUNCTION, read_cb);
                curl_easy_setopt(easy, CURLOPT_READDATA, &req);
                // без CURLOPT_UPLOAD вообще не вызывается функция read_cb
                curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);

                set_common(req);
            }
            // http request
            else
            {
                auto &req = std::get<http_request>(j.req);
                auto &state = std::get<http_request::proto_state>(jg.proto_state);

                curl_easy_setopt(easy, CURLOPT_URL, req.uri.c_str());
                // тип прокси-сервера определяется по url: https://curl.haxx.se/libcurl/c/CURLOPT_PROXY.html
                // e.g.: socks5://51.15.45.8:1080
                if (!req.proxy.empty())
                    curl_easy_setopt(easy, CURLOPT_PROXY, req.proxy.c_str());

                //bool content_type_found = false;
                // добавляем пользовательские заголовки
                for (std::string &h : req.headers)
                {
                    state.curl_headers = curl_slist_append(state.curl_headers, h.c_str());
                    //if (h.substr(0, 12) == "Content-Type")
                    //    content_type_found = true;
                }

                // По идее, curl должен сам делать заголовок Content-Type: multipart/..
                // при использовании функций, формирующих эти самые кусочки (curl_formadd и т.п.).
                // Если что - раскомментировать соотв. кусочки.

                //if (!j.request_parts.empty())
                //    j.curl_header = curl_slist_append(j.curl_header, "Content-Type: multipart/form-data");

                if (state.curl_headers)
                    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, state.curl_headers);

                if (const auto *parts = std::get_if<std::vector<mcurl_content_part>>(&req.body); parts != nullptr && !parts->empty())
                {
                    state.mime = curl_mime_init(easy);
                    for (auto &p: *parts)
                    {
                        auto part = curl_mime_addpart(state.mime);
                        if (!p.name.empty())
                            curl_mime_name(part, p.name.c_str());
                        if (!p.filename.empty())
                            curl_mime_filename(part, p.filename.c_str());
                        if (!p.content_type.empty())
                            curl_mime_type(part, p.content_type.c_str());

                        if (p.disposition != mcurl_content_part::disposition_t::FormData)
                        {
                            std::string disposition = "Content-Disposition: ";
                            disposition += (p.disposition == mcurl_content_part::disposition_t::Attachment ? "attachment" : "inline");
                            if (!p.name.empty())
                                disposition += "; name=\"" + p.name + "\"";
                            if (!p.filename.empty())
                                disposition += "; filename=\"" + p.filename + "\"";
                            auto headers = curl_slist_append(nullptr, disposition.c_str());
                            curl_mime_headers(part, headers, 1);
                        }

                        if (p.source_type == mcurl_content_part::source_type_t::File)
                        {
                            curl_mime_filedata(part, p.source.c_str());
                        }
                        else
                        {
                            curl_mime_data(part, p.source.data(), p.source.size());
                        }
                    }
                    curl_easy_setopt(easy, CURLOPT_MIMEPOST, state.mime);
                }
                else if (const auto *body = std::get_if<std::string>(&req.body); body != nullptr && !body->empty())
                {
                    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body->data());
                    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, body->size());
                    curl_easy_setopt(easy, CURLOPT_POST, true);
                }
                else
                {
                    curl_easy_setopt(easy, CURLOPT_HTTPGET, true);
                }

                if(!req.method.empty()) {
                    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
                }

                set_common(req);
            }
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body_cb);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &jg.success.body);
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_headers_cb);
            curl_easy_setopt(easy, CURLOPT_HEADERDATA, &jg);
            //curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, true);
            curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, jg.fail.error);
            curl_easy_setopt(easy, CURLOPT_PRIVATE, &jg);
            curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 20L);
            curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 8L);
            curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);

            curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1);

            // добавление easy handle инициирует работу (запуск таймера и т.п.)
            CURLMcode rc = curl_multi_add_handle(_multi, easy);

            mcode_or_die("new_job_cb: curl_multi_add_handle", rc);

        }
        catch (const std::runtime_error &e)
        {
            strncpy(jg.fail.error, e.what(), CURL_ERROR_SIZE);
            jg.fail.error[CURL_ERROR_SIZE - 1] = '\0';
            finalize_job();
        }
        catch(...)
        {
            strncpy(jg.fail.error, "error fetching new curl job", CURL_ERROR_SIZE);
            finalize_job();
        }

        // пробуем забрать еще одно задание
        if (_new_job_watcher.is_active())
            _new_job_watcher.send();
    }

    void check_multi_info() noexcept
    {
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(_multi, &msgs_left)))
        {
            if (msg->msg == CURLMSG_DONE)
            {
                CURL *easy = msg->easy_handle;
                //CURLcode res = msg->data.result;

                // man: "If you want to re-use an easy handle that was added to the multi handle for transfer,
                // you must first remove it from the multi stack and then re-add it again
                // (possibly after having altered some options at your own choice)."
                curl_multi_remove_handle(_multi, easy);

                job_on_the_go *job;
                curl_easy_getinfo(easy, CURLINFO_PRIVATE, &job);

                // передаем результат нaружу
                try
                {
                    if (job->fail.error[0])
                    {
                        job->j.on_event(job->j.req, mcurl_event{job->fail});
                    }
                    else
                    {
                        // If we are succesful we need some extra data
                        long status = -1;
                        CURLcode ret = curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &status);

                        if (ret == CURLE_OK)
                        {
                            job->success.status = status;
                            job->j.on_event(job->j.req, mcurl_event{job->success});
                        }
                        else
                        {
                            strncpy(job->fail.error, curl_easy_strerror(ret), sizeof(job->fail.error) - 1);
                            job->fail.error[sizeof(job->fail.error) - 1] = '\0';
                            job->fail.status = msg->data.result;
                            job->j.on_event(job->j.req, mcurl_event{job->fail});
                        }
                    }
                }
                catch(...) { }

                // очищаем данные текущего задания
                _on_the_go.erase(easy);

                curl_easy_reset(easy);
                _free_easy_handles.push_front(easy);

                // попробуем взять еще одно задание
                if (_new_job_watcher.is_active())
                    _new_job_watcher.send();
                else if (_on_the_go.empty() && _terminate_on_finish)
                    terminate();
                return;

            }
        }
    }

    static void event_cb(struct ev_loop *, ev_io *w, int revents)
    {
        mcurl *owner = static_cast<mcurl*>(w->data);
        CURLMcode rc;
        int action = (revents & EV_READ ? CURL_POLL_IN : 0) | (revents & EV_WRITE ? CURL_POLL_OUT : 0);

        int running_handles;
        rc = curl_multi_socket_action(owner->_multi, w->fd, action, &running_handles);
        mcode_or_die("event_cb: curl_multi_socket_action", rc);
        owner->check_multi_info();
        if (running_handles <= 0)
        {
            // last transfer done, kill timeout
            owner->_timeout_timer.stop();
        }
    }

    static int multi_timer_cb(CURLM*, long timeout_ms, mcurl *owner)
    {
        // man: A timeout_ms value of -1 means you should delete your timer.
        owner->_timeout_timer.stop();
        if (timeout_ms >= 0)
            owner->_timeout_timer.start(timeout_ms / 1000.0);
        return 0;
    }

    static int socket_cb(CURL*, curl_socket_t s, int what, mcurl *owner, sock_info *si)
    {
        if (what == CURL_POLL_REMOVE)
        {
            if (si)
            {
                // отцепляем libev от сокета и удаляем контейнер со всеми запчастями
                si->ev.stop();
                delete si;
                curl_multi_assign(owner->_multi, s, nullptr);
            }
        }
        else
        {
            int kind = (what & CURL_POLL_IN ? EV_READ : 0) | (what & CURL_POLL_OUT ? EV_WRITE : 0);
            if (!si) // цепляем libev к сокету
            {
                si = new sock_info;
                si->sock = s;
                //si->easy = easy_handle;
                si->ev.data = owner;
                si->ev.set(owner->_loop);
                si->ev.set_(owner, event_cb);
                si->ev.set(si->sock, kind);
                si->ev.start();
                curl_multi_assign(owner->_multi, s, si);
            }
            else    // меняем тип ожидаемых на сокете событий
            {
                si->ev.set(kind); // перезапуск внутри ev::io::set()
            }
        }

        return 0;
    }

};

#endif // MCURL_H
