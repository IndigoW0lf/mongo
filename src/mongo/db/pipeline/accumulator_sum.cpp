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

#include <cmath>
#include <limits>

#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/pipeline/accumulator.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_count.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"
#include "mongo/util/summation.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(sum, genericParseSingleExpressionAccumulator<AccumulatorSum>);
REGISTER_STABLE_EXPRESSION(sum, ExpressionFromAccumulator<AccumulatorSum>::parse);
REGISTER_REMOVABLE_WINDOW_FUNCTION(sum, AccumulatorSum, WindowFunctionSum);
REGISTER_ACCUMULATOR_WITH_MIN_VERSION(count,
                                      parseCountAccumulator,
                                      multiversion::FeatureCompatibilityVersion::kVersion_5_1);

REGISTER_WINDOW_FUNCTION(count, window_function::parseCountWindowFunction);

namespace {
const char subTotalName[] = "subTotal";
const char subTotalErrorName[] = "subTotalError";  // Used for extra precision.
}  // namespace

void applyPartialSum(const std::vector<Value>& arr,
                     BSONType& nonDecimalTotalType,
                     BSONType& totalType,
                     DoubleDoubleSummation& nonDecimalTotal,
                     Decimal128& decimalTotal) {
    tassert(6294002,
            "The partial sum's first element must be an int",
            arr[AggSumValueElems::kNonDecimalTotalTag].getType() == NumberInt);
    nonDecimalTotalType = Value::getWidestNumeric(
        nonDecimalTotalType,
        static_cast<BSONType>(arr[AggSumValueElems::kNonDecimalTotalTag].getInt()));
    totalType = Value::getWidestNumeric(totalType, nonDecimalTotalType);

    tassert(6294003,
            "The partial sum's second element must be a double",
            arr[AggSumValueElems::kNonDecimalTotalSum].getType() == NumberDouble);
    tassert(6294004,
            "The partial sum's third element must be a double",
            arr[AggSumValueElems::kNonDecimalTotalAddend].getType() == NumberDouble);

    auto sum = arr[AggSumValueElems::kNonDecimalTotalSum].getDouble();
    auto addend = arr[AggSumValueElems::kNonDecimalTotalAddend].getDouble();
    nonDecimalTotal.addDouble(sum);

    // If sum is +=INF and addend is +=NAN, 'nonDecimalTotal' becomes NAN after adding
    // INF and NAN, which is different from the unsharded behavior. So, does not add
    // 'addend' when sum == INF and addend == NAN. Does not add this logic to
    // 'DoubleDoubleSummation' because this behavior is specific to sharded $sum.
    if (std::isfinite(sum) || !std::isnan(addend)) {
        nonDecimalTotal.addDouble(addend);
    }

    if (arr.size() == AggSumValueElems::kMaxSizeOfArray) {
        totalType = NumberDecimal;
        tassert(6294005,
                "The partial sum's last element must be a decimal",
                arr[AggSumValueElems::kDecimalTotal].getType() == NumberDecimal);
        decimalTotal = decimalTotal.add(arr[AggSumValueElems::kDecimalTotal].getDecimal());
    }
}

