////////////////////////////////////////////////////////////////////////////
//
// Copyright 2017 Realm Inc.
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

#include <catch2/catch_all.hpp>

#include "sync/session/session_util.hpp"

#include <realm/object-store/feature_checks.hpp>
#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/object_store.hpp>
#include <realm/object-store/property.hpp>
#include <realm/object-store/schema.hpp>

#include "util/event_loop.hpp"
#include "util/test_utils.hpp"

#include <realm/util/time.hpp>
#include <realm/util/scope_exit.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace realm;
using namespace realm::util;

static const std::string dummy_auth_url = "https://realm.example.org";
static const std::string dummy_device_id = "123400000000000000000000";

TEST_CASE("SyncSession: management by SyncUser", "[sync][session]") {
    if (!EventLoop::has_implementation())
        return;

    TestSyncManager init_sync_manager;
    auto& server = init_sync_manager.sync_server();
    auto app = init_sync_manager.app();
    const std::string realm_base_url = server.base_url();

    SECTION("a SyncUser can properly retrieve its owned sessions") {
        std::string path_1;
        std::string path_2;
        auto user =
            app->sync_manager()->get_user("user1a", ENCODE_FAKE_JWT("fake_refresh_token"),
                                          ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        auto session1 = sync_session(
            user, "/test1a-1", [](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded, &path_1);
        auto session2 = sync_session(
            user, "/test1a-2", [](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded, &path_2);
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session1, *session2);
        });

        // Check the sessions on the SyncUser.
        REQUIRE(user->all_sessions().size() == 2);
        auto s1 = user->session_for_on_disk_path(path_1);
        REQUIRE(s1);
        CHECK(s1->config().partition_value == "/test1a-1");
        auto s2 = user->session_for_on_disk_path(path_2);
        REQUIRE(s2);
        CHECK(s2->config().partition_value == "/test1a-2");
    }

    SECTION("a SyncUser properly unbinds its sessions upon logging out") {
        auto user =
            app->sync_manager()->get_user("user1b", ENCODE_FAKE_JWT("fake_refresh_token"),
                                          ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        auto session1 = sync_session(user, "/test1b-1", [](auto, auto) {});
        auto session2 = sync_session(user, "/test1b-2", [](auto, auto) {});
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session1, *session2);
        });

        // Log the user out.
        user->log_out();
        // The sessions should log themselves out.
        EventLoop::main().run_until([&] {
            return sessions_are_inactive(*session1, *session2);
        });
        CHECK(user->all_sessions().size() == 0);
    }

    SECTION("a SyncUser defers binding new sessions until it is logged in") {
        const std::string user_id = "user1c";
        auto user =
            app->sync_manager()->get_user(user_id, ENCODE_FAKE_JWT("fake_refresh_token"),
                                          ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        user->log_out();
        REQUIRE(user->state() == SyncUser::State::LoggedOut);
        auto session1 = sync_session(user, "/test1c-1", [](auto, auto) {});
        auto session2 = sync_session(user, "/test1c-2", [](auto, auto) {});
        // Run the runloop many iterations to see if the sessions spuriously bind.
        spin_runloop();
        REQUIRE(session1->state() == SyncSession::State::Inactive);
        REQUIRE(session2->state() == SyncSession::State::Inactive);
        REQUIRE(user->all_sessions().size() == 0);
        // Log the user back in via the sync manager.
        user = app->sync_manager()->get_user(user_id, ENCODE_FAKE_JWT("fake_refresh_token"),
                                             ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session1, *session2);
        });
        REQUIRE(user->all_sessions().size() == 2);
    }

    SECTION("a SyncUser properly rebinds existing sessions upon logging back in") {
        const std::string user_id = "user1d";
        auto user =
            app->sync_manager()->get_user(user_id, ENCODE_FAKE_JWT("fake_refresh_token"),
                                          ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        auto session1 = sync_session(user, "/test1d-1", [](auto, auto) {});
        auto session2 = sync_session(user, "/test1d-2", [](auto, auto) {});
        // Make sure the sessions are bound.
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session1, *session2);
        });
        REQUIRE(user->all_sessions().size() == 2);
        // Log the user out.
        user->log_out();
        REQUIRE(user->state() == SyncUser::State::LoggedOut);
        // Run the runloop many iterations to see if the sessions spuriously rebind.
        spin_runloop();
        REQUIRE(session1->state() == SyncSession::State::Inactive);
        REQUIRE(session2->state() == SyncSession::State::Inactive);
        REQUIRE(user->all_sessions().size() == 0);
        // Log the user back in via the sync manager.
        user = app->sync_manager()->get_user(user_id, ENCODE_FAKE_JWT("fake_refresh_token"),
                                             ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session1, *session2);
        });
        REQUIRE(user->all_sessions().size() == 2);
    }

    SECTION("sessions that were destroyed can be properly recreated when requested again") {
        const std::string path = "/test1e";
        std::weak_ptr<SyncSession> weak_session;
        std::string on_disk_path;
        util::Optional<SyncConfig> config;
        auto user =
            app->sync_manager()->get_user("user1e", ENCODE_FAKE_JWT("fake_refresh_token"),
                                          ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        {
            // Create the session within a nested scope, so we can control its lifetime.
            auto session = sync_session(
                user, path, [](auto, auto) {}, SyncSessionStopPolicy::Immediately, &on_disk_path);
            weak_session = session;
            config = session->config();
            REQUIRE(on_disk_path.size() > 0);
            REQUIRE(weak_session.lock());
        }
        // Wait for the session to die. It may not happen immediately if a progress or error handler
        // is called on a background thread and keeps the session alive past the scope of the above block.
        EventLoop::main().run_until([&] {
            return weak_session.expired();
        });

        // The next time we request it, it'll be created anew.
        // The call to `get_session()` should result in `SyncUser::register_session()` being called.
        auto session = sync_session(
            user, path, [](auto, auto) {}, SyncSessionStopPolicy::Immediately, &on_disk_path);
        CHECK(session);
        session = user->session_for_on_disk_path(on_disk_path);
        CHECK(session);
    }

    SECTION("a user can create multiple sessions for the same URL") {
        auto user =
            app->sync_manager()->get_user("user", ENCODE_FAKE_JWT("fake_refresh_token"),
                                          ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
        auto create_session = [&]() {
            // Note that this should put the sessions at different paths.
            return sync_session(
                user, "/test", [](auto, auto) {}, SyncSessionStopPolicy::Immediately);
        };
        REQUIRE(create_session());
        REQUIRE(create_session());
    }
}

TEST_CASE("sync: log-in", "[sync][session]") {
    if (!EventLoop::has_implementation())
        return;

    // Disable file-related functionality and metadata functionality for testing purposes.
    TestSyncManager init_sync_manager;
    auto app = init_sync_manager.app();
    auto user = app->sync_manager()->get_user("user", ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);

    SECTION("Can log in") {
        std::atomic<int> error_count(0);
        auto session = sync_session(user, "/test", [&](auto, auto) {
            ++error_count;
        });

        std::atomic<bool> download_did_complete(false);
        session->wait_for_download_completion([&](auto) {
            download_did_complete = true;
        });
        EventLoop::main().run_until([&] {
            return download_did_complete.load() || error_count > 0;
        });
        CHECK(error_count == 0);
    }

    // TODO: write a test that logs out a Realm with multiple sessions, then logs it back in?
    // TODO: write tests that check that a Session properly handles various types of errors reported via its callback.
}

TEST_CASE("SyncSession: close() API", "[sync][session]") {
    TestSyncManager init_sync_manager;
    auto app = init_sync_manager.app();
    auto user = app->sync_manager()->get_user("close-api-tests-user", ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), "https://realm.example.org",
                                              dummy_device_id);

    SECTION("Behaves properly when called on session in the 'active' or 'inactive' state") {
        auto session = sync_session(
            user, "/test-close-for-active", [](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded);
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session);
        });
        REQUIRE(sessions_are_active(*session));
        session->close();
        EventLoop::main().run_until([&] {
            return sessions_are_inactive(*session);
        });
        REQUIRE(sessions_are_inactive(*session));
        // Try closing the session again. This should be a no-op.
        session->close();
        REQUIRE(sessions_are_inactive(*session));
    }

    SECTION("Close session after it was detached from the SyncManager") {
        auto session = sync_session(
            user, "/test-close-after-detach", [](auto, auto) {}, SyncSessionStopPolicy::Immediately);
        session->detach_from_sync_manager();
        REQUIRE_NOTHROW(session->close());
    }
}

