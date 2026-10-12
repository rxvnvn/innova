#include <boost/test/unit_test.hpp>

#include <string>

#include "innovarpc.h"
#include "main.h"
#include "util.h"

// RPC-SPLIT: focused contract tests for the getinfo / getinfo(true) /
// getibdmetrics split. Verifies:
//   * default getinfo is concise and does NOT contain "ibdmetrics";
//   * getinfo false == default;
//   * getinfo true contains the full nested ibdmetrics object;
//   * getibdmetrics returns exactly that same object;
//   * invalid arguments are rejected with an RPC parameter error;
//   * the contract is identical in IBD and non-IBD states.
namespace {

const json_spirit::Value* ObjFind(const json_spirit::Object& obj,
                                  const std::string& key)
{
    for (json_spirit::Object::const_iterator it = obj.begin(); it != obj.end(); ++it)
        if (it->name_ == key)
            return &it->value_;
    return NULL;
}

// Ordinary top-level fields every getinfo variant must keep.
const char* const kStandardFields[] = {
    "version", "buildcommit", "protocolversion", "walletversion", "balance",
    "blocks", "timeoffset", "connections", "difficulty", "testnet",
    "keypoololdest", "keypoolsize", "paytxfee", "datadir",
    "initialblockdownload", "errors", NULL
};

void CheckStandardFieldsPresent(const json_spirit::Object& o)
{
    for (int i = 0; kStandardFields[i] != NULL; ++i)
        BOOST_CHECK_MESSAGE(ObjFind(o, kStandardFields[i]) != NULL,
                            "missing standard getinfo field: " << kStandardFields[i]);
}

const json_spirit::Object& IbdOf(const json_spirit::Object& o)
{
    const json_spirit::Value* m = ObjFind(o, "ibdmetrics");
    BOOST_REQUIRE(m != NULL);
    BOOST_REQUIRE(m->type() == json_spirit::obj_type);
    return m->get_obj();
}

} // namespace

BOOST_AUTO_TEST_SUITE(rpc_getinfo_tests)

// 1. getinfo with no arguments: concise, keeps ordinary fields, no ibdmetrics.
BOOST_AUTO_TEST_CASE(getinfo_default_is_concise_and_omits_ibdmetrics)
{
    const json_spirit::Array params;
    const json_spirit::Value v = tableRPC.execute("getinfo", params);
    BOOST_REQUIRE(v.type() == json_spirit::obj_type);
    const json_spirit::Object o = v.get_obj();
    CheckStandardFieldsPresent(o);
    BOOST_CHECK(ObjFind(o, "ibdmetrics") == NULL);
}

// 2. getinfo false must be equivalent to the default form.
BOOST_AUTO_TEST_CASE(getinfo_false_equals_default)
{
    json_spirit::Array pFalse;
    pFalse.push_back(false);
    const json_spirit::Object oFalse =
        tableRPC.execute("getinfo", pFalse).get_obj();
    const json_spirit::Object oDefault =
        tableRPC.execute("getinfo", json_spirit::Array()).get_obj();
    BOOST_CHECK(ObjFind(oFalse, "ibdmetrics") == NULL);
    CheckStandardFieldsPresent(oFalse);
    BOOST_CHECK_EQUAL(oFalse.size(), oDefault.size());
    for (json_spirit::Object::const_iterator it = oDefault.begin();
         it != oDefault.end(); ++it)
        BOOST_CHECK_MESSAGE(ObjFind(oFalse, it->name_) != NULL,
                            "getinfo false missing key " << it->name_);
}

// 3. getinfo true includes the full ibdmetrics object with retained schema.
BOOST_AUTO_TEST_CASE(getinfo_true_includes_full_ibdmetrics)
{
    json_spirit::Array p;
    p.push_back(true);
    const json_spirit::Object o = tableRPC.execute("getinfo", p).get_obj();
    CheckStandardFieldsPresent(o);

    const json_spirit::Object& ibd = IbdOf(o);
    const char* const anchors[] = {
        "start_time_unix", "now_unix", "uptime_seconds", "height",
        "global_active_current", "global_active_max", "refill_calls",
        "block_receive_total", "block_result_accepted_active",
        "semantic_health", NULL
    };
    for (int i = 0; anchors[i] != NULL; ++i)
        BOOST_CHECK_MESSAGE(ObjFind(ibd, anchors[i]) != NULL,
                            "ibdmetrics missing field " << anchors[i]);

    // Nested structures must be retained, not flattened to a stub.
    const json_spirit::Value* semObj = ObjFind(ibd, "semantic_health");
    BOOST_REQUIRE(semObj != NULL);
    BOOST_CHECK(semObj->type() == json_spirit::obj_type);
    BOOST_CHECK(semObj->get_obj().size() > 0);

    // Hundreds of counters, not a placeholder.
    BOOST_CHECK(ibd.size() > 100);
}

