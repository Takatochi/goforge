#pragma once

/**
 * @file router.hpp
 * @brief Gin-style segment radix/trie `Router` and `RouteGroup` supporting static paths,
 *        named parameters (`:id`), catch-all wildcards (`*filepath`), nested middleware chains,
 *        `405 Method Not Allowed` with `Allow` header, and compile-time DI-injected handlers.
 */

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"
#include "goforge/di.hpp"
#include "goforge/http/http_types.hpp"
#include "goforge/web/context.hpp"

namespace goforge::web {

namespace detail {

inline std::vector<std::string_view> split_path_segments(std::string_view path) {
    std::vector<std::string_view> segs;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        std::size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        segs.push_back(path.substr(start, i - start));
    }
    return segs;
}

inline std::string join_paths(std::string_view prefix, std::string_view suffix) {
    if (prefix.empty() || prefix == "/") {
        if (suffix.empty()) return "/";
        if (suffix.front() == '/') return std::string(suffix);
        return "/" + std::string(suffix);
    }
    std::string out(prefix);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    if (suffix.empty() || suffix == "/") return out;
    if (suffix.front() != '/') out.push_back('/');
    out.append(suffix);
    return out;
}

/// Adapts either `void(HttpContext&)` or DI-injected `void(HttpContext&, Deps...)` into a `HandlerFunc`.
template <typename Fn>
HandlerFunc adapt_handler(Fn&& fn) {
    using DecayedFn = std::decay_t<Fn>;
    if constexpr (std::is_invocable_r_v<void, DecayedFn&, HttpContext&>) {
        return HandlerFunc(std::forward<Fn>(fn));
    } else {
        using Traits = goforge::detail::FunctionTraits<DecayedFn>;
        using ArgsTuple = typename Traits::args_tuple;
        static_assert(
            std::tuple_size_v<ArgsTuple> >= 1 &&
            std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, ArgsTuple>>, HttpContext>,
            "Handler first parameter must be HttpContext& (e.g., [](HttpContext& c, MyService& svc) { ... })."
        );

        auto holder = std::make_shared<DecayedFn>(std::forward<Fn>(fn));
        return [holder](HttpContext& ctx) {
            auto call_with_deps = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                auto resolve_dep = [&]<typename Param>() -> decltype(auto) {
                    using Clean = std::remove_cvref_t<Param>;
                    if constexpr (std::is_same_v<Clean, ServiceScope>) {
                        return ctx.scope();
                    } else if constexpr (std::is_same_v<Clean, ServiceProvider>) {
                        return ctx.scope().root();
                    } else if constexpr (goforge::detail::IsSharedPtr<Clean>::value) {
                        using Elem = typename goforge::detail::IsSharedPtr<Clean>::element_type;
                        using RawT = std::remove_cv_t<Elem>;
                        return std::static_pointer_cast<Elem>(ctx.resolve_ptr<RawT>());
                    } else if constexpr (std::is_reference_v<Param>) {
                        return static_cast<Param>(ctx.resolve<Clean>());
                    } else {
                        return ctx.resolve<Clean>();
                    }
                };
                (*holder)(ctx, resolve_dep.template operator()<std::tuple_element_t<Is + 1, ArgsTuple>>()...);
            };
            call_with_deps(std::make_index_sequence<std::tuple_size_v<ArgsTuple> - 1>{});
        };
    }
}

struct StringViewHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>, StringViewHash, std::equal_to<>> static_children;

    std::string param_name;
    std::unique_ptr<TrieNode> param_child;

    std::string wildcard_name;
    std::unique_ptr<TrieNode> wildcard_child;

    bool has_handler{false};
    std::vector<HandlerFunc> chain;
};

struct RouteMatch {
    bool matched{false};
    const std::vector<HandlerFunc>* chain{nullptr};
    std::unordered_map<std::string, std::string> params;
};

