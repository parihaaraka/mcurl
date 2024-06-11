#include "mcurl.h"
#include <algorithm>
#include <string.h>
#include <random>

std::atomic<int> MCurl::instances_counter{0};

MCurl::MCurl(struct ev_loop *loop)
    : _loop(loop), _multi(0), _max_simultanous_transfers(10)
{
    if (!instances_counter.load())
    {
        CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (res)
            throw std::runtime_error(curl_easy_strerror(res));
        ++instances_counter;
    }

    _multi = curl_multi_init();
    if (!_multi)
    {
        curl_global_cleanup();
        throw std::runtime_error("curl_multi_init() failed");
    }

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

MCurl::~MCurl()
{
    // прибиваем всю мелочь
    _mode_wanted = 't';
    stop_cb(_stop_signal_watcher, 0);

    // задраиваем люки
    curl_multi_cleanup(_multi);   // curl_multi_cleanup should be called when all easy handles are removed

    --instances_counter;
    if (!instances_counter.load())
        curl_global_cleanup();
}

void MCurl::enqueue(MCurl::Job &&job)
{
    std::lock_guard<std::mutex> lk(_locker);
    _in_queue.push(std::move(job));
    if (_new_job_watcher.is_active())
        _new_job_watcher.send();
}

void MCurl::start(bool terminate_on_finish, struct ev_loop *loop)
{
    _terminate_on_finish = terminate_on_finish;
    if (isActive())
        return;

    if (loop)
    {
        _timeout_timer.set(loop);
        _new_job_watcher.set(loop);
        _stop_signal_watcher.set(loop);
        _loop = loop;
    }

    _timeout_timer.set<MCurl, &MCurl::timer_cb>(this);

    _mode_wanted = 'a';  // active
    _stop_signal_watcher.set<MCurl, &MCurl::stop_cb>(this);
    _stop_signal_watcher.start();

    _new_job_watcher.set<MCurl, &MCurl::new_job_cb>(this);
    _new_job_watcher.start();

    // если задания добавляли до запуска, то заберем их
    _new_job_watcher.send();
}

void MCurl::stop()
{
    if (_mode_wanted == 's')
        return;
    _mode_wanted = 's';
    _stop_signal_watcher.send();
}

void MCurl::terminate()
{
    if (_mode_wanted == 't')
        return;
    _mode_wanted = 't';
    _stop_signal_watcher.send();
}

MCurl &MCurl::onComplete(const std::function<void (std::shared_ptr<MCurl::UserData> &)> &callback)
{
    _completeCallback = callback;
    return *this;
}

MCurl &MCurl::onFailue(const std::function<void (std::shared_ptr<MCurl::UserData> &, std::string &request, std::vector<std::string> &request_header, const char *error, long status)> &callback)
{
    _failureCallback = callback;
    return *this;
}

MCurl &MCurl::onSuccess(const std::function<void (std::shared_ptr<MCurl::UserData> &, std::string &request, std::vector<std::string> &request_header, std::string &response, std::string &response_header, long status)> &callback)
{
    _successCallback = callback;
    return *this;
}

MCurl &MCurl::onTrace(const std::function<void(std::shared_ptr<UserData>&, const char *key, unsigned char *value, size_t value_size)> &callback)
{
    _traceCallback = callback;
    return *this;
}

std::string MCurl::timestamp()
{
    time_t t = time(nullptr);
    tm tmp;
    gmtime_r(&t, &tmp);
    std::array<char, 64> buf;
    size_t len = strftime(buf.data(), buf.size(), "%a, %d %b %y %T GMT", &tmp);
    return std::string(buf.data(), buf.data() + len);
}

std::string MCurl::encode1522(const std::string &value, bool wrap)
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

void MCurl::new_job_cb(ev::async &, int) noexcept
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
    MCurl::Job job = std::move(_in_queue.front());
    _in_queue.pop();
    lk.unlock();
    MCurl::Job *cur_job_ptr = &job;

    // функция для завершения задания при ошибке его инициализации и запуска
    auto finalize_job = [this, &cur_job_ptr, &easy]()
    {
        // если длина ошибки превышает размер буфера, то это избавит от проблем :)
        cur_job_ptr->error[CURL_ERROR_SIZE - 1] = 0;

        if (easy)
        {
            // очищаем данные текущего задания
            _on_the_go.erase(easy);

            curl_easy_reset(easy);
            _free_easy_handles.push_front(easy);
        }

        if (_failureCallback)
        {
            try
            {
                _failureCallback(cur_job_ptr->user_data,
                                 cur_job_ptr->request,
                                 cur_job_ptr->request_header,
                                 cur_job_ptr->error,
                                 1000);
            }
            catch(...) { }
        }

        if (_completeCallback)
        {
            try
            {
                _completeCallback(cur_job_ptr->user_data);
            }
            catch(...) { }
        }
    };

    try
    {
        if (!easy)
        {
            // почему-то не создался хендл
            throw std::runtime_error("unable to acquire easy handle");
        }

        if(_traceCallback) {
            curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, trace_cb);
            curl_easy_setopt(easy, CURLOPT_DEBUGDATA, this);
            curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
        }

        _on_the_go.insert(std::make_pair(easy, std::move(job)));
        MCurl::Job &j = _on_the_go.at(easy);
        cur_job_ptr = &j;

        // протокол (smtp или http) определяется по url
        // e.g.: smtp://mx.mydomain.com:25
        curl_easy_setopt(easy, CURLOPT_URL, j.url.c_str());

        // тип прокси-сервера также определяется по url: https://curl.haxx.se/libcurl/c/CURLOPT_PROXY.html
        // e.g.: socks5://51.15.45.8:1080
        if (!j.proxy.empty())
            curl_easy_setopt(easy, CURLOPT_PROXY, j.proxy.c_str());

        //curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);

        // smtp request
        if (!j.url.compare(0, 4, "smtp"))
        {
            if (j.recipients.empty())
                throw std::runtime_error("recipient is not specified");

            if (j.sender.empty())
                throw std::runtime_error("sender is not specified");

            // https://curl.haxx.se/mail/tracker-2013-06/0202.html
            // якобы, использовать один и тот же разделитель небезопасно...
            j._boundary = generateBoundary();

            j.request_header.push_back("Date: " + timestamp());
            j.request_header.push_back("From: " + j.sender);
            j.request_header.push_back("To: " + j.recipients.at(0));
            std::string cc;
            for (size_t i = 1; i < j.recipients.size(); ++i)
                cc += (i > 1 ? ",\r\n " : "") + j.recipients.at(i);
            if (!cc.empty())
                j.request_header.push_back("Cc: " + cc);

            if (!j.subject.empty())
                j.request_header.push_back("Subject: " + encode1522(j.subject, base64::encoder::im_line_length));

            if (!j.request.empty() || !j.request_parts.empty())
                j.request_header.push_back("Content-Type: multipart/mixed; boundary=\"" + j._boundary + "\"");

            curl_easy_setopt(easy, CURLOPT_MAIL_FROM, j.sender.c_str());

            for (auto &r: j.recipients)
                j.curl_recipients = curl_slist_append(j.curl_recipients, r.c_str());
            curl_easy_setopt(easy, CURLOPT_MAIL_RCPT, j.curl_recipients);
            curl_easy_setopt(easy, CURLOPT_MAIL_FROM, j.sender.c_str() );

            curl_easy_setopt(easy, CURLOPT_READFUNCTION, read_cb);
            curl_easy_setopt(easy, CURLOPT_READDATA, &j);
            // без CURLOPT_UPLOAD вообще не вызывается функция read_cb
            curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);

        }
        // http request
        else
        {
            //bool content_type_found = false;
            // добавляем пользовательские заголовки
            for (std::string &h : j.request_header)
            {
                j.curl_header = curl_slist_append(j.curl_header, h.data());
                //if (h.substr(0, 12) == "Content-Type")
                //    content_type_found = true;
            }

            // По идее, curl должен сам делать заголовок Content-Type: multipart/..
            // при использовании функций, формирующих эти самые кусочки (curl_formadd и т.п.).
            // Если что - раскомментировать соотв. кусочки.

            //if (!j.request_parts.empty())
            //    j.curl_header = curl_slist_append(j.curl_header, "Content-Type: multipart/form-data");

            if (j.curl_header)
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, j.curl_header);

            if (!j.request_parts.empty())
            {
                curl_httppost *lastptr = nullptr;
                for (size_t i = 0; i < j.request_parts.size(); ++i)
                {
                    auto const &part = j.request_parts[i];
                    //  DEPRECATED in 7.56.0 !!!
                    curl_formadd(&(j.formpost), &lastptr,
                                 CURLFORM_COPYNAME, part.name.c_str(),
                                 CURLFORM_COPYCONTENTS, part.source.c_str(),
                                 CURLFORM_END);
                }
                curl_easy_setopt(easy, CURLOPT_HTTPPOST, j.formpost);
            }
            else if (!j.request.empty())
            {
                curl_easy_setopt(easy, CURLOPT_POSTFIELDS, j.request.data());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, j.request.size());
                curl_easy_setopt(easy, CURLOPT_POST, true);
            }
            else
            {
                curl_easy_setopt(easy, CURLOPT_HTTPGET, true);
            }

            if(!j.custom_method.empty()) {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, j.custom_method.c_str());
            }
        }
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &j.response);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEHEADER, &j.response_header);
        //curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, true);
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, j.error);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, &j);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 20L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 8L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);

        if (!j.user.empty())
        {
            curl_easy_setopt(easy, CURLOPT_USERNAME, j.user.c_str());
            curl_easy_setopt(easy, CURLOPT_PASSWORD, j.password.c_str());
        }

        if ( ! j.verify_peer ) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L); // don't verify the certificate's name against host
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L); // don't verify the peer's SSL certificate
        }
        else if (!j.ca.empty())
            curl_easy_setopt(easy, CURLOPT_CAINFO, std::string(j.ca).c_str());

        if ( ! j.cert.empty() ) {
            curl_easy_setopt(easy, CURLOPT_SSLCERT, std::string(j.cert + ".pem").c_str());
            curl_easy_setopt(easy, CURLOPT_SSLKEY, std::string(j.cert + ".key").c_str());
        }
        curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1);

        // добавление easy handle инициирует работу (запуск таймера и т.п.)
        CURLMcode rc = curl_multi_add_handle(_multi, easy);

        mcode_or_die("new_job_cb: curl_multi_add_handle", rc);

    }
    catch (const std::runtime_error &e)
    {
        strncpy(cur_job_ptr->error, e.what(), CURL_ERROR_SIZE);
        finalize_job();
    }
    catch(...)
    {
        strncpy(cur_job_ptr->error, "error on fetching new curl job", CURL_ERROR_SIZE);
        finalize_job();
    }

    // пробуем забрать еще одно задание
    if (_new_job_watcher.is_active())
        _new_job_watcher.send();
}

