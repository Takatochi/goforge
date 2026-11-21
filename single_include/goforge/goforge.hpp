#pragma once
// =====================================================================================
//  goforge — single-header core distribution (GENERATED FILE, DO NOT EDIT)
//  Fx-Style DI, Lifecycle, BlockingPool, Gin-Style Router, HttpContext, Middleware &
//  Production Observability for C++20/23 on top of gorun.
//
//  Source:  https://github.com/Takatochi/goforge
//  Author:  Artur Havor
//  License: Apache License 2.0
//
//  Regenerate with: python scripts/amalgamate.py
// =====================================================================================
// ---- begin goforge/goforge.hpp ----

/**
 * @file goforge.hpp
 * @brief Umbrella header for the goforge C++20/23 web & application framework built on gorun.
 */

// ---- begin goforge/json.hpp ----

/**
 * @file json.hpp
 * @brief Ergonomic JSON serialization, DTO binding, `std::optional<T>` support, and
 *        validation primitives powered by `nlohmann/json`.
 *
 * Provides:
 * - `goforge::Json` (alias for `nlohmann::json`) and `goforge::H` (Gin-style object map builder)
 * - Automatic `std::optional<T>` serialization/deserialization (`null` <-> `std::nullopt`)
 * - `GOFORGE_JSON_DTO(Type, fields...)` for strict DTOs (missing field -> 400 Bad Request)
 * - `GOFORGE_JSON_DTO_DEFAULT(Type, fields...)` for partial/PATCH DTOs using struct defaults
 * - Automatic `.validate()` hook detection (`ValidatableDto` concept -> 422 Unprocessable Entity)
 */

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

// Native `std::optional<T>` support across all goforge DTOs!
namespace nlohmann {

template <typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json& j, const std::optional<T>& opt) {
        if (opt.has_value()) {
            j = *opt;
        } else {
            j = nullptr;
        }
    }

    static void from_json(const json& j, std::optional<T>& opt) {
        if (j.is_null()) {
            opt = std::nullopt;
        } else {
            opt = j.get<T>();
        }
    }
};

} // namespace nlohmann

/**
 * @brief Declares strict JSON serialization & deserialization for a DTO struct.
 *        Missing fields or type mismatches produce a clear `400 Bad Request` bind error.
 */
#define GOFORGE_JSON_DTO(Type, ...) NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, __VA_ARGS__)

/**
 * @brief Declares JSON serialization & deserialization using C++ member default values
 *        for omitted JSON fields (ideal for PATCH requests, optional fields, and configs).
 */
#define GOFORGE_JSON_DTO_DEFAULT(Type, ...) NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Type, __VA_ARGS__)

namespace goforge {

/// Primary JSON value type (`nlohmann::json`).
using Json = nlohmann::json;

/// Gin-style shorthand for building inline JSON objects: `goforge::H{{"status", "ok"}, {"count", 42}}`.
using H = nlohmann::json;

/**
 * @brief Concept satisfied by DTO structs defining `std::optional<std::string> validate() const`.
 *        When satisfied, `HttpContext::bind_json<T>()` automatically runs `validate()` and responds
 *        with `422 Unprocessable Entity` if validation fails.
 */
template <typename T>
concept ValidatableDto = requires(const T& dto) {
    { dto.validate() } -> std::convertible_to<std::optional<std::string>>;
};

/**
 * @struct BindResult
 * @brief Result of parsing and validating a JSON request body into a typed DTO `T`.
 */
template <typename T>
struct BindResult {
    std::optional<T> value;
    int http_status{200};
    std::string error_code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
};

/// Builds a standardized JSON error payload: `{"error": code, "message": msg}`.
inline Json make_error_json(std::string_view error_code, std::string_view message = "") {
    Json obj = Json::object();
    obj["error"] = std::string(error_code);
    if (!message.empty()) {
        obj["message"] = std::string(message);
    }
    return obj;
}

/// Parses a raw JSON payload into a generic `goforge::Json` document with clean error reporting.
inline BindResult<Json> parse_json_body(std::string_view body) {
    BindResult<Json> res;
    if (body.empty()) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = "request body is empty";
        return res;
    }
    try {
        res.value = Json::parse(body.begin(), body.end());
        res.http_status = 200;
    } catch (const Json::parse_error& ex) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = ex.what();
    } catch (const std::exception& ex) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = ex.what();
    }
    return res;
}

/**
 * @brief Parses JSON body into typed DTO `T`, checking schema/types (`400 Bad Request`) and
 *        invoking `dto.validate()` (`422 Unprocessable Entity`) if `T` satisfies `ValidatableDto`.
 */
template <typename T>
BindResult<T> parse_and_validate_json(std::string_view body) {
    BindResult<T> res;
    auto parsed = parse_json_body(body);
    if (!parsed.ok()) {
        res.http_status = parsed.http_status;
        res.error_code = std::move(parsed.error_code);
        res.message = std::move(parsed.message);
        return res;
    }

    try {
        T dto = parsed.value->template get<T>();
        if constexpr (ValidatableDto<T>) {
            if (auto validation_err = dto.validate(); validation_err.has_value()) {
                res.http_status = 422;
                res.error_code = "validation_failed";
                res.message = std::move(*validation_err);
                return res;
            }
        }
        res.value = std::move(dto);
        res.http_status = 200;
    } catch (const Json::type_error& ex) {
        res.http_status = 400;
        res.error_code = "invalid_payload";
        res.message = ex.what();
    } catch (const Json::out_of_range& ex) {
        res.http_status = 400;
        res.error_code = "missing_field";
        res.message = ex.what();
    } catch (const std::exception& ex) {
        res.http_status = 400;
        res.error_code = "invalid_payload";
        res.message = ex.what();
    }
    return res;
}

} // namespace goforge
// ---- end goforge/json.hpp ----
// ---- begin goforge/di.hpp ----

/**
 * @file di.hpp
 * @brief Compile-time typed Dependency Injection container for C++20/23 (`ServiceCollection`,
 *        `ServiceProvider`, `ServiceScope`) with Singleton, Transient, and Scoped lifetimes,
 *        automatic argument resolution, cycle detection, and captive-dependency validation.
 */

#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

class ServiceProvider;
class ServiceScope;

/**
 * @enum Lifetime
 * @brief Controls the lifetime and sharing semantics of a registered service.
 */
enum class Lifetime : std::uint8_t {
    Singleton, ///< One shared instance per `ServiceProvider` (application lifetime)
    Transient, ///< A fresh instance created on every resolution
    Scoped     ///< One shared instance per `ServiceScope` (e.g., per HTTP request)
};

/// Base exception type for all Dependency Injection container errors.
class DiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when a requested or declared service dependency is not registered.
class ServiceNotFoundError : public DiError {
public:
    using DiError::DiError;
};

/// Thrown during `ServiceCollection::build()` when a circular dependency graph is detected.
class CircularDependencyError : public DiError {
public:
    using DiError::DiError;
};

/// Thrown during `ServiceCollection::build()` when a `Singleton` depends on a `Scoped` service.
class CaptiveDependencyError : public DiError {
public:
    using DiError::DiError;
};

/// Thrown when resolving a `Scoped` service directly from the root `ServiceProvider` without a `ServiceScope`.
class ScopeRequiredError : public DiError {
public:
    using DiError::DiError;
};

namespace detail {

/**
 * @brief Extracts a readable compile-time type name without requiring ABI demangling.
 */
template <typename T>
inline std::string type_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string_view p = __PRETTY_FUNCTION__;
    auto start = p.find("T = ");
    if (start != std::string_view::npos) {
        start += 4;
        auto end = p.find_first_of(";]", start);
        if (end != std::string_view::npos) {
            return std::string(p.substr(start, end - start));
        }
    }
    return std::string(p);
#elif defined(_MSC_VER)
    std::string_view p = __FUNCSIG__;
    auto start = p.find("type_name<");
    if (start != std::string_view::npos) {
        start += 10;
        auto end = p.rfind(">(void)");
        if (end != std::string_view::npos && end > start) {
            return std::string(p.substr(start, end - start));
        }
    }
    return std::string(p);
#else
    return typeid(T).name();
#endif
}

template <typename T>
struct IsSharedPtr : std::false_type {};

template <typename T>
struct IsSharedPtr<std::shared_ptr<T>> : std::true_type {
    using element_type = T;
};

template <typename T>
struct IsUniquePtr : std::false_type {};

template <typename T>
struct IsUniquePtr<std::unique_ptr<T>> : std::true_type {
    using element_type = T;
};

/// Extracts function signature traits for free functions, function pointers, and lambdas (const or mutable).
template <typename T>
struct FunctionTraits : FunctionTraits<decltype(&std::decay_t<T>::operator())> {};

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct FunctionTraits<R(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const noexcept> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) noexcept> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

/// Deduces the registered service type from a factory return type (`T`, `std::shared_ptr<T>`, or `std::unique_ptr<T>`).
template <typename R>
struct UnwrapReturnType {
    using type = std::remove_cvref_t<R>;
};

template <typename T>
struct UnwrapReturnType<std::shared_ptr<T>> {
    using type = std::remove_cv_t<T>;
};

template <typename T>
struct UnwrapReturnType<std::unique_ptr<T>> {
    using type = std::remove_cv_t<T>;
};

/// Normalizes a factory return value into `std::shared_ptr<ServiceType>`.
template <typename ServiceType, typename R>
std::shared_ptr<ServiceType> normalize_service_ptr(R&& value) {
    using DecayedR = std::decay_t<R>;
    if constexpr (IsSharedPtr<DecayedR>::value) {
        if (!value) {
            throw DiError("Factory returned nullptr for service: " + type_name<ServiceType>());
        }
        return std::static_pointer_cast<ServiceType>(std::forward<R>(value));
    } else if constexpr (IsUniquePtr<DecayedR>::value) {
        if (!value) {
            throw DiError("Factory returned nullptr for service: " + type_name<ServiceType>());
        }
        std::shared_ptr<typename DecayedR::element_type> sp(std::move(value));
        return std::static_pointer_cast<ServiceType>(std::move(sp));
    } else {
        return std::static_pointer_cast<ServiceType>(
            std::make_shared<DecayedR>(std::forward<R>(value))
        );
    }
}

/// Identifies built-in container self-parameters (`ServiceProvider&`, `ServiceScope&`) excluded from graph deps.
template <typename Param>
constexpr bool is_container_param_v =
    std::is_same_v<std::remove_cvref_t<Param>, ServiceProvider> ||
    std::is_same_v<std::remove_cvref_t<Param>, ServiceScope>;

/// Extracts the underlying service type required for a dependency parameter (`T&`, `const T&`, `std::shared_ptr<T>`).
template <typename CleanParam>
struct UnwrapSharedOrDirect {
    using type = CleanParam;
};

template <typename T>
struct UnwrapSharedOrDirect<std::shared_ptr<T>> {
    using type = std::remove_cv_t<T>;
};

