#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "goforge/goforge.hpp"

#define GF_CHECK(cond)                                                                      \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::cerr << "[FAIL] " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

using namespace goforge;
using namespace goforge::query;

// ============================================================================
// Models & DTOs using unified GOFORGE_MODEL_TABLE / GOFORGE_MODEL
// ============================================================================

struct User {
    int id{0};
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{0};
    bool active{true};
    std::optional<std::string> bio;
    std::vector<std::string> tags;
};
GOFORGE_MODEL_TABLE(User, "users", id, name, email, role, age, active, bio, tags)

struct Order {
    int id{0};
    int user_id{0};
    double price{0.0};
    int qty{1};
};
GOFORGE_MODEL_TABLE(Order, "orders", id, user_id, price, qty)

struct CreateUserReq {
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{18};
    std::optional<std::string> bio;

    std::optional<std::string> validate() const {
        if (name.empty()) return "name cannot be empty";
        if (email.find('@') == std::string::npos) return "invalid email";
        if (age < 13) return "age must be >= 13";
        return std::nullopt;
    }
};
GOFORGE_JSON_DTO_DEFAULT(CreateUserReq, name, email, role, age, bio)

// ============================================================================
// Mini Proof-of-Concept AST -> SQL Visitor (verifying future EF-ORM readiness)
// ============================================================================
template <typename Expr>
std::string demo_compile_sql(const Expr& expr, std::vector<std::string>& bind_params) {
    using E = std::remove_cvref_t<Expr>;
    if constexpr (std::is_same_v<E, IdentityExpr>) {
        return "value";
    } else if constexpr (requires { expr.ptr; expr.name; }) {
        return std::string(expr.name);
    } else if constexpr (requires { expr.value; }) {
        bind_params.push_back(Json(expr.value).dump());
        return "$" + std::to_string(bind_params.size());
    } else if constexpr (requires { E::op; expr.lhs; expr.rhs; }) {
        if constexpr (std::is_same_v<decltype(E::op), const CmpOp>) {
            const char* sym = "=";
            switch (E::op) {
                case CmpOp::Eq: sym = "="; break;
                case CmpOp::Ne: sym = "<>"; break;
                case CmpOp::Lt: sym = "<"; break;
                case CmpOp::Le: sym = "<="; break;
                case CmpOp::Gt: sym = ">"; break;
                case CmpOp::Ge: sym = ">="; break;
            }
            std::string lhs_sql = demo_compile_sql(expr.lhs, bind_params);
            std::string rhs_sql = demo_compile_sql(expr.rhs, bind_params);
            return lhs_sql + " " + sym + " " + rhs_sql;
        } else if constexpr (std::is_same_v<decltype(E::op), const LogicOp>) {
            const char* sym = (E::op == LogicOp::And) ? " AND " : " OR ";
            std::string lhs_sql = demo_compile_sql(expr.lhs, bind_params);
            std::string rhs_sql = demo_compile_sql(expr.rhs, bind_params);
            return "(" + lhs_sql + sym + rhs_sql + ")";
        }
    }
    return "?";
}