void AccumulatorSum::processInternal(const Value& input, bool merging) {
    if (!input.numeric()) {
        // Ignore non-numeric inputs when not merging.
        if (!merging) {
            return;
        }

        switch (input.getType()) {
            // TODO SERVER-64227: Remove 'Object' case which is no longer necessary when we
            // branch for 6.1.
            case Object:
                // Process merge document, see getValue() below.
                nonDecimalTotal.addDouble(
                    input[subTotalName].getDouble());  // Sum without adjusting type.
                processInternal(input[subTotalErrorName],
                                false);  // Sum adjusting for type of error.
                break;
            // The merge-side must be ready to process the full state of a partial sum from a
            // shard-side if a shard chooses to do so. See Accumulator::getValue() for details.
            case Array:
                applyPartialSum(input.getArray(),
                                nonDecimalTotalType,
                                totalType,
                                nonDecimalTotal,
                                decimalTotal);
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return;
    }

    // Upgrade to the widest type required to hold the result.
    totalType = Value::getWidestNumeric(totalType, input.getType());

    // Keep the nonDecimalTotal's type so that the type information can be serialized too for
    // 'toBeMerged' scenarios.
    if (input.getType() != NumberDecimal) {
        nonDecimalTotalType = Value::getWidestNumeric(nonDecimalTotalType, input.getType());
    }
    switch (input.getType()) {
        case NumberLong:
            nonDecimalTotal.addLong(input.getLong());
            break;
        case NumberInt:
            nonDecimalTotal.addInt(input.getInt());
            break;
        case NumberDouble:
            nonDecimalTotal.addDouble(input.getDouble());
            break;
        case NumberDecimal:
            decimalTotal = decimalTotal.add(input.coerceToDecimal());
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

intrusive_ptr<AccumulatorState> AccumulatorSum::create(ExpressionContext* const expCtx) {
    return new AccumulatorSum(expCtx);
}

Value serializePartialSum(BSONType nonDecimalTotalType,
                          BSONType totalType,
                          const DoubleDoubleSummation& nonDecimalTotal,
                          const Decimal128& decimalTotal) {
    auto [sum, addend] = nonDecimalTotal.getDoubleDouble();

    // The partial sum is serialized in the following form.
    //
    // [nonDecimalTotalType, sum, addend, decimalTotal]
    //
    // Presence of the 'decimalTotal' element indicates that the total type of the partial sum
    // is 'NumberDecimal'.
    auto valueArrayStream = ValueArrayStream();
    valueArrayStream << static_cast<int>(nonDecimalTotalType) << sum << addend;
    if (totalType == NumberDecimal) {
        valueArrayStream << decimalTotal;
    }

    return valueArrayStream.done();
}

Value AccumulatorSum::getValue(bool toBeMerged) {
    // Serialize the full state of the partial sum result to avoid incorrect results for certain
    // data set which are composed of 'NumberDecimal' values which cancel each other when being
    // summed and other numeric type values which contribute mostly to sum result and a partial sum
    // of some of 'NumberDecimal' values and other numeric type values happen to lose precision
    // because 'NumberDecimal' can't represent the partial sum precisely, or the other way around.
    //
    // For example, [{n: 1e+34}, {n: NumberDecimal("0,1")}, {n: NumberDecimal("0.11")}, {n:
    // -1e+34}].
    //
    // More fundamentally, addition is neither commutative nor associative on computer. So, it's
    // desirable to keep the full state of the partial sum along the way to maintain the result as
    // close to the real truth as possible until all additions are done.
    //
    // This requires changing over-the-wire data format from a shard-side to a merge-side and is
    // incompatible change and is gated with FCV until 6.0.
    //
    // TODO SERVER-64227: Remove FCV gating which is unnecessary when we branch for 6.1.
    auto&& fcv = serverGlobalParams.featureCompatibility;
    auto canUseNewPartialResultFormat = fcv.isVersionInitialized() &&
        fcv.isGreaterThanOrEqualTo(multiversion::FeatureCompatibilityVersion::kVersion_6_0);
    if (canUseNewPartialResultFormat && toBeMerged) {
        return serializePartialSum(nonDecimalTotalType, totalType, nonDecimalTotal, decimalTotal);
    }

    switch (totalType) {
        case NumberInt:
            if (nonDecimalTotal.fitsLong())
                return Value::createIntOrLong(nonDecimalTotal.getLong());
            [[fallthrough]];
        case NumberLong:
            if (nonDecimalTotal.fitsLong())
                return Value(nonDecimalTotal.getLong());
            // TODO SERVER-64227: Remove the following 'if' block which is buggy.
            if (toBeMerged) {
                // The value was too large for a NumberLong, so output a document with two values
                // adding up to the desired total. Older MongoDB versions used to ignore signed
                // integer overflow and cause undefined behavior, that in practice resulted in
                // values that would wrap around modulo 2**64. Now an older mongos with a newer
                // mongod will yield an error that $sum resulted in a non-numeric type, which is
                // OK for this case. Output the error using the totalType, so in the future we can
                // determine the correct totalType for the sum. For the error to exceed 2**63,
                //  more than 2**53 integers would have to be summed, which is impossible.
                double total;
                double error;
                std::tie(total, error) = nonDecimalTotal.getDoubleDouble();
                long long llerror = static_cast<long long>(error);
                return Value(DOC(subTotalName << total << subTotalErrorName << llerror));
            }
            // Sum doesn't fit a NumberLong, so return a NumberDouble instead.
            [[fallthrough]];
        case NumberDouble:
            return Value(nonDecimalTotal.getDouble());
        case NumberDecimal: {
            return Value(decimalTotal.add(nonDecimalTotal.getDecimal()));
        }
        default:
            MONGO_UNREACHABLE;
    }
}

AccumulatorSum::AccumulatorSum(ExpressionContext* const expCtx) : AccumulatorState(expCtx) {
    // This is a fixed size AccumulatorState so we never need to update this.
    _memUsageBytes = sizeof(*this);
}

void AccumulatorSum::reset() {
    totalType = NumberInt;
    nonDecimalTotalType = NumberInt;
    nonDecimalTotal = {};
    decimalTotal = {};
}
}  // namespace mongo