template <typename Param>
struct DependencyServiceType {
    using type = typename UnwrapSharedOrDirect<std::remove_cvref_t<Param>>::type;
};

struct DependencyInfo {
    std::type_index key;
    std::string name;
};

template <typename Param>
void append_dependency_if_service(std::vector<DependencyInfo>& out) {
    if constexpr (!is_container_param_v<Param>) {
        using ServiceT = typename DependencyServiceType<Param>::type;
        out.push_back(DependencyInfo{std::type_index(typeid(ServiceT)), type_name<ServiceT>()});
    }
}

template <typename Tuple, std::size_t... Is>
std::vector<DependencyInfo> collect_dependencies_impl(std::index_sequence<Is...>) {
    std::vector<DependencyInfo> deps;
    deps.reserve(sizeof...(Is));
    (append_dependency_if_service<std::tuple_element_t<Is, Tuple>>(deps), ...);
    return deps;
}

template <typename Tuple>
std::vector<DependencyInfo> collect_dependencies() {
    return collect_dependencies_impl<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

/// Per-service lazy initialization slot using `gorun::Mutex` (never blocks OS worker threads!).
struct InstanceSlot {
    gorun::Mutex init_mtx;
    std::atomic<bool> ready{false};
    std::shared_ptr<void> instance{};
};

struct ServiceDescriptor {
    std::type_index key{typeid(void)};
    std::string name;
    Lifetime lifetime{Lifetime::Singleton};
    std::vector<DependencyInfo> dependencies;
    std::function<std::shared_ptr<void>(ServiceProvider&, ServiceScope*)> factory;
    std::shared_ptr<InstanceSlot> singleton_slot; ///< Pre-allocated for Singletons so resolution is lock-free on lookup
};

struct ProviderState {
    std::unordered_map<std::type_index, ServiceDescriptor> descriptors;
};

} // namespace detail

/**
 * @class ServiceScope
 * @brief Represents an isolated DI scope (e.g., a single HTTP request or background job).
 *
 * - `Scoped` services are constructed once per `ServiceScope` and destroyed when the scope is destroyed.
 * - `Singleton` services are delegated to the root `ServiceProvider`.
 * - `Transient` services are created fresh on every resolution.
 */
class ServiceScope {
    friend class ServiceProvider;

    ServiceProvider* root_;
    gorun::Mutex slots_mtx_;
    std::unordered_map<std::type_index, std::shared_ptr<detail::InstanceSlot>> scoped_slots_;

    std::shared_ptr<detail::InstanceSlot> get_or_create_slot(std::type_index key) {
        std::lock_guard<gorun::Mutex> lk(slots_mtx_);
        auto& slot = scoped_slots_[key];
        if (!slot) {
            slot = std::make_shared<detail::InstanceSlot>();
        }
        return slot;
    }

public:
    explicit ServiceScope(ServiceProvider& root) noexcept : root_(&root) {}

    ServiceScope(const ServiceScope&) = delete;
    ServiceScope& operator=(const ServiceScope&) = delete;
    ServiceScope(ServiceScope&&) = delete;
    ServiceScope& operator=(ServiceScope&&) = delete;

    /// Returns the root `ServiceProvider` owning this scope.
    ServiceProvider& root() noexcept { return *root_; }

    /// Resolves service `T` as a `std::shared_ptr<T>` within this scope.
    template <typename T>
    std::shared_ptr<std::remove_cv_t<T>> resolve_ptr();

    /// Resolves service `T` as a reference `T&` within this scope.
    template <typename T>
    T& resolve();

    /// Invokes a callable `fn(Deps...)`, automatically resolving all arguments within this scope.
    template <typename Fn>
    decltype(auto) invoke(Fn&& fn);
};

/**
 * @class ServiceProvider
 * @brief Immutable, concurrency-safe Dependency Injection resolver built from `ServiceCollection::build()`.
 *
 * Uses `gorun::Mutex` for lazy Singleton initialization so green routines contending on first-time
 * service construction park cleanly without blocking OS worker threads. After initialization,
 * Singleton resolution is lock-free (single atomic acquire load).
 */
class ServiceProvider {
    friend class ServiceCollection;
    friend class ServiceScope;

    std::shared_ptr<const detail::ProviderState> state_;

    explicit ServiceProvider(std::shared_ptr<const detail::ProviderState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<void> resolve_erased(std::type_index key, std::string_view requested_name, ServiceScope* scope) {
        auto it = state_->descriptors.find(key);
        if (it == state_->descriptors.end()) {
            throw ServiceNotFoundError("Service not registered: " + std::string(requested_name));
        }

        const detail::ServiceDescriptor& desc = it->second;

        if (desc.lifetime == Lifetime::Transient) {
            return desc.factory(*this, scope);
        }

        if (desc.lifetime == Lifetime::Singleton) {
            auto& slot = *desc.singleton_slot;
            if (slot.ready.load(std::memory_order_acquire)) {
                return slot.instance;
            }
            std::lock_guard<gorun::Mutex> lk(slot.init_mtx);
            if (!slot.ready.load(std::memory_order_relaxed)) {
                slot.instance = desc.factory(*this, nullptr);
                slot.ready.store(true, std::memory_order_release);
            }
            return slot.instance;
        }

        // Lifetime::Scoped
        if (!scope) {
            throw ScopeRequiredError(
                "Cannot resolve Scoped service '" + desc.name +
                "' from root ServiceProvider without an active ServiceScope (call create_scope())."
            );
        }

        auto slot = scope->get_or_create_slot(key);
        if (slot->ready.load(std::memory_order_acquire)) {
            return slot->instance;
        }
        std::lock_guard<gorun::Mutex> lk(slot->init_mtx);
        if (!slot->ready.load(std::memory_order_relaxed)) {
            slot->instance = desc.factory(*this, scope);
            slot->ready.store(true, std::memory_order_release);
        }
        return slot->instance;
    }

    template <typename Param>
    decltype(auto) resolve_param(ServiceScope* scope) {
        using Clean = std::remove_cvref_t<Param>;
        if constexpr (std::is_same_v<Clean, ServiceProvider>) {
            return static_cast<ServiceProvider&>(*this);
        } else if constexpr (std::is_same_v<Clean, ServiceScope>) {
            if (!scope) {
                throw ScopeRequiredError("Cannot inject ServiceScope& outside of an active ServiceScope.");
            }
            return static_cast<ServiceScope&>(*scope);
        } else if constexpr (detail::IsSharedPtr<Clean>::value) {
            using Elem = typename detail::IsSharedPtr<Clean>::element_type;
            using RawT = std::remove_cv_t<Elem>;
            auto ptr = std::static_pointer_cast<RawT>(
                resolve_erased(std::type_index(typeid(RawT)), detail::type_name<RawT>(), scope)
            );
            return std::static_pointer_cast<Elem>(std::move(ptr));
        } else {
            static_assert(
                std::is_reference_v<Param> || std::is_copy_constructible_v<Clean>,
                "Injected non-reference parameter must be copy-constructible, or take T& / const T& / std::shared_ptr<T>."
            );
            auto ptr = std::static_pointer_cast<Clean>(
                resolve_erased(std::type_index(typeid(Clean)), detail::type_name<Clean>(), scope)
            );
            if constexpr (std::is_reference_v<Param>) {
                return static_cast<Param>(*ptr);
            } else {
                return *ptr;
            }
        }
    }

    template <typename Fn, typename ArgsTuple, std::size_t... Is>
    decltype(auto) invoke_impl(Fn&& fn, ServiceScope* scope, std::index_sequence<Is...>) {
        (void)scope;
        return std::invoke(
            std::forward<Fn>(fn),
            resolve_param<std::tuple_element_t<Is, ArgsTuple>>(scope)...
        );
    }

public:
    ServiceProvider() = default;

    /// Returns true if a service of type `T` is registered in this container.
    template <typename T>
    [[nodiscard]] bool contains() const noexcept {
        using CleanT = std::remove_cvref_t<T>;
        return state_ && state_->descriptors.contains(std::type_index(typeid(CleanT)));
    }

    /// Resolves service `T` as a `std::shared_ptr<T>`.
    template <typename T>
    std::shared_ptr<std::remove_cv_t<T>> resolve_ptr() {
        using RawT = std::remove_cv_t<T>;
        return std::static_pointer_cast<RawT>(
            resolve_erased(std::type_index(typeid(RawT)), detail::type_name<RawT>(), nullptr)
        );
    }

    /// Resolves service `T` as a reference `T&`.
    template <typename T>
    T& resolve() {
        return *resolve_ptr<std::remove_cvref_t<T>>();
    }

    /// Creates a new `ServiceScope` for per-request / per-job Scoped services.
    [[nodiscard]] ServiceScope create_scope() noexcept {
        return ServiceScope(*this);
    }

    /// Invokes `fn(Deps...)` by automatically resolving all its parameters from the root provider.
    template <typename Fn>
    decltype(auto) invoke(Fn&& fn) {
        using Traits = detail::FunctionTraits<std::decay_t<Fn>>;
        using ArgsTuple = typename Traits::args_tuple;
        return invoke_impl<Fn, ArgsTuple>(
            std::forward<Fn>(fn),
            nullptr,
            std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
        );
    }

    /// Returns total number of registered service types.
    [[nodiscard]] std::size_t service_count() const noexcept {
        return state_ ? state_->descriptors.size() : 0;
    }
};

template <typename T>
inline std::shared_ptr<std::remove_cv_t<T>> ServiceScope::resolve_ptr() {
    using RawT = std::remove_cv_t<T>;
    return std::static_pointer_cast<RawT>(
        root_->resolve_erased(std::type_index(typeid(RawT)), detail::type_name<RawT>(), this)
    );
}

template <typename T>
inline T& ServiceScope::resolve() {
    return *resolve_ptr<std::remove_cvref_t<T>>();
}

template <typename Fn>
inline decltype(auto) ServiceScope::invoke(Fn&& fn) {
    using Traits = detail::FunctionTraits<std::decay_t<Fn>>;
    using ArgsTuple = typename Traits::args_tuple;
    return root_->template invoke_impl<Fn, ArgsTuple>(
        std::forward<Fn>(fn),
        this,
        std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
    );
}

/**
 * @class ServiceCollection
 * @brief Builder for registering services, factories, instances, and interface bindings before `build()`.
 *
 * `build()` validates the complete dependency graph up-front:
 * - Missing dependencies (`ServiceNotFoundError`)
 * - Circular dependencies (`CircularDependencyError`, reporting the full cycle chain)
 * - Captive dependencies where a `Singleton` depends on a `Scoped` service (`CaptiveDependencyError`)
 */
class ServiceCollection {
    std::unordered_map<std::type_index, detail::ServiceDescriptor> descriptors_;

