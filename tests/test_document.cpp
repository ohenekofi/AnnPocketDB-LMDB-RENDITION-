// Coverage for document::Value/MsgPackCodec/PlainTextCodec/DocumentStore
// (previously untested): round-tripping every ValueType (including nested
// array/map) through the real MessagePack codec, the legacy flat
// PlainTextCodec, DocumentStore CRUD via both the Value and Document
// overloads, dotted/array field-path resolution, and DocumentStore's
// automatic secondary-index maintenance (including sparse/missing-field
// indexing) when an index::IndexManager is attached.
#include "test_common.hpp"

#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/document/document.hpp"
#include "annpocketdb/index/index.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::document;
using namespace annpocketdb::index;

int main() {
    // ---- MsgPackCodec round-trip: every ValueType, including nested array/map ----
    {
        MsgPackCodec codec;

        Value::Map inner;
        inner["city"] = Value::string("Accra");
        inner["zip"] = Value::integer(-233);

        Value::Array arr;
        arr.push_back(Value::integer(1));
        arr.push_back(Value::string("two"));
        arr.push_back(Value::array(Value::Array{Value::integer(3), Value::boolean(true)}));

        Value::Map top;
        top["n"] = Value::null();
        top["b"] = Value::boolean(true);
        top["i"] = Value::integer(-42);
        top["u"] = Value::uinteger(42);
        top["f"] = Value::floating(3.5);
        top["s"] = Value::string("hello");
        top["by"] = Value::bytes(ByteBuffer{std::byte{1}, std::byte{2}, std::byte{3}});
        top["arr"] = Value::array(arr);
        top["map"] = Value::map(inner);

        Value doc = Value::map(top);
        ByteBuffer enc;
        CHECK_OK(codec.encodeValue(doc, enc));
        Value back;
        CHECK_OK(codec.decodeValue(toByteView(enc), back));

        CHECK(back.isMap());
        const auto& m = back.asMap();
        CHECK(m.at("n").isNull());
        CHECK(m.at("b").isBool() && m.at("b").asBool() == true);
        CHECK(m.at("i").isInt() && m.at("i").asInt() == -42);
        CHECK(m.at("u").isUInt() && m.at("u").asUInt() == 42);
        CHECK(m.at("f").isNumber() && m.at("f").asFloat() == 3.5);
        CHECK(m.at("s").isString() && m.at("s").asString() == "hello");
        CHECK(m.at("by").isBytes() && m.at("by").asBytes().size() == 3);
        CHECK(m.at("arr").isArray() && m.at("arr").asArray().size() == 3);
        CHECK(m.at("arr").asArray()[1].asString() == "two");
        CHECK(m.at("arr").asArray()[2].isArray());
        CHECK(m.at("map").isMap());
        CHECK(m.at("map").asMap().at("city").asString() == "Accra");
        CHECK(m.at("map").asMap().at("zip").asInt() == -233);
    }

    // ---- PlainTextCodec: legacy flat key=value round-trip ----
    {
        PlainTextCodec codec;
        std::vector<std::pair<std::string, std::string>> fields = {
            {"name", "alice"}, {"role", "admin"},
        };
        ByteBuffer enc;
        CHECK_OK(codec.encode(fields, enc));
        std::vector<std::pair<std::string, std::string>> back;
        CHECK_OK(codec.decode(toByteView(enc), back));
        CHECK(back.size() == 2);
        std::string name;
        CHECK_OK(codec.extractField(toByteView(enc), parseFieldPath("name"), name));
        CHECK(name == "alice");
    }

    // ---- parseFieldPath / getByPath ----
    {
        Value::Map addr;
        addr["city"] = Value::string("Accra");
        Value::Map user;
        user["address"] = Value::map(addr);
        user["tags"] = Value::array(Value::Array{Value::string("x"), Value::string("y")});
        Value::Map root;
        root["user"] = Value::map(user);
        Value rootVal = Value::map(root);

        Value out;
        CHECK_OK(getByPath(rootVal, parseFieldPath("user.address.city"), out));
        CHECK(out.isString() && out.asString() == "Accra");

        CHECK_OK(getByPath(rootVal, parseFieldPath("user.tags.1"), out));
        CHECK(out.isString() && out.asString() == "y");

        Status missing = getByPath(rootVal, parseFieldPath("user.address.country"), out);
        CHECK(!missing.ok());
        CHECK(missing.code() == StatusCode::NotFound);

        Status badIndex = getByPath(rootVal, parseFieldPath("user.tags.9"), out);
        CHECK(!badIndex.ok());
        CHECK(badIndex.code() == StatusCode::NotFound);
    }

    // ---- DocumentStore CRUD (no index attached) ----
    {
        auto path = anntest::scratchDbPath("test_document_crud.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        auto codecR = createMsgPackCodec();
        CHECK(codecR.ok());
        if (!codecR) TEST_MAIN_END();
        auto codec = codecR.moveValue();
        DocumentStore store(*codec);

        Value::Map fields;
        fields["name"] = Value::string("bob");
        fields["age"] = Value::integer(30);
        Value v = Value::map(fields);

        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(store.put(wtxn, toByteView(std::string("u1")), v));
            CHECK_OK(wtxn.commit());
        }
        {
            auto rtxnR = env.beginRead();
            CHECK(rtxnR.ok());
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            auto got = store.getValue(rtxn, toByteView(std::string("u1")));
            CHECK(got.ok());
            if (got.ok()) {
                CHECK(got.value().isMap());
                CHECK(got.value().asMap().at("name").asString() == "bob");
                // MessagePack encodes small non-negative integers as
                // "positive fixint", the same wire byte regardless of
                // whether the original Value was Int or UInt -- decodeValue
                // always reconstructs these as UInt (see decodeValue's
                // "positive fixint" branch), so a round-tripped
                // Value::integer(30) legitimately comes back isUInt(), not
                // isInt(). Compare via the canonical string form, which is
                // correct either way, rather than assuming a specific type.
                CHECK(got.value().asMap().at("age").toCanonicalString() == "30");
            }
            // Document overload returns the same raw bytes.
            auto rawGot = store.get(rtxn, toByteView(std::string("u1")));
            CHECK(rawGot.ok());
        }
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(store.remove(wtxn, toByteView(std::string("u1"))));
            CHECK_OK(wtxn.commit());
        }
        {
            auto rtxnR = env.beginRead();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK(!store.get(rtxn, toByteView(std::string("u1"))).ok());
        }
        // remove() on a missing key fails.
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            Status s = store.remove(wtxn, toByteView(std::string("nosuchkey")));
            CHECK(!s.ok());
            CHECK_OK(wtxn.abort());
        }
        CHECK_OK(env.close());
    }

    // ---- DocumentStore + IndexManager: automatic maintenance, sparse indexing ----
    {
        auto path = anntest::scratchDbPath("test_document_indexed.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        auto codecR = createMsgPackCodec();
        CHECK(codecR.ok());
        auto codec = codecR.moveValue();

        btree::BTree tree(env.pageManager());
        IndexManager indexes(tree, env.pageManager());
        {
            auto rtxnR = env.beginRead();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK_OK(indexes.loadCatalog(rtxn));
        }
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(indexes.createIndex("by_email", "email", wtxn));
            CHECK_OK(indexes.saveCatalog(wtxn));
            CHECK_OK(wtxn.commit());
        }

        DocumentStore store(*codec, &indexes);

        // doc1 has an email field; doc2 does not (sparse indexing case).
        Value::Map d1; d1["email"] = Value::string("a@x.com");
        Value::Map d2; d2["name"] = Value::string("no-email-here");
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(store.put(wtxn, toByteView(std::string("doc1")), Value::map(d1)));
            CHECK_OK(store.put(wtxn, toByteView(std::string("doc2")), Value::map(d2)));
            // saveCatalog already happened automatically inside put() since an
            // IndexManager is attached -- no manual saveIndexes() call needed.
            CHECK_OK(wtxn.commit());
        }
        {
            auto idxR = indexes.openIndex("by_email");
            CHECK(idxR.ok());
            if (idxR.ok()) {
                auto cntR = idxR.value().entryCount();
                CHECK(cntR.ok());
                // Only doc1 has the indexed field -- doc2 must not appear.
                if (cntR.ok()) CHECK(cntR.value() == 1);
                // maintainOnPut indexes Value::toIndexBytes() (a type-tagged
                // encoding), not the raw field bytes -- search with the same
                // encoding a String field is actually stored under.
                ByteBuffer emailKey = Value::string("a@x.com").toIndexBytes();
                auto foundR = idxR.value().find(toByteView(emailKey));
                CHECK(foundR.ok());
            }
        }
        // Remove doc1: its index entry must be dropped too.
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(store.remove(wtxn, toByteView(std::string("doc1"))));
            CHECK_OK(wtxn.commit());
        }
        {
            auto idxR = indexes.openIndex("by_email");
            CHECK(idxR.ok());
            if (idxR.ok()) {
                auto cntR = idxR.value().entryCount();
                CHECK(cntR.ok());
                if (cntR.ok()) CHECK(cntR.value() == 0);
            }
        }
        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
