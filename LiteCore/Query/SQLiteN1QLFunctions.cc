//
//  SQLiteN1QLFunctions.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/25/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "SQLite_Internal.hh"
#include "SQLiteFleeceUtil.hh"
#include "Path.hh"
#include "Error.hh"
#include "Logging.hh"
#include "function_ref.hh"
#include <regex>
#include <cmath>
#include <string>

#ifdef _MSC_VER
#undef min
#undef max
#endif

using namespace fleece;
using namespace std;

namespace litecore {

    // Implementations of N1QL functions (except for a few that are built into SQLite.)


#pragma mark - ARRAY AGGREGATES:


    static void aggregateNumericArrayOperation(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                               function_ref<void(double, bool&)> op) {
        bool stop = false;
        for (int i = 0; i < argc; ++i) {
            sqlite3_value *arg = argv[i];
            switch (sqlite3_value_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root)
                        return;
                    for (Array::iterator item(root->asArray()); item; ++item) {
                        op(item->asDouble(), stop);
                        if(stop) {
                            return;
                        }
                    }

                    break;
                }
                case SQLITE_NULL:
                    sqlite3_result_null(ctx);
                    return;
                default:
                    sqlite3_result_zeroblob(ctx, 0);
                    return;
            }
        }
    }

    static void aggregateArrayOperation(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                               function_ref<void(const Value *, bool&)> op) {
        bool stop = false;
        for (int i = 0; i < argc; ++i) {
            sqlite3_value *arg = argv[i];
            switch (sqlite3_value_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root)
                        return;

                    if(root->type() != valueType::kArray) {
                        sqlite3_result_zeroblob(ctx, 0);
                        return;
                    }

                    for (Array::iterator item(root->asArray()); item; ++item) {
                        op(item.value(), stop);
                        if(stop) {
                            return;
                        }
                    }

                    break;
                }

                case SQLITE_NULL:
                    sqlite3_result_null(ctx);
                    return;
                default:
                    sqlite3_result_zeroblob(ctx, 0);
                    return;
            }
        }
    }


    // array_sum() function adds up numbers. Any argument that's a number will be added.
    // Any argument that's a Fleece array will have all numeric values in it added.
    static void fl_array_sum(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double sum = 0.0;
        aggregateNumericArrayOperation(ctx, argc, argv, [&sum](double num, bool& stop) {
            sum += num;
        });

        sqlite3_result_double(ctx, sum);
    }

    static void fl_array_avg(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double sum = 0.0;
        double count = 0.0;
        aggregateNumericArrayOperation(ctx, argc, argv, [&sum, &count](double num, bool& stop) {
            sum += num;
            count++;
        });

        if(count == 0.0) {
            sqlite3_result_double(ctx, 0.0);
        } else {
            sqlite3_result_double(ctx, sum / count);
        }
    }

    static void fl_array_contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        slice comparand = valueAsStringSlice(argv[1]);
        bool found = false;
        aggregateArrayOperation(ctx, argc, argv, [&comparand, &found](const Value* val, bool& stop) {
            if(val->toString().compare(comparand) == 0) {
                found = stop = true;
            }
        });

        sqlite3_result_int(ctx, found ? 1 : 0);
    }

    static void fl_array_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_int64 count = 0;
        aggregateArrayOperation(ctx, argc, argv, [&count](const Value* val, bool& stop) {
            if(val->type() != valueType::kNull) {
                count++;
            }
        });

        sqlite3_result_int64(ctx, count);
    }

    static void fl_array_ifnull(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value* foundVal = nullptr;
        aggregateArrayOperation(ctx, argc, argv, [&foundVal](const Value* val, bool& stop) {
            if(val != nullptr && val->type() != valueType::kNull) {
                foundVal = val;
                stop = true;
            }
        });

        if(!foundVal) {
            sqlite3_result_zeroblob(ctx, 0);
        } else {
            setResultFromValue(ctx, foundVal);
        }
    }

    static void fl_array_length(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_int64 count = 0;
        aggregateArrayOperation(ctx, argc, argv, [&count](const Value* val, bool& stop) {
            count++;
        });

        sqlite3_result_int64(ctx, count);
    }

    static void fl_array_max(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double max = numeric_limits<double>::min();
        bool nonEmpty = false;
        aggregateNumericArrayOperation(ctx, argc, argv, [&max, &nonEmpty](double num, bool &stop) {
            max = std::max(num, max);
            nonEmpty = true;
        });

        if(nonEmpty) {
            sqlite3_result_double(ctx, max);
        } else {
            sqlite3_result_zeroblob(ctx, 0);
        }
    }

    static void fl_array_min(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double max = numeric_limits<double>::max();
        bool nonEmpty = false;
        aggregateNumericArrayOperation(ctx, argc, argv, [&max, &nonEmpty](double num, bool &stop) {
            max = std::min(num, max);
            nonEmpty = true;
        });

        if(nonEmpty) {
            sqlite3_result_double(ctx, max);
        } else {
            sqlite3_result_zeroblob(ctx, 0);
        }
    }


