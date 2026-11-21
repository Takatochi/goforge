#include <iostream>
#include <string>
#include "goforge/goforge.hpp"

struct Greeter {
    std::string hello(std::string_view who) const {
        return "Hello, " + std::string(who) + " from goforge!";
    }
};

int main() {
    auto app = goforge::WebApp::create()
        .singleton<Greeter>()
        .use_defaults(false);

    app.get("/hi/:name", [](goforge::web::HttpContext& c, Greeter& g) {
        c.json(200, goforge::H{{"msg", g.hello(c.param_or("name"))}});
    });

    goforge::http::HttpRequest req;
    req.method = "GET";
    req.path = "/hi/Conan";
    auto resp = app.dispatch(req);
    std::cout << resp.status << " " << resp.body << "\n";
    return resp.status == 200 ? 0 : 1;
}
