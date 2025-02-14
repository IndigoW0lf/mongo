/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/idl/server_parameter_specialized_test_gen.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace test {

template <typename T = ServerParameter>
T* getServerParameter(StringData name) {
    return ServerParameterSet::getNodeParameterSet()->get<T>(name);
}

template <typename Validator>
void ASSERT_APPENDED_VALUE(ServerParameter* sp, Validator validator) {
    BSONObjBuilder b;
    sp->append(nullptr, b, sp->name());
    auto obj = b.obj();

    ASSERT_EQ(obj.nFields(), 1);
    auto elem = obj[sp->name()];
    ASSERT_FALSE(elem.eoo());
    validator(elem);
}

void ASSERT_APPENDED_INT(ServerParameter* sp, long exp) {
    ASSERT_APPENDED_VALUE(sp, [&exp](const BSONElement& elem) {
        if (elem.type() == NumberInt) {
            ASSERT_EQ(elem.Int(), exp);
        } else {
            ASSERT_EQ(elem.type(), NumberLong);
            ASSERT_EQ(elem.Long(), exp);
        }
    });
}

void ASSERT_APPENDED_STRING(ServerParameter* sp, StringData exp) {
    ASSERT_APPENDED_VALUE(sp, [&exp](const BSONElement& elem) {
        ASSERT_EQ(elem.type(), String);
        ASSERT_EQ(elem.String(), exp);
    });
}

void ASSERT_APPENDED_OBJECT(ServerParameter* sp, const BSONObj& exp) {
    ASSERT_APPENDED_VALUE(sp, [&exp](const BSONElement& elem) {
        ASSERT_EQ(elem.type(), Object);

        UnorderedFieldsBSONObjComparator comparator;
        ASSERT(comparator.evaluate(elem.Obj() == exp));
    });
}

// specializedDummy

void SpecializedDummyServerParameter::append(OperationContext*,
                                             BSONObjBuilder& b,
                                             const std::string& name) {
    b << name << "Dummy Value";
}

Status SpecializedDummyServerParameter::setFromString(const std::string& value) {
    return Status::OK();
}

TEST(SpecializedServerParameter, dummy) {
    auto* dsp = getServerParameter("specializedDummy");
    ASSERT_APPENDED_STRING(dsp, "Dummy Value");
    ASSERT_OK(dsp->setFromString("new value"));
    ASSERT_NOT_OK(dsp->set(BSON("" << BSON_ARRAY("bar")).firstElement()));
    ASSERT_OK(dsp->set(BSON(""
                            << "bar")
                           .firstElement()));
}

// specializedWithCtor

namespace {
std::string gSCSP("Initial Value");
}  // namespace

SpecializedConstructorServerParameter::SpecializedConstructorServerParameter(
    StringData name, ServerParameterType spt)
    : ServerParameter(name, spt) {
    gSCSP = "Value from ctor";
}

void SpecializedConstructorServerParameter::append(OperationContext*,
                                                   BSONObjBuilder& b,
                                                   const std::string& name) {
    b << name << gSCSP;
}

Status SpecializedConstructorServerParameter::setFromString(const std::string& value) {
    gSCSP = value;
    return Status::OK();
}

TEST(SpecializedServerParameter, withCtor) {
    auto* csp = getServerParameter("specializedWithCtor");
    ASSERT_APPENDED_STRING(csp, "Value from ctor");
    ASSERT_OK(csp->setFromString("Updated Value"));
    ASSERT_EQ(gSCSP, "Updated Value");
    ASSERT_APPENDED_STRING(csp, "Updated Value");
}

// specializedWithValue

void SpecializedWithValueServerParameter::append(OperationContext*,
                                                 BSONObjBuilder& b,
                                                 const std::string& name) {
    b << name << _data;
}

Status SpecializedWithValueServerParameter::setFromString(const std::string& value) {
    return NumberParser{}(value, &_data);
}

TEST(SpecializedServerParameter, withValue) {
    using cls = SpecializedWithValueServerParameter;
    ASSERT_EQ(cls::kDataDefault, 43);

    auto* wv = getServerParameter<cls>("specializedWithValue");
    ASSERT_EQ(wv->_data, cls::kDataDefault);
    ASSERT_APPENDED_INT(wv, cls::kDataDefault);
    ASSERT_OK(wv->setFromString("102"));
    ASSERT_APPENDED_INT(wv, 102);
    ASSERT_EQ(wv->_data, 102);
}

// specializedWithStringValue

void SpecializedWithStringValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder& b,
                                                       const std::string& name) {
    b << name << _data;
}

Status SpecializedWithStringValueServerParameter::setFromString(const std::string& value) {
    _data = value;
    return Status::OK();
}