// 4. getibdmetrics returns exactly the object embedded by getinfo true.
BOOST_AUTO_TEST_CASE(getibdmetrics_matches_getinfo_true)
{
    const json_spirit::Value g =
        tableRPC.execute("getibdmetrics", json_spirit::Array());
    BOOST_REQUIRE(g.type() == json_spirit::obj_type);
    const json_spirit::Object& gi = g.get_obj();

    json_spirit::Array p;
    p.push_back(true);
    const json_spirit::Object oExtended =
        tableRPC.execute("getinfo", p).get_obj();
    const json_spirit::Object& ti = IbdOf(oExtended);

    BOOST_CHECK_EQUAL(gi.size(), ti.size());
    for (json_spirit::Object::const_iterator it = ti.begin(); it != ti.end(); ++it)
        BOOST_CHECK_MESSAGE(ObjFind(gi, it->name_) != NULL,
                            "getibdmetrics missing key " << it->name_);
    for (json_spirit::Object::const_iterator it = gi.begin(); it != gi.end(); ++it)
        BOOST_CHECK_MESSAGE(ObjFind(ti, it->name_) != NULL,
                            "getinfo true ibdmetrics missing key " << it->name_);
}

// 5. Invalid arguments: wrong types and excessive arguments.
BOOST_AUTO_TEST_CASE(getinfo_rejects_invalid_arguments)
{
    // Wrong type (integer / string) must raise an RPC error object.
    json_spirit::Array pInt;
    pInt.push_back(1);
    BOOST_CHECK_THROW(tableRPC.execute("getinfo", pInt), json_spirit::Object);

    json_spirit::Array pStr;
    pStr.push_back(std::string("true"));
    BOOST_CHECK_THROW(tableRPC.execute("getinfo", pStr), json_spirit::Object);

    // Excessive arguments must be rejected.
    json_spirit::Array pTwo;
    pTwo.push_back(true);
    pTwo.push_back(true);
    BOOST_CHECK_THROW(tableRPC.execute("getinfo", pTwo), json_spirit::Object);

    // A wrong-type argument must specifically be RPC_INVALID_PARAMETER.
    try
    {
        tableRPC.execute("getinfo", pInt);
        BOOST_FAIL("expected getinfo to reject a non-boolean argument");
    }
    catch (const json_spirit::Object& err)
    {
        const json_spirit::Value* code = ObjFind(err, "code");
        BOOST_REQUIRE(code != NULL);
        BOOST_CHECK_EQUAL(code->get_int(), RPC_INVALID_PARAMETER);
    }

    // getibdmetrics takes no arguments.
    json_spirit::Array pAny;
    pAny.push_back(true);
    BOOST_CHECK_THROW(tableRPC.execute("getibdmetrics", pAny), json_spirit::Object);
    try
    {
        tableRPC.execute("getibdmetrics", pAny);
        BOOST_FAIL("expected getibdmetrics to reject arguments");
    }
    catch (const json_spirit::Object& err)
    {
        const json_spirit::Value* code = ObjFind(err, "code");
        BOOST_REQUIRE(code != NULL);
        BOOST_CHECK_EQUAL(code->get_int(), RPC_INVALID_PARAMETER);
    }
}

// 6. The contract is identical in an IBD-looking state.
BOOST_AUTO_TEST_CASE(contract_holds_in_ibd_state)
{
    const bool fRegTestSaved = fRegTest;
    const bool fReindexSaved = fReindex;

    // Force IsInitialBlockDownload() == true via the import/reindex path.
    fRegTest = false;
    fReindex = true;
    BOOST_REQUIRE(IsInitialBlockDownload());

    const json_spirit::Object oDefault =
        tableRPC.execute("getinfo", json_spirit::Array()).get_obj();
    BOOST_CHECK(ObjFind(oDefault, "ibdmetrics") == NULL);
    const json_spirit::Value* ibdFlag = ObjFind(oDefault, "initialblockdownload");
    BOOST_REQUIRE(ibdFlag != NULL);
    BOOST_CHECK(ibdFlag->get_bool());

    json_spirit::Array p;
    p.push_back(true);
    const json_spirit::Object oExtended = tableRPC.execute("getinfo", p).get_obj();
    BOOST_CHECK(ObjFind(oExtended, "ibdmetrics") != NULL);

    const json_spirit::Value g =
        tableRPC.execute("getibdmetrics", json_spirit::Array());
    BOOST_CHECK(g.type() == json_spirit::obj_type);

    fReindex = fReindexSaved;
    fRegTest = fRegTestSaved;
}

BOOST_AUTO_TEST_SUITE_END()