void MCurl::stop_cb(ev::async &, int) noexcept
{
    if (_mode_wanted == 's')
    {
        _stop_signal_watcher.stop();
        _new_job_watcher.stop();
    }
    else if (_mode_wanted == 't')
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

void MCurl::check_multi_info() noexcept
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

            MCurl::Job *job;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &job);

            // передаем результат нaружу
            try
            {
                if (job->error[0])
                {
                    if (_failureCallback)
                        _failureCallback(job->user_data,
                                         job->request,
                                         job->request_header,
                                         job->error,
                                         msg->data.result);
                }
                else
                {
                    // If we are succesful we need some extra data
                    long status = -1;
                    CURLcode ret = curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &status);

                    if (ret == CURLE_OK)
                    {
                        if (_successCallback)
                            _successCallback(job->user_data,
                                             job->request,
                                             job->request_header,
                                             job->response,
                                             job->response_header,
                                             status);
                    }
                    else
                    {
                        if (_failureCallback)
                            _failureCallback(job->user_data,
                                             job->request,
                                             job->request_header,
                                             curl_easy_strerror(ret),
                                             msg->data.result);
                    }
                }
            }
            catch(...) { }

            // обратный вызов при любом результате
            if (_completeCallback)
            {
                try
                {
                    _completeCallback(job->user_data);
                }
                catch(...) { }
            }

            // очищаем данные текущего задания
            _on_the_go.erase(easy);

            curl_easy_reset(easy);
            _free_easy_handles.push_front(easy);

            // попробуем взять еще одно задание
            if (_new_job_watcher.is_active())
                _new_job_watcher.send();
        }
    }
}