// ============================================================================
// Test 1: Expression Tree AST, `it` placeholder, and `GF_FN` shorthand
// ============================================================================
static void test_query_ast_and_shorthand() {
    std::vector<User> users{
        {1, "Artur", "artur@goforge.dev", "admin", 25, true, "Creator", {"cpp", "go"}},
        {2, "Olena", "olena@goforge.dev", "admin", 31, true, std::nullopt, {"rust", "cpp"}},
        {3, "Max", "max@example.com", "user", 17, true, "Student", {"python"}},
        {4, "Ivan", "ivan@goforge.dev", "mod", 22, false, std::nullopt, {"go"}},
        {5, "Bohdan", "bohdan@goforge.dev", "user", 29, true, "Backend", {"cpp", "sql", "go"}}
    };

    // Zero-lambda AST filtering + sorting + projection
    auto admins = from(users)
        .where(col(&User::age) >= 18 && col(&User::role) == "admin")
        .order_by_desc(&User::age)
        .select(&User::name)
        .to_vec();
    GF_CHECK((admins == std::vector<std::string>{"Olena", "Artur"}));

    // String predicates (.contains, .icontains, .starts_with, .ends_with)
    auto dev_emails = from(users)
        .where(col(&User::email).ends_with("@goforge.dev") && col(&User::name).icontains("a"))
        .order_by(&User::id)
        .select(&User::name)
        .to_vec();
    GF_CHECK((dev_emails == std::vector<std::string>{"Artur", "Olena", "Ivan", "Bohdan"}));

    // Set membership (.in, .not_in), range (.between), nullability (.is_null / .is_not_null), negation (!)
    auto staff_no_bio = from(users)
        .where(col(&User::role).in({"admin", "mod"}) && col(&User::bio).is_null())
        .order_by(&User::id)
        .select(&User::id)
        .to_vec();
    GF_CHECK((staff_no_bio == std::vector<int>{2, 4}));

    auto inactive_ids = from(users)
        .where(!col(&User::active))
        .select(&User::id)
        .to_vec();
    GF_CHECK((inactive_ids == std::vector<int>{4}));

    auto twenties = from(users)
        .where(col(&User::age).between(20, 29) && col(&User::role).not_in({"mod"}))
        .order_by(&User::age)
        .select(&User::name)
        .to_vec();
    GF_CHECK((twenties == std::vector<std::string>{"Artur", "Bohdan"}));

    // Arithmetic AST expression (price * qty > 100)
    std::vector<Order> orders{
        {1, 1, 50.0, 1},
        {2, 1, 45.0, 3},
        {3, 2, 120.0, 2}
    };
    auto big_totals = from(orders)
        .where(col(&Order::price) * col(&Order::qty) > 100.0)
        .select(col(&Order::price) * col(&Order::qty))
        .to_vec();
    GF_CHECK(big_totals.size() == 2);
    GF_CHECK(std::abs(big_totals[0] - 135.0) < 1e-6);
    GF_CHECK(std::abs(big_totals[1] - 240.0) < 1e-6);

    // Identity placeholder `it` on primitive ranges
    auto nums = from({3, 12, 7, 25, 18, 4, 12})
        .where(it >= 10 && it <= 20)
        .distinct()
        .order_by(it)
        .select(it * 2)
        .to_vec();
    GF_CHECK((nums == std::vector<int>{24, 36}));

    // Opt-in short lambda macro GF_FN / GF_FN2
    auto multi_tag = from(users)
        .where(GF_FN(u, u.tags.size() >= 3 && u.active))
        .first();
    GF_CHECK(multi_tag.has_value() && multi_tag->name == "Bohdan");

    int sum_ages = from(users).aggregate(0, GF_FN2(acc, u, acc + u.age));
    GF_CHECK(sum_ages == 25 + 31 + 17 + 22 + 29);

    std::cout << "[PASS] Test 1: Query AST expressions, `it` placeholder, and GF_FN shorthand\n";
}

