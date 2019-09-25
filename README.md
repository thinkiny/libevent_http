# libevent_http

## Example:

``` c++
    //loop in another thread
    auto loop = http::EventLoop::New();
    loop->RunInBackground();
    auto resp = http::Get("http://www.baidu.com", loop).SetTimeout(1000).Execute();
    printf("%d:%lu\n", resp.GetResponseCode(), resp.GetBody().len);

    //callback
    http::Get("http://www.baidu.com")
        .AddHeader("Connection", "keep-alive")
        .SetTimeout(1000)
        .Execute([](http::Response&& r) {
                printf("%d:%lu\n", r.GetResponseCode(), r.GetBody().len);
        });

    //promise
    std::promise<http::Response> p;
    auto future = http::Get("http://www.baidu.com").SetTimeout(1000).Execute(p);
    auto resp = future.get();
    printf("%d:%lu\n", resp.GetResponseCode(), resp.GetBody().len);
```