    template <typename ServiceType, typename Factory>
    ServiceCollection& register_factory(Lifetime lifetime, Factory&& factory) {
        using CleanService = std::remove_cv_t<ServiceType>;
        using DecayedFactory = std::decay_t<Factory>;
        using Traits = detail::FunctionTraits<DecayedFactory>;
        using ArgsTuple = typename Traits::args_tuple;

        detail::ServiceDescriptor desc;
        desc.key = std::type_index(typeid(CleanService));
        desc.name = detail::type_name<CleanService>();
        desc.lifetime = lifetime;
        desc.dependencies = detail::collect_dependencies<ArgsTuple>();
        if (lifetime == Lifetime::Singleton) {
            desc.singleton_slot = std::make_shared<detail::InstanceSlot>();
        }

        auto fn_holder = std::make_shared<DecayedFactory>(std::forward<Factory>(factory));
        desc.factory = [fn_holder](ServiceProvider& provider, ServiceScope* scope) -> std::shared_ptr<void> {
            auto result = provider.template invoke_impl<DecayedFactory&, ArgsTuple>(
                *fn_holder,
                scope,
                std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
            );
            std::shared_ptr<CleanService> typed =
                detail::normalize_service_ptr<CleanService>(std::move(result));
            return std::static_pointer_cast<void>(std::move(typed));
        };

        descriptors_.insert_or_assign(desc.key, std::move(desc));
        return *this;
    }

    template <typename ServiceType, typename ImplType, typename... Deps>
    ServiceCollection& register_ctor(Lifetime lifetime) {
        static_assert(
            std::is_same_v<ServiceType, ImplType> || std::is_base_of_v<ServiceType, ImplType>,
            "ImplType must derive from ServiceType."
        );
        return register_factory<ServiceType>(
            lifetime,
            [](Deps... deps) -> std::shared_ptr<ServiceType> {
                return std::static_pointer_cast<ServiceType>(
                    std::make_shared<ImplType>(std::forward<Deps>(deps)...)
                );
            }
        );
    }

    static void validate_graph(const std::unordered_map<std::type_index, detail::ServiceDescriptor>& descs) {
        // 1. Verify all declared dependencies exist
        for (const auto& [key, desc] : descs) {
            for (const auto& dep : desc.dependencies) {
                if (!descs.contains(dep.key)) {
                    throw ServiceNotFoundError(
                        "Missing dependency: service '" + desc.name +
                        "' requires unregistered service '" + dep.name + "'."
                    );
                }
            }
        }

        // 2. 3-color DFS cycle detection (0 = unvisited, 1 = visiting, 2 = visited)
        std::unordered_map<std::type_index, int> color;
        std::vector<std::type_index> path;

        auto dfs_cycle = [&](auto&& self, std::type_index node) -> void {
            color[node] = 1;
            path.push_back(node);

            const auto& desc = descs.at(node);
            for (const auto& dep : desc.dependencies) {
                int c = color[dep.key];
                if (c == 1) {
                    std::ostringstream oss;
                    oss << "Circular dependency detected: ";
                    bool in_cycle = false;
                    for (const auto& p : path) {
                        if (p == dep.key) in_cycle = true;
                        if (in_cycle) oss << descs.at(p).name << " -> ";
                    }
                    oss << descs.at(dep.key).name;
                    throw CircularDependencyError(oss.str());
                }
                if (c == 0) {
                    self(self, dep.key);
                }
            }

            path.pop_back();
            color[node] = 2;
        };

        for (const auto& [key, _] : descs) {
            if (color[key] == 0) {
                dfs_cycle(dfs_cycle, key);
            }
        }

        // 3. Captive dependency validation: no Singleton may depend (directly or transitively) on a Scoped service
        for (const auto& [root_key, root_desc] : descs) {
            if (root_desc.lifetime != Lifetime::Singleton) continue;

            std::unordered_set<std::type_index> visited;
            std::vector<std::type_index> stack;
            stack.push_back(root_key);

            while (!stack.empty()) {
                std::type_index cur = stack.back();
                stack.pop_back();
                if (!visited.insert(cur).second) continue;

                for (const auto& dep : descs.at(cur).dependencies) {
                    const auto& dep_desc = descs.at(dep.key);
                    if (dep_desc.lifetime == Lifetime::Scoped) {
                        throw CaptiveDependencyError(
                            "Captive dependency detected: Singleton '" + root_desc.name +
                            "' cannot depend on Scoped service '" + dep_desc.name + "'."
                        );
                    }
                    stack.push_back(dep.key);
                }
            }
        }
    }

public:
    ServiceCollection() = default;

    /// Registers a pre-built instance `std::shared_ptr<T>` as a `Singleton`.
    template <typename T>
    ServiceCollection& add_instance(std::shared_ptr<T> instance) {
        using CleanT = std::remove_cv_t<T>;
        if (!instance) {
            throw DiError("Cannot register nullptr instance for service: " + detail::type_name<CleanT>());
        }
        detail::ServiceDescriptor desc;
        desc.key = std::type_index(typeid(CleanT));
        desc.name = detail::type_name<CleanT>();
        desc.lifetime = Lifetime::Singleton;
        desc.singleton_slot = std::make_shared<detail::InstanceSlot>();
        desc.singleton_slot->instance = std::static_pointer_cast<void>(std::move(instance));
        desc.singleton_slot->ready.store(true, std::memory_order_release);
        desc.factory = [slot = desc.singleton_slot](ServiceProvider&, ServiceScope*) {
            return slot->instance;
        };
        descriptors_.insert_or_assign(desc.key, std::move(desc));
        return *this;
    }

    /// Registers a value by copying/moving it into a `std::shared_ptr<T>` `Singleton`.
    template <typename T>
        requires (!detail::IsSharedPtr<std::decay_t<T>>::value)
    ServiceCollection& add_instance(T&& value) {
        using CleanT = std::decay_t<T>;
        return add_instance<CleanT>(std::make_shared<CleanT>(std::forward<T>(value)));
    }

    /// Registers a factory `R(Deps...)` with automatic service type deduction and dependency injection.
    template <typename Factory>
    ServiceCollection& provide(Lifetime lifetime, Factory&& factory) {
        using Traits = detail::FunctionTraits<std::decay_t<Factory>>;
        using ServiceT = typename detail::UnwrapReturnType<typename Traits::return_type>::type;
        return register_factory<ServiceT>(lifetime, std::forward<Factory>(factory));
    }

    /// Registers a `Singleton` factory with automatic service type deduction.
    template <typename Factory>
    ServiceCollection& add_singleton_factory(Factory&& factory) {
        return provide(Lifetime::Singleton, std::forward<Factory>(factory));
    }

    /// Registers a `Transient` factory with automatic service type deduction.
    template <typename Factory>
    ServiceCollection& add_transient_factory(Factory&& factory) {
        return provide(Lifetime::Transient, std::forward<Factory>(factory));
    }

    /// Registers a `Scoped` factory with automatic service type deduction.
    template <typename Factory>
    ServiceCollection& add_scoped_factory(Factory&& factory) {
        return provide(Lifetime::Scoped, std::forward<Factory>(factory));
    }

    /// Registers a factory explicitly bound to `ServiceType` (e.g., an interface).
    template <typename ServiceType, typename Factory>
    ServiceCollection& add_factory(Lifetime lifetime, Factory&& factory) {
        return register_factory<ServiceType>(lifetime, std::forward<Factory>(factory));
    }

    /// Registers a `Singleton` constructed directly with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    ServiceCollection& add_singleton() {
        return register_ctor<Impl, Impl, Deps...>(Lifetime::Singleton);
    }

    /// Registers a `Singleton` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
        requires (!std::is_same_v<Interface, Impl> && std::is_base_of_v<Interface, Impl>)
    ServiceCollection& add_singleton_as() {
        return register_ctor<Interface, Impl, Deps...>(Lifetime::Singleton);
    }

    /// Registers a `Transient` constructed directly with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    ServiceCollection& add_transient() {
        return register_ctor<Impl, Impl, Deps...>(Lifetime::Transient);
    }

    /// Registers a `Transient` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
        requires (!std::is_same_v<Interface, Impl> && std::is_base_of_v<Interface, Impl>)
    ServiceCollection& add_transient_as() {
        return register_ctor<Interface, Impl, Deps...>(Lifetime::Transient);
    }

    /// Registers a `Scoped` service constructed directly with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    ServiceCollection& add_scoped() {
        return register_ctor<Impl, Impl, Deps...>(Lifetime::Scoped);
    }

    /// Registers a `Scoped` service implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
        requires (!std::is_same_v<Interface, Impl> && std::is_base_of_v<Interface, Impl>)
    ServiceCollection& add_scoped_as() {
        return register_ctor<Interface, Impl, Deps...>(Lifetime::Scoped);
    }

    /**
     * @brief Validates the dependency graph (missing deps, cycles, captive scopes) and builds an immutable `ServiceProvider`.
     */
    [[nodiscard]] ServiceProvider build() const {
        validate_graph(descriptors_);
        auto state = std::make_shared<detail::ProviderState>();
        state->descriptors = descriptors_;
        return ServiceProvider(std::move(state));
    }
};

} // namespace goforge
// ---- end goforge/di.hpp ----
// ---- begin goforge/lifecycle.hpp ----

/**
 * @file lifecycle.hpp
 * @brief Uber Fx-style ordered application lifecycle (`Lifecycle`, `LifecycleHook`) with
 *        automatic rollback on startup failure and reverse-order graceful shutdown.
 */

#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

/**
 * @struct LifecycleHook
 * @brief A pair of `on_start` / `on_stop` callbacks registered with `Lifecycle::append(...)`.
 */
struct LifecycleHook {
    std::string name;                                 ///< Human-readable hook identifier for diagnostics
    std::function<void(gorun::Context)> on_start{};   ///< Invoked in registration order during `App::start()`
    std::function<void(gorun::Context)> on_stop{};    ///< Invoked in reverse order during `App::stop()`
};

/**
 * @class Lifecycle
 * @brief Manages ordered startup and reverse-order shutdown of application components.
 *
 * Mirrors Uber Fx's `fx.Lifecycle`:
 * - Services can inject `goforge::Lifecycle&` in their constructors/factories and call `append(...)`.
 * - `start(ctx)` runs hooks `[0 .. N-1]`. If hook `k` throws, `on_stop` is automatically invoked in
 *   reverse order for hooks `[k-1 .. 0]` before rethrowing the original startup error.
 * - `stop(ctx)` runs `on_stop` in reverse order `[N-1 .. 0]`, ensuring every component gets a chance
 *   to clean up even if an earlier `on_stop` throws.
 */
class Lifecycle {
    gorun::Mutex mtx_;
    std::vector<LifecycleHook> hooks_;
    std::size_t started_count_{0};

public:
    Lifecycle() = default;
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    /// Appends a lifecycle hook. Safe to call from constructors, factories, or invoke callbacks.
    void append(LifecycleHook hook) {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        hooks_.push_back(std::move(hook));
    }

    /// Returns the total number of registered hooks.
    [[nodiscard]] std::size_t hook_count() {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        return hooks_.size();
    }