inline bool match_trie(const TrieNode* node,
                       const std::vector<std::string_view>& segs,
                       std::size_t depth,
                       RouteMatch& out) {
    if (!node) return false;

    if (depth == segs.size()) {
        if (node->has_handler) {
            out.matched = true;
            out.chain = &node->chain;
            return true;
        }
        // Allow trailing empty wildcard match (e.g. "/static/*filepath" matching "/static/")
        if (node->wildcard_child && node->wildcard_child->has_handler) {
            out.params.insert_or_assign(node->wildcard_name, std::string{});
            out.matched = true;
            out.chain = &node->wildcard_child->chain;
            return true;
        }
        return false;
    }

    // 1. Static segment priority (zero-allocation transparent string_view lookup!)
    auto it = node->static_children.find(segs[depth]);
    if (it != node->static_children.end()) {
        if (match_trie(it->second.get(), segs, depth + 1, out)) {
            return true;
        }
    }

    // 2. Named parameter (:id) priority
    if (node->param_child) {
        std::string decoded = url_decode(segs[depth], false);
        auto prev_it = out.params.find(node->param_name);
        std::optional<std::string> prev_val;
        if (prev_it != out.params.end()) prev_val = prev_it->second;

        out.params.insert_or_assign(node->param_name, std::move(decoded));
        if (match_trie(node->param_child.get(), segs, depth + 1, out)) {
            return true;
        }
        if (prev_val) {
            out.params.insert_or_assign(node->param_name, std::move(*prev_val));
        } else {
            out.params.erase(node->param_name);
        }
    }

    // 3. Catch-all wildcard (*filepath) priority
    if (node->wildcard_child && node->wildcard_child->has_handler) {
        std::string rest;
        for (std::size_t i = depth; i < segs.size(); ++i) {
            if (i > depth) rest.push_back('/');
            rest.append(url_decode(segs[i], false));
        }
        out.params.insert_or_assign(node->wildcard_name, std::move(rest));
        out.matched = true;
        out.chain = &node->wildcard_child->chain;
        return true;
    }

    return false;
}

} // namespace detail

class Router;

/**
 * @class RouteGroup
 * @brief Scoped route prefix + inherited middleware list (mirrors `gin.RouterGroup`).
 */
class RouteGroup {
    friend class Router;

    Router* router_;
    std::string prefix_;
    std::vector<HandlerFunc> middlewares_;

    RouteGroup(Router& router, std::string prefix, std::vector<HandlerFunc> middlewares)
        : router_(&router)
        , prefix_(std::move(prefix))
        , middlewares_(std::move(middlewares)) {}

public:
    /// Appends one or more middleware functions to this group (inherited by child groups & routes).
    RouteGroup& use(HandlerFunc mw) {
        middlewares_.push_back(std::move(mw));
        return *this;
    }

    /// Creates a nested child `RouteGroup` inheriting this group's prefix and middleware chain.
    [[nodiscard]] RouteGroup group(std::string_view sub_prefix) const {
        return RouteGroup(*router_, detail::join_paths(prefix_, sub_prefix), middlewares_);
    }

    /// Registers a route for an arbitrary HTTP method (supports both `HttpContext&` and DI-injected handlers).
    template <typename Fn>
    RouteGroup& handle(std::string_view method, std::string_view path, Fn&& fn);