// ============================================================================
// Test 2: Fluent Operations (then_by, group_by, join, select_many, paginate)
// ============================================================================
static void test_query_fluent_operations() {
    std::vector<User> users{
        {1, "Artur", "a@dev", "admin", 25, true, std::nullopt, {"cpp", "go"}},
        {2, "Olena", "o@dev", "admin", 31, true, std::nullopt, {"rust", "cpp"}},
        {3, "Max", "m@dev", "user", 25, true, std::nullopt, {"go"}},
        {4, "Bohdan", "b@dev", "user", 29, true, std::nullopt, {"sql"}}
    };

    // Multi-key sorting: order_by(role).then_by_desc(age)
    auto sorted_names = from(users)
        .order_by(&User::role)
        .then_by_desc(&User::age)
        .select(&User::name)
        .to_vec();
    GF_CHECK((sorted_names == std::vector<std::string>{"Olena", "Artur", "Bohdan", "Max"}));

    // select_many + distinct
    auto all_tags = from(users)
        .select_many(&User::tags)
        .distinct()
        .order_by(it)
        .to_vec();
    GF_CHECK((all_tags == std::vector<std::string>{"cpp", "go", "rust", "sql"}));

    // group_by + group aggregations
    auto groups = from(users).group_by(&User::role).to_vec();
    GF_CHECK(groups.size() == 2);
    GF_CHECK(groups[0].key == "admin" && groups[0].count() == 2);
    GF_CHECK(groups[0].sum(&User::age) == 56);
    GF_CHECK(std::abs(*groups[0].avg(&User::age) - 28.0) < 1e-6);

    // Inner join (Users <-> Orders)
    std::vector<Order> orders{
        {10, 1, 50.0, 2},
        {11, 2, 75.0, 1},
        {12, 1, 25.0, 1}
    };
    auto joined = from(users)
        .join(orders, &User::id, &Order::user_id,
              GF_FN2(u, o, u.name + ":" + std::to_string(o.id)))
        .to_vec();
    GF_CHECK((joined == std::vector<std::string>{"Artur:10", "Artur:12", "Olena:11"}));

    // Aggregations & predicates
    GF_CHECK(from(users).sum(&User::age) == 110);
    GF_CHECK(std::abs(*from(users).avg(&User::age) - 27.5) < 1e-6);
    GF_CHECK(from(users).min_by(&User::age)->id == 1);
    GF_CHECK(from(users).max_by(&User::age)->id == 2);
    GF_CHECK(from(users).all(col(&User::active) == true));
    GF_CHECK(from(users).any(col(&User::age) > 30));
    GF_CHECK(from(users).none(col(&User::age) < 18));

    // Pagination + automatic JSON serialization of Page<User>
    auto page2 = from(users).order_by(&User::id).paginate(2, 2);
    GF_CHECK(page2.total == 4);
    GF_CHECK(page2.page == 2);
    GF_CHECK(page2.page_size == 2);
    GF_CHECK(page2.total_pages == 2);
    GF_CHECK(page2.has_prev && !page2.has_next);
    GF_CHECK(page2.items.size() == 2 && page2.items[0].id == 3 && page2.items[1].id == 4);

    Json pj = page2;
    GF_CHECK(pj["total"] == 4 && pj["total_pages"] == 2 && pj["items"].size() == 2);

    std::cout << "[PASS] Test 2: Fluent query ops (then_by, group_by, join, select_many, Page<T> JSON)\n";
}

// ============================================================================
// Test 3: GOFORGE_MODEL Reflection & Future SQL AST Compilation Readiness
// ============================================================================
static void test_model_reflection_and_sql_ast() {
    static_assert(EntityMeta<User>::registered);
    static_assert(table_name_of<User>() == "users");
    static_assert(column_name_of(&User::age) == "age");
    static_assert(column_name_of(&User::email) == "email");

    // col(&User::age) automatically embeds column name "age" at compile time!
    constexpr auto age_col = col(&User::age);
    static_assert(age_col.name == "age");

    auto pred = (col(&User::age) >= 18) && (col(&User::role) == "admin");
    std::vector<std::string> params;
    std::string sql = demo_compile_sql(pred, params);
    GF_CHECK(sql == "(age >= $1 AND role = $2)");
    GF_CHECK(params.size() == 2 && params[0] == "18" && params[1] == "\"admin\"");

    // Same predicate evaluates identically in memory!
    User u1{1, "Artur", "a@dev", "admin", 25, true, std::nullopt, {}};
    User u2{2, "Max", "m@dev", "user", 25, true, std::nullopt, {}};
    GF_CHECK(pred.eval(u1) == true);
    GF_CHECK(pred.eval(u2) == false);

    std::cout << "[PASS] Test 3: GOFORGE_MODEL compile-time reflection & SQL-ready AST verification\n";
}

// ============================================================================
// Services & Zero-Lambda Controller for Tests 4 & 5
// ============================================================================
struct IUserStore {
    virtual ~IUserStore() = default;
    virtual User create(const CreateUserReq& req) = 0;
    virtual std::optional<User> find(int id) const = 0;
    virtual Page<User> search(std::string_view q, std::size_t page, std::size_t limit) const = 0;
};

struct InMemoryUserStore final : IUserStore {
    mutable gorun::RwMutex rw;
    int next_id{3};
    std::unordered_map<int, User> rows{
        {1, {1, "Artur", "artur@goforge.dev", "admin", 25, true, "Creator", {"cpp"}}},
        {2, {2, "Olena", "olena@goforge.dev", "user", 30, true, std::nullopt, {"go"}}}
    };

    User create(const CreateUserReq& req) override {
        std::unique_lock lk(rw);
        int id = next_id++;
        User u{id, req.name, req.email, req.role, req.age, true, req.bio, {}};
        rows[id] = u;
        return u;
    }

