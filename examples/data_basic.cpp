// SPDX-License-Identifier: GPL-2.0-only
// flyG example: data module - formats, serialization, document database
#include <flyg/flyg.hpp>

#include <iostream>

using namespace flyg;
using namespace flyg::data;

struct Config {
    std::string name;
    int port = 0;
    std::vector<std::string> hosts;
};
FLYG_FIELDS(Config, name, port, hosts)

int main() {
    const Value v = json::parse(R"({"name":"demo","port":8080,"hosts":["a","b"]})");
    std::cout << yaml::emit(v) << toml::write(v) << xml::write(xml::from_value("config", v)) << "\n";

    const Config cfg = from_value<Config>(v);
    std::cout << json::stringify(to_value(cfg)) << "\n";
    const auto bin = serialize(cfg, Format::binary);
    std::cout << "binary size: " << bin.size() << "\n";

    Database db;  // in-memory
    auto users = db.collection("users");
    users.create_index("age");
    users.insert(json::parse(R"({"name":"ann","age":31})"));
    users.insert(json::parse(R"({"name":"bob","age":25})"));
    FindOptions o;
    o.sort = {{"name", -1}};
    for (const auto& d : users.find(json::parse(R"({"age":{"$gte":25}})"), o)) std::cout << json::stringify(d) << "\n";
    std::cout << json::stringify(users.explain(json::parse(R"({"age":{"$gte":25}})"))) << "\n";
}