TEST_CASE("SyncSession: pause()/resume() API", "[sync][session]") {
    TestSyncManager init_sync_manager;
    auto app = init_sync_manager.app();
    auto user = app->sync_manager()->get_user("close-api-tests-user", ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), "https://realm.example.org",
                                              dummy_device_id);

    auto session = sync_session(
        user, "/test-close-for-active", [](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded);
    EventLoop::main().run_until([&] {
        return sessions_are_active(*session);
    });
    REQUIRE(sessions_are_active(*session));

    SECTION("making the session inactive and then pausing it should end up in the paused state") {
        session->force_close();
        EventLoop::main().run_until([&] {
            return sessions_are_inactive(*session);
        });
        REQUIRE(sessions_are_inactive(*session));

        session->pause();
        EventLoop::main().run_until([&] {
            return session->state() == SyncSession::State::Paused;
        });
        REQUIRE(session->state() == SyncSession::State::Paused);
    }

    SECTION("pausing from the active state should end up in the paused state") {
        session->pause();
        EventLoop::main().run_until([&] {
            return session->state() == SyncSession::State::Paused;
        });
        REQUIRE(session->state() == SyncSession::State::Paused);

        // Pausing it again should be a no-op
        session->pause();
        REQUIRE(session->state() == SyncSession::State::Paused);

        // "Logging out" the session should be a no-op.
        session->force_close();
        REQUIRE(session->state() == SyncSession::State::Paused);
    }

    // Reviving the session via revive_if_needed() should be a no-op.
    session->revive_if_needed();
    REQUIRE(session->state() == SyncSession::State::Paused);

    // Only resume() can revive a paused session.
    session->resume();
    EventLoop::main().run_until([&] {
        return sessions_are_active(*session);
    });
    REQUIRE(sessions_are_active(*session));
}