    std::optional<User> find(int id) const override {
        std::shared_lock lk(rw);
        auto it_row = rows.find(id);
        if (it_row == rows.end()) return std::nullopt;
        return it_row->second;
    }

    Page<User> search(std::string_view q, std::size_t page, std::size_t limit) const override {
        std::shared_lock lk(rw);
        auto seq = from_values(rows);
        if (!q.empty()) {
            seq = seq.where(col(&User::name).icontains(q) || col(&User::email).icontains(q));
        }
        return seq.order_by(&User::id).paginate(page, limit);
    }
};

struct UserController {
    IUserStore& store;
    explicit UserController(IUserStore& s) : store(s) {}

    ApiResponse create(Body<CreateUserReq> dto) {
        return created(store.create(*dto)).header("X-Created-By", "UserController");
    }

    std::optional<User> get_by_id(Path<"id", int> id) const {
        return store.find(*id);
    }

    Page<User> list(Query<"q"> q, Query<"page", int> page, Query<"limit", int> limit) const {
        return store.search(q.value_or(""),
                            static_cast<std::size_t>(std::max(1, page.value_or(1))),
                            static_cast<std::size_t>(std::max(1, limit.value_or(10))));
    }

    Result<User> get_with_header(Path<"id", int> id, Header<"X-Api-Key"> key) const {
        if (key.value_or("") != "secret") {
            return unauthorized("missing or invalid X-Api-Key");
        }
        auto u = store.find(*id);
        if (!u) return not_found("user does not exist");
        return *u;
    }

    ApiResponse remove_dummy(Path<"id", int>) {
        return no_content();
    }
};

// ============================================================================
// Test 4: WebApp Facade, Typed Extractors, Smart Returns & Zero-Lambda Controllers
// ============================================================================
static void test_webapp_extractors_and_controllers() {
    auto app = WebApp::create({.port = 0})
        .singleton<IUserStore, InMemoryUserStore>()
        .use_defaults(false); // mounts /healthz, /readyz, /metrics + recovery + metrics

    // Zero-lambda Controller routes!
    app.controller<UserController, IUserStore>("/api/v1/users")
       .post("", &UserController::create)
       .get("", &UserController::list)
       .get("/:id", &UserController::get_by_id)
       .get("/:id/secure", &UserController::get_with_header)
       .del("/:id", &UserController::remove_dummy);

    // Lightweight Minimal API route (no HttpContext& needed!)
    app.get("/api/v1/ping", []() {
        return H{{"pong", true}};
    });

    auto make_req = [](std::string method, std::string path, std::string query = "", std::string body = "") {
        http::HttpRequest r;
        r.method = std::move(method);
        r.path = std::move(path);
        r.raw_query = std::move(query);
        r.body = std::move(body);
        return r;
    };

    // 1. Minimal API ping
    {
        auto resp = app.dispatch(make_req("GET", "/api/v1/ping"));
        GF_CHECK(resp.status == 200);
        GF_CHECK(Json::parse(resp.body)["pong"] == true);
    }

    // 2. Built-in /healthz from use_defaults()
    {
        auto resp = app.dispatch(make_req("GET", "/healthz"));
        GF_CHECK(resp.status == 200);
    }

    // 3. Controller POST -> 201 Created + custom header
    {
        auto resp = app.dispatch(make_req(
            "POST", "/api/v1/users", "",
            Json(H{{"name", "Dmytro"}, {"email", "dmytro@goforge.dev"}, {"age", 27}}).dump()
        ));
        GF_CHECK(resp.status == 201);
        GF_CHECK(resp.headers.get_or("X-Created-By") == "UserController");
        auto j = Json::parse(resp.body);
        GF_CHECK(j["id"] == 3 && j["name"] == "Dmytro");
    }

    // 4. Controller POST validation failure -> automatic 422 Unprocessable Entity
    {
        auto resp = app.dispatch(make_req(
            "POST", "/api/v1/users", "",
            Json(H{{"name", "Kid"}, {"email", "kid@goforge.dev"}, {"age", 10}}).dump()
        ));
        GF_CHECK(resp.status == 422);
        GF_CHECK(Json::parse(resp.body)["error"] == "validation_failed");
    }

    // 5. Controller GET /:id -> std::optional<User> (200 OK & 404 Not Found & 400 Invalid Path)
    {
        auto r200 = app.dispatch(make_req("GET", "/api/v1/users/1"));
        GF_CHECK(r200.status == 200 && Json::parse(r200.body)["name"] == "Artur");

        auto r404 = app.dispatch(make_req("GET", "/api/v1/users/999"));
        GF_CHECK(r404.status == 404 && Json::parse(r404.body)["error"] == "not_found");

        auto r400 = app.dispatch(make_req("GET", "/api/v1/users/not-a-number"));
        GF_CHECK(r400.status == 400 && Json::parse(r400.body)["error"] == "invalid_path_param");
    }

    // 6. Controller GET list with Query<"q">, Query<"page", int>, Page<User> JSON!
    {
        auto r = app.dispatch(make_req("GET", "/api/v1/users", "q=art&page=1&limit=5"));
        GF_CHECK(r.status == 200);
        auto j = Json::parse(r.body);
        GF_CHECK(j["total"] == 1 && j["items"][0]["name"] == "Artur");

        auto bad_q = app.dispatch(make_req("GET", "/api/v1/users", "page=xyz"));
        GF_CHECK(bad_q.status == 400 && Json::parse(bad_q.body)["error"] == "invalid_query_param");
    }

    // 7. Controller Result<User> + Header<"X-Api-Key">
    {
        auto req_unauth = make_req("GET", "/api/v1/users/1/secure");
        GF_CHECK(app.dispatch(req_unauth).status == 401);

        auto req_ok = make_req("GET", "/api/v1/users/1/secure");
        req_ok.headers.set("X-Api-Key", "secret");
        auto resp_ok = app.dispatch(req_ok);
        GF_CHECK(resp_ok.status == 200 && Json::parse(resp_ok.body)["id"] == 1);
    }

    // 8. Controller DELETE -> 204 No Content
    {
        auto r204 = app.dispatch(make_req("DELETE", "/api/v1/users/1"));
        GF_CHECK(r204.status == 204 && r204.body.empty());
    }

    std::cout << "[PASS] Test 4: WebApp facade, typed extractors, smart returns & zero-lambda controllers\n";
}