    /// Starts all registered hooks in FIFO order. Automatically rolls back already-started hooks on failure.
    void start(gorun::Context ctx) {
        std::vector<LifecycleHook> snapshot;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = hooks_;
            started_count_ = 0;
        }

        std::size_t started = 0;
        try {
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                if (ctx.is_canceled()) {
                    throw std::runtime_error(
                        "Lifecycle start aborted before hook '" + snapshot[i].name + "': " +
                        std::string(gorun::to_string(*ctx.err()))
                    );
                }
                if (snapshot[i].on_start) {
                    snapshot[i].on_start(ctx);
                }
                started = i + 1;
            }
            std::lock_guard<gorun::Mutex> lk(mtx_);
            started_count_ = started;
        } catch (...) {
            // Rollback hooks [started-1 .. 0] in reverse order
            auto [rb_ctx, rb_cancel] = gorun::with_cancel(gorun::background());
            GORUN_DEFER { rb_cancel(); };
            for (std::size_t i = started; i > 0; --i) {
                const auto& hook = snapshot[i - 1];
                if (hook.on_stop) {
                    try {
                        hook.on_stop(rb_ctx);
                    } catch (...) {
                    }
                }
            }
            {
                std::lock_guard<gorun::Mutex> lk(mtx_);
                started_count_ = 0;
            }
            throw;
        }
    }

    /**
     * @brief Stops all started hooks in reverse (LIFO) order.
     * @return List of error messages from hooks whose `on_stop` threw (empty if clean shutdown).
     */
    std::vector<std::string> stop(gorun::Context ctx) {
        std::vector<LifecycleHook> snapshot;
        std::size_t count = 0;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = hooks_;
            count = started_count_;
            started_count_ = 0;
        }

        std::vector<std::string> errors;
        for (std::size_t i = count; i > 0; --i) {
            const auto& hook = snapshot[i - 1];
            if (!hook.on_stop) continue;
            try {
                hook.on_stop(ctx);
            } catch (const std::exception& ex) {
                errors.push_back("Hook '" + hook.name + "' on_stop failed: " + ex.what());
            } catch (...) {
                errors.push_back("Hook '" + hook.name + "' on_stop failed with unknown exception");
            }
        }
        return errors;
    }
};

} // namespace goforge
// ---- end goforge/lifecycle.hpp ----
// ---- begin goforge/blocking_pool.hpp ----

/**
 * @file blocking_pool.hpp
 * @brief Dedicated OS-thread pool for offloading synchronous/blocking I/O (e.g., libpq, sqlite3,
 *        disk I/O) without blocking `gorun` M:N green-thread worker threads.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

/**
 * @class BlockingPool
 * @brief Offloads blocking syscalls / legacy synchronous drivers onto dedicated OS threads while
 *        parking the calling `gorun` green routine on a `gorun::Channel` (zero worker starvation).
 */
class BlockingPool {
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::jthread> threads_;
    bool stopping_{false};

    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            if (task) {
                task();
            }
        }
    }

public:
    explicit BlockingPool(std::size_t thread_count = 4) {
        if (thread_count == 0) {
            thread_count = 1;
        }
        threads_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~BlockingPool() {
        shutdown();
    }

    BlockingPool(const BlockingPool&) = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;

    /// Returns the number of dedicated blocking OS threads.
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return threads_.size();
    }

    /// Gracefully drains pending tasks and joins all blocking OS worker threads.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    /**
     * @brief Executes a blocking callable `fn()` on the dedicated OS pool and parks the calling
     *        `gorun` green routine until completion (rethrows any exception thrown by `fn`).
     */
    template <typename Fn>
    auto run(Fn&& fn) -> std::invoke_result_t<std::decay_t<Fn>&> {
        using R = std::invoke_result_t<std::decay_t<Fn>&>;

        if constexpr (std::is_void_v<R>) {
            auto done_ch = std::make_shared<gorun::Channel<std::exception_ptr>>(1);
            auto fn_ptr = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (stopping_) {
                    throw std::runtime_error("BlockingPool is shutting down");
                }
                tasks_.emplace_back([done_ch, fn_ptr]() {
                    std::exception_ptr ep;
                    try {
                        (*fn_ptr)();
                    } catch (...) {
                        ep = std::current_exception();
                    }
                    done_ch->send(std::move(ep));
                });
            }
            cv_.notify_one();

            auto res = done_ch->recv();
            if (res && *res) {
                std::rethrow_exception(*res);
            }
        } else {
            struct Box {
                std::optional<R> value;
                std::exception_ptr error;
            };
            auto done_ch = std::make_shared<gorun::Channel<Box>>(1);
            auto fn_ptr = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (stopping_) {
                    throw std::runtime_error("BlockingPool is shutting down");
                }
                tasks_.emplace_back([done_ch, fn_ptr]() {
                    Box box;
                    try {
                        box.value.emplace((*fn_ptr)());
                    } catch (...) {
                        box.error = std::current_exception();
                    }
                    done_ch->send(std::move(box));
                });
            }
            cv_.notify_one();

            auto res = done_ch->recv();
            if (!res) {
                throw std::runtime_error("BlockingPool channel closed unexpectedly");
            }
            if (res->error) {
                std::rethrow_exception(res->error);
            }
            return std::move(*res->value);
        }
    }
};

} // namespace goforge
// ---- end goforge/blocking_pool.hpp ----
// ---- begin goforge/app.hpp ----

/**
 * @file app.hpp
 * @brief Uber Fx-style `AppBuilder` and `App` orchestrating DI resolution, `Lifecycle` startup/shutdown,
 *        tracked background `gorun` green routines, and `BlockingPool` integration.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"




namespace goforge {

/**
 * @struct AppConfig
 * @brief Configuration for `goforge::App` startup/shutdown timeouts and blocking offload pool.
 */
struct AppConfig {
    std::chrono::milliseconds start_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds stop_timeout{std::chrono::seconds(15)};
    std::size_t blocking_pool_threads{4};
};

/**
 * @class App
 * @brief Top-level application runtime owning the `ServiceProvider`, `Lifecycle`, and background routines.
 */
class App {
    friend class AppBuilder;

    AppConfig config_;
    ServiceProvider provider_;
    std::shared_ptr<Lifecycle> lifecycle_;
    std::shared_ptr<BlockingPool> blocking_pool_;
    std::vector<std::function<void(ServiceProvider&)>> invokers_;

    gorun::Context app_ctx_;
    gorun::CancelFunc app_cancel_;
    gorun::WaitGroup bg_wg_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    gorun::Channel<int> shutdown_signal_ch_{1};

    App(AppConfig cfg,
        ServiceProvider provider,
        std::shared_ptr<Lifecycle> lifecycle,
        std::shared_ptr<BlockingPool> blocking_pool,
        std::vector<std::function<void(ServiceProvider&)>> invokers)
        : config_(cfg)
        , provider_(std::move(provider))
        , lifecycle_(std::move(lifecycle))
        , blocking_pool_(std::move(blocking_pool))
        , invokers_(std::move(invokers)) {
        auto [ctx, cancel] = gorun::with_cancel(gorun::background());
        app_ctx_ = std::move(ctx);
        app_cancel_ = std::move(cancel);
    }

public:
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    ~App() {
        if (started_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_acquire)) {
            try {
                stop();
            } catch (...) {
            }
        }
    }

    /// Accesses the root `ServiceProvider`.
    [[nodiscard]] ServiceProvider& services() noexcept { return provider_; }

    /// Accesses the `Lifecycle` coordinator.
    [[nodiscard]] Lifecycle& lifecycle() noexcept { return *lifecycle_; }

    /// Accesses the dedicated `BlockingPool` for offloading blocking I/O.
    [[nodiscard]] BlockingPool& blocking_pool() noexcept { return *blocking_pool_; }

    /// Returns the root application `gorun::Context` (canceled when `stop()` begins).
    [[nodiscard]] gorun::Context context() const noexcept { return app_ctx_; }

    /// Creates a new `ServiceScope` (e.g., per HTTP request).
    [[nodiscard]] ServiceScope create_scope() noexcept { return provider_.create_scope(); }

    /**
     * @brief Spawns a managed background green routine on `gorun` that receives the application
     *        `gorun::Context` and is automatically awaited during `App::stop()`.
     */
    template <typename Fn>
    void spawn_background(Fn&& fn) {
        bg_wg_.add(1);
        gorun::Context ctx = app_ctx_;
        gorun::go([this, ctx, func = std::forward<Fn>(fn)]() mutable {
            GORUN_DEFER { bg_wg_.done(); };
            func(ctx);
        });
    }

    /**
     * @brief Executes all registered `invoke(...)` functions (instantiating services & hooks),
     *        then starts all `Lifecycle` hooks in FIFO order with `config_.start_timeout`.
     */
    void start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        try {
            for (auto& inv : invokers_) {
                inv(provider_);
            }
            auto [start_ctx, cancel] = gorun::with_timeout(app_ctx_, config_.start_timeout);
            GORUN_DEFER { cancel(); };
            lifecycle_->start(start_ctx);
        } catch (...) {
            started_.store(false, std::memory_order_release);
            throw;
        }
    }

    /**
     * @brief Performs graceful shutdown:
     * 1. Cancels the root `app_ctx_` so background routines exit cooperatively.
     * 2. Waits for all `spawn_background(...)` green routines via `gorun::WaitGroup`.
     * 3. Runs `Lifecycle::stop()` hooks in reverse (LIFO) order.
     * 4. Shuts down the `BlockingPool`.
     */
    std::vector<std::string> stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return {};
        }

        if (app_cancel_) {
            app_cancel_();
        }
        shutdown_signal_ch_.close();

        bg_wg_.wait();

        auto [stop_ctx, cancel] = gorun::with_timeout(gorun::background(), config_.stop_timeout);
        GORUN_DEFER { cancel(); };
        auto errors = lifecycle_->stop(stop_ctx);

        if (blocking_pool_) {
            blocking_pool_->shutdown();
        }
        return errors;
    }

    /// Triggers shutdown for an application blocked inside `run()`.
    void request_shutdown() {
        shutdown_signal_ch_.close();
    }

    /// Starts the application, parks until `request_shutdown()` is called, then executes graceful `stop()`.
    void run() {
        start();
        (void)shutdown_signal_ch_.recv();
        stop();
    }

    /**
     * @brief Starts the application, installs `SIGINT` / `SIGTERM` signal handlers, parks until either
     *        an OS termination signal arrives or `request_shutdown()` is called, then runs graceful `stop()`.
     */
    void run_until_signal() {
        signal_flag().store(false, std::memory_order_release);
        std::signal(SIGINT, &App::on_os_signal);
        std::signal(SIGTERM, &App::on_os_signal);

        start();

        // Lightweight signal-polling green routine (does not block any OS worker thread)
        gorun::go([this]() {
            while (!app_ctx_.is_canceled() && !stopped_.load(std::memory_order_acquire)) {
                if (signal_flag().load(std::memory_order_acquire)) {
                    request_shutdown();
                    return;
                }
                gorun::sleep_for(std::chrono::milliseconds(25));
            }
        });

        (void)shutdown_signal_ch_.recv();
        stop();
    }