    template <typename Fn>
    RouteGroup& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& head(std::string_view path, Fn&& fn) { return handle("HEAD", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& options(std::string_view path, Fn&& fn) { return handle("OPTIONS", path, std::forward<Fn>(fn)); }
};

/**
 * @class Router
 * @brief Gin-style HTTP router with segment trie matching, route groups, global/group middleware,
 *        automatic `404`/`405` handling, and per-request `ServiceScope` DI execution.
 */
class Router {
    friend class RouteGroup;

    std::unordered_map<std::string, std::unique_ptr<detail::TrieNode>> trees_;
    std::vector<HandlerFunc> global_middlewares_;
    HandlerFunc no_route_handler_;

    void add_route(std::string_view method,
                   std::string_view full_path,
                   const std::vector<HandlerFunc>& group_mws,
                   HandlerFunc endpoint) {
        auto& root = trees_[std::string(method)];
        if (!root) {
            root = std::make_unique<detail::TrieNode>();
        }

        auto segs = detail::split_path_segments(full_path);
        detail::TrieNode* cur = root.get();

        for (std::size_t i = 0; i < segs.size(); ++i) {
            std::string_view seg = segs[i];
            if (!seg.empty() && seg.front() == '*') {
                if (i + 1 != segs.size()) {
                    throw std::invalid_argument(
                        "Catch-all wildcard '" + std::string(seg) +
                        "' must be the final segment in route: " + std::string(full_path)
                    );
                }
                std::string name(seg.substr(1));
                if (name.empty()) {
                    throw std::invalid_argument("Wildcard segment requires a name in route: " + std::string(full_path));
                }
                if (!cur->wildcard_child) {
                    cur->wildcard_name = std::move(name);
                    cur->wildcard_child = std::make_unique<detail::TrieNode>();
                } else if (cur->wildcard_name != name) {
                    throw std::invalid_argument("Conflicting wildcard parameter name in route: " + std::string(full_path));
                }
                cur = cur->wildcard_child.get();
            } else if (!seg.empty() && seg.front() == ':') {
                std::string name(seg.substr(1));
                if (name.empty()) {
                    throw std::invalid_argument("Parameter segment requires a name in route: " + std::string(full_path));
                }
                if (!cur->param_child) {
                    cur->param_name = std::move(name);
                    cur->param_child = std::make_unique<detail::TrieNode>();
                } else if (cur->param_name != name) {
                    throw std::invalid_argument(
                        "Conflicting param name ':" + name + "' vs ':" + cur->param_name +
                        "' in route: " + std::string(full_path)
                    );
                }
                cur = cur->param_child.get();
            } else {
                auto& next = cur->static_children[std::string(seg)];
                if (!next) {
                    next = std::make_unique<detail::TrieNode>();
                }
                cur = next.get();
            }
        }

        std::vector<HandlerFunc> chain;
        chain.reserve(group_mws.size() + 1);
        for (const auto& mw : group_mws) {
            chain.push_back(mw);
        }
        chain.push_back(std::move(endpoint));

        cur->has_handler = true;
        cur->chain = std::move(chain);
    }

public:
    Router() = default;

    /// Appends a global middleware executed for every route (including 404/405).
    Router& use(HandlerFunc mw) {
        global_middlewares_.push_back(std::move(mw));
        return *this;
    }

    /// Sets a custom 404 Not Found handler.
    Router& no_route(HandlerFunc handler) {
        no_route_handler_ = std::move(handler);
        return *this;
    }

    /// Creates a top-level `RouteGroup` (e.g. `router.group("/api/v1")`).
    [[nodiscard]] RouteGroup group(std::string_view prefix) {
        return RouteGroup(*this, detail::join_paths("", prefix), {});
    }

    template <typename Fn>
    Router& handle(std::string_view method, std::string_view path, Fn&& fn) {
        add_route(method, path, {}, detail::adapt_handler(std::forward<Fn>(fn)));
        return *this;
    }

    template <typename Fn>
    Router& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& head(std::string_view path, Fn&& fn) { return handle("HEAD", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& options(std::string_view path, Fn&& fn) { return handle("OPTIONS", path, std::forward<Fn>(fn)); }

    /**
     * @brief Dispatches a single `HttpRequest` through global middleware, route middleware, and handler,
     *        creating an automatic per-request DI `ServiceScope` if `provider` is provided.
     */
    [[nodiscard]] http::HttpResponse dispatch(const http::HttpRequest& req,
                                              gorun::Context go_ctx = gorun::background(),
                                              ServiceProvider* provider = nullptr) const {
        std::optional<ServiceScope> scope_storage;
        ServiceScope* scope_ptr = nullptr;
        if (provider) {
            scope_storage.emplace(*provider);
            scope_ptr = &(*scope_storage);
        }

        HttpContext ctx(req, std::move(go_ctx), scope_ptr);
        auto segs = detail::split_path_segments(req.path);

        detail::RouteMatch match;
        if (auto it = trees_.find(req.method); it != trees_.end()) {
            detail::match_trie(it->second.get(), segs, 0, match);
        }

        if (match.matched && match.chain) {
            ctx.set_params(std::move(match.params));
            ctx.set_handler_views(&global_middlewares_, match.chain);
        } else {
            // Check whether another HTTP method matches this path -> 405 Method Not Allowed
            std::vector<std::string> allowed_methods;
            for (const auto& [m, root] : trees_) {
                if (m == req.method) continue;
                detail::RouteMatch probe;
                if (detail::match_trie(root.get(), segs, 0, probe)) {
                    allowed_methods.push_back(m);
                }
            }

            if (!allowed_methods.empty()) {
                std::sort(allowed_methods.begin(), allowed_methods.end());
                std::string allow_hdr;
                for (std::size_t i = 0; i < allowed_methods.size(); ++i) {
                    if (i > 0) allow_hdr += ", ";
                    allow_hdr += allowed_methods[i];
                }
                ctx.set_handler_with_fallback(&global_middlewares_, [allow = std::move(allow_hdr)](HttpContext& c) {
                    c.set_header("Allow", allow);
                    c.error(405, "method_not_allowed");
                });
            } else if (no_route_handler_) {
                ctx.set_handler_with_fallback(&global_middlewares_, no_route_handler_);
            } else {
                ctx.set_handler_with_fallback(&global_middlewares_, [](HttpContext& c) {
                    c.error(404, "not_found");
                });
            }
        }

        ctx.next();

        return std::move(ctx.response());
    }

    /// Adapts this router into a `goforge::http::HttpHandler` suitable for `HttpServer`.
    [[nodiscard]] std::function<http::HttpResponse(const http::HttpRequest&, gorun::Context)>
    to_http_handler(ServiceProvider* provider = nullptr) const {
        return [this, provider](const http::HttpRequest& req, gorun::Context ctx) {
            return dispatch(req, std::move(ctx), provider);
        };
    }
};

template <typename Fn>
inline RouteGroup& RouteGroup::handle(std::string_view method, std::string_view path, Fn&& fn) {
    router_->add_route(
        method,
        detail::join_paths(prefix_, path),
        middlewares_,
        detail::adapt_handler(std::forward<Fn>(fn))
    );
    return *this;
}

} // namespace goforge::web