TEST(SpecializedServerParameter, withStringValue) {
    using cls = SpecializedWithStringValueServerParameter;
    ASSERT_EQ(cls::kDataDefault, "Hello World"_sd);

    auto* wsv = getServerParameter<cls>("specializedWithStringValue");
    ASSERT_EQ(wsv->_data, cls::kDataDefault);
    ASSERT_APPENDED_STRING(wsv, cls::kDataDefault);
    ASSERT_OK(wsv->setFromString("Goodbye Land"));
    ASSERT_APPENDED_STRING(wsv, "Goodbye Land");
    ASSERT_EQ(wsv->_data, "Goodbye Land");
}

// specializedWithAtomicValue

void SpecializedWithAtomicValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder& b,
                                                       const std::string& name) {
    b << name << _data.load();
}

Status SpecializedWithAtomicValueServerParameter::setFromString(const std::string& value) {
    std::uint32_t val;

    auto status = NumberParser{}(value, &val);
    if (!status.isOK()) {
        return status;
    }

    _data.store(val);
    return Status::OK();
}

TEST(SpecializedServerParameter, withAtomicValue) {
    using cls = SpecializedWithAtomicValueServerParameter;
    ASSERT_EQ(cls::kDataDefault, 42);

    auto* wv = getServerParameter<cls>("specializedWithAtomicValue");
    ASSERT_EQ(wv->_data.load(), cls::kDataDefault);
    ASSERT_APPENDED_INT(wv, cls::kDataDefault);
    ASSERT_OK(wv->set(BSON("" << 99).firstElement()));
    ASSERT_APPENDED_INT(wv, 99);
    ASSERT_OK(wv->setFromString("101"));
    ASSERT_APPENDED_INT(wv, 101);
    ASSERT_EQ(wv->_data.load(), 101);
}

// specializedWithMultiValue

void SpecializedMultiValueServerParameter::append(OperationContext*,
                                                  BSONObjBuilder& b,
                                                  const std::string& name) {
    b << name << BSON("value" << _data.value << "flag" << _data.flag);
}

Status SpecializedMultiValueServerParameter::set(const BSONElement& value) try {
    auto obj = value.Obj();
    _data.value = obj["value"].String();
    _data.flag = obj["flag"].Bool();
    return Status::OK();
} catch (const AssertionException&) {
    return {ErrorCodes::BadValue, "Failed parsing extra data"};
}

Status SpecializedMultiValueServerParameter::setFromString(const std::string& value) {
    return set(BSON("" << BSON("value" << value << "flag" << false)).firstElement());
}

TEST(SpecializedServerParameter, multiValue) {
    auto* edsp = getServerParameter("specializedWithMultiValue");
    ASSERT_APPENDED_OBJECT(edsp,
                           BSON("value"
                                << "start value"
                                << "flag" << true));
    ASSERT_OK(edsp->setFromString("second value"));
    ASSERT_APPENDED_OBJECT(edsp,
                           BSON("value"
                                << "second value"
                                << "flag" << false));
    ASSERT_OK(edsp->set(BSON("" << BSON("value"
                                        << "third value"
                                        << "flag" << true))
                            .firstElement()));
    ASSERT_APPENDED_OBJECT(edsp,
                           BSON("value"
                                << "third value"
                                << "flag" << true));
}

// specializedWithCtorAndValue

SpecializedWithCtorAndValueServerParameter::SpecializedWithCtorAndValueServerParameter(
    StringData name, ServerParameterType spt)
    : ServerParameter(name, spt) {}

void SpecializedWithCtorAndValueServerParameter::append(OperationContext*,
                                                        BSONObjBuilder& b,
                                                        const std::string& name) {
    b << name << _data;
}

Status SpecializedWithCtorAndValueServerParameter::setFromString(const std::string& value) {
    return NumberParser{}(value, &_data);
}

TEST(SpecializedServerParameter, withCtorAndValue) {
    using cls = SpecializedWithCtorAndValueServerParameter;
    auto* cvsp = getServerParameter<cls>("specializedWithCtorAndValue");
    ASSERT_APPENDED_INT(cvsp, cls::kDataDefault);
    ASSERT_OK(cvsp->setFromString(std::to_string(cls::kDataDefault + 1)));
    ASSERT_EQ(cvsp->_data, cls::kDataDefault + 1);
    ASSERT_APPENDED_INT(cvsp, cls::kDataDefault + 1);
}

// specializedWithOptions

namespace {
std::string gSWO = "Initial Value";
}  // namespace

Status SpecializedWithOptions::setFromString(const std::string& value) {
    gSWO = value;
    return Status::OK();
}