#if !defined(GORUN_DISABLE_NET)
// ============================================================================
// Test 5: Live HTTP/1.1 TCP Roundtrip via WebApp::start() / stop() (Native gorun::net)
// ============================================================================
static void test_webapp_live_http() {
    auto app = WebApp::create({.host = "127.0.0.1", .port = 0})
        .singleton<IUserStore, InMemoryUserStore>()
        .use_defaults(false);

    app.controller<UserController, IUserStore>("/api/v1/users")
       .get("", &UserController::list)
       .get("/:id", &UserController::get_by_id);

    app.start();
    const std::uint16_t port = app.bound_port();
    GF_CHECK(port != 0);

    auto sock = gorun::net::TcpStream::connect("127.0.0.1", port);
    const std::string raw_req =
        "GET /api/v1/users/1 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    GF_CHECK(sock.write_all(raw_req).ok());

    std::string raw_resp;
    char buf[2048];
    for (;;) {
        auto rd = sock.read(buf, sizeof(buf));
        if (!rd.ok() || rd.eof()) break;
        raw_resp.append(buf, static_cast<std::size_t>(rd.bytes));
    }

    GF_CHECK(raw_resp.find("HTTP/1.1 200 OK") != std::string::npos);
    GF_CHECK(raw_resp.find("\"Artur\"") != std::string::npos);

    auto errs = app.stop();
    GF_CHECK(errs.empty());

    std::cout << "[PASS] Test 5: Live HTTP/1.1 TCP roundtrip via WebApp::start()/stop() (port " << port << ")\n";
}
#endif

int main() {
    std::cout << "=== goforge Stage 6: Ergonomic WebApp, Controllers & Fluent Query AST Tests ===\n";
    test_query_ast_and_shorthand();
    test_query_fluent_operations();
    test_model_reflection_and_sql_ast();
    test_webapp_extractors_and_controllers();
#if !defined(GORUN_DISABLE_NET)
    test_webapp_live_http();
#endif
    std::cout << "=== All Stage 6 tests passed! ===\n";
    return 0;
}
