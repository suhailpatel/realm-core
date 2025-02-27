////////////////////////////////////////////////////////////////////////////
//
// Copyright 2019 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#define CATCH_CONFIG_ENABLE_BENCHMARKING

#include "util/index_helpers.hpp"
#include "util/test_file.hpp"
#include "util/test_utils.hpp"

#include <realm/object-store/impl/object_accessor_impl.hpp>
#include <realm/object-store/impl/realm_coordinator.hpp>
#include <realm/object-store/binding_context.hpp>
#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/property.hpp>
#include <realm/object-store/results.hpp>
#include <realm/object-store/schema.hpp>
#include <realm/object-store/util/scheduler.hpp>

#include <realm/db.hpp>
#include <realm/query_expression.hpp>

#include <memory>
#include <vector>

using namespace realm;
using std::any;

struct TestContext : CppContext {
    std::map<std::string, AnyDict> defaults;

    using CppContext::CppContext;
    TestContext(TestContext& parent, Obj obj, realm::Property const& prop)
        : CppContext(parent, obj, prop)
        , defaults(parent.defaults)
    {
    }

    util::Optional<std::any> default_value_for_property(ObjectSchema const& object, Property const& prop)
    {
        auto obj_it = defaults.find(object.name);
        if (obj_it == defaults.end())
            return util::none;
        auto prop_it = obj_it->second.find(prop.name);
        if (prop_it == obj_it->second.end())
            return util::none;
        return prop_it->second;
    }

    void will_change(Object const&, Property const&) {}
    void did_change() {}
    std::string print(std::any)
    {
        return "not implemented";
    }
    bool allow_missing(std::any)
    {
        return false;
    }
};

TEST_CASE("Benchmark index change calculations", "[benchmark][index]") {
    _impl::CollectionChangeBuilder c;

    auto all_modified = [](ObjKey) {
        return true;
    };
    auto none_modified = [](ObjKey) {
        return false;
    };

    SECTION("reports inserts/deletes for simple reorderings") {
        auto calc = [&](const ObjKeys& old_keys, const ObjKeys& new_keys,
                        util::UniqueFunction<bool(ObjKey)> modifications) {
            return _impl::CollectionChangeBuilder::calculate(old_keys, new_keys, std::move(modifications), false);
        };
        std::vector<int64_t> indices = {};
        constexpr size_t indices_size = 10000;
        indices.reserve(indices_size);
        for (size_t i = 0; i < indices_size; ++i) {
            indices.push_back(i);
        }
        ObjKeys objkeys(indices);

        BENCHMARK("no changes")
        {
            c = calc(objkeys, objkeys, none_modified);
        };
        REQUIRE(c.insertions.empty());
        REQUIRE(c.deletions.empty());

        BENCHMARK("all modified")
        {
            c = calc(objkeys, objkeys, all_modified);
        };
        REQUIRE(c.insertions.empty());
        REQUIRE(c.deletions.empty());

        BENCHMARK("calc 1")
        {
            c = calc({1, 2, 3}, {1, 3, 2}, none_modified);
        };
        REQUIRE_INDICES(c.insertions, 1);
        REQUIRE_INDICES(c.deletions, 2);

        BENCHMARK("calc 2")
        {
            c = calc({1, 2, 3}, {2, 1, 3}, none_modified);
        };
        REQUIRE_INDICES(c.insertions, 0);
        REQUIRE_INDICES(c.deletions, 1);

        BENCHMARK("calc 3")
        {
            c = calc({1, 2, 3}, {2, 3, 1}, none_modified);
        };
        REQUIRE_INDICES(c.insertions, 2);
        REQUIRE_INDICES(c.deletions, 0);

        BENCHMARK("calc 4")
        {
            c = calc({1, 2, 3}, {3, 1, 2}, none_modified);
        };
        REQUIRE_INDICES(c.insertions, 0);
        REQUIRE_INDICES(c.deletions, 2);

        BENCHMARK("calc 5")
        {
            c = calc({1, 2, 3}, {3, 2, 1}, none_modified);
        };
        REQUIRE_INDICES(c.insertions, 0, 1);
        REQUIRE_INDICES(c.deletions, 1, 2);
    }
}

