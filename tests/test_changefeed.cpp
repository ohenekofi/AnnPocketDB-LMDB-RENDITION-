// Coverage for changefeed::DurableChangeFeed/ChangeSink/ChangeEvent
// (previously untested): durable poll-based consumption of Insert/Update/
// Delete events in commit order, resume-token semantics (pollFrom(afterToken)
// returns only genuinely new events), latestSequence(), the limit parameter,
// and one light live-subscribe check proving notifyCommitted() only fires
// after a real, successful commit.
#include "test_common.hpp"

#include <string>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/changefeed/changefeed.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::changefeed;

int main() {
    auto path = anntest::scratchDbPath("test_changefeed.apdb");
    EnvironmentOptions opts;
    opts.enableChangefeed = true;
    auto envR = Environment::open(path, opts);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();
    CHECK(env.changeFeed() != nullptr);

    // ---- Insert, Update, Delete, each its own committed transaction ----
    CHECK_OK(env.put(toByteView(std::string("k1")), toByteView(std::string("v1"))));
    CHECK_OK(env.put(toByteView(std::string("k1")), toByteView(std::string("v2"))));
    CHECK_OK(env.remove(toByteView(std::string("k1"))));

    Sequence lastSeq = kInvalidSequence;
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        txn::ReadTransaction rtxn = rtxnR.moveValue();

        auto pollR = env.changeFeed()->pollFrom(rtxn, kInvalidSequence, 0);
        CHECK(pollR.ok());
        if (pollR.ok()) {
            const auto& events = pollR.value();
            CHECK(events.size() == 3);
            if (events.size() == 3) {
                CHECK(events[0].type == ChangeType::Insert);
                CHECK(toStringView(toByteView(events[0].key)) == "k1");
                CHECK(events[0].oldValue.empty());
                CHECK(toStringView(toByteView(events[0].newValue)) == "v1");

                CHECK(events[1].type == ChangeType::Update);
                CHECK(toStringView(toByteView(events[1].oldValue)) == "v1");
                CHECK(toStringView(toByteView(events[1].newValue)) == "v2");

                CHECK(events[2].type == ChangeType::Delete);
                CHECK(toStringView(toByteView(events[2].oldValue)) == "v2");
                CHECK(events[2].newValue.empty());

                CHECK(events[0].sequence < events[1].sequence);
                CHECK(events[1].sequence < events[2].sequence);
                lastSeq = events[2].sequence;
            }
        }

        auto latestR = env.changeFeed()->latestSequence(rtxn);
        CHECK(latestR.ok());
        if (latestR.ok()) CHECK(latestR.value() == lastSeq);
    }

    // ---- resume semantics: nothing new yet ----
    {
        auto rtxnR = env.beginRead();
        txn::ReadTransaction rtxn = rtxnR.moveValue();
        auto pollR = env.changeFeed()->pollFrom(rtxn, lastSeq, 0);
        CHECK(pollR.ok());
        if (pollR.ok()) CHECK(pollR.value().empty());
    }

    // One more commit, then resume from the same token -> exactly one new event.
    CHECK_OK(env.put(toByteView(std::string("k2")), toByteView(std::string("v3"))));
    Sequence secondSeq = kInvalidSequence;
    {
        auto rtxnR = env.beginRead();
        txn::ReadTransaction rtxn = rtxnR.moveValue();
        auto pollR = env.changeFeed()->pollFrom(rtxn, lastSeq, 0);
        CHECK(pollR.ok());
        if (pollR.ok()) {
            CHECK(pollR.value().size() == 1);
            if (pollR.value().size() == 1) {
                CHECK(pollR.value()[0].type == ChangeType::Insert);
                CHECK(toStringView(toByteView(pollR.value()[0].key)) == "k2");
                secondSeq = pollR.value()[0].sequence;
            }
        }
        auto latestR = env.changeFeed()->latestSequence(rtxn);
        CHECK(latestR.ok());
        if (latestR.ok()) CHECK(latestR.value() == secondSeq);
    }

    // ---- limit caps the number of events returned ----
    CHECK_OK(env.put(toByteView(std::string("k3")), toByteView(std::string("v4"))));
    CHECK_OK(env.put(toByteView(std::string("k4")), toByteView(std::string("v5"))));
    {
        auto rtxnR = env.beginRead();
        txn::ReadTransaction rtxn = rtxnR.moveValue();
        auto pollR = env.changeFeed()->pollFrom(rtxn, kInvalidSequence, /*limit=*/2);
        CHECK(pollR.ok());
        if (pollR.ok()) CHECK(pollR.value().size() == 2);
    }

    // ---- live subscribe: handler fires only after a real, successful commit ----
    {
        std::vector<ChangeEvent> captured;
        env.changeFeed()->subscribe([&](const ChangeEvent& ev) { captured.push_back(ev); });
        CHECK_OK(env.put(toByteView(std::string("k5")), toByteView(std::string("v6"))));
        CHECK(captured.size() == 1);
        if (captured.size() == 1) {
            CHECK(captured[0].type == ChangeType::Insert);
            CHECK(toStringView(toByteView(captured[0].key)) == "k5");
        }
        env.changeFeed()->unsubscribe();
        // After unsubscribe, further commits must not reach the handler.
        CHECK_OK(env.put(toByteView(std::string("k6")), toByteView(std::string("v7"))));
        CHECK(captured.size() == 1);
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
