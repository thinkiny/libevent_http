#pragma once

#include <event2/event.h>
#include <event2/thread.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/http_struct.h>
#include <unordered_map>
#include <functional>
#include <memory>
#include <unistd.h>
#include <future>
#include <vector>
#include <string.h>
#include <sys/eventfd.h>
#include <errno.h>

namespace http {

namespace internal {

template <typename T, void(*Free)(T*)>
class Handle {
    static void SafeFree(T *t) {
        if(t) {
            Free(t);
        }
    }
public:
    Handle(T *t) : handle_(t, SafeFree){}
    Handle() : handle_(nullptr, SafeFree) {}

    operator bool() {
        return handle_.get() != nullptr;
    }

    operator T*() {
        return handle_.get();
    }

    T* operator->() {
        return handle_.get();
    }

private:
    std::unique_ptr<T, decltype(Free)> handle_;
};

const char* evhttp_err2str(evhttp_request_error error) {
    switch(error) {
        case EVREQ_HTTP_TIMEOUT:
            return "Timeout reached";
        case EVREQ_HTTP_EOF:
            return "EOF reached";
        case EVREQ_HTTP_INVALID_HEADER:
            return "Error while reading header, or invalid header";
        case EVREQ_HTTP_BUFFER_ERROR:
            return "Error encountered while reading or writing";
        case EVREQ_HTTP_REQUEST_CANCEL:
            return "The evhttp_cancel_request() called on this request";
        case EVREQ_HTTP_DATA_TOO_LONG:
            return "Body is greater then max_size";
        default:
            return "Unknown error";
    }
}

using EvHttpConnection = Handle<evhttp_connection, evhttp_connection_free>;
using EvHttpUri = Handle<evhttp_uri, evhttp_uri_free>;
using EventBase = Handle<event_base, event_base_free>;
using Event = Handle<event, event_free>;

}

class EventLoop {
    EventLoop(const EventLoop&);
    EventLoop& operator=(const EventLoop&);
    EventLoop(EventLoop&&);
public:
    static std::shared_ptr<EventLoop> New() {
        return std::make_shared<EventLoop>();
    }

    EventLoop() {
        evthread_use_pthreads();
        base_ = event_base_new();
        notify_fd_ = eventfd(0, EFD_NONBLOCK);
        notify_ev_ = event_new(base_, notify_fd_, EV_READ | EV_PERSIST, Wakeup, nullptr);
        event_add(notify_ev_, nullptr);
    }

    ~EventLoop() {
        event_del(notify_ev_);
        if(bg_.joinable()) {
            bg_.join();
        }
        close(notify_fd_);
    }

    event_base* GetEventBase() {
        return base_;
    }

    void Interrupt() {
        uint64_t dummy = 1;
        dummy = write(notify_fd_, &dummy, sizeof(dummy));
    }

    void Run() {
        event_base_loop(base_, 0);
    }

    void Stop() {
        event_base_loopexit(base_, nullptr);
    }

    void RunInBackground() {
        bg_ = std::thread(event_base_loop, static_cast<event_base*>(base_), 0);
    }

private:
    static void Wakeup(evutil_socket_t fd, short event, void *arg) {
        uint64_t dummy;
        dummy = read(fd, &dummy, sizeof(dummy));
    }

private:
    int notify_fd_ = -1;
    internal::Event notify_ev_;
    internal::EventBase base_;
    std::thread bg_;
};

class Response {
    Response(const Response&);
    Response& operator=(const Response&);
public:
    struct BodySlice {
        char *content;
        size_t len;

        char& operator[](int n) {
            return content[n];
        }
    };

    Response(evhttp_request *req) : req_(req) {
        evhttp_request_own(req_);
    }

    Response(Response&& response) {
        req_ = response.req_;
        response.req_ = nullptr;
    }

    ~Response() {
        if(req_) {
            evhttp_request_free(req_);
        }
    }

    int GetResponseCode() const {
        return req_->response_code;
    }

    const char* GetHeader(const char *key) const {
        return evhttp_find_header(req_->input_headers, key);
    }