#pragma mark - CONDITIONAL TESTS (NULL / MISSING / INF / NAN):


    static void ifmissing(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        for(int i = 0; i < argc; i++) {
            if(sqlite3_value_type(argv[i]) != SQLITE_NULL) {
                sqlite3_result_value(ctx, argv[i]);
                return;
            }
        }
    }

    static void ifmissingornull(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        for(int i = 0; i < argc; i++) {
            if(sqlite3_value_type(argv[i]) != SQLITE_NULL && sqlite3_value_bytes(argv[i]) > 0) {
                sqlite3_result_value(ctx, argv[i]);
                return;
            }
        }
    }

    static void ifnull(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        for(int i = 0; i < argc; i++) {
            if(sqlite3_value_bytes(argv[i]) > 0) {
                sqlite3_result_value(ctx, argv[i]);
                return;
            }
        }
    }

    static void missingif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto slice0 = valueAsSlice(argv[0]);
        auto slice1 = valueAsSlice(argv[1]);
        if(slice0.buf == nullptr || slice1.buf == nullptr || slice0.size == 0 || slice1.size == 0) {
            sqlite3_result_null(ctx);
        }

        if(slice0.compare(slice1) == 0) {
            sqlite3_result_null(ctx);
        } else {
            setResultBlobFromSlice(ctx, slice0);
        }
    }

    static void nullif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto slice0 = valueAsSlice(argv[0]);
        auto slice1 = valueAsSlice(argv[1]);
        if(slice0.buf == nullptr || slice1.buf == nullptr || slice0.size == 0 || slice1.size == 0) {
            sqlite3_result_null(ctx);
        }

        if(slice0.compare(slice1) == 0) {
            sqlite3_result_zeroblob(ctx, 0);
        } else {
            setResultBlobFromSlice(ctx, slice0);
        }
    }

#if 0
    static void ifinf(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double num = 0.0;
        bool success = false;
        aggregateArrayOperation(ctx, argc, argv, [&num, &success](const Value* val, bool &stop) {
            if(val->type() != valueType::kNumber) {
                stop = true;
                return;
            }

            double nextNum = val->asDouble();
            if(!isinf(nextNum)) {
                num = nextNum;
                success = true;
                stop = true;
            }
        });

        if(!success) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_double(ctx, num);
        }
    }
#endif

#if 0
    static void ifnan(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double num = 0.0;
        bool success = false;
        aggregateArrayOperation(ctx, argc, argv, [&num, &success](const Value* val, bool &stop) {
            if(val->type() != valueType::kNumber) {
                stop = true;
                return;
            }

            double nextNum = val->asDouble();
            if(!isnan(nextNum)) {
                num = nextNum;
                success = true;
                stop = true;
            }
        });

        if(!success) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_double(ctx, num);
        }
    }