TEST(SpecializedServerParameter, withOptions) {
    auto* swo = getServerParameter("specializedWithOptions");
    ASSERT_APPENDED_STRING(swo, "###");
    ASSERT_OK(swo->setFromString("second value"));
    ASSERT_EQ(gSWO, "second value");
    ASSERT_APPENDED_STRING(swo, "###");

    auto* dswo = getServerParameter("deprecatedWithOptions");
    ASSERT_APPENDED_STRING(dswo, "###");
    ASSERT_OK(dswo->setFromString("third value"));
    ASSERT_EQ(gSWO, "third value");
    ASSERT_APPENDED_STRING(dswo, "###");
}

// specializedRuntimeOnly

void SpecializedRuntimeOnly::append(OperationContext*, BSONObjBuilder&, const std::string&) {}

Status SpecializedRuntimeOnly::setFromString(const std::string& value) {
    return Status::OK();
}

Status SpecializedRedactedSettable::setFromString(const std::string& value) {
    std::cout << "Setting to: " << value << "\n";
    _data = value;
    return Status::OK();
}

TEST(SpecializedServerParameter, SpecializedRedactedSettable) {
    using namespace std::literals;
    using namespace unittest::match;

    auto* sp = getServerParameter("specializedRedactedSettable");
    ASSERT(sp);
    auto down = dynamic_cast<SpecializedRedactedSettable*>(sp);
    ASSERT(down);
    auto& dataMember = down->_data;

    auto store = [&](auto&& name, auto&& value) {
        return sp->set(BSON(name << value).firstElement());
    };
    auto load = [&] {
        BSONObjBuilder bob;
        sp->append(nullptr, bob, sp->name());
        return bob.obj();
    };

    ASSERT_OK(store("", "hello"));
    ASSERT_THAT(load(), BSONObjHas(BSONElementIs(Eq(sp->name()), Eq(String), Eq("###"))))
        << "value redacted by append";
    ASSERT_THAT(dataMember, Eq("hello")) << "value preseved in _data member";

    ASSERT_THAT(store("", std::vector{"zzzzz"s}),
                StatusIs(Eq(ErrorCodes::BadValue),
                         AllOf(ContainsRegex("[uU]nsupported type"),
                               ContainsRegex("###"),
                               Not(ContainsRegex("zzzzz")))))
        << "value redacted in `set` Status when failing from unsupported element type";
    ASSERT_THAT(dataMember, Eq("hello")) << "Unchanged by failed `set` call";
}

TEST(SpecializedServerParameter, withScope) {
    using SPT = ServerParameterType;

    auto* nodeSet = ServerParameterSet::getNodeParameterSet();
    auto* clusterSet = ServerParameterSet::getClusterParameterSet();

    static constexpr auto kSpecializedWithOptions = "specializedWithOptions"_sd;
    auto* nodeSWO = nodeSet->getIfExists(kSpecializedWithOptions);
    ASSERT(nullptr != nodeSWO);
    ASSERT(nullptr == clusterSet->getIfExists(kSpecializedWithOptions));

    auto* clusterSWO =
        makeServerParameter<SpecializedWithOptions>(kSpecializedWithOptions, SPT::kClusterWide);
    ASSERT(clusterSWO != nodeSWO);
    ASSERT(clusterSWO == clusterSet->getIfExists(kSpecializedWithOptions));

    // Duplicate key
    ASSERT_THROWS_CODE(
        makeServerParameter<SpecializedWithOptions>(kSpecializedWithOptions, SPT::kClusterWide),
        DBException,
        23784);

    // Require runtime only.
    static constexpr auto kSpecializedRuntimeOnly = "specializedRuntimeOnly"_sd;
    auto* clusterSRO =
        makeServerParameter<SpecializedRuntimeOnly>(kSpecializedRuntimeOnly, SPT::kClusterWide);
    ASSERT(nullptr != clusterSRO);
    // Pointer now belongs to ServerParameterSet, no need to delete.
}

// specializedWithValidateServerParameter

void SpecializedWithValidateServerParameter::append(OperationContext*,
                                                    BSONObjBuilder&,
                                                    const std::string&) {}

Status SpecializedWithValidateServerParameter::setFromString(const std::string& str) {
    return NumberParser{}(str, &_data);
}

Status SpecializedWithValidateServerParameter::validate(const BSONElement& newValueElement) const {
    try {
        auto val = newValueElement.Int();
        if (val < 0) {
            return Status{ErrorCodes::BadValue,
                          "specializedWithValidate must be a non-negative integer"};
        }
    } catch (const AssertionException&) {
        return {ErrorCodes::BadValue, "Failed parsing specializedWithValidate"};
    }

    return Status::OK();
}