TEST_CASE("Benchmark object", "[benchmark][object]") {
    using namespace std::string_literals;
    using AnyVec = std::vector<std::any>;
    using AnyDict = std::map<std::string, std::any>;
    _impl::RealmCoordinator::assert_no_open_realms();

    InMemoryTestFile config;
    config.automatic_change_notifications = false;
    config.schema = Schema{
        {"all types",
         {
             {"pk", PropertyType::Int, Property::IsPrimary{true}},
             {"bool", PropertyType::Bool},
             {"int", PropertyType::Int},
             {"float", PropertyType::Float},
             {"double", PropertyType::Double},
             {"string", PropertyType::String},
             {"data", PropertyType::Data},
             {"date", PropertyType::Date},
             {"object", PropertyType::Object | PropertyType::Nullable, "link target"},

             {"bool array", PropertyType::Array | PropertyType::Bool},
             {"int array", PropertyType::Array | PropertyType::Int},
             {"float array", PropertyType::Array | PropertyType::Float},
             {"double array", PropertyType::Array | PropertyType::Double},
             {"string array", PropertyType::Array | PropertyType::String},
             {"data array", PropertyType::Array | PropertyType::Data},
             {"date array", PropertyType::Array | PropertyType::Date},
             {"object array", PropertyType::Array | PropertyType::Object, "array target"},
         }},
        {"link target",
         {
             {"value", PropertyType::Int},
         },
         {
             {"origin", PropertyType::LinkingObjects | PropertyType::Array, "all types", "object"},
         }},
        {"array target",
         {
             {"value", PropertyType::Int},
         }},
        {"person",
         {
             {"name", PropertyType::String, Property::IsPrimary{true}},
             {"age", PropertyType::Int},
             {"scores", PropertyType::Array | PropertyType::Int},
             {"assistant", PropertyType::Object | PropertyType::Nullable, "person"},
             {"team", PropertyType::Array | PropertyType::Object, "person"},
         }},
    };

    config.schema_version = 0;
    auto r = Realm::get_shared_realm(config);
    TestContext d(r);

    SECTION("create object") {
        r->begin_transaction();
        ObjectSchema all_types = *r->schema().find("all types");

        int64_t benchmark_pk = 0;
        BENCHMARK("create object")
        {
            return Object::create(d, r, all_types,
                                  std::any(AnyDict{
                                      {"pk", benchmark_pk++},
                                      {"bool", true},
                                      {"int", INT64_C(5)},
                                      {"float", 2.2f},
                                      {"double", 3.3},
                                      {"string", "hello"s},
                                      {"data", "olleh"s},
                                      {"date", Timestamp(10, 20)},
                                      {"object", AnyDict{{"value", INT64_C(10)}}},

                                      {"bool array", AnyVec{true, false}},
                                      {"int array", AnyVec{INT64_C(5), INT64_C(6)}},
                                      {"float array", AnyVec{1.1f, 2.2f}},
                                      {"double array", AnyVec{3.3, 4.4}},
                                      {"string array", AnyVec{"a"s, "b"s, "c"s}},
                                      {"data array", AnyVec{"d"s, "e"s, "f"s}},
                                      {"date array", AnyVec{}},
                                      {"object array", AnyVec{AnyDict{{"value", INT64_C(20)}}}},
                                  }),
                                  CreatePolicy::ForceCreate);
        };
        r->commit_transaction();
    }

    SECTION("update object") {
        auto table = r->read_group().get_table("class_all types");
        r->begin_transaction();
        ObjectSchema all_types = *r->schema().find("all types");
        auto obj = Object::create(d, r, all_types,
                                  std::any(AnyDict{
                                      {"pk", INT64_C(0)},
                                      {"bool", true},
                                      {"int", INT64_C(5)},
                                      {"float", 2.2f},
                                      {"double", 3.3},
                                      {"string", "hello"s},
                                      {"data", "olleh"s},
                                      {"date", Timestamp(10, 20)},
                                      {"object", AnyDict{{"value", INT64_C(10)}}},

                                      {"bool array", AnyVec{true, false}},
                                      {"int array", AnyVec{INT64_C(5), INT64_C(6)}},
                                      {"float array", AnyVec{1.1f, 2.2f}},
                                      {"double array", AnyVec{3.3, 4.4}},
                                      {"string array", AnyVec{"a"s, "b"s, "c"s}},
                                      {"data array", AnyVec{"d"s, "e"s, "f"s}},
                                      {"date array", AnyVec{}},
                                      {"object array", AnyVec{AnyDict{{"value", INT64_C(20)}}}},
                                  }),
                                  CreatePolicy::ForceCreate);
        r->commit_transaction();

        Results result(r, table);
        size_t num_modifications = 0;
        auto token = result.add_notification_callback([&](CollectionChangeSet c) {
            num_modifications += c.modifications.count();
        });

        advance_and_notify(*r);
        int64_t update_int = 1;
        ColKey col_int = table->get_column_key("int");
        REQUIRE(col_int);
        BENCHMARK_ADVANCED("update object")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            meter.measure([&d, &all_types, &update_int, &r] {
                auto shadow = Object::create(d, r, all_types,
                                             std::any(AnyDict{
                                                 {"pk", INT64_C(0)},
                                                 {"int", update_int},
                                             }),
                                             CreatePolicy::UpdateModified);
            });
            r->commit_transaction();
            advance_and_notify(*r);
            REQUIRE(result.size() == 1);
            REQUIRE(result.get(0).get<Int>(col_int) == update_int);
            update_int++;
        };
    }

    SECTION("Update and Read multiple objects") {
        auto table = r->read_group().get_table("class_all types");
        ObjectSchema all_types = *r->schema().find("all types");
        r->begin_transaction();
        std::vector<Object> objs;
        objs.reserve(1000);

        std::random_device dev;
        std::mt19937 rng(dev());
        std::uniform_int_distribution<std::mt19937::result_type> dist(500, 5000);
        int64_t benchmark_pk = dist(rng);
        for (size_t i = 0; i < 1000; ++i) {
            auto obj = Object::create(d, r, all_types,
                                      std::any(AnyDict{
                                          {"pk", benchmark_pk + i},
                                          {"bool", true},
                                          {"int", INT64_C(5)},
                                          {"float", 2.2f},
                                          {"double", 3.3},
                                          {"string", "hello"s},
                                          {"data", "olleh"s},
                                          {"date", Timestamp(10, 20)},
                                          {"object", AnyDict{{"value", INT64_C(10)}}},

                                          {"bool array", AnyVec{true, false}},
                                          {"int array", AnyVec{INT64_C(5), INT64_C(6)}},
                                          {"float array", AnyVec{1.1f, 2.2f}},
                                          {"double array", AnyVec{3.3, 4.4}},
                                          {"string array", AnyVec{"a"s, "b"s, "c"s}},
                                          {"data array", AnyVec{"d"s, "e"s, "f"s}},
                                          {"date array", AnyVec{}},
                                          {"object array", AnyVec{AnyDict{{"value", INT64_C(20)}}}},
                                      }),
                                      CreatePolicy::ForceCreate);
            objs.push_back(obj);
        }
        r->commit_transaction();
        advance_and_notify(*r);
        ColKey col_int = table->get_column_key("int");
        BENCHMARK_ADVANCED("update object get_obj()")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            meter.measure([&objs, &col_int] {
                for (Object& obj : objs) {
                    obj.get_obj().set(col_int, 10);
                    REQUIRE(obj.get_obj().get<Int>(col_int) == 10);
                }
            });
            r->commit_transaction();
        };

        // TODO remove this once Object::obj() is deleted.
        //  BENCHMARK_ADVANCED("update object obj()")(Catch::Benchmark::Chronometer meter)
        //  {
        //      r->begin_transaction();
        //      meter.measure([&objs, &col_int] {
        //          for (Object& obj : objs) {
        //              obj.obj().set(col_int, 10);
        //              REQUIRE(obj.obj().get<Int>(col_int) == 10);
        //          }
        //      });
        //      r->commit_transaction();
        //  };
    }

    SECTION("change notifications reporting") {
        auto table = r->read_group().get_table("class_person");
        Results result(r, table);
        size_t num_calls = 0;
        size_t num_insertions = 0;
        size_t num_deletions = 0;
        size_t num_modifications = 0;
        auto token = result.add_notification_callback([&](CollectionChangeSet c) {
            num_insertions += c.insertions.count();
            num_deletions += c.deletions.count();
            num_modifications += c.modifications_new.count();
            ++num_calls;
        });

        advance_and_notify(*r);
        ObjectSchema person_schema = *r->schema().find("person");
        constexpr size_t num_objects = 1000;

        BENCHMARK_ADVANCED("create notifications")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            result.clear();
            r->commit_transaction();
            advance_and_notify(*r);
            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            r->begin_transaction();
            for (size_t i = 0; i < num_objects; ++i) {
                auto name = util::format("person_", i);
                AnyDict person{
                    {"name", name},
                    {"age", static_cast<int64_t>(i)},
                };
                Object::create(d, r, person_schema, util::Any(person), CreatePolicy::ForceCreate);
            }
            r->commit_transaction();
            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == num_objects);
            REQUIRE(num_modifications == 0);
            REQUIRE(num_deletions == 0);
            REQUIRE(result.size() == num_objects);
        };

        r->begin_transaction();
        result.clear();
        r->commit_transaction();
        advance_and_notify(*r);
        num_calls = 0;

        BENCHMARK_ADVANCED("delete notifications")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            result.clear();
            r->commit_transaction();
            advance_and_notify(*r);
            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            r->begin_transaction();
            for (size_t i = 0; i < num_objects; ++i) {
                auto name = util::format("person_", i);
                AnyDict person{
                    {"name", name},
                    {"age", static_cast<int64_t>(i)},
                };
                Object::create(d, r, person_schema, util::Any(person), CreatePolicy::ForceCreate);
            }
            r->commit_transaction();
            advance_and_notify(*r);
            REQUIRE(num_insertions == num_objects);
            REQUIRE(num_modifications == 0);
            REQUIRE(num_deletions == 0);
            REQUIRE(result.size() == num_objects);

            r->begin_transaction();
            result.clear();
            r->commit_transaction();

            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == num_objects);
            REQUIRE(num_modifications == 0);
            REQUIRE(num_deletions == num_objects);
            REQUIRE(result.size() == 0);
        };

        BENCHMARK_ADVANCED("modify notifications")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            result.clear();
            r->commit_transaction();
            advance_and_notify(*r);
            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            r->begin_transaction();
            for (size_t i = 0; i < num_objects; ++i) {
                auto name = util::format("person_", i);
                AnyDict person{
                    {"name", name},
                    {"age", static_cast<int64_t>(i)},
                };
                Object::create(d, r, person_schema, util::Any(person), CreatePolicy::ForceCreate);
            }
            r->commit_transaction();
            advance_and_notify(*r);
            REQUIRE(result.size() == num_objects);
            REQUIRE(num_insertions == num_objects);
            REQUIRE(num_modifications == 0);
            REQUIRE(num_deletions == 0);
            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            r->begin_transaction();
            for (size_t i = 0; i < num_objects; ++i) {
                auto name = util::format("person_", i);
                AnyDict person{
                    {"name", name}, {"age", static_cast<int64_t>(i + 1)}, // age differs
                };
                Object::create(d, r, person_schema, util::Any(person), CreatePolicy::UpdateModified);
            }
            r->commit_transaction();

            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == 0);
            REQUIRE(num_modifications == num_objects);
            REQUIRE(num_deletions == 0);
            REQUIRE(result.size() == num_objects);
        };
    }

    SECTION("merging notifications from different versions") {
        advance_and_notify(*r);
        ObjectSchema schema = *r->schema().find("all types");

        r->begin_transaction();
        AnyDict values{
            {"pk", INT64_C(0)},
            {"bool", true},
            {"int", INT64_C(5)},
            {"float", 2.2f},
            {"double", 3.3},
            {"string", "hello"s},
            {"data", "olleh"s},
            {"date", Timestamp(10, 20)},
            {"object", AnyDict{{"value", INT64_C(10)}}},

            {"bool array", AnyVec{true, false}},
            {"int array", AnyVec{INT64_C(5), INT64_C(6)}},
            {"float array", AnyVec{1.1f, 2.2f}},
            {"double array", AnyVec{3.3, 4.4}},
            {"string array", AnyVec{"a"s, "b"s, "c"s}},
            {"data array", AnyVec{"d"s, "e"s, "f"s}},
            {"date array", AnyVec{}},
            {"object array", AnyVec{AnyDict{{"value", INT64_C(20)}}}},
        };
        Object obj = Object::create(d, r, schema, util::Any(values), CreatePolicy::ForceCreate);
        r->commit_transaction();
        advance_and_notify(*r);

        BENCHMARK_ADVANCED("object modify notifications")(Catch::Benchmark::Chronometer meter)
        {
            struct CallbackState {
                Object obj;
                NotificationToken token;
                size_t num_insertions = 0;
                size_t num_deletions = 0;
                size_t num_modifications = 0;
                size_t num_calls = 0;
            };
            std::vector<CallbackState> notifiers;
            auto get_object = [&] {
                auto r = Realm::get_shared_realm(config);
                auto obj = r->read_group().get_table("class_all types")->get_object(0);
                return Object(r, obj);
            };
            auto change_object = [&] {
                r->begin_transaction();
                int64_t int_value = obj.get_column_value<int64_t>("int");
                obj.set_column_value("int", int_value + 1);
                obj.set_column_value("bool", !obj.get_column_value<bool>("bool"));
                obj.set_column_value("float", obj.get_column_value<float>("float") + 1);
                obj.set_column_value("double", obj.get_column_value<double>("double") + 1);
                obj.set_column_value("string", int_value % 2 == 0 ? "even"s : "odd"s);
                Timestamp ts = obj.get_column_value<Timestamp>("date");
                obj.set_column_value("date", Timestamp{ts.get_seconds(), ts.get_nanoseconds() + 1});
                r->commit_transaction();
            };

            notifiers.clear();
            constexpr size_t num_modifications = 300;
            for (size_t i = 0; i < num_modifications; ++i) {
                Object o = get_object();
                auto token = o.add_notification_callback([&notifiers, i](CollectionChangeSet c) {
                    notifiers[i].num_insertions += c.insertions.count();
                    notifiers[i].num_modifications += c.modifications.count();
                    notifiers[i].num_deletions += c.deletions.count();
                    notifiers[i].num_calls++;
                });
                notifiers.push_back({std::move(o), std::move(token), 0, 0, 0, 0});
                change_object();
            }

            REQUIRE(std::all_of(notifiers.begin(), notifiers.end(), [](auto& it) {
                return it.num_calls == 0 && it.num_modifications == 0;
            }));

            // Each of the Objects now has a different source version and state at
            // that version, so they should all see different changes despite
            // being for the same Object
            meter.measure([&] {
                for (auto& notifier : notifiers)
                    advance_and_notify(*notifier.obj.get_realm());
            });

            REQUIRE(std::all_of(notifiers.begin(), notifiers.end(), [](auto& it) {
                return it.num_calls == 1 && it.num_modifications == 1;
            }));

            // After making another change, they should all get the same notification
            change_object();
            for (auto& notifier : notifiers)
                advance_and_notify(*notifier.obj.get_realm());

            REQUIRE(std::all_of(notifiers.begin(), notifiers.end(), [](auto& it) {
                return it.num_calls == 2 && it.num_modifications == 2;
            }));
        };
    }

    SECTION("change notifications sorted") {
        auto table = r->read_group().get_table("class_person");
        auto age_col = table->get_column_key("age");
        Results result = Results(r, table).sort({{"age", true}});
        size_t num_insertions = 0;
        size_t num_deletions = 0;
        size_t num_modifications = 0;
        auto token = result.add_notification_callback([&](CollectionChangeSet c) {
            num_insertions += c.insertions.count();
            num_deletions += c.deletions.count();
            num_modifications += c.modifications_new.count();
        });

        advance_and_notify(*r);
        ObjectSchema person_schema = *r->schema().find("person");
        auto add_objects = [&](size_t num_objects, size_t start_index = 0) {
            r->begin_transaction();
            for (size_t i = 0; i < num_objects; ++i) {
                size_t index = i + start_index;
                auto name = util::format("person_", index);
                AnyDict person{
                    {"name", name},
                    {"age", static_cast<int64_t>(index)},
                };
                Object::create(d, r, person_schema, util::Any(person), CreatePolicy::ForceCreate);
            }
            r->commit_transaction();
        };

        BENCHMARK_ADVANCED("prepend insertions")(Catch::Benchmark::Chronometer meter)
        {
            constexpr size_t num_initial_objects = 1000;
            constexpr size_t num_prepend_objects = 1000;
            r->begin_transaction();
            result.clear();
            r->commit_transaction();
            advance_and_notify(*r);
            add_objects(num_initial_objects, num_prepend_objects);
            advance_and_notify(*r);

            add_objects(num_prepend_objects, 0);

            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == num_prepend_objects);
            REQUIRE(num_modifications == 0);
            REQUIRE(num_deletions == 0);
            REQUIRE(result.size() == num_prepend_objects + num_initial_objects);
            REQUIRE(result.get(0).get<int64_t>(age_col) == 0);
            REQUIRE(result.get(result.size() - 1).get<int64_t>(age_col) ==
                    num_prepend_objects + num_initial_objects - 1);
        };

        BENCHMARK_ADVANCED("insert, delete odds")(Catch::Benchmark::Chronometer meter)
        {
            constexpr size_t num_objects = 800;
            r->begin_transaction();
            result.clear();
            r->commit_transaction();
            advance_and_notify(*r);

            // insert
            add_objects(num_objects);
            advance_and_notify(*r);

            // remove odds
            r->begin_transaction();
            for (size_t i = result.size() - 1; i > 0; --i) {
                if (i % 2 == 1) {
                    Obj odd = result.get(i);
                    odd.remove();
                }
            }
            r->commit_transaction();

            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == 0);
            REQUIRE(num_modifications == 0);
            REQUIRE(num_deletions == num_objects / 2);
            REQUIRE(result.size() == num_objects / 2);
            REQUIRE(result.get(0).get<int64_t>(age_col) == 0);
            REQUIRE(result.get(1).get<int64_t>(age_col) == 2);
        };

        constexpr size_t num_objects = 1000;
        r->begin_transaction();
        result.clear();
        r->commit_transaction();
        advance_and_notify(*r);
        add_objects(num_objects);
        advance_and_notify(*r);

        BENCHMARK_ADVANCED("modify all")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            for (size_t i = 0; i < table->size(); ++i) {
                Obj obj = table->get_object(i);
                obj.set(age_col, obj.get<int64_t>(age_col) + 1);
            }
            r->commit_transaction();

            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == 0);
            REQUIRE(num_modifications == num_objects);
            REQUIRE(num_deletions == 0);
            REQUIRE(result.size() == num_objects);
        };

        BENCHMARK_ADVANCED("modify odds")(Catch::Benchmark::Chronometer meter)
        {
            r->begin_transaction();
            result.clear();
            r->commit_transaction();
            advance_and_notify(*r);

            r->begin_transaction();
            for (size_t i = 0; i < num_objects; ++i) {
                auto name = util::format("person_", i);
                AnyDict person{
                    {"name", name},
                    {"age", static_cast<int64_t>(i * 2)},
                };
                Object::create(d, r, person_schema, util::Any(person), CreatePolicy::ForceCreate);
            }
            r->commit_transaction();

            advance_and_notify(*r);

            r->begin_transaction();
            for (size_t i = 0; i < table->size(); ++i) {
                Obj obj = table->get_object(i);
                int64_t age = obj.get<int64_t>(age_col);
                if ((age >> 1) % 2 == 1) {
                    obj.set(age_col, age - 1);
                }
            }
            r->commit_transaction();

            num_insertions = 0;
            num_modifications = 0;
            num_deletions = 0;

            meter.measure([&r] {
                on_change_but_no_notify(*r);
            });
            r->notify();
            REQUIRE(num_insertions == 0);
            REQUIRE(num_modifications == num_objects / 2);
            REQUIRE(num_deletions == 0);
            REQUIRE(result.size() == num_objects);
        };
    }
}

TEST_CASE("Benchmark object notification delivery", "[benchmark][notifications]") {
    _impl::RealmCoordinator::assert_no_open_realms();

    InMemoryTestFile config;
    config.automatic_change_notifications = false;
    config.schema = Schema{{"object", {{"value", PropertyType::Int}}}};
    config.cache = false;
    auto r = Realm::get_shared_realm(config);

    r->begin_transaction();
    auto obj = r->read_group().get_table("class_object")->create_object();
    r->commit_transaction();

    Object object(r, obj);
    auto token = object.add_notification_callback([&](auto) {});

    BENCHMARK("refresh after minimal change") {
        constexpr const int end_value = 1000;
        auto r2 = Realm::get_shared_realm(config);
        auto obj2 = *r2->read_group().get_table("class_object")->begin();
        for (int i = 0; i < end_value; ++i) {
            r2->begin_transaction();
            obj2.set<int64_t>("value", i);
            r2->commit_transaction();
            r->refresh();
        }
    };
}
