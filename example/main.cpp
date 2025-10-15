#include <iostream>
#include "mcurl.h"
#include "function2.hpp"

void signal_cb(ev::sig &w, int)
{
    std::cout << "caught signal " << w.signum << std::endl;
    w.loop.break_loop();
}

int main(int argc, char *argv[])
{
    //mcurl sender;
    //  make the result handler capture non-copyable objects:
    mcurl<MCURL_CALLABLE(fu2::unique_function)> sender;

    ev::default_loop loop;
    ev::sig term_signal_watcher;
    term_signal_watcher.set(loop.raw_loop);
    term_signal_watcher.set<&signal_cb>();
    term_signal_watcher.start(SIGTERM);
    term_signal_watcher.loop.unref();

    ev::sig int_signal_watcher;
    int_signal_watcher.set(loop.raw_loop);
    int_signal_watcher.set<&signal_cb>();
    int_signal_watcher.start(SIGINT);
    int_signal_watcher.loop.unref();

    size_t reqCount = 2;
    if (argc > 1)
    {
        auto tmp = strtol(argv[1], nullptr, 10);
        if (tmp > 0)
            reqCount = static_cast<size_t>(tmp);
    }

    for (size_t i = 0; i < reqCount; ++i)
    {
        http_request r;
        r.uri = "https://httpbin.org/post";
        r.body = "request=" + std::to_string(i);

        auto print = [](size_t i, mcurl_request &, mcurl_event e)
        {
            if (std::holds_alternative<mcurl_success>(e))
            {
                auto &res = std::get<mcurl_success>(e);
                std::cout << i << ": response " << res.status << std::endl;
                for (auto &h: res.headers)
                    std::cout << "   " << h.first << ": " << h.second;
                std::cout << std::endl << res.body << std::endl;
            }
            else if (std::holds_alternative<mcurl_fail>(e))
            {
                auto &res = std::get<mcurl_fail>(e);
                std::cout << i << ": error " << res.error << std::endl
                     << "status " << res.status << std::endl;
            }
        };

        sender.enqueue({r}, [i, &sender, reqCount, &print](mcurl_request &r, mcurl_event e){
            print(i, r, e);
            // request again
            auto &req = std::get<http_request>(r);
            auto j = i + reqCount;
            req.body = "request=" + std::to_string(j);

            sender.enqueue({std::move(r)}, [j, &print](mcurl_request &r, mcurl_event e){
                print(j, r, e);
            });
        });
    }

    sender.start(true, loop.raw_loop);
    loop.run(0);
    std::cout << "mcurl loop stopped" << std::endl;
    return 0;
}