TEST(SpecializedServerParameter, withValidate) {
    auto* nodeSet = ServerParameterSet::getNodeParameterSet();

    constexpr auto kSpecializedWithValidate = "specializedWithValidate"_sd;
    auto* validateSP = nodeSet->getIfExists(kSpecializedWithValidate);
    ASSERT(nullptr != validateSP);

    // Assert that validate works by itself.
    ASSERT_OK(validateSP->validate(BSON(kSpecializedWithValidate << 5).firstElement()));
    ASSERT_OK(validateSP->validate(BSON(kSpecializedWithValidate << 0).firstElement()));
    ASSERT_NOT_OK(validateSP->validate(BSON(kSpecializedWithValidate << -1).firstElement()));

    // Assert that validate works when called within set.
    ASSERT_OK(validateSP->set(BSON(kSpecializedWithValidate << 5).firstElement()));
    ASSERT_OK(validateSP->set(BSON(kSpecializedWithValidate << 0).firstElement()));
    ASSERT_NOT_OK(validateSP->set(BSON(kSpecializedWithValidate << -1).firstElement()));
}

// specializedWithClusterServerParameter

void SpecializedClusterServerParameter::append(OperationContext*,
                                               BSONObjBuilder& builder,
                                               const std::string& name) {
    BSONObjBuilder subObjBuilder = builder.subobjStart(name);
    _data.serialize(&subObjBuilder);
    subObjBuilder.done();
}

Status SpecializedClusterServerParameter::set(const BSONElement& newValueElement) {
    Status status = validate(newValueElement);
    if (!status.isOK()) {
        return status;
    }

    _data.parse(newValueElement.Obj());
    return Status::OK();
}

Status SpecializedClusterServerParameter::validate(const BSONElement& newValueElement) const {
    try {
        auto obj = newValueElement.Obj();
        auto strValue = obj["strData"_sd].String();
        auto intValue = obj["intData"_sd].Int();

        if (strValue.size() == 0 || intValue < 0) {
            return Status{ErrorCodes::BadValue,
                          "Invalid fields provided to specializedCluster parameter"};
        }
    } catch (const AssertionException&) {
        return {ErrorCodes::BadValue, "Failed parsing specializedCluster parameter"};
    }

    return Status::OK();
}

Status SpecializedClusterServerParameter::reset() {
    _data.reset();
    return Status::OK();
}

const LogicalTime SpecializedClusterServerParameter::getClusterParameterTime() const {
    return _data.getClusterParameterTime();
}

TEST(SpecializedServerParameter, clusterServerParameter) {
    auto* clusterSet = ServerParameterSet::getClusterParameterSet();
    constexpr auto kSpecializedCSPName = "specializedCluster"_sd;

    auto* specializedCsp = clusterSet->getIfExists(kSpecializedCSPName);
    ASSERT(nullptr != specializedCsp);

    // Assert that the parameter can be set.
    BSONObjBuilder builder;
    SpecializedClusterServerParameterData data;
    LogicalTime updateTime = LogicalTime(Timestamp(Date_t::now()));
    data.setClusterParameterTime(updateTime);
    data.setIntData(50);
    data.setStrData("hello");
    data.serialize(&builder);
    ASSERT_OK(specializedCsp->set(builder.asTempObj()));

    // Assert that the parameter cannot be set from strings.
    ASSERT_NOT_OK(specializedCsp->setFromString(""));

    // Assert that the clusterParameterTime can be retrieved.
    ASSERT_EQ(specializedCsp->getClusterParameterTime(), updateTime);

    // Assert that the parameter can be appended to a builder.
    builder.resetToEmpty();
    specializedCsp->append(nullptr, builder, kSpecializedCSPName.toString());
    auto obj = builder.asTempObj()["specializedCluster"_sd].Obj();
    ASSERT_EQ(obj.nFields(), 3);
    ASSERT_EQ(obj["clusterParameterTime"_sd].timestamp(), updateTime.asTimestamp());
    ASSERT_EQ(obj["strData"_sd].String(), "hello");
    ASSERT_EQ(obj["intData"_sd].Int(), 50);

    // Assert that invalid parameter values fail validation directly and implicitly during set.
    builder.resetToEmpty();
    updateTime = LogicalTime(Timestamp(Date_t::now()));
    data.setClusterParameterTime(updateTime);
    data.setIntData(-1);
    data.setStrData("");
    data.serialize(&builder);
    ASSERT_NOT_OK(specializedCsp->validate(builder.asTempObj()));
    ASSERT_NOT_OK(specializedCsp->set(builder.asTempObj()));

    // Assert that the parameter can be reset to its defaults.
    builder.resetToEmpty();
    ASSERT_OK(specializedCsp->reset());
    specializedCsp->append(nullptr, builder, kSpecializedCSPName.toString());
    obj = builder.asTempObj()["specializedCluster"_sd].Obj();
    ASSERT_EQ(obj.nFields(), 3);
    ASSERT_EQ(obj["clusterParameterTime"_sd].timestamp(), LogicalTime().asTimestamp());
    ASSERT_EQ(obj["strData"_sd].String(), "default");
    ASSERT_EQ(obj["intData"_sd].Int(), 30);
}

}  // namespace test
}  // namespace mongo