    BodySlice GetBody() {
        auto *buf = req_->input_buffer;
        if(buf) {
            return {reinterpret_cast<char*>(evbuffer_pullup(buf, -1)),
                    req_->body_size};
        }
        return {nullptr, 0};
    }

private:
    evhttp_request *req_;
};

class Get {
    static void GetError(evhttp_request_error error, void *arg) {
        Get *get_req = (Get*)arg;
        get_req->error_func_(internal::evhttp_err2str(error));
        get_req->has_error_ = true;
    }

    static void GetComplete(evhttp_request *req, void *arg) {
        Get *get_req = (Get*)arg;
        if(!get_req->has_error_) {
            get_req->finish_func_(Response(req));
        }
    }
public:
    typedef std::function<void(Response&&)> FinishFunc;
    typedef std::function<void(const char*)> ErrorFunc;

    Get(const Get&) = delete;
    Get& operator=(const Get&) = delete;
    Get(Get&&) = default;

    Get(const char *url) {
        loop_ = std::make_shared<EventLoop>();
        loop_->RunInBackground();
        Init(url);
    }

    Get(const char *url, std::shared_ptr<EventLoop> loop) {
        loop_ = loop;
        Init(url);
    }

    Get& SetTimeout(int ms) & {
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        evhttp_connection_set_timeout_tv(conn_, &tv);
        return *this;
    }

    Get&& SetTimeout(int ms) && {
        return std::move(this->SetTimeout(ms));
    }

    Get& SetRetryMax(int times) & {
        evhttp_connection_set_retries(conn_, times);
        return *this;
    }

    Get&& SetRetryMax(int times) && {
        return std::move(this->SetRetryMax(times));
    }

    Get& AddHeader(const char *key, std::string value) & {
        headers_[key] = std::move(value);
        return *this;
    }

    Get&& AddHeader(const char *key, std::string value) && {
        return std::move(this->AddHeader(key, value));
    }

    template <typename T>
    void Execute(T&& finish_func) {
        std::string path_query;
        auto *req = evhttp_request_new(&GetComplete, this);
        auto *path = evhttp_uri_get_path(uri_);
        auto *query = evhttp_uri_get_query(uri_);
        path_query += path[0] != '\0' ? path : "/";
        if(query && query[0] != '\0') {
            path_query += "?";
            path_query += query;
        }

        if(error_func_) {
            evhttp_request_set_error_cb(req, GetError);
        }

        has_error_ = false;
        finish_func_ = std::forward<T>(finish_func);
        for(auto& header : headers_) {
            evhttp_add_header(req->output_headers, header.first.c_str(), header.second.c_str());
        }

        evhttp_add_header(req->output_headers, "Host", evhttp_uri_get_host(uri_));
        if(evhttp_make_request(conn_, req, EVHTTP_REQ_GET,  path_query.c_str()) < 0) {
            throw std::runtime_error(strerror(errno));
        }
        loop_->Interrupt();
    }

    template <typename T, typename U>
    void Execute(T&& finish_func, U&& error_func) {
        error_func_ = std::forward<U>(error_func);
        Execute(std::forward<T>(finish_func));
    }

    Response Execute() {
        std::promise<Response> promise;
        return Execute(promise).get();
    }

    std::future<Response> Execute(std::promise<Response>& promise) {
        try {
            Execute([&promise](Response&& res) mutable {
                    promise.set_value(std::move(res));
                }, [&promise](const char *msg) mutable {
                    promise.set_exception(std::make_exception_ptr(std::runtime_error(msg)));
                });
        } catch(...) {
            promise.set_exception(std::current_exception());
        }
        return promise.get_future();
    }

private:
    void Init(const char *url) {
        uri_ = evhttp_uri_parse(url);
        if(!uri_) {
            throw std::invalid_argument("invalid url");
        }

        const auto *host = evhttp_uri_get_host(uri_);
        const int port = evhttp_uri_get_port(uri_);
        if(!host) {
            throw std::invalid_argument("can't get host");
        }

        conn_ = evhttp_connection_base_new(loop_->GetEventBase(), nullptr, host, port < 0 ? 80 : port);
        if(!conn_) {
            throw std::invalid_argument((std::string("can't resolve host: ") + host).c_str());
        }
    }

private:
    bool has_error_;
    FinishFunc finish_func_;
    ErrorFunc error_func_;
    std::unordered_map<std::string, std::string> headers_;
    internal::EvHttpUri uri_;
    internal::EvHttpConnection conn_;
    std::shared_ptr<EventLoop> loop_;
};

}