void MCurl::event_cb(struct ev_loop *, ev_io *w, int revents)
{
    MCurl *owner = static_cast<MCurl*>(w->data);
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

void MCurl::timer_cb(ev::timer &, int)
{
    int running_handles;
    CURLMcode rc = curl_multi_socket_action(_multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
    mcode_or_die("timer_cb: curl_multi_socket_action", rc);
    check_multi_info();
}

int MCurl::multi_timer_cb(CURLM *, long timeout_ms, MCurl *owner)
{
    // man: A timeout_ms value of -1 means you should delete your timer.
    owner->_timeout_timer.stop();
    if (timeout_ms >= 0)
        owner->_timeout_timer.start(timeout_ms / 1000.0);
    return 0;
}

void MCurl::mcode_or_die(const std::string &where, CURLMcode code)
{
    if (code != CURLM_OK && code != CURLM_BAD_SOCKET)
    {
        throw std::runtime_error(where + " error: " + curl_multi_strerror(code));
    }
}

int MCurl::socket_cb(CURL *easy_handle, curl_socket_t s, int what, MCurl *owner, SockInfo *si)
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
            si = new SockInfo;
            si->sock = s;
            si->easy = easy_handle;
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

size_t MCurl::write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
    size_t total = size * nmemb;
    std::string *dest = static_cast<std::string*>(data);
    dest->append(static_cast<char*>(ptr), total);
    return total;
}

size_t MCurl::read_cb(char *buffer, size_t size, size_t nitems, void *user_ptr)
{
    Job *j = reinterpret_cast<Job*>(user_ptr);
    if (j->_data.size() == j->_bytes_done)
    {
        // определяем, на каком именно этапе опустошился буфер
        switch (j->_stage)
        {
        case Job::SmtpStage::None:
            // начинаем заголовок
            j->_stage = Job::SmtpStage::Header;
            j->_partnum = -1;
            for (std::string &h : j->request_header)
                j->_data += h + "\r\n";
            j->_data += "\r\n";
            break;
        case Job::SmtpStage::Header:
            // Начинаем текстовую часть сообщения
            // (кодируется в base64, чтобы можно было использовать юникод).
            // Если тело пустое, то пофиг - всё равно воткнем заголовок и пустое тело.
            j->_stage = Job::SmtpStage::Body;
            j->_data = "--" + j->_boundary + "\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Transfer-Encoding: base64\r\n\r\n" +
                    base64::encode(j->request, base64::CRLF) + "\r\n";
            break;
        case Job::SmtpStage::PartHeader:
        {
            // передаем тело вложения или поле формы
            j->_stage = Job::SmtpStage::PartBody;
            Job::ContentPart &p = j->request_parts.at(static_cast<size_t>(j->_partnum));

            if (p.source_type == Job::ContentPart::SourceType::File)
            {
                // вложение забрать из файла
                j->_in_stream = std::unique_ptr<std::ifstream>(new std::ifstream(p.source, std::ios::binary | std::ios_base::in));
                //j->_in_stream.open(p.source, std::ios::binary | std::ios_base::in);
                if (!*j->_in_stream)
                {
                    j->_in_stream->close();
                    return CURL_READFUNC_ABORT;
                }
            }
            else
            {
                // вложение передано в буфере
                j->_data = base64::encode(p.source, base64::CRLF);
                if (!j->_data.empty())
                    break;
            }
        }
        //[[clang::fallthrough]];
        case Job::SmtpStage::PartBody:
        {
            Job::ContentPart &p = j->request_parts.at(static_cast<size_t>(j->_partnum));
            // если источник - файл в хорошем состоянии, то нужно прочитать еще кусок
            if (p.source_type == Job::ContentPart::SourceType::File && *j->_in_stream)
            {
                std::vector<char> buf(100 * 1024);
                j->_in_stream->read(buf.data(), buf.size());
                // если достигнут конец файла, то меняем размер буфера до фактически считанного количества байт
                if (j->_in_stream->eof())
                    buf.resize(j->_in_stream->gcount());
                bool is_bad = j->_in_stream->bad();
                // при любом недоразумении (включая конец файла) закрываем файл
                if (!j->_in_stream)
                    j->_in_stream->close();

                // если проблема не в EOF, то прерываем задание
                if (is_bad)
                    return CURL_READFUNC_ABORT;

                // формируем закодированный в base64 кусок файла
                j->_data.resize(2 * buf.size() + 3);
                long len = 0;
                if (!buf.empty())
                    len += j->_b64encoder.encode(buf.data(), buf.size(), &j->_data[0]);
                if (!j->_in_stream->is_open())
                    len += j->_b64encoder.encode_end(&j->_data[0] + len);
                j->_data.resize(len);

                // если снова получился непустой буфер, то продолжим передачу в рамках текущего этапа
                if (len)
                    break;
            }
        }
        //[[clang::fallthrough]];
        case Job::SmtpStage::Body:
        {
            // проверяем, не закончилось ли содержимое
            if (j->_partnum == static_cast<long>(j->request_parts.size()) - 1)
            {
                // завершающий boundary
                j->_stage = Job::SmtpStage::Footer;
                j->_data = "--" + j->_boundary + "--\r\n";
                break;
            }

            // переход к первому или очередному вложению
            ++j->_partnum;
            // начинаем заголовки вложений или полей формы
            j->_stage = Job::SmtpStage::PartHeader;
            Job::ContentPart &p = j->request_parts.at(static_cast<size_t>(j->_partnum));
            j->_data = "--" + j->_boundary +
                    "\r\nContent-Type: " + p.content_type +
                    "\r\nContent-Transfer-Encoding: base64\r\n";

            // inline - умолчательное значение (для тела письма)
            if (p.disposition != Job::ContentPart::Disposition::Inline)
            {
                // form-data вряд ли используется в протоколе smtp
                // (оставлено для доработки кода к универсальному виду)
                j->_data += std::string("Content-Disposition: ") +
                        (p.disposition == Job::ContentPart::Disposition::FormData ? "form-data" : "attachment");
                if (!p.name.empty())
                    j->_data += ";\r\n name=\"" + encode1522(p.name, true) + '"';
                if (!p.filename.empty())
                    j->_data += ";\r\n filename=\"" + encode1522(p.filename, true) + '"';
                j->_data += "\r\n";
            }
            j->_data += "\r\n";
            break;
        }
        case Job::SmtpStage::Footer:
            // всё отправлено
            j->_data.clear();
            j->_stage = Job::SmtpStage::None;
            break;
        }

        // рестарт отсчета переданной части текущего буфера
        j->_bytes_done = 0;
    }

    size_t len = std::min<size_t>(size * nitems, j->_data.size() - j->_bytes_done);
    if (len)
    {
        // собственно, передача очередного куска данных curl'у
        memcpy(buffer, j->_data.data() + j->_bytes_done, len);
        j->_bytes_done += len;
    }
    return len;
}

int MCurl::trace_cb(CURL *easy, curl_infotype type, unsigned char *data, size_t size, MCurl *owner) {
    const char *text;

    switch(type) {
        case CURLINFO_TEXT:
            fprintf(stderr, "== Info: %s", data);
            /* FALLTHROUGH */
        default: /* in case a new one is introduced to shock us */
            return 0;

        case CURLINFO_HEADER_OUT:
            text = "=> Send header";
            break;
        case CURLINFO_DATA_OUT:
            text = "=> Send data";
            break;
        case CURLINFO_HEADER_IN:
            text = "<= Recv header";
            break;
        case CURLINFO_DATA_IN:
            text = "<= Recv data";
            break;
    }
    if(owner->_traceCallback) {
        try {
            MCurl::Job *job;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &job);
            owner->_traceCallback(job->user_data, text, data, size);
        } catch(...) {
        }
    }
    return 0;
}

std::string MCurl::generateBoundary(size_t length)
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

MCurl::Job::ContentPart MCurl::Job::ContentPart::file(std::string file_path, std::string destination_file_name)
{
    Job::ContentPart p;
    p.source = file_path;
    if (destination_file_name.empty())
    {
        size_t pos = file_path.find_last_of("/\\");
        if (pos == std::string::npos)
            destination_file_name = file_path;
        else
            destination_file_name = file_path.substr(pos + 1);
    }
    p.filename = destination_file_name;
    return p;
}

MCurl::Job::ContentPart MCurl::Job::ContentPart::file_from_buffer(std::string buffer, std::string destination_file_name)
{
    Job::ContentPart p;
    p.source_type = SourceType::Buffer;
    p.source = buffer;
    if (!destination_file_name.empty())
    {
        p.filename = destination_file_name;
    }
    return p;
}

MCurl::Job::ContentPart MCurl::Job::ContentPart::form_data(std::string name, std::string buffer)
{
    Job::ContentPart p;
    p.source_type = SourceType::Buffer;
    p.disposition = Disposition::FormData;
    p.source = buffer;
    p.name = name;
    return p;
}