TEST_CASE("SyncSession: shutdown_and_wait() API", "[sync][session]") {
    TestSyncManager init_sync_manager;
    auto app = init_sync_manager.app();
    auto user = app->sync_manager()->get_user("close-api-tests-user", ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), "https://realm.example.org",
                                              dummy_device_id);

    SECTION("Behaves properly when called on session in the 'active' or 'inactive' state") {
        auto session = sync_session(
            user, "/test-close-for-active", [](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded);
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session);
        });
        REQUIRE(sessions_are_active(*session));
        session->shutdown_and_wait();
        session->close();
        EventLoop::main().run_until([&] {
            return sessions_are_inactive(*session);
        });
        REQUIRE(sessions_are_inactive(*session));
        // Try closing the session again. This should be a no-op.
        session->close();
        REQUIRE(sessions_are_inactive(*session));
    }
}

TEST_CASE("SyncSession: update_configuration()", "[sync][session]") {
    TestSyncManager init_sync_manager({}, {false});
    auto app = init_sync_manager.app();
    auto user = app->sync_manager()->get_user("userid", ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
    auto session = sync_session(
        user, "/update_configuration", [](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded);

    SECTION("updates reported configuration") {
        auto config = session->config();
        REQUIRE(config.client_validate_ssl);
        config.client_validate_ssl = false;
        session->update_configuration(std::move(config));
        REQUIRE_FALSE(session->config().client_validate_ssl);
    }

    SECTION("handles reconnects while it's trying to deactivate session") {
        bool wait_called = false;
        session->wait_for_download_completion([&](Status s) {
            REQUIRE(s == ErrorCodes::OperationAborted);
            REQUIRE(session->config().client_validate_ssl);
            REQUIRE(session->state() == SyncSession::State::Inactive);

            wait_called = true;
            session->revive_if_needed();

            REQUIRE(session->state() != SyncSession::State::Inactive);
        });

        auto config = session->config();
        config.client_validate_ssl = false;
        session->update_configuration(std::move(config));
        REQUIRE(wait_called);
    }
}

TEST_CASE("sync: error handling", "[sync][session]") {
    using ProtocolError = realm::sync::ProtocolError;
    using ProtocolErrorInfo = realm::sync::ProtocolErrorInfo;
    TestSyncManager init_sync_manager;
    auto app = init_sync_manager.app();
    // Create a valid session.
    std::function<void(std::shared_ptr<SyncSession>, SyncError)> error_handler = [](auto, SyncError err) {
        REQUIRE(err.server_requests_action == ProtocolErrorInfo::Action::Transient);
    };
    const std::string user_id = "user1d";
    std::string on_disk_path;
    auto user = app->sync_manager()->get_user(user_id, ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), "https://realm.example.org",
                                              dummy_device_id);
    auto session = sync_session(
        user, "/test1e",
        [&](auto session, SyncError error) {
            error_handler(std::move(session), std::move(error));
        },
        SyncSessionStopPolicy::AfterChangesUploaded, &on_disk_path);
    // Make sure the sessions are bound.
    EventLoop::main().run_until([&] {
        return sessions_are_active(*session);
    });

    SECTION("Doesn't treat unknown system errors as being fatal") {
        std::error_code code = std::error_code{EBADF, std::generic_category()};
        sync::SessionErrorInfo err{code, "Not a real error message", true};
        err.server_requests_action = ProtocolErrorInfo::Action::Transient;
        SyncSession::OnlyForTesting::handle_error(*session, std::move(err));
        CHECK(!sessions_are_inactive(*session));
    }

    SECTION("Properly handles a client reset error") {
        int code = 0;
        util::Optional<SyncError> final_error;
        error_handler = [&](auto, SyncError error) {
            final_error = std::move(error);
        };

        SECTION("for bad_server_file_ident") {
            code = static_cast<int>(ProtocolError::bad_server_file_ident);
        }

        SECTION("for bad_client_file_ident") {
            code = static_cast<int>(ProtocolError::bad_client_file_ident);
        }

        SECTION("for bad_server_version") {
            code = static_cast<int>(ProtocolError::bad_server_version);
        }

        SECTION("for diverging_histories") {
            code = static_cast<int>(ProtocolError::diverging_histories);
        }

        sync::SessionErrorInfo initial_error{std::error_code{code, realm::sync::protocol_error_category()},
                                             "Something bad happened", true};
        initial_error.server_requests_action = ProtocolErrorInfo::Action::ClientReset;
        std::time_t just_before_raw = std::time(nullptr);
        SyncSession::OnlyForTesting::handle_error(*session, std::move(initial_error));
        REQUIRE(session->state() == SyncSession::State::Inactive);
        std::time_t just_after_raw = std::time(nullptr);
        auto just_before = util::localtime(just_before_raw);
        auto just_after = util::localtime(just_after_raw);
        // At this point final_error should be populated.
        CHECK(bool(final_error));
        CHECK(final_error->is_client_reset_requested());
        CHECK(final_error->server_requests_action == ProtocolErrorInfo::Action::ClientReset);
        // The original file path should be present.
        CHECK(final_error->user_info[SyncError::c_original_file_path_key] == on_disk_path);
        // The path to the recovery file should be present, and should contain all necessary components.
        std::string recovery_path = final_error->user_info[SyncError::c_recovery_file_path_key];
        auto idx = recovery_path.find("recovered_realm");
        CHECK(idx != std::string::npos);
        idx = recovery_path.find(app->sync_manager()->recovery_directory_path());
        CHECK(idx != std::string::npos);
        if (just_before.tm_year == just_after.tm_year) {
            idx = recovery_path.find(util::format_local_time(just_after_raw, "%Y"));
            CHECK(idx != std::string::npos);
        }
        if (just_before.tm_mon == just_after.tm_mon) {
            idx = recovery_path.find(util::format_local_time(just_after_raw, "%m"));
            CHECK(idx != std::string::npos);
        }
        if (just_before.tm_yday == just_after.tm_yday) {
            idx = recovery_path.find(util::format_local_time(just_after_raw, "%d"));
            CHECK(idx != std::string::npos);
        }
    }
}

struct RegularUser {
    static auto user(std::shared_ptr<SyncManager> sync_manager)
    {
        return sync_manager->get_user("user-dying-state", ENCODE_FAKE_JWT("fake_refresh_token"),
                                      ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
    }
};

TEMPLATE_TEST_CASE("sync: stop policy behavior", "[sync][session]", RegularUser)
{
    using ProtocolError = realm::sync::ProtocolError;
    const std::string dummy_auth_url = "https://realm.example.org";
    if (!EventLoop::has_implementation())
        return;

    // Server is initially stopped so we can control when the session exits the dying state.
    TestSyncManager init_sync_manager({}, {false});
    auto& server = init_sync_manager.sync_server();
    auto sync_manager = init_sync_manager.app()->sync_manager();
    auto schema = Schema{
        {"object",
         {
             {"_id", PropertyType::Int, Property::IsPrimary{true}},
             {"value", PropertyType::Int},
         }},
    };

    std::atomic<bool> error_handler_invoked(false);
    Realm::Config config;
    auto user = TestType::user(sync_manager);

    auto create_session = [&](SyncSessionStopPolicy stop_policy) {
        auto session = sync_session(
            user, "/test-dying-state",
            [&](auto, auto) {
                error_handler_invoked = true;
            },
            stop_policy, nullptr, schema, &config);
        EventLoop::main().run_until([&] {
            return sessions_are_active(*session);
        });

        // Add an object so there's something to upload
        auto r = Realm::get_shared_realm(config);
        TableRef table = ObjectStore::table_for_object_type(r->read_group(), "object");
        r->begin_transaction();
        table->create_object_with_primary_key(0);
        r->commit_transaction();

        return session;
    };

    SECTION("Immediately") {
        SECTION("transitions directly to Inactive even with the server stopped") {
            auto session = create_session(SyncSessionStopPolicy::Immediately);
            session->close();
            REQUIRE(sessions_are_inactive(*session));
        }
    }

    SECTION("AfterChangesUploaded") {
        auto session = create_session(SyncSessionStopPolicy::AfterChangesUploaded);
        // Now close the session, causing the state to transition to Dying.
        // (it should remain stuck there until we start the server)
        session->close();
        REQUIRE(session->state() == SyncSession::State::Dying);

        SECTION("transitions to Inactive once the server is started") {
            server.start();
            EventLoop::main().run_until([&] {
                return sessions_are_inactive(*session);
            });
        }

        SECTION("transitions back to Active if the session is revived") {
            std::shared_ptr<SyncSession> session2;
            {
                auto realm = Realm::get_shared_realm(config);
                session2 = user->sync_manager()->get_existing_session(config.path);
            }
            REQUIRE(session->state() == SyncSession::State::Active);
            REQUIRE(session2 == session);
        }

        SECTION("transitions to Inactive if a fatal error occurs") {
            std::error_code code =
                std::error_code{static_cast<int>(ProtocolError::bad_syntax), realm::sync::protocol_error_category()};
            sync::SessionErrorInfo err{code, "Not a real error message", false};
            err.server_requests_action = realm::sync::ProtocolErrorInfo::Action::ProtocolViolation;
            SyncSession::OnlyForTesting::handle_error(*session, std::move(err));
            CHECK(sessions_are_inactive(*session));
            // The session shouldn't report fatal errors when in the dying state.
            CHECK(!error_handler_invoked);
        }

        SECTION("ignores non-fatal errors and does not transition to Inactive") {
            // Fire a simulated *non-fatal* error.
            std::error_code code =
                std::error_code{static_cast<int>(ProtocolError::other_error), realm::sync::protocol_error_category()};
            sync::SessionErrorInfo err{code, "Not a real error message", true};
            err.server_requests_action = realm::sync::ProtocolErrorInfo::Action::Transient;
            SyncSession::OnlyForTesting::handle_error(*session, std::move(err));
            REQUIRE(session->state() == SyncSession::State::Dying);
            CHECK(!error_handler_invoked);
        }
    }

    SECTION("can change to Immediately after opening the session") {
        auto session = create_session(SyncSessionStopPolicy::AfterChangesUploaded);
        REQUIRE(session->state() == SyncSession::State::Active);

        auto config = session->config();
        config.stop_policy = SyncSessionStopPolicy::Immediately;
        session->update_configuration(std::move(config));

        session->close();
        REQUIRE(sessions_are_inactive(*session));
    }
}

TEST_CASE("session restart", "[sync][session]") {
    if (!EventLoop::has_implementation())
        return;

    TestSyncManager init_sync_manager({}, {false});
    auto& server = init_sync_manager.sync_server();
    auto app = init_sync_manager.app();
    Realm::Config config;
    auto schema = Schema{
        {"object",
         {
             {"_id", PropertyType::Int, Property::IsPrimary{true}},
             {"value", PropertyType::Int},
         }},
    };

    auto user = app->sync_manager()->get_user("userid", ENCODE_FAKE_JWT("fake_refresh_token"),
                                              ENCODE_FAKE_JWT("fake_access_token"), dummy_auth_url, dummy_device_id);
    auto session = sync_session(
        user, "/test-restart", [&](auto, auto) {}, SyncSessionStopPolicy::AfterChangesUploaded, nullptr, schema,
        &config);

    EventLoop::main().run_until([&] {
        return sessions_are_active(*session);
    });

    server.start();

    // Add an object so there's something to upload
    auto realm = Realm::get_shared_realm(config);
    TableRef table = ObjectStore::table_for_object_type(realm->read_group(), "object");
    realm->begin_transaction();
    table->create_object_with_primary_key(0);
    realm->commit_transaction();

    // Close the current session and start a new one
    // The stop policy is ignored when closing the current session
    session->restart_session();

    REQUIRE(session->state() == SyncSession::State::Active);
    REQUIRE(!wait_for_upload(*realm));
}

TEST_CASE("sync: non-synced metadata table doesn't result in non-additive schema changes", "[sync][session]") {
    if (!EventLoop::has_implementation())
        return;

    // Disable file-related functionality and metadata functionality for testing purposes.
    TestSyncManager init_sync_manager;

    // Create a synced Realm containing a class with two properties.
    {
        SyncTestFile config1(init_sync_manager.app(), "schema-version-test");
        config1.schema_version = 1;
        config1.schema = Schema{
            {"object",
             {{"_id", PropertyType::Int, Property::IsPrimary{true}},
              {"property1", PropertyType::Int},
              {"property2", PropertyType::Int}}},
        };

        auto realm1 = Realm::get_shared_realm(config1);
        wait_for_upload(*realm1);
    }

    // Download the existing Realm into a second local file without specifying a schema,
    // mirroring how `openAsync` works.
    SyncTestFile config2(init_sync_manager.app(), "schema-version-test");
    config2.schema_version = 1;
    {
        auto realm2 = Realm::get_shared_realm(config2);
        wait_for_download(*realm2);
    }

    // Open the just-downloaded Realm while specifying a schema that contains a class with
    // only a single property. This should not result in us trying to remove `property2`,
    // and will throw an exception if it does.
    {
        SyncTestFile config3(init_sync_manager.app(), "schema-version-test");
        config3.path = config2.path;
        config3.schema_version = 1;
        config3.schema = Schema{
            {"object", {{"_id", PropertyType::Int, Property::IsPrimary{true}}, {"property1", PropertyType::Int}}},
        };

        auto realm3 = Realm::get_shared_realm(config3);
    }
}