#endif

#if 0
    static void ifnanorinf(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double num = 0.0;
        bool success = false;
        aggregateArrayOperation(ctx, argc, argv, [&num, &success](const Value* val, bool &stop) {
            if(val->type() != valueType::kNumber) {
                stop = true;
                return;
            }

            double nextNum = val->asDouble();
            if(!isinf(nextNum) && !isnan(nextNum)) {
                num = nextNum;
                success = true;
                stop = true;
            }
        });

        if(!success) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_double(ctx, num);
        }
    }
#endif

#if 0
    static void thisif(sqlite3_context* ctx, int argc, sqlite3_value **argv, double val) noexcept {
        auto slice0 = valueAsSlice(argv[0]);
        auto slice1 = valueAsSlice(argv[1]);
        if(slice0.buf == nullptr || slice1.buf == nullptr || slice0.size == 0 || slice1.size == 0) {
            sqlite3_result_null(ctx);
        }

        if(slice0.compare(slice1) == 0) {
            sqlite3_result_zeroblob(ctx, 0);
        } else {
            sqlite3_result_double(ctx, val);
        }
    }

    static void nanif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        thisif(ctx, argc, argv, numeric_limits<double>::quiet_NaN());
    }

    static void neginfif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        thisif(ctx, argc, argv, -numeric_limits<double>::infinity());
    }

    static void posinfif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        thisif(ctx, argc, argv, numeric_limits<double>::infinity());
    }
#endif

#pragma mark - STRINGS:


#if 0
    static void fl_base64(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsSlice(argv[0]);
        string base64 = arg0.base64String();
        sqlite3_result_text(ctx, (char *)base64.c_str(), (int)base64.size(), SQLITE_TRANSIENT);
    }


    static void fl_base64_decode(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsStringSlice(argv[0]);
        size_t expectedLen = (arg0.size + 3) / 4 * 3;
        alloc_slice decoded(expectedLen);
        arg0.readBase64Into(decoded);
        if(sqlite3_value_type(argv[0]) == SQLITE_TEXT) {
            setResultTextFromSlice(ctx, decoded);
        } else {
            setResultBlobFromSlice(ctx, decoded);
        }
    }
#endif

    static string lowercase(string input) {
        string result(input.size(), '\0');
        transform(input.begin(), input.end(), result.begin(), ptr_fun<int, int>(tolower));
        return result;
    }

    static void contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsStringSlice(argv[0]);
        auto arg1 = valueAsStringSlice(argv[1]);
        sqlite3_result_int(ctx, arg0.find(arg1).buf != nullptr);
    }

#if 0
    static void init_cap(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg = valueAsStringSlice(argv[0]).asString();
        string result = lowercase(arg);
        auto iter = result.begin();
        while(iter != result.end()) {
            *iter = (char)toupper(*iter);
            iter = find_if(iter, result.end(), not1(ptr_fun<int, int>(isalpha)));
            iter = find_if(iter, result.end(),      ptr_fun<int, int>(isalpha));
        }

        sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
    }