private:
    static std::atomic<bool>& signal_flag() noexcept {
        static std::atomic<bool> flag{false};
        return flag;
    }

    static void on_os_signal(int) noexcept {
        signal_flag().store(true, std::memory_order_release);
    }
};

/**
 * @class AppBuilder
 * @brief Fluent Uber Fx-style builder for wiring DI providers, invoke callbacks, and lifecycle hooks.
 */
class AppBuilder {
    AppConfig config_{};
    ServiceCollection services_{};
    std::vector<std::function<void(ServiceProvider&)>> invokers_;

public:
    AppBuilder() = default;

    /// Sets application startup/shutdown timeouts and blocking pool sizing.
    AppBuilder& with_config(AppConfig cfg) {
        config_ = cfg;
        return *this;
    }

    /// Direct access to the underlying `ServiceCollection`.
    ServiceCollection& services() noexcept { return services_; }

    /// Registers a pre-built instance as a `Singleton`.
    template <typename T>
    AppBuilder& provide_instance(T&& value) {
        services_.add_instance(std::forward<T>(value));
        return *this;
    }

    /// Registers a `Singleton` constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_singleton() {
        services_.add_singleton<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Singleton` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_singleton_as() {
        services_.add_singleton_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a `Transient` constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_transient() {
        services_.add_transient<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Transient` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_transient_as() {
        services_.add_transient_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a `Scoped` service constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_scoped() {
        services_.add_scoped<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Scoped` service implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_scoped_as() {
        services_.add_scoped_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a factory `R(Deps...)` with the given `Lifetime` (default: `Singleton`).
    template <typename Factory>
    AppBuilder& provide(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        services_.provide(lifetime, std::forward<Factory>(factory));
        return *this;
    }

    /// Registers a factory explicitly bound to `ServiceType` (e.g., an interface).
    template <typename ServiceType, typename Factory>
    AppBuilder& provide_as(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        services_.add_factory<ServiceType>(lifetime, std::forward<Factory>(factory));
        return *this;
    }

    /**
     * @brief Registers an `invoke(Deps...)` function executed during `App::start()` to trigger eager
     *        service instantiation and register `Lifecycle` hooks (mirrors `fx.Invoke`).
     */
    template <typename Fn>
    AppBuilder& invoke(Fn&& fn) {
        auto holder = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        invokers_.push_back([holder](ServiceProvider& sp) {
            sp.invoke(*holder);
        });
        return *this;
    }

    /**
     * @brief Validates the full dependency graph (missing deps, cycles, captive scopes) and builds `App`.
     */
    [[nodiscard]] std::unique_ptr<App> build() {
        auto lifecycle = std::make_shared<Lifecycle>();
        auto blocking_pool = std::make_shared<BlockingPool>(config_.blocking_pool_threads);

        services_.add_instance<Lifecycle>(lifecycle);
        services_.add_instance<BlockingPool>(blocking_pool);

        ServiceProvider provider = services_.build();
        return std::unique_ptr<App>(new App(
            config_,
            std::move(provider),
            std::move(lifecycle),
            std::move(blocking_pool),
            std::move(invokers_)
        ));
    }
};

} // namespace goforge
// ---- end goforge/app.hpp ----
// ---- begin goforge/http/http_types.hpp ----

/**
 * @file http_types.hpp
 * @brief HTTP/1.1 request/response models, case-insensitive headers, status reason phrases,
 *        and incremental parsing/serialization helpers.
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>



namespace goforge::http {

namespace detail {

inline std::string to_lower_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline std::string_view trim_ows(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
    return sv;
}

inline std::string_view default_reason_phrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

} // namespace detail

/**
 * @class HeaderMap
 * @brief Case-insensitive HTTP header container preserving canonical wire names for responses.
 */
class HeaderMap {
    struct Entry {
        std::string canonical_name;
        std::string value;
    };
    std::unordered_map<std::string, Entry> entries_;

public:
    void set(std::string_view name, std::string value) {
        std::string lower = detail::to_lower_ascii(name);
        entries_.insert_or_assign(std::move(lower), Entry{std::string(name), std::move(value)});
    }

    [[nodiscard]] bool contains(std::string_view name) const {
        return entries_.contains(detail::to_lower_ascii(name));
    }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const {
        auto it = entries_.find(detail::to_lower_ascii(name));
        if (it == entries_.end()) return std::nullopt;
        return std::string_view(it->second.value);
    }

    [[nodiscard]] std::string get_or(std::string_view name, std::string_view fallback = "") const {
        auto v = get(name);
        return v ? std::string(*v) : std::string(fallback);
    }

    void erase(std::string_view name) {
        entries_.erase(detail::to_lower_ascii(name));
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [_, entry] : entries_) {
            fn(entry.canonical_name, entry.value);
        }
    }
};

/**
 * @struct HttpRequest
 * @brief Parsed HTTP/1.1 request message.
 */
struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string raw_query;
    std::string version{"HTTP/1.1"};
    HeaderMap headers;
    std::string body;
    std::string remote_addr;

    /// Returns whether this connection should remain open for HTTP/1.1 Keep-Alive.
    [[nodiscard]] bool keep_alive() const {
        auto conn = headers.get("Connection");
        if (conn) {
            std::string lower = detail::to_lower_ascii(*conn);
            if (lower.find("close") != std::string::npos) return false;
            if (lower.find("keep-alive") != std::string::npos) return true;
        }
        return version == "HTTP/1.1";
    }
};

/**
 * @struct HttpResponse
 * @brief HTTP/1.1 response message with wire serializer and convenience factories.
 */
struct HttpResponse {
    int status{200};
    std::string status_text;
    HeaderMap headers;
    std::string body;

    static HttpResponse text(int code, std::string content) {
        HttpResponse r;
        r.status = code;
        r.body = std::move(content);
        r.headers.set("Content-Type", "text/plain; charset=utf-8");
        return r;
    }

    static HttpResponse json(int code, std::string raw_json_body) {
        HttpResponse r;
        r.status = code;
        r.body = std::move(raw_json_body);
        r.headers.set("Content-Type", "application/json; charset=utf-8");
        return r;
    }

    static HttpResponse json(int code, const char* raw_json_body) {
        return json(code, std::string(raw_json_body ? raw_json_body : ""));
    }

    static HttpResponse json(int code, const Json& obj) {
        return json(code, obj.dump());
    }

    template <typename T>
        requires (!std::convertible_to<T, std::string_view> && !std::same_as<std::decay_t<T>, Json>)
    static HttpResponse json(int code, const T& dto) {
        return json(code, Json(dto).dump());
    }

    static HttpResponse error(int code, std::string_view error_code, std::string_view message = "") {
        return json(code, make_error_json(error_code, message));
    }

    static HttpResponse empty(int code = 204) {
        HttpResponse r;
        r.status = code;
        return r;
    }

    /// Serializes this response into an HTTP/1.1 wire buffer.
    [[nodiscard]] std::string serialize(bool keep_alive) const {
        std::string_view reason = status_text.empty()
            ? detail::default_reason_phrase(status)
            : std::string_view(status_text);

        std::string out;
        out.reserve(128 + body.size());
        out += "HTTP/1.1 ";
        out += std::to_string(status);
        out += ' ';
        out += reason;
        out += "\r\n";

        bool has_content_length = false;
        bool has_connection = false;
        bool has_server = false;

        headers.for_each([&](const std::string& k, const std::string& v) {
            std::string lower = detail::to_lower_ascii(k);
            if (lower == "content-length") has_content_length = true;
            if (lower == "connection") has_connection = true;
            if (lower == "server") has_server = true;
            out += k;
            out += ": ";
            out += v;
            out += "\r\n";
        });

        if (!has_server) {
            out += "Server: goforge\r\n";
        }
        if (!has_content_length) {
            out += "Content-Length: ";
            out += std::to_string(body.size());
            out += "\r\n";
        }
        if (!has_connection) {
            out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        }
        out += "\r\n";
        out += body;
        return out;
    }
};

enum class ParseStatus : std::uint8_t {
    Ok,
    Incomplete,
    BadRequest,
    HeadersTooLarge,
    PayloadTooLarge
};

struct ParseHeaderResult {
    ParseStatus status{ParseStatus::Incomplete};
    HttpRequest request;
    std::size_t header_end_pos{0}; ///< Offset immediately after "\r\n\r\n"
    std::size_t content_length{0};
};

/**
 * @brief Parses HTTP/1.1 request line and headers from `raw_buf` (non-destructive).
 */
inline ParseHeaderResult parse_http_headers(
    std::string_view raw_buf,
    std::size_t max_header_bytes,
    std::size_t max_body_bytes
) {
    ParseHeaderResult res;
    auto sep = raw_buf.find("\r\n\r\n");
    if (sep == std::string_view::npos) {
        if (raw_buf.size() > max_header_bytes) {
            res.status = ParseStatus::HeadersTooLarge;
        } else {
            res.status = ParseStatus::Incomplete;
        }
        return res;
    }

    if (sep + 4 > max_header_bytes) {
        res.status = ParseStatus::HeadersTooLarge;
        return res;
    }

    res.header_end_pos = sep + 4;
    std::string_view head = raw_buf.substr(0, sep);

    // 1. Parse request line: METHOD SP REQUEST-TARGET SP HTTP-VERSION
    auto first_crlf = head.find("\r\n");
    std::string_view req_line = (first_crlf == std::string_view::npos) ? head : head.substr(0, first_crlf);

    auto sp1 = req_line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0) {
        res.status = ParseStatus::BadRequest;
        return res;
    }
    auto sp2 = req_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp2 == sp1 + 1 || sp2 + 1 >= req_line.size()) {
        res.status = ParseStatus::BadRequest;
        return res;
    }

    std::string_view method = req_line.substr(0, sp1);
    std::string_view target = req_line.substr(sp1 + 1, sp2 - (sp1 + 1));
    std::string_view version = req_line.substr(sp2 + 1);

    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        res.status = ParseStatus::BadRequest;
        return res;
    }

    res.request.method = std::string(method);
    res.request.target = std::string(target);
    res.request.version = std::string(version);

    auto qpos = target.find('?');
    if (qpos == std::string_view::npos) {
        res.request.path = std::string(target);
    } else {
        res.request.path = std::string(target.substr(0, qpos));
        res.request.raw_query = std::string(target.substr(qpos + 1));
    }

    // 2. Parse header lines
    if (first_crlf != std::string_view::npos) {
        std::size_t pos = first_crlf + 2;
        while (pos < head.size()) {
            auto next = head.find("\r\n", pos);
            std::string_view line = (next == std::string_view::npos)
                ? head.substr(pos)
                : head.substr(pos, next - pos);
            pos = (next == std::string_view::npos) ? head.size() : next + 2;

            if (line.empty()) continue;
            auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                res.status = ParseStatus::BadRequest;
                return res;
            }
            std::string_view name = line.substr(0, colon);
            std::string_view val = detail::trim_ows(line.substr(colon + 1));
            res.request.headers.set(name, std::string(val));
        }
    }

    // 3. Parse optional Content-Length
    if (auto cl = res.request.headers.get("Content-Length")) {
        std::size_t len = 0;
        const char* first = cl->data();
        const char* last = first + cl->size();
        auto [ptr, ec] = std::from_chars(first, last, len);
        if (ec != std::errc{} || ptr != last) {
            res.status = ParseStatus::BadRequest;
            return res;
        }
        if (len > max_body_bytes) {
            res.status = ParseStatus::PayloadTooLarge;
            return res;
        }
        res.content_length = len;
    }

    res.status = ParseStatus::Ok;
    return res;
}

} // namespace goforge::http
// ---- end goforge/http/http_types.hpp ----
// ---- begin goforge/web/context.hpp ----

/**
 * @file context.hpp
 * @brief Gin-style per-request `HttpContext` with path/query parameters, URL decoding,
 *        typed key-value store, `gorun::Context` cancellation, `ServiceScope` DI access,
 *        response builders, and middleware chain control (`next()`, `abort()`).
 */

#include <any>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"




namespace goforge::web {

class HttpContext;

/// Middleware and route handler signature (mirrors `gin.HandlerFunc`).
using HandlerFunc = std::function<void(HttpContext&)>;

namespace detail {

inline int hex_digit_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

inline std::string url_decode(std::string_view input, bool plus_as_space = true) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (plus_as_space && c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < input.size()) {
            int hi = hex_digit_value(input[i + 1]);
            int lo = hex_digit_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

inline std::unordered_map<std::string, std::string> parse_query_string(std::string_view raw) {
    std::unordered_map<std::string, std::string> result;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        auto amp = raw.find('&', pos);
        std::string_view pair = (amp == std::string_view::npos)
            ? raw.substr(pos)
            : raw.substr(pos, amp - pos);
        pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;

        if (pair.empty()) continue;
        auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            result.insert_or_assign(url_decode(pair), std::string{});
        } else {
            result.insert_or_assign(
                url_decode(pair.substr(0, eq)),
                url_decode(pair.substr(eq + 1))
            );
        }
    }
    return result;
}

template <typename T>
std::optional<T> parse_number(std::string_view sv) noexcept {
    if (sv.empty()) return std::nullopt;
    T value{};
    const char* first = sv.data();
    const char* last = first + sv.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace detail

/**
 * @brief Sleeps cooperatively inside a green routine until `duration` elapses or `ctx` is canceled.
 * @return `true` if the full duration elapsed, `false` if canceled by `ctx` (e.g., Timeout middleware).
 */
inline bool sleep_with_context(gorun::Context ctx, std::chrono::milliseconds duration) {
    if (ctx.is_canceled()) return false;
    bool completed = false;
    gorun::select(
        gorun::on_timeout(duration, [&]() { completed = true; }),
        ctx.on_done([&]() { completed = false; })
    );
    return completed;
}

/**
 * @class HttpContext
 * @brief Per-request execution context passed through the middleware chain and route handler.
 */
class HttpContext {
    const http::HttpRequest* req_;
    http::HttpResponse resp_;
    gorun::Context go_ctx_;
    ServiceScope* scope_{nullptr};

    std::unordered_map<std::string, std::string> params_;
    mutable bool query_parsed_{false};
    mutable std::unordered_map<std::string, std::string> query_params_;
    std::unordered_map<std::string, std::any> values_;

    std::vector<HandlerFunc> handlers_;
    std::size_t handler_index_{0};
    bool aborted_{false};
    bool written_{false};

    void ensure_query_parsed() const {
        if (!query_parsed_) {
            query_params_ = detail::parse_query_string(req_->raw_query);
            query_parsed_ = true;
        }
    }

public:
    HttpContext(const http::HttpRequest& req,
                gorun::Context go_ctx,
                ServiceScope* scope = nullptr)
        : req_(&req)
        , go_ctx_(std::move(go_ctx))
        , scope_(scope) {}

    HttpContext(const HttpContext&) = delete;
    HttpContext& operator=(const HttpContext&) = delete;

    // ---- Request inspection ----
    [[nodiscard]] const http::HttpRequest& request() const noexcept { return *req_; }
    [[nodiscard]] const std::string& method() const noexcept { return req_->method; }
    [[nodiscard]] const std::string& path() const noexcept { return req_->path; }
    [[nodiscard]] const std::string& raw_query() const noexcept { return req_->raw_query; }
    [[nodiscard]] const std::string& body() const noexcept { return req_->body; }
    [[nodiscard]] const std::string& remote_addr() const noexcept { return req_->remote_addr; }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const {
        return req_->headers.get(name);
    }

    [[nodiscard]] std::string header_or(std::string_view name, std::string_view fallback = "") const {
        return req_->headers.get_or(name, fallback);
    }

    // ---- Path parameters (:id, *filepath) ----
    void set_params(std::unordered_map<std::string, std::string> params) {
        params_ = std::move(params);
    }

    [[nodiscard]] std::optional<std::string_view> param(std::string_view key) const {
        auto it = params_.find(std::string(key));
        if (it == params_.end()) return std::nullopt;
        return std::string_view(it->second);
    }

    [[nodiscard]] std::string param_or(std::string_view key, std::string_view fallback = "") const {
        auto v = param(key);
        return v ? std::string(*v) : std::string(fallback);
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] std::optional<T> param_as(std::string_view key) const noexcept {
        auto v = param(key);
        if (!v) return std::nullopt;
        return detail::parse_number<T>(*v);
    }

    // ---- Query parameters (?key=val&...) ----
    [[nodiscard]] std::optional<std::string_view> query(std::string_view key) const {
        ensure_query_parsed();
        auto it = query_params_.find(std::string(key));
        if (it == query_params_.end()) return std::nullopt;
        return std::string_view(it->second);
    }

    [[nodiscard]] std::string query_or(std::string_view key, std::string_view fallback = "") const {
        auto v = query(key);
        return v ? std::string(*v) : std::string(fallback);
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] std::optional<T> query_as(std::string_view key) const noexcept {
        auto v = query(key);
        if (!v) return std::nullopt;
        return detail::parse_number<T>(*v);
    }

    // ---- Per-request typed key-value bag (Gin-style Set/Get) ----
    template <typename T>
    void set(std::string key, T&& value) {
        values_.insert_or_assign(std::move(key), std::forward<T>(value));
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(const std::string& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) return std::nullopt;
        if (const T* ptr = std::any_cast<T>(&it->second)) {
            return *ptr;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool has_value(const std::string& key) const {
        return values_.contains(key);
    }

    // ---- gorun::Context cancellation & timeouts ----
    [[nodiscard]] gorun::Context go_ctx() const noexcept { return go_ctx_; }

    void set_go_ctx(gorun::Context ctx) noexcept { go_ctx_ = std::move(ctx); }

    [[nodiscard]] bool is_canceled() const noexcept { return go_ctx_.is_canceled(); }

    // ---- DI ServiceScope integration ----
    [[nodiscard]] bool has_scope() const noexcept { return scope_ != nullptr; }

    [[nodiscard]] ServiceScope& scope() {
        if (!scope_) {
            throw ScopeRequiredError("HttpContext has no active DI ServiceScope attached.");
        }
        return *scope_;
    }

    template <typename T>
    [[nodiscard]] T& resolve() {
        return scope().resolve<T>();
    }

    template <typename T>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<T>> resolve_ptr() {
        return scope().resolve_ptr<T>();
    }

    template <typename Fn>
    decltype(auto) invoke(Fn&& fn) {
        return scope().invoke(std::forward<Fn>(fn));
    }

    // ---- Typed JSON Request Binding & Validation (Gin ShouldBindJSON style) ----

    /// Parses the raw HTTP body into a generic `goforge::Json` value without writing an error response.
    [[nodiscard]] BindResult<Json> try_bind_json_value() const {
        return parse_json_body(req_->body);
    }

    /// Parses the raw HTTP body into a `goforge::Json` value; automatically aborts with `400` on parse error.
    [[nodiscard]] std::optional<Json> bind_json_value() {
        auto res = try_bind_json_value();
        if (!res.ok()) {
            abort_with_error(res.http_status, res.error_code, res.message);
            return std::nullopt;
        }
        return std::move(res.value);
    }

    /**
     * @brief Parses and validates the JSON body into DTO `T` without writing an error response.
     *        Checks JSON syntax/schema (`400`) and calls `dto.validate()` (`422`) if defined.
     */
    template <typename T>
    [[nodiscard]] BindResult<T> try_bind_json() const {
        return parse_and_validate_json<T>(req_->body);
    }

    /**
     * @brief Parses and validates the JSON body into DTO `T` (mirrors Gin's `ShouldBindJSON`).
     *        On failure, automatically writes a structured JSON error (`400` or `422`), aborts the
     *        middleware chain, and returns `std::nullopt`.
     */
    template <typename T>
    [[nodiscard]] std::optional<T> bind_json() {
        auto res = try_bind_json<T>();
        if (!res.ok()) {
            abort_with_error(res.http_status, res.error_code, res.message);
            return std::nullopt;
        }
        return std::move(res.value);
    }

    // ---- Response builders ----
    HttpContext& status(int code) {
        resp_.status = code;
        written_ = true;
        return *this;
    }

    HttpContext& set_header(std::string_view name, std::string value) {
        resp_.headers.set(name, std::move(value));
        return *this;
    }

    void text(int code, std::string body) {
        resp_.status = code;
        resp_.body = std::move(body);
        resp_.headers.set("Content-Type", "text/plain; charset=utf-8");
        written_ = true;
    }

    /// Writes a pre-serialized raw JSON string response.
    void json_raw(int code, std::string raw_json_body) {
        resp_.status = code;
        resp_.body = std::move(raw_json_body);
        resp_.headers.set("Content-Type", "application/json; charset=utf-8");
        written_ = true;
    }

    /// Writes a pre-serialized JSON string (backwards-compatible overload).
    void json(int code, std::string raw_json_body) {
        json_raw(code, std::move(raw_json_body));
    }

    /// Exact-match overload for raw JSON string literals (prevents ambiguity with `nlohmann::json(const char*)`).
    void json(int code, const char* raw_json_body) {
        json_raw(code, std::string(raw_json_body ? raw_json_body : ""));
    }

    /// Serializes a `goforge::Json` / `goforge::H` value to JSON and sets `Content-Type: application/json`.
    void json(int code, const Json& value) {
        json_raw(code, value.dump());
    }

    /// Serializes any DTO (`GOFORGE_JSON_DTO`), `std::vector<Dto>`, `std::map`, etc. directly to JSON!
    template <typename T>
        requires (!std::convertible_to<T, std::string_view> && !std::same_as<std::decay_t<T>, Json>)
    void json(int code, const T& dto) {
        json_raw(code, Json(dto).dump());
    }

    /// Writes a standardized structured JSON error: `{"error": error_code, "message": message}`.
    void error(int code, std::string_view error_code, std::string_view message = "") {
        json(code, make_error_json(error_code, message));
    }

    void no_content() {
        resp_.status = 204;
        resp_.body.clear();
        written_ = true;
    }

    [[nodiscard]] bool is_written() const noexcept { return written_; }
    [[nodiscard]] http::HttpResponse& response() noexcept { return resp_; }
    [[nodiscard]] const http::HttpResponse& response() const noexcept { return resp_; }

    // ---- Middleware chain execution ----
    void set_handlers(std::vector<HandlerFunc> chain) {
        handlers_ = std::move(chain);
        handler_index_ = 0;
        aborted_ = false;
    }

    void next() {
        while (handler_index_ < handlers_.size() && !aborted_) {
            const std::size_t idx = handler_index_++;
            if (handlers_[idx]) {
                handlers_[idx](*this);
            }
        }
    }

    void abort() noexcept {
        aborted_ = true;
        handler_index_ = handlers_.size();
    }

    void abort_with_text(int code, std::string msg) {
        text(code, std::move(msg));
        abort();
    }

    void abort_with_json(int code, std::string raw_json_body) {
        json_raw(code, std::move(raw_json_body));
        abort();
    }

    void abort_with_json(int code, const char* raw_json_body) {
        json_raw(code, std::string(raw_json_body ? raw_json_body : ""));
        abort();
    }

    void abort_with_json(int code, const Json& value) {
        json(code, value);
        abort();
    }

    template <typename T>
        requires (!std::convertible_to<T, std::string_view> && !std::same_as<std::decay_t<T>, Json>)
    void abort_with_json(int code, const T& dto) {
        json(code, dto);
        abort();
    }

    void abort_with_error(int code, std::string_view error_code, std::string_view message = "") {
        error(code, error_code, message);
        abort();
    }

    [[nodiscard]] bool is_aborted() const noexcept { return aborted_; }
};

} // namespace goforge::web
// ---- end goforge/web/context.hpp ----
// ---- begin goforge/web/router.hpp ----

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

struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> static_children;

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

    // 1. Static segment priority
    auto it = node->static_children.find(std::string(segs[depth]));
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

        std::vector<HandlerFunc> full_chain = global_middlewares_;

        if (match.matched && match.chain) {
            ctx.set_params(std::move(match.params));
            full_chain.insert(full_chain.end(), match.chain->begin(), match.chain->end());
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
                full_chain.push_back([allow = std::move(allow_hdr)](HttpContext& c) {
                    c.set_header("Allow", allow);
                    c.error(405, "method_not_allowed");
                });
            } else if (no_route_handler_) {
                full_chain.push_back(no_route_handler_);
            } else {
                full_chain.push_back([](HttpContext& c) {
                    c.error(404, "not_found");
                });
            }
        }

        ctx.set_handlers(std::move(full_chain));
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
// ---- end goforge/web/router.hpp ----
// ---- begin goforge/web/middleware.hpp ----

/**
 * @file middleware.hpp
 * @brief Built-in Gin-style middleware components for `goforge::web::Router`:
 *        `recovery`, `request_logger`, `timeout`, `rate_limit` (via `gorun::Semaphore`),
 *        and `bearer_auth`.
 */

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gorun/gorun.hpp"


namespace goforge::web::middleware {

/**
 * @struct RequestLogEntry
 * @brief Structured access-log record emitted by `request_logger(...)`.
 */
struct RequestLogEntry {
    std::string method;
    std::string path;
    std::string remote_addr;
    int status{200};
    std::int64_t latency_us{0};
};

/**
 * @brief Catches uncaught C++ exceptions in downstream middleware/handlers and returns
 *        a `500 Internal Server Error` JSON response without crashing the server.
 */
inline HandlerFunc recovery(std::function<void(const HttpContext&, const std::string&)> on_error = nullptr) {
    return [cb = std::move(on_error)](HttpContext& c) {
        try {
            c.next();
        } catch (const std::exception& ex) {
            if (cb) cb(c, ex.what());
            c.abort_with_error(500, "internal_server_error", ex.what());
        } catch (...) {
            if (cb) cb(c, "unknown exception");
            c.abort_with_error(500, "internal_server_error");
        }
    };
}

/**
 * @brief Measures request latency and emits a structured `RequestLogEntry` after `c.next()`.
 */
inline HandlerFunc request_logger(std::function<void(const RequestLogEntry&)> sink) {
    return [sink = std::move(sink)](HttpContext& c) {
        auto t0 = std::chrono::steady_clock::now();
        c.next();
        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0
        ).count();
        if (sink) {
            sink(RequestLogEntry{
                .method = c.method(),
                .path = c.path(),
                .remote_addr = c.remote_addr(),
                .status = c.response().status,
                .latency_us = dt
            });
        }
    };
}

/**
 * @brief Attaches a `gorun::with_timeout(c.go_ctx(), limit)` deadline to `HttpContext`.
 *
 * Cooperative handlers/middleware observing `c.go_ctx()` (via `gorun::select`, `sleep_with_context`,
 * or `AsyncTcpSocket`) abort immediately upon deadline expiration. If the deadline expires without
 * an explicit response (or with `DeadlineExceeded`), this middleware returns `504 Gateway Timeout`.
 */
inline HandlerFunc timeout(std::chrono::milliseconds limit) {
    return [limit](HttpContext& c) {
        auto [tctx, cancel] = gorun::with_timeout(c.go_ctx(), limit);
        GORUN_DEFER { cancel(); };

        gorun::Context prev = c.go_ctx();
        c.set_go_ctx(tctx);
        GORUN_DEFER { c.set_go_ctx(prev); };

        c.next();

        if (tctx.err() == gorun::ContextError::DeadlineExceeded && !c.is_written()) {
            c.abort_with_error(504, "gateway_timeout", "request deadline exceeded");
        }
    };
}

/**
 * @brief Concurrency limiter backed by `gorun::Semaphore` (zero OS-thread blocking).
 * @param max_concurrent Maximum number of concurrent green routines allowed through this middleware.
 * @param reject_immediately If `true`, immediately returns `429 Too Many Requests` when saturated;
 *                           if `false`, parks excess green routines on `gorun::Semaphore` until a slot opens.
 */
inline HandlerFunc rate_limit(std::size_t max_concurrent, bool reject_immediately = false) {
    auto sem = std::make_shared<gorun::Semaphore>(
        static_cast<std::ptrdiff_t>(max_concurrent > 0 ? max_concurrent : 1)
    );
    return [sem, reject_immediately](HttpContext& c) {
        if (reject_immediately) {
            if (!sem->try_acquire()) {
                c.abort_with_error(429, "too_many_requests");
                return;
            }
        } else {
            sem->acquire();
        }
        GORUN_DEFER { sem->release(); };
        c.next();
    };
}

/**
 * @brief Bearer token authentication middleware (`Authorization: Bearer <token>`).
 * @param validator Receives the raw token and returns an authenticated subject string (or `std::nullopt` to reject with 401).
 */
inline HandlerFunc bearer_auth(std::function<std::optional<std::string>(std::string_view)> validator) {
    return [validator = std::move(validator)](HttpContext& c) {
        auto auth = c.header("Authorization");
        constexpr std::string_view kPrefix = "Bearer ";
        if (!auth || auth->size() <= kPrefix.size() || auth->substr(0, kPrefix.size()) != kPrefix) {
            c.abort_with_error(401, "unauthorized", "missing bearer token");
            return;
        }
        std::string_view token = auth->substr(kPrefix.size());
        auto subject = validator ? validator(token) : std::nullopt;
        if (!subject) {
            c.abort_with_error(401, "unauthorized", "invalid token");
            return;
        }
        c.set("auth_subject", std::move(*subject));
        c.next();
    };
}

} // namespace goforge::web::middleware
// ---- end goforge/web/middleware.hpp ----
// ---- begin goforge/obs/logger.hpp ----

/**
 * @file logger.hpp
 * @brief Non-OS-blocking structured JSON logger (`StructuredLogger`) with RFC-8259 string escaping,
 *        UTC timestamps, log-level filtering, and built-in Gin-style access log middleware.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "gorun/gorun.hpp"



namespace goforge::obs {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

namespace detail {

inline std::string_view level_to_string(LogLevel lvl) noexcept {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

inline void append_json_escaped(std::string& out, std::string_view input) {
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

inline std::string utc_timestamp_iso8601() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(now);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &tt);
#else
    gmtime_r(&tt, &utc_tm);
#endif
    char buf[32];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        utc_tm.tm_year + 1900,
        utc_tm.tm_mon + 1,
        utc_tm.tm_mday,
        utc_tm.tm_hour,
        utc_tm.tm_min,
        utc_tm.tm_sec,
        static_cast<int>(ms.count())
    );
    return std::string(buf);
}

} // namespace detail

/**
 * @class StructuredLogger
 * @brief Thread-safe, green-routine-friendly JSON line logger synchronized with `gorun::Mutex`.
 */
class StructuredLogger {
    gorun::Mutex mtx_;
    std::ostream* out_;
    std::atomic<LogLevel> min_level_;

public:
    explicit StructuredLogger(LogLevel min_level = LogLevel::Info, std::ostream& out = std::cout) noexcept
        : out_(&out), min_level_(min_level) {}

    void set_level(LogLevel lvl) noexcept {
        min_level_.store(lvl, std::memory_order_release);
    }

    [[nodiscard]] LogLevel level() const noexcept {
        return min_level_.load(std::memory_order_acquire);
    }

    void log(LogLevel lvl,
             std::string_view msg,
             std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        if (static_cast<std::uint8_t>(lvl) < static_cast<std::uint8_t>(min_level_.load(std::memory_order_acquire))) {
            return;
        }

        std::string line;
        line.reserve(128 + msg.size());
        line += "{\"ts\":";
        detail::append_json_escaped(line, detail::utc_timestamp_iso8601());
        line += ",\"level\":";
        detail::append_json_escaped(line, detail::level_to_string(lvl));
        line += ",\"msg\":";
        detail::append_json_escaped(line, msg);

        for (const auto& [k, v] : fields) {
            line.push_back(',');
            detail::append_json_escaped(line, k);
            line.push_back(':');
            detail::append_json_escaped(line, v);
        }
        line += "}\n";

        std::lock_guard<gorun::Mutex> lk(mtx_);
        (*out_) << line;
        out_->flush();
    }

    void debug(std::string_view msg,
               std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Debug, msg, fields);
    }

    void info(std::string_view msg,
              std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Info, msg, fields);
    }

    void warn(std::string_view msg,
              std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Warn, msg, fields);
    }

    void error(std::string_view msg,
               std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Error, msg, fields);
    }

    /// Returns a Gin-style access logging middleware writing structured JSON lines for each HTTP request.
    [[nodiscard]] web::HandlerFunc access_log_middleware() {
        return web::middleware::request_logger([this](const web::middleware::RequestLogEntry& e) {
            std::string status_str = std::to_string(e.status);
            std::string latency_str = std::to_string(e.latency_us);
            LogLevel lvl = (e.status >= 500) ? LogLevel::Error
                         : (e.status >= 400) ? LogLevel::Warn
                                             : LogLevel::Info;
            log(lvl, "http_request", {
                {"method", e.method},
                {"path", e.path},
                {"status", status_str},
                {"latency_us", latency_str},
                {"remote", e.remote_addr}
            });
        });
    }
};

} // namespace goforge::obs
// ---- end goforge/obs/logger.hpp ----
// ---- begin goforge/obs/metrics.hpp ----

/**
 * @file metrics.hpp
 * @brief Production observability for `goforge`: `MetricsRegistry` (RPS, status counters,
 *        in-flight gauge, p50/p95/p99 latency percentiles, JSON & Prometheus exposition)
 *        and `HealthRegistry` (`/healthz`, `/readyz`, `/metrics`).
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"




namespace goforge::obs {

struct MetricsSnapshot {
    double uptime_sec{0.0};
    double avg_rps{0.0};
    std::uint64_t total_requests{0};
    std::int64_t in_flight{0};
    std::uint64_t status_2xx{0};
    std::uint64_t status_3xx{0};
    std::uint64_t status_4xx{0};
    std::uint64_t status_5xx{0};
    std::uint64_t latency_p50_us{0};
    std::uint64_t latency_p95_us{0};
    std::uint64_t latency_p99_us{0};
    std::uint64_t latency_max_us{0};
};

/**
 * @class MetricsRegistry
 * @brief Lightweight HTTP metrics collector tracking request counts, status classes, in-flight gauge,
 *        and sliding-window latency percentiles (p50/p95/p99/max) without blocking OS threads.
 */
class MetricsRegistry {
    static constexpr std::size_t kWindowSize = 4096;

    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::int64_t> in_flight_{0};
    std::atomic<std::uint64_t> status_2xx_{0};
    std::atomic<std::uint64_t> status_3xx_{0};
    std::atomic<std::uint64_t> status_4xx_{0};
    std::atomic<std::uint64_t> status_5xx_{0};

    mutable gorun::Mutex lat_mtx_;
    std::array<std::uint64_t, kWindowSize> latencies_{};
    std::size_t lat_pos_{0};
    std::size_t lat_count_{0};

    static std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double q) noexcept {
        if (sorted.empty()) return 0;
        double idx = std::ceil(q * static_cast<double>(sorted.size())) - 1.0;
        if (idx < 0.0) idx = 0.0;
        std::size_t i = static_cast<std::size_t>(idx);
        if (i >= sorted.size()) i = sorted.size() - 1;
        return sorted[i];
    }

public:
    MetricsRegistry() = default;

    void record(int status_code, std::uint64_t latency_us) {
        total_requests_.fetch_add(1, std::memory_order_relaxed);
        if (status_code >= 200 && status_code < 300) {
            status_2xx_.fetch_add(1, std::memory_order_relaxed);
        } else if (status_code >= 300 && status_code < 400) {
            status_3xx_.fetch_add(1, std::memory_order_relaxed);
        } else if (status_code >= 400 && status_code < 500) {
            status_4xx_.fetch_add(1, std::memory_order_relaxed);
        } else if (status_code >= 500) {
            status_5xx_.fetch_add(1, std::memory_order_relaxed);
        }

        std::lock_guard<gorun::Mutex> lk(lat_mtx_);
        latencies_[lat_pos_] = latency_us;
        lat_pos_ = (lat_pos_ + 1) % kWindowSize;
        if (lat_count_ < kWindowSize) {
            ++lat_count_;
        }
    }

    [[nodiscard]] MetricsSnapshot snapshot() const {
        MetricsSnapshot s;
        auto now = std::chrono::steady_clock::now();
        s.uptime_sec = std::chrono::duration<double>(now - started_at_).count();
        s.total_requests = total_requests_.load(std::memory_order_relaxed);
        s.in_flight = in_flight_.load(std::memory_order_relaxed);
        s.status_2xx = status_2xx_.load(std::memory_order_relaxed);
        s.status_3xx = status_3xx_.load(std::memory_order_relaxed);
        s.status_4xx = status_4xx_.load(std::memory_order_relaxed);
        s.status_5xx = status_5xx_.load(std::memory_order_relaxed);
        s.avg_rps = (s.uptime_sec > 1e-6) ? (static_cast<double>(s.total_requests) / s.uptime_sec) : 0.0;

        std::vector<std::uint64_t> copy;
        {
            std::lock_guard<gorun::Mutex> lk(lat_mtx_);
            copy.assign(latencies_.begin(), latencies_.begin() + static_cast<std::ptrdiff_t>(lat_count_));
        }

        if (!copy.empty()) {
            std::sort(copy.begin(), copy.end());
            s.latency_p50_us = percentile_sorted(copy, 0.50);
            s.latency_p95_us = percentile_sorted(copy, 0.95);
            s.latency_p99_us = percentile_sorted(copy, 0.99);
            s.latency_max_us = copy.back();
        }
        return s;
    }

    [[nodiscard]] std::string to_json() const {
        auto s = snapshot();
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "{"
            << "\"uptime_sec\":" << s.uptime_sec << ","
            << "\"total_requests\":" << s.total_requests << ","
            << "\"in_flight\":" << s.in_flight << ","
            << "\"avg_rps\":" << s.avg_rps << ","
            << "\"status\":{"
            << "\"2xx\":" << s.status_2xx << ","
            << "\"3xx\":" << s.status_3xx << ","
            << "\"4xx\":" << s.status_4xx << ","
            << "\"5xx\":" << s.status_5xx
            << "},"
            << "\"latency_us\":{"
            << "\"p50\":" << s.latency_p50_us << ","
            << "\"p95\":" << s.latency_p95_us << ","
            << "\"p99\":" << s.latency_p99_us << ","
            << "\"max\":" << s.latency_max_us
            << "}"
            << "}";
        return oss.str();
    }

    [[nodiscard]] std::string to_prometheus() const {
        auto s = snapshot();
        std::ostringstream oss;
        oss << "# TYPE goforge_http_requests_total counter\n"
            << "goforge_http_requests_total " << s.total_requests << "\n"
            << "# TYPE goforge_http_in_flight gauge\n"
            << "goforge_http_in_flight " << s.in_flight << "\n"
            << "# TYPE goforge_http_responses_by_class_total counter\n"
            << "goforge_http_responses_by_class_total{class=\"2xx\"} " << s.status_2xx << "\n"
            << "goforge_http_responses_by_class_total{class=\"3xx\"} " << s.status_3xx << "\n"
            << "goforge_http_responses_by_class_total{class=\"4xx\"} " << s.status_4xx << "\n"
            << "goforge_http_responses_by_class_total{class=\"5xx\"} " << s.status_5xx << "\n"
            << "# TYPE goforge_http_latency_microseconds summary\n"
            << "goforge_http_latency_microseconds{quantile=\"0.50\"} " << s.latency_p50_us << "\n"
            << "goforge_http_latency_microseconds{quantile=\"0.95\"} " << s.latency_p95_us << "\n"
            << "goforge_http_latency_microseconds{quantile=\"0.99\"} " << s.latency_p99_us << "\n";
        return oss.str();
    }

    /// Returns a Gin-style middleware recording every HTTP request's status and latency.
    [[nodiscard]] web::HandlerFunc middleware() {
        return [this](web::HttpContext& c) {
            in_flight_.fetch_add(1, std::memory_order_relaxed);
            auto t0 = std::chrono::steady_clock::now();
            bool recorded = false;
            GORUN_DEFER {
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                if (!recorded) {
                    auto dt = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0
                        ).count()
                    );
                    record(500, dt);
                }
            };
            c.next();
            auto dt = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0
                ).count()
            );
            record(c.response().status, dt);
            recorded = true;
        };
    }
};

struct HealthCheckResult {
    bool healthy{true};
    std::string detail{"ok"};
};

/**
 * @class HealthRegistry
 * @brief Kubernetes-style liveness (`/healthz`), readiness (`/readyz`), and `/metrics` endpoint mount.
 */
class HealthRegistry {
    struct CheckEntry {
        std::string name;
        std::function<HealthCheckResult()> fn;
    };

    gorun::Mutex mtx_;
    std::vector<CheckEntry> checks_;

public:
    HealthRegistry() = default;

    void add_check(std::string name, std::function<HealthCheckResult()> check_fn) {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        checks_.push_back(CheckEntry{std::move(name), std::move(check_fn)});
    }

    /// Evaluates all readiness checks and returns `{all_healthy, json_body}`.
    [[nodiscard]] std::pair<bool, std::string> evaluate_readiness() {
        std::vector<CheckEntry> snapshot;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = checks_;
        }

        bool all_ok = true;
        std::string checks_json = "{";
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            HealthCheckResult r;
            try {
                r = snapshot[i].fn ? snapshot[i].fn() : HealthCheckResult{true, "ok"};
            } catch (const std::exception& ex) {
                r = HealthCheckResult{false, ex.what()};
            } catch (...) {
                r = HealthCheckResult{false, "unknown error"};
            }
            if (!r.healthy) all_ok = false;
            if (i > 0) checks_json.push_back(',');
            detail::append_json_escaped(checks_json, snapshot[i].name);
            checks_json += ":{\"healthy\":";
            checks_json += (r.healthy ? "true" : "false");
            checks_json += ",\"detail\":";
            detail::append_json_escaped(checks_json, r.detail);
            checks_json.push_back('}');
        }
        checks_json.push_back('}');

        std::string body = std::string("{\"status\":\"") + (all_ok ? "ready" : "degraded") +
                           "\",\"checks\":" + checks_json + "}";
        return {all_ok, std::move(body)};
    }

    /// Mounts `/healthz`, `/readyz`, and `/metrics` onto a `goforge::web::Router`.
    void mount(web::Router& router, MetricsRegistry& metrics) {
        router.get("/healthz", [](web::HttpContext& c) {
            c.json(200, H{{"status", "alive"}});
        });

        router.get("/readyz", [this](web::HttpContext& c) {
            auto [ok, body] = evaluate_readiness();
            c.json_raw(ok ? 200 : 503, std::move(body));
        });

        router.get("/metrics", [&metrics](web::HttpContext& c) {
            if (c.query_or("format") == "prometheus") {
                c.text(200, metrics.to_prometheus());
            } else {
                c.json_raw(200, metrics.to_json());
            }
        });
    }
};

} // namespace goforge::obs
// ---- end goforge/obs/metrics.hpp ----

// ---- end goforge/goforge.hpp ----
