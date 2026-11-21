#pragma once

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
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...)> {
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const noexcept> {
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) noexcept> {
    using class_type = C;
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

    template <typename D>
    using CtorDepParam = std::conditional_t<
        std::is_reference_v<D> || detail::IsSharedPtr<std::remove_cvref_t<D>>::value,
        D,
        D&
    >;

    template <typename ServiceType, typename ImplType, typename... Deps>
    ServiceCollection& register_ctor(Lifetime lifetime) {
        static_assert(
            std::is_same_v<ServiceType, ImplType> || std::is_base_of_v<ServiceType, ImplType>,
            "ImplType must derive from ServiceType."
        );
        return register_factory<ServiceType>(
            lifetime,
            [](CtorDepParam<Deps>... deps) -> std::shared_ptr<ServiceType> {
                return std::static_pointer_cast<ServiceType>(
                    std::make_shared<ImplType>(std::forward<CtorDepParam<Deps>>(deps)...)
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

    /// Returns true if a descriptor for `T` has already been registered.
    template <typename T>
    [[nodiscard]] bool has() const noexcept {
        using CleanT = std::remove_cv_t<T>;
        return descriptors_.contains(std::type_index(typeid(CleanT)));
    }

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