#endif

    static void length(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg = valueAsStringSlice(argv[0]).asString();
        sqlite3_result_int64(ctx, arg.size());
    }

    static void lower(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg = valueAsStringSlice(argv[0]).asString();
        string result = lowercase(arg);
        sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
    }

    static void ltrim(string &s, const char* chars = nullptr) {
        if(chars != nullptr) {
            auto startPos = s.find_first_not_of(chars);
            if(string::npos != startPos) {
                s = s.substr(startPos);
            }
        } else {
            s.erase(s.begin(), find_if(s.begin(), s.end(), not1(ptr_fun<int, int>(isspace))));
        }
    }

    static void rtrim(string& s, const char* chars = nullptr) {
        if(chars != nullptr) {
            auto endPos = s.find_last_not_of(chars);
            if(string::npos != endPos) {
                s = s.substr(0, endPos + 1);
            }
        } else {
            s.erase(find_if(s.rbegin(), s.rend(), not1(ptr_fun<int, int>(isspace))).base(), s.end());
        }
    }

    static void ltrim(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        if(argc == 2) {
            ltrim(val, (const char*)sqlite3_value_text(argv[1]));
        } else {
            ltrim(val);
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }

#if 0
    static void position(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        unsigned long result = val.find((char *)sqlite3_value_text(argv[1]));
        if(result == string::npos) {
            sqlite3_result_int64(ctx, -1);
        } else {
            sqlite3_result_int64(ctx, result);
        }
    }
#endif

#if 0
    static void repeat(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto base = valueAsStringSlice(argv[0]).asString();
        auto num = sqlite3_value_int(argv[1]);
        stringstream result;
        for(int i = 0; i < num; i++) {
            result << base;
        }

        auto resultStr = result.str();
        sqlite3_result_text(ctx, resultStr.c_str(), (int)resultStr.size(), SQLITE_TRANSIENT);
    }
#endif

#if 0
    static void replace(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        auto search = valueAsStringSlice(argv[1]).asString();
        auto replacement = valueAsStringSlice(argv[2]).asString();
        int n = -1;
        if(argc == 4) {
            n = sqlite3_value_int(argv[3]);
        }

        size_t start_pos = 0;
        while(n-- && (start_pos = val.find(search, start_pos)) != std::string::npos) {
            val.replace(start_pos, search.length(), replacement);
            start_pos += replacement.length(); // In case 'replacement' contains 'search', like replacing 'x' with 'yx'
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }
#endif

#if 0
    static void reverse(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        reverse(val.begin(), val.end());
        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }
#endif

    static void rtrim(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        if(argc == 2) {
            rtrim(val, (const char *)sqlite3_value_text(argv[1]));
        } else {
            rtrim(val);
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }

#if 0
    static void substr(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        if(argc == 3) {
            val = val.substr(sqlite3_value_int(argv[1]), sqlite3_value_int(argv[2]));
        } else {
            val = val.substr(sqlite3_value_int(argv[1]));
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }
#endif

    static void trim(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = valueAsStringSlice(argv[0]).asString();
        if(argc == 2) {
            auto chars = (const char *)sqlite3_value_text(argv[1]);
            ltrim(val, chars);
            rtrim(val, chars);
        } else {
            ltrim(val);
            rtrim(val);
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }

    static void upper(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg = valueAsStringSlice(argv[0]).asString();
        string result(arg.size(), '\0');
        transform(arg.begin(), arg.end(), result.begin(), ptr_fun<int, int>(toupper));
        sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
    }


#pragma mark - REGULAR EXPRESSIONS:


    static void regexp_like(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsStringSlice(argv[0]);
        auto arg1 = valueAsStringSlice(argv[1]);
        regex r((char *)arg1.buf);
        int result = regex_search((char *)arg0.buf, r) ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void regexp_position(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsStringSlice(argv[0]);
        auto arg1 = valueAsStringSlice(argv[1]);
        regex r((char *)arg1.buf);
        cmatch pattern_match;
        if(!regex_search((char *)arg0.buf, pattern_match, r)) {
            sqlite3_result_int64(ctx, -1);
            return;
        }

        sqlite3_result_int64(ctx, pattern_match.prefix().length());
    }

    static void regexp_replace(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto expression = valueAsStringSlice(argv[0]).asString();
        auto pattern = valueAsStringSlice(argv[1]);
        auto repl = valueAsStringSlice(argv[2]).asString();
        string result;
        auto out = back_inserter(result);
        int n = -1;
        if(argc == 4) {
            n = sqlite3_value_int(argv[3]);
        }

        regex r((char *)pattern.buf);
        auto iter = sregex_iterator(expression.begin(), expression.end(), r);
        auto last_iter = iter;
        auto stop = sregex_iterator();
        for(; n-- && iter != stop; ++iter) {
            out = copy(iter->prefix().first, iter->prefix().second, out);
            out = iter->format(out, repl);
            last_iter = iter;
        }

        out = copy(last_iter->suffix().first, last_iter->suffix().second, out);
        sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
    }


#pragma mark - MATH:


    static bool isNumeric(sqlite3_context* ctx, sqlite3_value *arg) {
        auto type = sqlite3_value_type(arg);
        if (_usuallyTrue(type == SQLITE_FLOAT || type == SQLITE_INTEGER))
            return true;
        else {
            sqlite3_result_error(ctx, "Invalid numeric value", SQLITE_MISMATCH);
            return false;
        }
    }


    static void unaryFunction(sqlite3_context* ctx, sqlite3_value **argv, double (*fn)(double)) {
        sqlite3_value *arg = argv[0];
        if (_usuallyTrue(isNumeric(ctx, arg)))
            sqlite3_result_double(ctx, fn(sqlite3_value_double(arg)));
    }

    #define DefineUnaryMathFn(NAME, C_FN) \
        static void fl_##NAME(sqlite3_context* ctx, int argc, sqlite3_value **argv) { \
            unaryFunction(ctx, argv, C_FN); \
        }

    DefineUnaryMathFn(abs,   abs)
    DefineUnaryMathFn(acos,  acos)
    DefineUnaryMathFn(asin,  asin)
    DefineUnaryMathFn(atan,  atan)
    DefineUnaryMathFn(ceil,  ceil)
    DefineUnaryMathFn(cos,   cos)
    DefineUnaryMathFn(degrees, [](double rad) {return rad * 180 / M_PI;})
    DefineUnaryMathFn(exp,   exp)
    DefineUnaryMathFn(floor, floor)
    DefineUnaryMathFn(ln,    log)
    DefineUnaryMathFn(log,   log10)
    DefineUnaryMathFn(radians, [](double deg) {return deg * M_PI / 180;})
    DefineUnaryMathFn(sin,   sin)
    DefineUnaryMathFn(sqrt,  sqrt)
    DefineUnaryMathFn(tan,   tan)


    static void fl_atan2(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (isNumeric(ctx, argv[0]) && isNumeric(ctx, argv[1]))
            sqlite3_result_double(ctx, atan2(sqlite3_value_double(argv[0]),
                                             sqlite3_value_double(argv[1])));
    }

    static void fl_power(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (isNumeric(ctx, argv[0]) && isNumeric(ctx, argv[1]))
            sqlite3_result_double(ctx, pow(sqlite3_value_double(argv[0]),
                                           sqlite3_value_double(argv[1])));
    }

    static void fl_e(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        sqlite3_result_double(ctx, M_E);
    }

    static void fl_pi(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        sqlite3_result_double(ctx, M_PI);
    }

    static void roundTo(sqlite3_context* ctx, int argc, sqlite3_value **argv, double (*fn)(double)) {
        // Takes an optional 2nd argument giving the number of decimal places to round to.
        if (!isNumeric(ctx, argv[0]))
            return;
        double result = sqlite3_value_double(argv[0]);

        if(argc == 1) {
            result = fn(result);
        } else {
            if (!isNumeric(ctx, argv[1]))
                return;
            double scale = pow(10, sqlite3_value_double(argv[1]));
            result = fn(result * scale) / scale;
        }

        sqlite3_result_double(ctx, result);
    }

    static void fl_round(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        roundTo(ctx, argc, argv, round);
    }

    static void fl_trunc(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        roundTo(ctx, argc, argv, trunc);
    }

    static void fl_sign(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (!isNumeric(ctx, argv[0]))
            return;
        double num = sqlite3_value_double(argv[0]);
        sqlite3_result_int(ctx, num > 0 ? 1 : (num < 0 ? -1 : 0) );
    }


#pragma mark - TYPE TESTS & CONVERSIONS:


    static const string value_type(sqlite3_context* ctx, sqlite3_value *arg) {
        switch(sqlite3_value_type(arg)) {
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
                return "number";
            case SQLITE_TEXT:
                return "string";
            case SQLITE_NULL:
                return "missing";
            case SQLITE_BLOB:
            {
                if(sqlite3_value_bytes(arg) == 0) {
                    return "null";
                }

                auto fleece = fleeceParam(ctx, arg);
                if(fleece == nullptr) {
                    return "null";
                }

                switch(fleece->type()) {
                    case valueType::kArray:
                        return "array";
                    case valueType::kBoolean:
                        return "boolean";
                    case valueType::kData:
                        return "binary";
                    case valueType::kDict:
                        return "object";
                    case valueType::kNull:
                        return "null";
                    case valueType::kNumber:
                        return "number";
                    case valueType::kString:
                        return "string";
                }
            }
            default:
                return "missing";
        }
    }

    static void isarray(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "array" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void isatom(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto type = value_type(ctx, argv[0]);
        int result = (type == "boolean" || type == "number" || type == "string") ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void isboolean(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "boolean" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void isnumber(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "number" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void isobject(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "object" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void isstring(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "string" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    static void type(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto result =  value_type(ctx, argv[0]);
        sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
    }

    static void toatom(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        // MISSING is MISSING.
        // NULL is NULL.
        // Arrays of length 1 are the result of TOATOM() on their single element.
        // Objects of length 1 are the result of TOATOM() on their single value.
        // Booleans, numbers, and strings are themselves.
        // All other values are NULL.
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
            case SQLITE_TEXT:
                sqlite3_result_value(ctx, argv[0]);
                break;
            case SQLITE_BLOB:
                if(sqlite3_value_bytes(argv[0]) == 0) {
                    sqlite3_result_zeroblob(ctx, 0);
                    break;
                }

                auto fleece = fleeceParam(ctx, argv[0]);
                if(fleece == nullptr) {
                    sqlite3_result_zeroblob(ctx, 0);
                    break;
                }

                switch(fleece->type()) {
                    case valueType::kArray:
                    {
                        auto arr = fleece->asArray();
                        if(arr->count() != 1) {
                            sqlite3_result_zeroblob(ctx, 0);
                            break;
                        }

                        setResultFromValue(ctx, arr->get(0));
                        break;
                    }
                    case valueType::kDict:
                    {
                        auto dict = fleece->asDict();
                        if(dict->count() != 1) {
                            sqlite3_result_zeroblob(ctx, 0);
                            break;
                        }


                        auto iter = dict->begin();
                        setResultFromValue(ctx, iter.value());
                        break;
                    }
                    case valueType::kData:
                    default:                     // Other Fleece types never show up in blobs
                        sqlite3_result_zeroblob(ctx, 0);
                        break;

                }
        }
    }

    static void toboolean(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        // MISSING is MISSING.
        // NULL is NULL.
        // False is false.
        // Numbers +0, -0, and NaN are false.
        // Empty strings, arrays, and objects are false.
        // All other values are true.
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
            {
                auto val = sqlite3_value_double(argv[0]);
                if(val == 0.0 || isnan(val)) {
                    sqlite3_result_int(ctx, 0);
                } else {
                    sqlite3_result_int(ctx, 1);
                }
                break;
            }
            case SQLITE_TEXT:
            {
                // Need to call sqlite3_value_text here?
                auto result = sqlite3_value_bytes(argv[0]) > 0 ? 1 : 0;
                sqlite3_result_int(ctx, result);
                break;
            }
            case SQLITE_BLOB:
            {
                if(sqlite3_value_bytes(argv[0]) == 0) {
                    sqlite3_result_int(ctx, 0);
                    break;
                }

                auto fleece = fleeceParam(ctx, argv[0]);
                if(fleece == nullptr) {
                    sqlite3_result_int(ctx, 0);
                    break;
                }

                switch(fleece->type()) {
                    case valueType::kArray:
                    {
                        auto arr = fleece->asArray();
                        auto result = arr->count() > 0 ? 1 : 0;
                        sqlite3_result_int(ctx, result);
                        break;
                    }
                    case valueType::kData:
                        sqlite3_result_int(ctx, 1);
                        break;
                    case valueType::kDict:
                    {
                        auto dict = fleece->asDict();
                        auto result = dict->count() > 0 ? 1 : 0;
                        sqlite3_result_int(ctx, result);
                        break;
                    }
                    default:
                        // Other Fleece types never show up in blobs
                        sqlite3_result_int(ctx, 0);
                        break;
                }
            }
        }
    }

    static double tonumber(const string &s) {
        try {
            return stod(s);
        } catch (const invalid_argument&) {
            return NAN;
        } catch (const out_of_range&) {
            return NAN;
        }
    }

    static void tonumber(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        // MISSING is MISSING.
        // NULL is NULL.
        // False is 0.
        // True is 1.
        // Numbers are themselves.
        // Strings that parse as numbers are those numbers.
        // All other values are NULL.
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
            {
                sqlite3_result_value(ctx, argv[0]);
                break;
            }
            case SQLITE_TEXT:
            {
                auto txt = (const char *)sqlite3_value_text(argv[0]);
                string str(txt, sqlite3_value_bytes(argv[0]));
                double result = tonumber(str);
                if(result == NAN) {
                    sqlite3_result_zeroblob(ctx, 0);
                } else {
                    sqlite3_result_double(ctx, result);
                }
                break;
            }
            case SQLITE_BLOB:
            {
                // A blob is a Fleece array, dict, or data; all of which result in NULL.
                sqlite3_result_zeroblob(ctx, 0);
                break;
            }

        }
    }

    static void tostring(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        // MISSING is MISSING.
        // NULL is NULL.
        // False is "false".
        // True is "true".
        // Numbers are their string representation.
        // Strings are themselves.
        // All other values are NULL.
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            {
                auto num = sqlite3_value_double(argv[0]);
                auto str = to_string(num);
                sqlite3_result_text(ctx, str.c_str(), (int)str.size(), SQLITE_TRANSIENT);
                break;
            }
            case SQLITE_INTEGER:
            {
                auto num = sqlite3_value_int64(argv[0]);
                auto str = to_string(num);
                sqlite3_result_text(ctx, str.c_str(), (int)str.size(), SQLITE_TRANSIENT);
                break;
            }
            case SQLITE_TEXT:
            {
                sqlite3_result_value(ctx, argv[0]);
                break;
            }
            case SQLITE_BLOB:
            {
                // A blob is a Fleece array, dict, or data; all of which result in NULL.
                sqlite3_result_zeroblob(ctx, 0);
                break;
            }
        }
    }


#pragma mark - REGISTRATION:


    static void unimplemented(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        Warn("Calling unimplemented N1QL function; query will fail");
        sqlite3_result_error(ctx, "unimplemented N1QL function", -1);
    }


    const SQLiteFunctionSpec kN1QLFunctionsSpec[] = {
//        { "array_append",     -1, unimplemented },
        { "array_avg",        -1, fl_array_avg },
//        { "array_concat",     -1, unimplemented },
        { "array_contains",   -1, fl_array_contains },
        { "array_count",      -1, fl_array_count },
//        { "array_distinct",    1, unimplemented },
//        { "array_flatten",     2, unimplemented },
//        { "array_agg",         1, unimplemented },
        { "array_ifnull",     -1, fl_array_ifnull },
//        { "array_insert",     -1, unimplemented },
//        { "array_intersect",  -1, unimplemented },
        { "array_length",     -1, fl_array_length },
        { "array_max",        -1, fl_array_max },
        { "array_min",        -1, fl_array_min },
//        { "array_position",    2, unimplemented },
//        { "array_prepend",    -1, unimplemented },
//        { "array_put",        -1, unimplemented },
//        { "array_range",       2, unimplemented },
//        { "array_range",       3, unimplemented },
//        { "array_remove",     -1, unimplemented },
//        { "array_repeat",      2, unimplemented },
//        { "array_replace",     3, unimplemented },
//        { "array_replace",     4, unimplemented },
//        { "array_reverse",     1, unimplemented },
//        { "array_sort",        1, unimplemented },
//        { "array_star",        1, unimplemented },
        { "array_sum",        -1, fl_array_sum },
//        { "array_symdiff",    -1, unimplemented },
//        { "array_symdiffn",   -1, unimplemented },
//        { "array_union",      -1, unimplemented },

        { "ifmissing",        -1, ifmissing },
        { "ifmissingornull",  -1, ifmissingornull },
        { "ifnull",           -1, ifnull },
        { "missingif",         2, missingif },
        { "nullif",            2, nullif },

//        { "ifinf",            -1, ifinf },
//        { "isnan",            -1, ifnan },
//        { "isnanorinf",       -1, ifnanorinf },
//        { "nanif",             2, nanif },
//        { "neginfif",          2, neginfif },
//        { "posinfif",          2, posinfif },

//        { "base64",            1, fl_base64 },
//        { "base64_encode",     1, fl_base64 },
//        { "base64_decode",     1, fl_base64_decode },

        { "contains",          2, contains },
//        { "initcap",           1, init_cap },
        { "length",            1, length },
        { "lower",             1, lower },
        { "ltrim",             1, ltrim },
        { "ltrim",             2, ltrim },
//        { "position",          2, position },
//        { "repeat",            2, repeat },
//        { "replace",           3, replace },
//        { "replace",           4, replace },
//        { "reverse",           1, reverse },
        { "rtrim",             1, rtrim },
        { "rtrim",             2, rtrim },
//        { "split",             1, unimplemented },
//        { "split",             2, unimplemented },
//        { "substr",            2, substr },
//        { "substr",            3, substr },
//        { "suffixes",          1, unimplemented },
//        { "title",             1, init_cap },
//        { "tokens",            2, unimplemented },
        { "trim",              1, trim },
        { "trim",              2, trim },
        { "upper",             1, upper },

        { "regexp_contains",   2, regexp_like, },
        { "regexp_like",       2, regexp_like },
        { "regexp_position",   2, regexp_position },
        { "regexp_replace",    3, regexp_replace },
        { "regexp_replace",    4, regexp_replace },

        { "isarray",           1, isarray },
        { "isatom",            1, isatom },
        { "isboolean",         1, isboolean },
        { "isnumber",          1, isnumber },
        { "isobject",          1, isobject },
        { "isstring",          1, isstring },
        { "type",              1, type },
        { "toarray",           1, unimplemented },
        { "toatom",            1, toatom },
        { "toboolean",         1, toboolean },
        { "tonumber",          1, tonumber },
        { "toobject",          1, unimplemented },
        { "tostring",          1, tostring },

        { "abs",               1, fl_abs },
        { "acos",              1, fl_acos },
        { "asin",              1, fl_asin },
        { "atan",              1, fl_atan },
        { "atan2",             2, fl_atan2 },
        { "ceil",              1, fl_ceil },
        { "cos",               1, fl_cos },
        { "degrees",           1, fl_degrees },
        { "e",                 0, fl_e },
        { "exp",               1, fl_exp },
        { "ln",                1, fl_ln },
        { "log",               1, fl_log },
        { "floor",             1, fl_floor },
        { "pi",                0, fl_pi },
        { "power",             2, fl_power },
        { "radians",           1, fl_radians },
        { "round",             1, fl_round },
        { "round",             2, fl_round },
        { "sign",              1, fl_sign },
        { "sin",               1, fl_sin },
        { "sqrt",              1, fl_sqrt },
        { "tan",               1, fl_tan },
        { "trunc",             1, fl_trunc },
        { "trunc",             2, fl_trunc },
        { }
    };

}